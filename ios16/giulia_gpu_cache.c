/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if defined(GG_CACHE_STANDALONE)
#include <stdint.h>
#include <stddef.h>
#else
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#endif
#if defined(GG_CACHE_STANDALONE) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_cache.h"
#if !defined(__aarch64__)
#error GPU cache publication requires AArch64
#endif
int giulia_gpu_cache_publish(const void *cpu,size_t bytes)
{
    uintptr_t start=(uintptr_t)cpu,last,at;
    uint64_t ctr;
    size_t line;
    if(!cpu || !bytes || bytes-1>(uintptr_t)-1-start)return 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line=(size_t)4<<((ctr>>16)&15);
    /* Wired allocations are page-aligned. Reject an implausible line size
     * before touching memory rather than assuming an Apple cache geometry. */
    if(line>4096)return 0;
    last=(start+bytes-1)&~(uintptr_t)(line-1);
    at=start&~(uintptr_t)(line-1);
    for(;;) {
        __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
        if(at==last)break;
        at+=line;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return 1;
}
#endif
