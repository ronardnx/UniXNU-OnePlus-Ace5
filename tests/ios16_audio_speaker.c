/* All register values are synthetic; this tests I/O order and failure handling. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/audio/giulia_audio_speaker.h"
#include "../ios16/common/giulia_sha256.h"

struct mock {
    const uint8_t *trusted_fixture;
    size_t trusted_size;
    uint16_t regs[2][256];
    unsigned reset[2], reads, writes, delays, starts, fallbacks;
    unsigned fail_read, fail_write, fail_reset, fail_delay, check_fails;
};
/* The I/O model authenticates an explicitly selected synthetic test fixture.
 * Production callback must hash the data; no real firmware is linked here. */
static int auth(void *v, const uint8_t *p, size_t n, const uint8_t expected[32])
{
    struct mock *m = v;
    assert(expected[0] == 0xc8 && expected[31] == 0xdc);
    return n != m->trusted_size || memcmp(p, m->trusted_fixture, n) != 0;
}
static int stock_auth(void *v, const uint8_t *p, size_t n, const uint8_t expected[32])
{
    uint8_t actual[32];
    (void)v;
    gwh_sha256(p, n, actual);
    return gwh_equal(actual, expected) ? 0 : -1;
}
static int rd(void *v, unsigned ch, uint8_t reg, uint16_t *value)
{
    struct mock *m = v;
    assert(ch < 2);
    if (++m->reads == m->fail_read) return -1;
    *value = m->regs[ch][reg];
    if (reg == 0x11 && m->check_fails) { m->check_fails--; *value = 0; }
    return 0;
}
static int wr(void *v, unsigned ch, uint8_t reg, uint16_t value)
{
    struct mock *m = v;
    assert(ch < 2 && !m->reset[ch]);
    if (++m->writes == m->fail_write) return -1;
    m->regs[ch][reg] = value;
    if (reg == 0x10 && value == 0x1234) m->starts++;
    if (reg == 0x24) m->fallbacks++;
    return 0;
}
static int rst(void *v, unsigned ch, unsigned asserted)
{
    struct mock *m = v;
    if (m->fail_reset == ch+1) return -1;
    m->reset[ch] = asserted;
    return 0;
}
static int delay(void *v, uint32_t us)
{
    struct mock *m = v;
    assert(us <= 1000000);
    return ++m->delays == m->fail_delay ? -1 : 0;
}
static void setup(struct gsipa *s, struct mock *m, const uint8_t *blob, size_t size)
{
    struct gsipa_ops ops = { m, auth, rd, wr, rst, delay };
    memset(m, 0, sizeof(*m));
    m->trusted_fixture = blob; m->trusted_size = size;
    m->regs[0][6] = m->regs[1][6] = 0x5880;
    m->regs[0][0x11] = m->regs[1][0x11] = 1;
    assert(gsipa_init(s, blob, size, &ops) == 0);
    assert(!m->reads && !m->writes && !m->delays);
}
static void stopped(const struct gsipa *s, const struct mock *m)
{
    assert(!s->started && !s->probed);
    assert(m->reset[0] && m->reset[1]);
}
int main(int argc, char **argv)
{
    uint8_t blob[8192], changed[8192], encoded[6] = { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45 };
    size_t size;
    FILE *file;
    struct gsipa s;
    struct mock m;
    unsigned i;
    assert(argc == 2 || argc == 3);
    file = fopen(argv[1], "rb"); assert(file);
    size = fread(blob, 1, sizeof(blob), file); fclose(file);
    assert(size > 188 && size < sizeof(blob));
    assert(gsipa_crc8((const uint8_t *)"123456789", 9) == 0xa1);
    if (argc == 3) {
        struct gsipa_ops ops = { &m, stock_auth, rd, wr, rst, delay };
        memset(&m, 0, sizeof(m));
        assert(strcmp(argv[2], "--validate-stock") == 0);
        assert(gsipa_init(&s, blob, size, &ops) == 0 && s.valid);
        assert(!m.reads && !m.writes && !m.delays);
        blob[size-1] ^= 1;
        i = gsipa_crc32(blob+4, size-4);
        blob[0] = (uint8_t)i; blob[1] = (uint8_t)(i >> 8);
        blob[2] = (uint8_t)(i >> 16); blob[3] = (uint8_t)(i >> 24);
        assert(gsipa_init(&s, blob, size, &ops) == GSIPA_EAUTH && !s.valid);
        puts("Stock SIPA SHA256 and bounds validated; no I/O executed");
        return 0;
    }

    setup(&s, &m, blob, size);
    assert(gsipa_start(&s, GSIPA_REQUIRED) == GSIPA_ENOTREADY);
    assert(!m.writes);
    assert(gsipa_probe(&s) == 0);
    for (i = 0; i < GSIPA_REQUIRED; i++)
        assert(gsipa_start(&s, i) == GSIPA_ENOTREADY);
    assert(!m.writes);
    assert(gsipa_start(&s, GSIPA_REQUIRED) == 0 && s.started);
    assert(m.regs[0][0x10] == 0x1234 && m.regs[1][0x10] == 0x1234);
    i = m.writes;
    assert(gsipa_start(&s, GSIPA_REQUIRED) == 0 && m.writes == i);
    assert(gsipa_start(&s, GSIPA_DSP_READY) == GSIPA_ENOTREADY);
    stopped(&s, &m);

    /* Nonzero efuse validates big-endian mask packing and alternate silicon CRC. */
    setup(&s, &m, blob, size);
    assert(gsipa_probe(&s) == 0);
    for (i = 0; i < 2; i++) {
        m.regs[i][0x20] = 0xabcd; m.regs[i][0x21] = 0xef01; m.regs[i][0x22] = 0x2345;
        m.regs[i][0x23] = gsipa_crc8(encoded, sizeof(encoded));
    }
    assert(gsipa_start(&s, GSIPA_REQUIRED) == 0 && !m.fallbacks);
    assert(gsipa_stop(&s) == 0); stopped(&s, &m);
    assert(gsipa_probe(&s) == 0);
    encoded[0] &= 0x0f; encoded[4] &= 0xf0; encoded[5] &= 0x1f;
    m.regs[0][0x23] = m.regs[1][0x23] = gsipa_crc8(encoded, sizeof(encoded));
    assert(gsipa_start(&s, GSIPA_REQUIRED) == 0 && !m.fallbacks);
    assert(gsipa_stop(&s) == 0); stopped(&s, &m);

    setup(&s, &m, blob, size);
    assert(gsipa_probe(&s) == 0);
    m.regs[1][0x23] = 0x55;
    assert(gsipa_start(&s, GSIPA_REQUIRED) == GSIPA_ETRIM);
    assert(m.fallbacks == 1); stopped(&s, &m);

    setup(&s, &m, blob, size);
    m.regs[1][6] = 0xffff;
    assert(gsipa_probe(&s) == GSIPA_EID); stopped(&s, &m);
    setup(&s, &m, blob, size);
    m.fail_read = 1;
    assert(gsipa_probe(&s) == GSIPA_EIO); stopped(&s, &m);
    setup(&s, &m, blob, size);
    m.fail_delay = 1;
    assert(gsipa_probe(&s) == GSIPA_EIO); stopped(&s, &m);
    for (i = 1; i <= 2; i++) {
        setup(&s, &m, blob, size);
        m.fail_reset = i;
        assert(gsipa_probe(&s) == GSIPA_EIO);
        assert(!s.started && !s.probed && s.cleanup_error == GSIPA_EIO);
        assert(m.reset[2-i]); /* Other amplifier is still independently reset. */
    }

    setup(&s, &m, blob, size);
    assert(gsipa_probe(&s) == 0);
    m.check_fails = 4;
    assert(gsipa_start(&s, GSIPA_REQUIRED) == 0 && m.starts == 6);
    assert(gsipa_stop(&s) == 0);
    setup(&s, &m, blob, size);
    assert(gsipa_probe(&s) == 0);
    m.check_fails = 5;
    assert(gsipa_start(&s, GSIPA_REQUIRED) == GSIPA_ECHECK && m.starts == 5);
    stopped(&s, &m);

    /* A bus fault must never retry a partly written startup program. */
    setup(&s, &m, blob, size);
    assert(gsipa_probe(&s) == 0);
    m.fail_write = 2;
    assert(gsipa_start(&s, GSIPA_REQUIRED) == GSIPA_EIO && !m.starts);
    stopped(&s, &m);
    setup(&s, &m, blob, size);
    assert(gsipa_probe(&s) == 0);
    assert(gsipa_start(&s, GSIPA_REQUIRED) == 0);
    m.fail_write = m.writes + 1;
    assert(gsipa_stop(&s) == GSIPA_EIO && s.cleanup_error == GSIPA_EIO);
    stopped(&s, &m);

    /* Validation precedes I/O even for a re-sealed malformed descriptor. */
    memcpy(changed, blob, size);
    memset(changed + 188 + 19*4, 0xff, 4);
    i = gsipa_crc32(changed+4, size-4);
    changed[0] = (uint8_t)i; changed[1] = (uint8_t)(i >> 8);
    changed[2] = (uint8_t)(i >> 16); changed[3] = (uint8_t)(i >> 24);
    { struct gsipa_ops ops = { &m, auth, rd, wr, rst, delay };
      i = m.writes;
      assert(gsipa_init(&s, changed, size, &ops) == GSIPA_EAUTH);
      assert(!s.valid && m.writes == i);
      m.trusted_fixture = changed;
      assert(gsipa_init(&s, changed, size, &ops) == GSIPA_EINVAL);
      assert(!s.valid && m.writes == i); }
    puts("SIPA fault-injection checks passed");
    return 0;
}
