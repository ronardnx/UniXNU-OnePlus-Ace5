/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_PACK_H
#define GIULIA_GPU_PACK_H
#include "giulia_gpu_firmware.h"
#include "giulia_wifi_sha256.h" /* Existing freestanding SHA-256, no Wi-Fi runtime dependency. */
#include "giulia_gpu_firmware_pin.h"
enum { GG_ASSET_GMU, GG_ASSET_SQE, GG_ASSET_AQE, GG_ASSET_ZAP, GG_ASSETS };
struct gg_asset { const uint8_t *data; size_t size; };
struct gg_pack { struct gg_asset asset[GG_ASSETS]; };

static inline int gg_pack_open(const void *input, size_t bytes, struct gg_pack *out)
{
    const uint8_t *p = (const uint8_t *)input;
    const uint8_t magic[8] = {'G','G','F','W','P','K','1',0};
    uint8_t hash[32];
    size_t i, j, next = 4096;
    struct gg_pack result = {{{0,0}}};
    if (!out) return 0;
    *out = result;
    if (!p || bytes != GG_FIRMWARE_PACK_BYTES || bytes < 4096) return 0;
    gwh_sha256(p, bytes, hash);
    if (!gwh_equal(hash, gg_firmware_pack_sha256)) return 0;
    for (i = 0; i < 8; ++i) if (p[i] != magic[i]) return 0;
    if (gg_le32(p+8) != 0x43051401 || gg_le32(p+12) != GG_ASSETS ||
        gg_le32(p+16) != bytes || gg_le32(p+20) != 1) return 0;
    for (i = 24; i < 64; ++i) if (p[i]) return 0;
    for (i = 0; i < GG_ASSETS; ++i) {
        const uint8_t *e = p + 64 + i*64;
        size_t offset = gg_le32(e+4), size = gg_le32(e+8), end;
        if (gg_le32(e) != i || offset != next || !size ||
            offset > bytes || size > bytes-offset || gg_le32(e+12)) return 0;
        for (j = 48; j < 64; ++j) if (e[j]) return 0;
        gwh_sha256(p+offset, size, hash);
        if (!gwh_equal(hash, e+16)) return 0;
        result.asset[i].data = p+offset; result.asset[i].size = size;
        end = offset+size; next = (end+4095) & ~(size_t)4095;
        if (next > bytes) return 0;
        for (j = end; j < next; ++j) if (p[j]) return 0;
    }
    if (next != bytes) return 0;
    for (j = 64+GG_ASSETS*64; j < 4096; ++j) if (p[j]) return 0;
    *out = result;
    return 1;
}
#endif
