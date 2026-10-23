/* Exercise the real observer-only helpers against a bounded SPMI v7 bus. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/giulia_keys.h"

static struct {
    uint32_t version, features, mapping[1024], status, data;
    unsigned commands, status_reads, delays, complete_after, apid, offset;
    unsigned type[2], subtype[2], sample[2];
} bus;

static uint32_t read32(void *context, unsigned space, unsigned offset)
{
    assert(context == &bus);
    if (space == GK_CORE) {
        if (offset == 0) return bus.version;
        if (offset == 4) return bus.features;
        assert(offset >= 0x2000 && offset < 0x3000 && !(offset & 3));
        return bus.mapping[(offset - 0x2000) / 4];
    }
    assert(space == GK_OBSERVER && offset / 0x20 == bus.apid);
    if (offset % 0x20 == 8) {
        bus.status_reads++;
        return bus.status_reads <= bus.complete_after ? 0 : bus.status;
    }
    assert(offset % 0x20 == 0x18);
    return bus.data;
}

static void write32(void *context, unsigned space, unsigned offset, uint32_t value)
{
    assert(context == &bus);
    /* Any PMIC/channel-data write, IRQ acknowledgement or extra byte fails. */
    assert(space == GK_OBSERVER && offset < 0x8000 && !(offset % 0x20));
    assert((value & ~0xff0u) == 0x08000000u);
    bus.apid = offset / 0x20;
    bus.offset = (value >> 4) & 255;
    bus.commands++;
    bus.status_reads = 0;
    unsigned ppid = (bus.mapping[bus.apid] >> 8) & 0xfff;
    assert(ppid == 0x013 || ppid == 0x18d);
    unsigned channel = ppid == 0x18d;
    if (bus.offset == 4) bus.data = bus.type[channel];
    else if (bus.offset == 5) bus.data = bus.subtype[channel];
    else { assert(bus.offset == 0x10 || bus.offset == 255); bus.data = bus.sample[channel]; }
}

static void delay_us(void *context, unsigned us)
{
    assert(context == &bus && us == 1);
    bus.delays++;
}

static const struct gk_io io = { &bus, read32, write32, delay_us };

static void reset(struct gk_hw *hw)
{
    memset(&bus, 0, sizeof(bus));
    memset(hw, 0, sizeof(*hw));
    bus.version = 0x70010000;
    bus.features = 0x80060380; /* Captured count 896; APIDs are not constants. */
    bus.mapping[533] = 0x01001300;
    bus.mapping[559] = 0x01018d00;
    bus.type[0] = 1; bus.subtype[0] = 9;
    bus.type[1] = 0x10; bus.subtype[1] = 0x13;
    bus.sample[0] = 0x10; bus.sample[1] = 1;
    bus.status = 1;
}

static unsigned channels_and_transactions(void)
{
    struct gk_hw hw;
    unsigned cases = 0, value;
    reset(&hw);
    assert(gk_resolve(&io, &hw) == GK_OK && !bus.commands);
    assert(hw.pon.apid == 533 && hw.gpio.apid == 559);
    assert(gk_identify(&io, &hw) == GK_OK && bus.commands == 4);
    assert(gk_read(&io, &hw, &hw.pon, 0x10, &value) == GK_OK && value == 0x10);
    assert(gk_read(&io, &hw, &hw.gpio, 0x10, &value) == GK_OK && value == 1);
    assert(!bus.delays); cases++;

    /* APID assignment may change across boots; include the highest channel. */
    reset(&hw); bus.features = 1024;
    bus.mapping[0] = bus.mapping[533]; bus.mapping[533] = 0;
    bus.mapping[1023] = bus.mapping[559]; bus.mapping[559] = 0;
    assert(gk_resolve(&io, &hw) == GK_OK && hw.pon.apid == 0 && hw.gpio.apid == 1023);
    assert(gk_identify(&io, &hw) == GK_OK && bus.apid == 1023); cases++;

    const uint32_t versions[] = {0, 0x50000000, 0xffffffff};
    for (unsigned i = 0; i < sizeof(versions) / sizeof(*versions); i++) {
        reset(&hw); bus.version = versions[i];
        assert(gk_resolve(&io, &hw) == GK_VERSION && !bus.commands); cases++;
    }
    const uint32_t features[] = {0, 1025, 0xffffffff};
    for (unsigned i = 0; i < sizeof(features) / sizeof(*features); i++) {
        reset(&hw); bus.features = features[i];
        assert(gk_resolve(&io, &hw) == GK_VERSION && !bus.commands); cases++;
    }
    for (unsigned channel = 0; channel < 2; channel++) {
        reset(&hw); bus.mapping[channel ? 559 : 533] = 0;
        assert(gk_resolve(&io, &hw) == GK_CHANNEL && !bus.commands); cases++;
        reset(&hw); bus.mapping[800] = bus.mapping[channel ? 559 : 533];
        assert(gk_resolve(&io, &hw) == GK_CHANNEL && !bus.commands); cases++;
    }
    reset(&hw); bus.mapping[800] = 0xffffffff;
    assert(gk_resolve(&io, &hw) == GK_IO && !bus.commands); cases++;
    reset(&hw); bus.mapping[533] = 0x01011300; /* Different SID, same peripheral. */
    assert(gk_resolve(&io, &hw) == GK_CHANNEL && !bus.commands); cases++;

    for (unsigned channel = 0; channel < 2; channel++) {
        for (unsigned subtype = 0; subtype < 2; subtype++) {
            reset(&hw); assert(gk_resolve(&io, &hw) == GK_OK);
            if (subtype) bus.subtype[channel] ^= 1;
            else bus.type[channel] ^= 1;
            assert(gk_identify(&io, &hw) == GK_IDENTITY); cases++;
        }
    }
    reset(&hw); assert(gk_resolve(&io, &hw) == GK_OK);
    bus.mapping[533] ^= 1;
    assert(gk_read(&io, &hw, &hw.pon, 0x10, &value) == GK_CHANGED && !bus.commands); cases++;
    reset(&hw); assert(gk_resolve(&io, &hw) == GK_OK);
    assert(gk_read(&io, &hw, &hw.pon, 256, &value) == GK_CHANNEL && !bus.commands); cases++;
    hw.pon.apid = 1024;
    assert(gk_read(&io, &hw, &hw.pon, 0x10, &value) == GK_CHANNEL && !bus.commands); cases++;

    const uint32_t errors[] = {2, 4, 8, 3, 5, 9, 15, 0xffffffff};
    for (unsigned i = 0; i < sizeof(errors) / sizeof(*errors); i++) {
        reset(&hw); assert(gk_resolve(&io, &hw) == GK_OK);
        bus.status = errors[i]; value = 0x1234;
        assert(gk_read(&io, &hw, &hw.gpio, 0x10, &value) == GK_IO);
        assert(value == 0x1234 && bus.commands == 1 && !bus.delays); cases++;
    }
    reset(&hw); assert(gk_resolve(&io, &hw) == GK_OK);
    bus.complete_after = 999;
    assert(gk_read(&io, &hw, &hw.pon, 0x10, &value) == GK_OK);
    assert(bus.commands == 1 && bus.status_reads == 1000 && bus.delays == 999); cases++;
    reset(&hw); assert(gk_resolve(&io, &hw) == GK_OK);
    bus.complete_after = 1000; value = 0x1234;
    assert(gk_read(&io, &hw, &hw.pon, 0x10, &value) == GK_TIMEOUT);
    assert(value == 0x1234 && bus.commands == 1 && bus.status_reads == 1000 && bus.delays == 1000); cases++;
    reset(&hw); assert(gk_resolve(&io, &hw) == GK_OK);
    bus.sample[0] = 0xabcdef80;
    assert(gk_read(&io, &hw, &hw.pon, 255, &value) == GK_OK && value == 0x80); cases++;
    return cases;
}

