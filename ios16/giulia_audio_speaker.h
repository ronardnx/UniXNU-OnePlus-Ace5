#ifndef GIULIA_AUDIO_SPEAKER_H
#define GIULIA_AUDIO_SPEAKER_H
/* SIA9177 register-program engine. No allocation, OS calls or MMIO.
 * OnePlusOSS sm8650 453d8875f815bf9dfad9035982861226a4823a6c:
 * sipa_parameter.h; sipa_regmap.[ch]; sipa_91xx.[ch]. Register values are
 * read from the separately authenticated, immutable stock sipa.bin.
 * Caller serializes calls, owns blob lifetime, and supplies real readiness.
 * Amp sequence success is NOT evidence of PCM playback or DSP readiness.
 */
#include <stdint.h>
#include <stddef.h>

#define GSIPA_OK 0
#define GSIPA_EINVAL -1
#define GSIPA_EIO -2
#define GSIPA_EID -3
#define GSIPA_ECHECK -4
#define GSIPA_ETRIM -5
#define GSIPA_ENOTREADY -6
#define GSIPA_EAUTH -7
#define GSIPA_DSP_READY 1u
#define GSIPA_MI2S_RUNNING 2u
#define GSIPA_CALIBRATED_GRAPH 4u
#define GSIPA_REQUIRED 7u
#define GSIPA_MAX_BLOB (1024u * 1024u)

struct gsipa_ops {
    void *opaque;
    /* Must compute SHA256 of the exact immutable blob and compare all 32
     * bytes against expected. Return zero only on a match. Never a flag stub. */
    int (*verify_sha256)(void *, const uint8_t *, size_t, const uint8_t expected[32]);
    /* channel 0/1 => I2C4 0x6c/0x6d, GPIO 173/178. Host-endian values;
     * the I2C adapter must transfer address u8 and data u16, MSB first. */
    int (*read)(void *, unsigned, uint8_t, uint16_t *);
    int (*write)(void *, unsigned, uint8_t, uint16_t);
    int (*reset)(void *, unsigned, unsigned); /* asserted=1 drives GPIO high */
    int (*delay_us)(void *, uint32_t);
};
struct gsipa {
    const uint8_t *data;
    uint32_t size;
    const uint8_t *chip[2];
    struct gsipa_ops ops;
    unsigned valid, probed, started;
    int cleanup_error;
};

