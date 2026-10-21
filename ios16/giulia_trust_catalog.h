/* Read-only parser for DarwinBoot's bundle of Apple v2 trust caches.
 * No dynamic loading, signature bypass, or wildcard match is implemented.
 * The caller verifies the source firmware and bounds the backing memory.
 */
#ifndef GIULIA_TRUST_CATALOG_H
#define GIULIA_TRUST_CATALOG_H
#include <stdint.h>
#include <stddef.h>

#define GTC_MAX_BYTES (16u * 1024u * 1024u)
struct gtc_view { const uint8_t *data; size_t size; uint32_t modules, entries; };
static inline uint32_t gtc_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int gtc_compare(const uint8_t *a, const uint8_t *b) {
    for (unsigned i = 0; i < 20; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}
static inline int gtc_validate(const void *buffer, size_t size, struct gtc_view *view) {
    if (!view) return 0;
    view->data = NULL; view->size = 0; view->modules = view->entries = 0;
    if (!buffer || size < 8 || size > GTC_MAX_BYTES) return 0;
    const uint8_t *data = (const uint8_t *)buffer;
    uint32_t modules = gtc_u32(data), total = 0;
    if (!modules || modules > 128 || modules > (size - 4) / 4) return 0;
    for (uint32_t i = 0; i < modules; i++) {
        size_t start = gtc_u32(data + 4 + 4*i);
        size_t end = i + 1 < modules ? gtc_u32(data + 8 + 4*i) : size;
        if (start < 4 + 4*modules || (start & 3) || end > size ||
            end < start || end - start < 24 || gtc_u32(data + start) != 2) return 0;
        uint32_t count = gtc_u32(data + start + 20);
        if (count > (end - start - 24) / 24 || end - start != 24 + (size_t)count*24) return 0;
        for (uint32_t j = 0; j < count; j++) {
            const uint8_t *entry = data + start + 24 + (size_t)j*24;
            if (entry[20] < 1 || entry[20] > 4 ||
                (j && gtc_compare(entry - 24, entry) >= 0)) return 0;
        }
        total += count;
    }
    view->data = data; view->size = size; view->modules = modules; view->entries = total;
    return 1;
}
static inline int gtc_contains(const struct gtc_view *view, const void *hash, size_t length) {
    if (!view || !view->data || !hash || length != 20) return 0;
    for (uint32_t i = 0; i < view->modules; i++) {
        size_t start = gtc_u32(view->data + 4 + 4*i);
        const uint8_t *entries = view->data + start + 24;
        uint32_t low = 0, high = gtc_u32(view->data + start + 20);
        while (low < high) {
            uint32_t middle = low + (high - low)/2;
            int order = gtc_compare((const uint8_t *)hash, entries + (size_t)middle*24);
            if (!order) return 1;
            if (order < 0) high = middle;
            else low = middle + 1;
        }
    }
    return 0;
}
#endif
