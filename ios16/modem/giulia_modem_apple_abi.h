/* Generated from exact 20H392 kernel dispatch tables. Shape validation
 * does not execute an operation, authorize access, or imply radio success.
 * Source kernel SHA256: 2ec297bac95bcba01046fd0148e97e7e5885c05a6c18827f39f534ba375c1221 */
#ifndef GIULIA_MODEM_APPLE_ABI_H
#define GIULIA_MODEM_APPLE_ABI_H
#include <stdint.h>

#define GIULIA_MODEM_APPLE_ABI_VERSION 1u
#define GIULIA_MODEM_APPLE_ABI_VARIABLE 0xffffffffu
enum giulia_modem_apple_client {
    GIULIA_MODEM_APPLE_BASEBAND = 0,
    GIULIA_MODEM_APPLE_ABP = 1,
    GIULIA_MODEM_APPLE_ABP_CONTROL = 2
};
enum giulia_modem_apple_shape_result {
    GIULIA_MODEM_APPLE_SHAPE_VALID = 0,
    GIULIA_MODEM_APPLE_SHAPE_UNKNOWN_CLIENT = -1,
    GIULIA_MODEM_APPLE_SHAPE_UNKNOWN_SELECTOR = -2,
    GIULIA_MODEM_APPLE_SHAPE_BAD_COUNTS = -3
};
struct giulia_modem_apple_dispatch_shape {
    uint32_t scalar_input_count, struct_input_bytes;
    uint32_t scalar_output_count, struct_output_bytes;
};

/* AppleBasebandUserClient; table 0xfffffff006fe0480; SHA256 179ba47e7f498c0311684c7e7f592b8f6eb45577eaeaad3f1f5537fbcc70a00d */
static const struct giulia_modem_apple_dispatch_shape
giulia_modem_apple_baseband_shapes[] = {
    {1u, 0u, 0u, 0u}, /* selector 0 */
    {1u, 0u, 0u, 0u}, /* selector 1 */
    {1u, 0u, 0u, 0u}, /* selector 2 */
    {0u, 0u, 1u, 0u}, /* selector 3 */
    {0u, 0u, 0u, 0u}, /* selector 4 */
    {0u, 0u, 0u, 0u}, /* selector 5 */
    {0u, 0u, 0u, 0u}, /* selector 6 */
    {0u, 0u, 0u, 0u}, /* selector 7 */
    {1u, 0u, 0u, 0u}, /* selector 8 */
    {0u, 0u, 0u, 0u}, /* selector 9 */
    {1u, 0u, 0u, 0u}, /* selector 10 */
    {0u, 0u, 1u, 0u}, /* selector 11 */
    {0u, 0u, 0u, 0u}, /* selector 12 */
    {1u, 4294967295u, 0u, 0u}, /* selector 13 */
    {1u, 4294967295u, 0u, 0u}, /* selector 14 */
    {0u, 64u, 0u, 0u}, /* selector 15 */
    {0u, 0u, 1u, 0u}, /* selector 16 */
    {0u, 0u, 1u, 0u}, /* selector 17 */
    {0u, 0u, 1u, 0u}, /* selector 18 */
    {1u, 0u, 0u, 0u}, /* selector 19 */
    {0u, 0u, 2u, 0u}, /* selector 20 */
    {1u, 0u, 0u, 0u}, /* selector 21 */
    {1u, 0u, 0u, 0u}, /* selector 22 */
    {0u, 0u, 1u, 0u}, /* selector 23 */
    {1u, 0u, 0u, 0u}, /* selector 24 */
    {1u, 0u, 0u, 0u}, /* selector 25 */
    {0u, 0u, 1u, 0u}, /* selector 26 */
    {1u, 0u, 0u, 0u}, /* selector 27 */
    {1u, 0u, 1u, 0u}, /* selector 28 */
    {2u, 0u, 0u, 0u}, /* selector 29 */
    {0u, 120u, 1u, 0u}, /* selector 30 */
    {0u, 48u, 0u, 0u}, /* selector 31 */
    {1u, 0u, 0u, 0u}, /* selector 32 */
    {2u, 4294967295u, 0u, 0u}, /* selector 33 */
    {2u, 4294967295u, 0u, 0u}, /* selector 34 */
    {1u, 0u, 0u, 0u}, /* selector 35 */
    {3u, 64u, 0u, 0u}, /* selector 36 */
    {3u, 0u, 0u, 64u}, /* selector 37 */
    {1u, 63u, 1u, 0u}, /* selector 38 */
    {0u, 0u, 1u, 32u}, /* selector 39 */
    {1u, 32u, 1u, 0u}, /* selector 40 */
    {0u, 0u, 0u, 160u}, /* selector 41 */
    {0u, 0u, 1u, 0u}, /* selector 42 */
    {0u, 0u, 0u, 0u}, /* selector 43 */
};

