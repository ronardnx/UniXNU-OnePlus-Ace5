#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_firmware.h"

static void word(uint8_t *p, uint32_t v)
{
    unsigned i;
    for (i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8*i));
}

static void header(uint8_t *p, uint32_t a, uint32_t s, uint32_t t, uint32_t v)
{
    word(p, a); word(p+4, s); word(p+8, t); word(p+12, v);
}

static void rejected(const uint8_t *data, size_t bytes, size_t capacity,
                     enum gg_fw_status expected)
{
    struct gg_fw_block out[64], before[64];
    size_t count = 999;
    memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out));
    assert(gg_fw_plan(data, bytes, out, capacity, &count) == expected);
    assert(count == 0 && !memcmp(out, before, sizeof(out)));
}

int main(int argc, char **argv)
{
    uint8_t data[80] = {0};
    struct gg_fw_block out[64];
    size_t count, offset, size;
    header(data, 0x60000000, 0, 7, 4096);
    header(data+16, 0, 4, 0xefbeadde, 0xefbeadde);
    assert(gg_fw_plan(data, 36, out, 64, &count) == GG_FW_OK && count == 2);
    assert(out[0].region == GG_KERNEL && out[0].type == GG_PREALLOC_PERSIST);
    assert(out[1].region == GG_ITCM && out[1].payload_offset == 32);
    rejected(data, 15, 64, GG_FW_TRUNCATED);
    rejected(data, 35, 64, GG_FW_TRUNCATED);
    rejected(data, 37, 64, GG_FW_TRUNCATED);
    rejected(data, 36, 1, GG_FW_CAPACITY);
    rejected(data, 16, 64, GG_FW_INVALID);
    word(data, 0xfffffffc); rejected(data, 36, 64, GG_FW_RANGE);
    word(data, 0x60000001); rejected(data, 36, 64, GG_FW_RANGE);
    word(data, 0x60000000); word(data+8, 8);
    rejected(data, 36, 64, GG_FW_INVALID);
    word(data+8, 7); header(data+36, 0, 0, 2, 1);
    rejected(data, 52, 64, GG_FW_ORDER);
    header(data+36, 0, 4, 0, 0);
    rejected(data, 56, 64, GG_FW_OVERLAP);
    header(data+36, 0x3ffc, 8, 0, 0);
    rejected(data, 60, 64, GG_FW_RANGE);
    assert(gg_fw_region(0x10007ffc, 4) == GG_DTCM);
    assert(gg_fw_region(0x10008000, 4) == GG_NONE);
    assert(gg_fw_region(0xdffffffc, 4) == GG_KERNEL_EXT);
    assert(gg_fw_region(0xdffffffc, 8) == GG_NONE);
    assert(gg_ucode_payload(data, 36, &offset, &size) == GG_FW_OK);
    assert(offset == 4 && size == 32);
    assert(gg_ucode_payload(data, 4, &offset, &size) == GG_FW_INVALID);
    assert(offset == 0 && size == 0);
    assert(gg_ucode_payload(data, 35, &offset, &size) == GG_FW_INVALID);
    if (argc == 2) {
        FILE *file = fopen(argv[1], "rb");
        long length;
        uint8_t *blob;
        assert(file && !fseek(file, 0, SEEK_END));
        length = ftell(file); assert(length > 0 && length < 1024*1024);
        rewind(file); blob = malloc((size_t)length); assert(blob);
        assert(fread(blob, 1, (size_t)length, file) == (size_t)length);
        fclose(file);
        assert(gg_fw_plan(blob, (size_t)length, out, 64, &count) == GG_FW_OK);
        assert(count == 15); /* Exact pinned gen70900 firmware: 11 metadata + 4 data. */
        printf("Stock GMU: %zu blocks, persistent buffers at %#x and %#x\n",
               count, out[9].address, out[10].address);
        free(blob);
    }
    puts("GPU firmware bounds, metadata, ordering and output atomicity: passed");
    return 0;
}
