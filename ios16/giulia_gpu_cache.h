/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_CACHE_H
#define GIULIA_GPU_CACHE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* CPU publication only; caller owns wired Normal RAM, with DMA quiescent.
 * Does not grant an SMMU lease, prove device coherence, or permit CPU writes
 * after handoff. Re-publish after every CPU modification before device use. */
int giulia_gpu_cache_publish(const void *cpu,size_t bytes);
#ifdef __cplusplus
}
#endif
#endif