static inline uint32_t gsipa_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
        (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint32_t gsipa_crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    unsigned b;
    while (n--) {
        crc ^= *p++;
        for (b = 0; b < 8; b++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}
static inline uint8_t gsipa_crc8(const uint8_t *p, unsigned n)
{
    uint8_t crc = 0;
    unsigned b;
    while (n--) {
        crc ^= *p++;
        for (b = 0; b < 8; b++) crc = (uint8_t)((crc >> 1) ^ ((crc & 1) ? 0x8c : 0));
    }
    return crc;
}
static inline unsigned gsipa_popcount(uint32_t n)
{
    unsigned bits = 0;
    while (n) { bits += n & 1u; n >>= 1; }
    return bits;
}
static inline int gsipa_list(const struct gsipa *s, const uint8_t *d, uint32_t stride)
{
    uint32_t off = gsipa_u32(d), n = gsipa_u32(d + 4), sz = gsipa_u32(d + 8);
    if (off > s->size || n > 4096 || (n && sz != stride) ||
        (!n && sz && sz != stride) || n > (s->size - off) / stride)
        return GSIPA_EINVAL;
    return GSIPA_OK;
}
static inline int gsipa_validate_regs(const struct gsipa *s, const uint8_t *d,
    unsigned proc, unsigned *bits)
{
    uint32_t i, j, n, stride = proc ? 44u : 32u;
    const uint8_t *p;
    if (gsipa_list(s, d, stride)) return GSIPA_EINVAL;
    n = gsipa_u32(d + 4);
    p = s->data + gsipa_u32(d);
    for (i = 0; i < n; i++, p += stride) {
        if (gsipa_u32(p) >= 255) return GSIPA_EINVAL;
        if (proc) {
            if (gsipa_u32(p + 4) > 65535 || gsipa_u32(p + 8) > 3 ||
                gsipa_u32(p + 16) > 1000000) return GSIPA_EINVAL;
            if (bits) *bits += gsipa_popcount(gsipa_u32(p + 4));
        }
        for (j = proc ? 5u : 2u; j < stride / 4; j++)
            if (gsipa_u32(p + 4*j) > 65535) return GSIPA_EINVAL;
    }
    return GSIPA_OK;
}
static inline int gsipa_validate_chip(const struct gsipa *s, const uint8_t *c)
{
    unsigned efuse_bits = 0, crc_bits = 0, i;
    const unsigned lists[] = { 19, 22, 26, 29, 32 };
    const uint8_t *ranges;
    uint32_t n;
    if (gsipa_u32(c) != 11 || gsipa_u32(c + 4) != 8 ||
        gsipa_u32(c + 8) != 16 || gsipa_u32(c + 12) >= 255 ||
        gsipa_list(s, c + 16, 8)) return GSIPA_EINVAL;
    n = gsipa_u32(c + 20);
    if (!n) return GSIPA_EINVAL;
    ranges = s->data + gsipa_u32(c + 16);
    for (i = 0; i < n; i++, ranges += 8)
        if (gsipa_u32(ranges) > gsipa_u32(ranges + 4) ||
            gsipa_u32(ranges + 4) > 65535) return GSIPA_EINVAL;
    if (gsipa_validate_regs(s, c + 64, 0, NULL) || !gsipa_u32(c + 68))
        return GSIPA_EINVAL;
    for (i = 0; i < sizeof(lists)/sizeof(lists[0]); i++)
        if (gsipa_validate_regs(s, c + 4*lists[i], 1,
            lists[i] == 26 ? &efuse_bits : (lists[i] == 29 ? &crc_bits : NULL)))
            return GSIPA_EINVAL;
    /* SIA9177 SRAM programming is a no-op in the BSP. Refuse other data. */
    if (gsipa_list(s, c + 172, 32) || gsipa_u32(c + 176) ||
        !gsipa_u32(c + 80) || !gsipa_u32(c + 92) ||
        gsipa_u32(c + 100) != 8 || efuse_bits < 48 || efuse_bits > 256 ||
        crc_bits != 8) return GSIPA_EINVAL;
    return GSIPA_OK;
}

/* CRC guards corruption; the callback verifies the pinned stock SHA256.
 * Bounds checking also covers the 4/8 channel layout before any I2C is allowed. */
static inline int gsipa_init(struct gsipa *s, const uint8_t *blob, size_t bytes,
    const struct gsipa_ops *ops)
{
    unsigned count = 0, candidate, ch, found, i;
    struct gsipa t;
    for (size_t byte = 0; byte < sizeof(t); byte++) ((uint8_t *)&t)[byte] = 0;
    static const uint8_t expected_sha256[32] = {
        0xc8,0x88,0x59,0x8d,0x7c,0x8e,0xeb,0x4e,
        0xd3,0xb4,0x6f,0xd5,0xe6,0x12,0xf3,0x9c,
        0xff,0x48,0x38,0xa6,0xb6,0xc6,0x98,0x08,
        0x60,0x52,0x04,0xd4,0xc4,0x97,0xa0,0xdc
    };
    if (!s) return GSIPA_EINVAL;
    *s = t;
    if (!blob || !ops || !ops->verify_sha256 || !ops->read || !ops->write || !ops->reset ||
        !ops->delay_us || bytes < 188 || bytes > GSIPA_MAX_BLOB ||
        gsipa_u32(blob + 4) != 0x10005 ||
        gsipa_crc32(blob + 4, bytes - 4) != gsipa_u32(blob)) return GSIPA_EINVAL;
    if (ops->verify_sha256(ops->opaque, blob, bytes, expected_sha256)) return GSIPA_EAUTH;
    for (candidate = 4; candidate <= 8; candidate += 4) {
        unsigned header = 12 + 44*candidate;
        if (bytes >= header && gsipa_u32(blob + header - 4) == bytes - header) {
            if (count) return GSIPA_EINVAL;
            count = candidate;
        }
    }
    if (!count) return GSIPA_EINVAL;
    t.data = blob + 12 + 44*count;
    t.size = (uint32_t)(bytes - 12 - 44*count);
    t.ops = *ops;
    for (ch = 0; ch < count; ch++) {
        const uint8_t *d = blob + 8 + 4*count + 12*ch;
        const uint8_t *c;
        uint32_t enabled = gsipa_u32(blob + 8 + 4*ch);
        if (enabled != (ch < 2 ? 1u : 0u) || gsipa_list(&t, d, 192))
            return GSIPA_EINVAL;
        if (ch >= 2) {
            if (gsipa_u32(d + 4)) return GSIPA_EINVAL;
            continue;
        }
        found = 0;
        c = t.data + gsipa_u32(d);
        for (i = 0; i < gsipa_u32(d + 4); i++, c += 192)
            if (gsipa_u32(c) == 11) {
                if (found++ || gsipa_validate_chip(&t, c)) return GSIPA_EINVAL;
                t.chip[ch] = c;
            }
        if (found != 1) return GSIPA_EINVAL;
    }
    t.valid = 1;
    *s = t;
    return GSIPA_OK;
}
static inline int gsipa_delay(struct gsipa *s, uint32_t us)
{
    return us && s->ops.delay_us(s->ops.opaque, us) ? GSIPA_EIO : GSIPA_OK;
}
static inline int gsipa_id(struct gsipa *s, unsigned ch)
{
    uint16_t id;
    if (s->ops.read(s->ops.opaque, ch, 6, &id)) return GSIPA_EIO;
    return (id == 0x5880 || (id >= 0x5890 && id <= 0x5891) ||
        (id >= 0x58a0 && id <= 0x58a1)) ? GSIPA_OK : GSIPA_EID;
}
static inline int gsipa_reg(struct gsipa *s, unsigned ch, const uint8_t *p,
    uint16_t *result)
{
    uint8_t addr = (uint8_t)gsipa_u32(p);
    uint16_t mask = (uint16_t)gsipa_u32(p + 4), val = (uint16_t)gsipa_u32(p + 20);
    uint16_t actual = 0;
    uint32_t action = gsipa_u32(p + 8);
    if (action == 0 || action == 2 || (action == 1 && mask != 65535))
        if (s->ops.read(s->ops.opaque, ch, addr, &actual)) return GSIPA_EIO;
    switch (action) {
    case 0: val = (uint16_t)((val & ~mask) | (actual & mask)); break;
    case 1:
        if (mask != 65535) val = (uint16_t)((actual & ~mask) | (val & mask));
        if (s->ops.write(s->ops.opaque, ch, addr, val)) return GSIPA_EIO;
        break;
    case 2: if ((actual & mask) != (val & mask)) return GSIPA_ECHECK; break;
    case 3: break;
    default: return GSIPA_EINVAL;
    }
    if (result) *result = val;
    return gsipa_delay(s, gsipa_u32(p + 16));
}
static inline int gsipa_proc(struct gsipa *s, unsigned ch, unsigned word)
{
    const uint8_t *d = s->chip[ch] + word*4;
    const uint8_t *p = s->data + gsipa_u32(d);
    uint32_t i;
    int ret;
    for (i = 0; i < gsipa_u32(d + 4); i++, p += 44)
        if ((ret = gsipa_reg(s, ch, p, NULL))) return ret;
    return GSIPA_OK;
}
static inline int gsipa_pack(struct gsipa *s, unsigned ch, unsigned word,
    uint8_t *buf, unsigned bytes, unsigned *bits)
{
    const uint8_t *d = s->chip[ch] + word*4;
    const uint8_t *p = s->data + gsipa_u32(d);
    uint32_t i, mask;
    int bit, ret;
    uint16_t val;
    *bits = 0;
    for (i = 0; i < bytes; i++) buf[i] = 0;
    for (i = 0; i < gsipa_u32(d + 4); i++, p += 44) {
        if ((ret = gsipa_reg(s, ch, p, &val))) return ret;
        mask = gsipa_u32(p + 4);
        for (bit = 15; bit >= 0; bit--) if (mask & (1u << bit)) {
            if (*bits >= bytes*8) return GSIPA_EINVAL;
            buf[*bits / 8] |= (uint8_t)(((val >> bit) & 1u) << (7 - (*bits % 8)));
            ++*bits;
        }
    }
    return GSIPA_OK;
}
static inline int gsipa_trim(struct gsipa *s, unsigned ch)
{
    uint8_t trim[32], crc[1];
    unsigned bits, crc_bits, bytes;
    int ret;
    if ((ret = gsipa_delay(s, 1500)) ||
        (ret = gsipa_pack(s, ch, 26, trim, sizeof(trim), &bits)) ||
        (ret = gsipa_pack(s, ch, 29, crc, sizeof(crc), &crc_bits))) return ret;
    if (bits < 48 || crc_bits != 8) return GSIPA_EINVAL;
    bytes = (bits + 7) / 8;
    if (gsipa_crc8(trim, bytes) == crc[0]) return GSIPA_OK;
    /* SIA9177-specific second silicon trim encoding, as in the BSP. */
    trim[0] &= 0x0f; trim[4] &= 0xf0; trim[5] &= 0x1f;
    if (gsipa_crc8(trim, bytes) == crc[0]) return GSIPA_OK;
    /* BSP applies fallback with PA_STATUS IV invalid. Here playback is blocked
     * because invalid IV must not silently bypass speaker protection. */
    ret = gsipa_proc(s, ch, 32);
    return ret ? ret : GSIPA_ETRIM;
}
static inline int gsipa_stop(struct gsipa *s)
{
    unsigned ch;
    int ret = 0, r;
    if (!s || !s->valid) return GSIPA_EINVAL;
    for (ch = 0; ch < 2; ch++) {
        if (s->probed) {
            r = gsipa_proc(s, ch, 22);
            if (r && !ret) ret = r;
        }
        /* Even when I2C/clock/check fails, assert both independent reset pins. */
        r = s->ops.reset(s->ops.opaque, ch, 1);
        if (r && !ret) ret = GSIPA_EIO;
    }
    s->started = 0; s->probed = 0;
    s->cleanup_error = ret;
    return ret;
}
static inline int gsipa_probe(struct gsipa *s)
{
    unsigned ch;
    int ret = 0;
    if (!s || !s->valid || s->started) return GSIPA_EINVAL;
    s->probed = 0; s->cleanup_error = 0;
    for (ch = 0; ch < 2; ch++)
        if (s->ops.reset(s->ops.opaque, ch, 1)) { ret = GSIPA_EIO; goto fail; }
    if ((ret = gsipa_delay(s, 5000))) goto fail;
    for (ch = 0; ch < 2; ch++)
        if (s->ops.reset(s->ops.opaque, ch, 0)) { ret = GSIPA_EIO; goto fail; }
    if ((ret = gsipa_delay(s, 5000))) goto fail;
    for (ch = 0; ch < 2; ch++) if ((ret = gsipa_id(s, ch))) goto fail;
    s->probed = 1;
    return GSIPA_OK;
fail:
    (void)gsipa_stop(s);
    return ret;
}
static inline int gsipa_start(struct gsipa *s, unsigned prerequisites)
{
    unsigned ch, i, tries;
    int ret = 0;
    if (!s || !s->valid) return GSIPA_EINVAL;
    if ((prerequisites & GSIPA_REQUIRED) != GSIPA_REQUIRED || !s->probed) {
        /* A lost DSP/clock/graph prerequisite invalidates an already running
         * PA too. Do not leave the last stream amplified after SSR/underrun. */
        if (s->started) (void)gsipa_stop(s);
        return GSIPA_ENOTREADY;
    }
    if (s->started) return GSIPA_OK;
    for (ch = 0; ch < 2; ch++) {
        const uint8_t *d = s->chip[ch] + 64;
        const uint8_t *p = s->data + gsipa_u32(d);
        for (i = 0; i < gsipa_u32(d + 4); i++, p += 32)
            if (s->ops.write(s->ops.opaque, ch, (uint8_t)gsipa_u32(p),
                (uint16_t)gsipa_u32(p + 8))) { ret = GSIPA_EIO; goto fail; }
        if ((ret = gsipa_delay(s, 150)) || (ret = gsipa_id(s, ch))) goto fail;
        for (tries = 0; tries < 5; tries++) {
            ret = gsipa_proc(s, ch, 19);
            if (!ret) break;
            /* I2C errors do not warrant replay of a partially written program. */
            if (ret != GSIPA_ECHECK) goto fail;
        }
        if (ret || (ret = gsipa_trim(s, ch))) goto fail;
    }
    s->started = 1;
    return GSIPA_OK;
fail:
    (void)gsipa_stop(s);
    return ret;
}
#endif