/* AppleBasebandPCIUserClient; table 0xfffffff006ed2040; SHA256 4fa041e556acce432eb12d42b0f030aa1c429ec6b70ec3dc582068e7fad8046a */
static const struct giulia_modem_apple_dispatch_shape
giulia_modem_apple_abp_shapes[] = {
    {1u, 0u, 0u, 0u}, /* selector 0 */
    {0u, 0u, 0u, 0u}, /* selector 1 */
    {2u, 4294967295u, 0u, 0u}, /* selector 2 */
    {2u, 0u, 0u, 4294967295u}, /* selector 3 */
    {2u, 4294967295u, 2u, 0u}, /* selector 4 */
    {1u, 0u, 0u, 4294967295u}, /* selector 5 */
    {1u, 0u, 0u, 0u}, /* selector 6 */
    {1u, 0u, 0u, 0u}, /* selector 7 */
    {3u, 0u, 0u, 0u}, /* selector 8 */
    {3u, 0u, 0u, 0u}, /* selector 9 */
    {1u, 0u, 0u, 0u}, /* selector 10 */
    {1u, 0u, 0u, 0u}, /* selector 11 */
    {1u, 0u, 0u, 0u}, /* selector 12 */
};

/* AppleBasebandPCIControlUserClient; table 0xfffffff006ed0518; SHA256 e3be53c44a84f3541bd8cfacfd84ff5a06830e7e01ba6386cab82cf85060e2e4 */
static const struct giulia_modem_apple_dispatch_shape
giulia_modem_apple_abp_control_shapes[] = {
    {1u, 0u, 0u, 0u}, /* selector 0 */
    {0u, 0u, 0u, 0u}, /* selector 1 */
    {3u, 0u, 0u, 4294967295u}, /* selector 2 */
    {4u, 0u, 0u, 0u}, /* selector 3 */
};

static inline const struct giulia_modem_apple_dispatch_shape *
giulia_modem_apple_abi_lookup(uint32_t client, uint32_t selector)
{
    switch (client) {
    case 0u: return selector < 44u ?
        &giulia_modem_apple_baseband_shapes[selector] : 0;
    case 1u: return selector < 13u ?
        &giulia_modem_apple_abp_shapes[selector] : 0;
    case 2u: return selector < 4u ?
        &giulia_modem_apple_abp_control_shapes[selector] : 0;
    default: return 0;
    }
}

static inline int giulia_modem_apple_abi_validate(uint32_t client, uint32_t selector,
    uint32_t scalar_in, uint64_t struct_in, uint32_t scalar_out, uint64_t struct_out)
{
    const struct giulia_modem_apple_dispatch_shape *s;
    if (client > GIULIA_MODEM_APPLE_ABP_CONTROL)
        return GIULIA_MODEM_APPLE_SHAPE_UNKNOWN_CLIENT;
    s = giulia_modem_apple_abi_lookup(client, selector);
    if (!s) return GIULIA_MODEM_APPLE_SHAPE_UNKNOWN_SELECTOR;
    if (scalar_in != s->scalar_input_count || scalar_out != s->scalar_output_count ||
        (s->struct_input_bytes != GIULIA_MODEM_APPLE_ABI_VARIABLE &&
         struct_in != s->struct_input_bytes) ||
        (s->struct_output_bytes != GIULIA_MODEM_APPLE_ABI_VARIABLE &&
         struct_out != s->struct_output_bytes))
        return GIULIA_MODEM_APPLE_SHAPE_BAD_COUNTS;
    return GIULIA_MODEM_APPLE_SHAPE_VALID;
}
#endif