static unsigned decoding(void)
{
    /* Exhaust all byte pairs, including unrelated PON and GPIO status bits. */
    for (unsigned pon = 0; pon < 256; pon++) {
        for (unsigned gpio = 0; gpio < 256; gpio++) {
            unsigned keys = gk_decode(pon, gpio);
            assert(!!(keys & 1) == !!(pon & 0x80));
            assert(!!(keys & 2) == !!(pon & 0x40));
            assert(!!(keys & 4) == !(gpio & 1));
            assert(!(keys & ~7u));
        }
    }
    return 256 * 256;
}

static unsigned debounce_and_delivery(void)
{
    struct gk_filter state = {0};
    assert(!gk_filter_sample(&state, 1, 100));
    assert(!gk_filter_sample(&state, 0, 110));
    assert(!gk_filter_sample(&state, 1, 120));
    assert(!gk_filter_sample(&state, 1, 159));
    assert(gk_filter_sample(&state, 1, 160) == 1);
    /* An unavailable HID service/event allocation leaves delivery pending. */
    assert(!state.delivered && gk_filter_sample(&state, 1, 170) == 1);
    state.delivered = 1;
    assert(!gk_filter_sample(&state, 1, 180));
    assert(!gk_filter_sample(&state, 0, 200));
    assert(!gk_filter_sample(&state, 1, 210));
    assert(!gk_filter_sample(&state, 0, 220));
    assert(!gk_filter_sample(&state, 0, 259));
    assert(gk_filter_sample(&state, 0, 260) == 1);
    assert(gk_filter_sample(&state, 0, 270) == 1 && state.delivered == 1);
    state.delivered = 0;
    assert(!gk_filter_sample(&state, 0, 280));

    memset(&state, 0, sizeof(state));
    assert(!gk_filter_sample(&state, 3, 100));
    assert(!gk_filter_sample(&state, 7, 120));
    assert(gk_filter_sample(&state, 7, 140) == 3); /* GPIO has its own clock. */
    state.delivered = 1; /* Only one event was accepted. */
    assert(gk_filter_sample(&state, 7, 150) == 2);
    state.delivered = 3;
    assert(gk_filter_sample(&state, 7, 160) == 4);
    state.delivered = 7;
    assert(!gk_filter_sample(&state, 7, 170));
    assert(!gk_filter_sample(&state, 0, 200));
    assert(gk_filter_sample(&state, 0, 240) == 7 && state.stable == 0);

    /* Startup held keys and long uptimes retain the full 40 ms interval. */
    memset(&state, 0, sizeof(state));
    uint64_t now = UINT64_C(1) << 40;
    assert(!gk_filter_sample(&state, 7, now));
    assert(!gk_filter_sample(&state, 7, now + 39));
    assert(gk_filter_sample(&state, 7, now + 40) == 7);
    return 3;
}

int main(void)
{
    unsigned transactions = channels_and_transactions();
    unsigned pairs = decoding();
    unsigned sequences = debounce_and_delivery();
    printf("Physical keys: %u channel/identity/transaction/fault cases, %u byte pairs and %u debounce/delivery sequences passed\n",
        transactions, pairs, sequences);
    return 0;
}
