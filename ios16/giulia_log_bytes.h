/* Byte-preserving short records, including embedded NUL and split UTF-8. */
#ifndef GIULIA_LOG_BYTES_H
#define GIULIA_LOG_BYTES_H
#include <stddef.h>
static inline int glb_hex(const unsigned char *data, size_t length, char *out, size_t capacity) {
    static const char digits[]="0123456789abcdef";
    if (!out || length > 64 || capacity < length*2+1 || (!data && length)) return 0;
    for (size_t i=0;i<length;i++) { out[2*i]=digits[data[i]>>4]; out[2*i+1]=digits[data[i]&15]; }
    out[2*length]=0;
    return 1;
}
#endif
