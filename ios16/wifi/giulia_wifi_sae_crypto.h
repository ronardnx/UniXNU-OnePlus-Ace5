/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * SAE group 19 over the pinned BearSSL constant-time P-256/i31 provider.
 * All scalar/coordinate encodings are 32-byte big endian; points are SEC1
 * uncompressed (04 || x || y). No allocator or g_crypto_funcs dependency. */
#ifndef GIULIA_WIFI_SAE_CRYPTO_H
#define GIULIA_WIFI_SAE_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int gsa_crypto_hmac(void *,const uint8_t *,size_t,const uint8_t *,size_t,uint8_t [32]);
int gsa_crypto_kdf(const uint8_t [32],const char *,const uint8_t *,size_t,uint8_t *,size_t);
int gsa_crypto_pwe(const uint8_t [6],const uint8_t [6],const uint8_t *,size_t,
                   const uint8_t *,size_t,unsigned,uint8_t [65]);
int gsa_crypto_scalar(const uint8_t [32]);
int gsa_crypto_add(const uint8_t [32],const uint8_t [32],uint8_t [32]);
int gsa_crypto_commit(const uint8_t [65],const uint8_t [32],const uint8_t [32],uint8_t [32],uint8_t [65]);
int gsa_crypto_shared(const uint8_t [65],const uint8_t [32],const uint8_t [32],const uint8_t [65],uint8_t [32]);
#ifdef __cplusplus
}
#endif
#endif
