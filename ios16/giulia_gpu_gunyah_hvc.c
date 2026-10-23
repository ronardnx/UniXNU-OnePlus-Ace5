/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if defined(GG_GUNYAH_HVC_STANDALONE)
#include <stdint.h>
#include <stddef.h>
#else
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#endif
#if defined(GG_GUNYAH_HVC_STANDALONE) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_gunyah_hvc.h"

#if defined(GG_GUNYAH_HVC_TEST)
#if !defined(GG_GUNYAH_HVC_STANDALONE)
#error Gunyah HVC test hook is standalone only
#endif
extern void ggh_hvc_test_invoke(struct ggh_hvc_regs *);
static void ggh_hvc_native(struct ggh_hvc_regs *r) { ggh_hvc_test_invoke(r); }
#else
#if !defined(__aarch64__)
#error Gunyah HVC requires AArch64
#endif
/* Private primitive: all callers below construct one of two fixed functions.
 * Canonical RM uses hvc #0 with the SMCCC function in x0. It does not use the
 * legacy hvc #0x601b/#0x601c ABI. Preserve Darwin's reserved x18 explicitly;
 * x19 holds the save-slot address and is preserved across the hypercall.
 * A separate stack slot avoids depending on a caller-clobbered register. */
static __attribute__((noinline)) void ggh_hvc_native(struct ggh_hvc_regs *r)
{
    uint64_t saved_x18;
    register uint64_t x0 __asm__("x0")=r->x[0];
    register uint64_t x1 __asm__("x1")=r->x[1];
    register uint64_t x2 __asm__("x2")=r->x[2];
    register uint64_t x3 __asm__("x3")=r->x[3];
    register uint64_t x4 __asm__("x4")=r->x[4];
    register uint64_t x5 __asm__("x5")=r->x[5];
    register uint64_t x6 __asm__("x6")=r->x[6];
    register uint64_t x7 __asm__("x7")=r->x[7];
    register uint64_t *slot __asm__("x19")=&saved_x18;
    __asm__ volatile("str x18, [%[slot]]\n"
        "hvc #0\n"
        "ldr x18, [%[slot]]\n"
        "str xzr, [%[slot]]\n"
        : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),
          "+r"(x4),"+r"(x5),"+r"(x6),"+r"(x7)
        : [slot] "r"(slot)
        : "x8","x9","x10","x11","x12","x13","x14","x15",
          "x16","x17","cc","memory");
    r->x[0]=x0;r->x[1]=x1;r->x[2]=x2;r->x[3]=x3;
    r->x[4]=x4;r->x[5]=x5;r->x[6]=x6;r->x[7]=x7;
}
#endif

struct ggh_hvc_lease {
    uint64_t epoch,tx_cap,rx_cap;
    void *cookie;
    int (*live)(void *,uint64_t,uint64_t,uint64_t);
};

static int ggh_hvc_range(const void *data,size_t size)
{
    return data && size && size<=GGH_HVC_MAX_BYTES &&
        size-1<=(uintptr_t)-1-(uintptr_t)data;
}

static int ggh_hvc_same(const struct ggh_hvc *h,const struct ggh_hvc_lease *s)
{
    return h->epoch==s->epoch && h->tx_cap==s->tx_cap && h->rx_cap==s->rx_cap &&
        h->lease_cookie==s->cookie && h->live==s->live;
}

static int ggh_hvc_live(const struct ggh_hvc *h,const struct ggh_hvc_lease *s)
{
    if(!ggh_hvc_same(h,s))return 0;
    return s->live(s->cookie,s->epoch,s->tx_cap,s->rx_cap)==1 &&
        ggh_hvc_same(h,s);
}

static int ggh_hvc_begin(struct ggh_hvc *h,struct ggh_hvc_lease *s)
{
    if(!h)return 0;
    h->attempted=0;
    s->epoch=h->epoch;s->tx_cap=h->tx_cap;s->rx_cap=h->rx_cap;
    s->cookie=h->lease_cookie;s->live=h->live;
    return s->epoch && s->tx_cap && s->rx_cap &&
        s->tx_cap!=(uint64_t)-1 && s->rx_cap!=(uint64_t)-1 &&
        s->live && ggh_hvc_live(h,s);
}

static int ggh_hvc_finish(struct ggh_hvc *h,const struct ggh_hvc_lease *s,
    struct ggh_hvc_regs *r)
{
    ggh_hvc_native(r);
    h->last=*r;h->native_code=r->x[0];h->attempted=1;
    return ggh_hvc_live(h,s);
}

int ggh_hvc_send(void *cookie,const void *data,size_t size,unsigned flags)
{
    struct ggh_hvc *h=(struct ggh_hvc *)cookie;
    struct ggh_hvc_lease s;
    struct ggh_hvc_regs r={{0}};
    uint8_t scratch[GGH_HVC_MAX_BYTES];
    size_t i;
    if(h)h->attempted=0;
    if(!ggh_hvc_range(data,size) || (flags&~GGH_HVC_PUSH) ||
        !ggh_hvc_begin(h,&s))return GGH_HVC_ERROR;
    for(i=0;i<size;++i)scratch[i]=((const uint8_t *)data)[i];
    r.x[0]=0xc600801bULL;r.x[1]=s.tx_cap;r.x[2]=size;
    r.x[3]=(uintptr_t)scratch;r.x[4]=flags;
    if(!ggh_hvc_finish(h,&s,&r))return GGH_HVC_ERROR;
    if(r.x[0]==61)return GGH_HVC_AGAIN;
    if(r.x[0] || r.x[1]>1)return GGH_HVC_ERROR;
    return GGH_HVC_OK;
}

int ggh_hvc_recv(void *cookie,void *data,size_t capacity,size_t *received)
{
    struct ggh_hvc *h=(struct ggh_hvc *)cookie;
    struct ggh_hvc_lease s;
    struct ggh_hvc_regs r={{0}};
    uint8_t scratch[GGH_HVC_MAX_BYTES];
    size_t i;
    if(h)h->attempted=0;
    if(!received || !ggh_hvc_range(data,capacity) ||
        !ggh_hvc_range(received,sizeof(*received)) ||
        !ggh_hvc_begin(h,&s))return GGH_HVC_ERROR;
    r.x[0]=0xc600801cULL;r.x[1]=s.rx_cap;
    r.x[2]=(uintptr_t)scratch;r.x[3]=capacity;
    if(!ggh_hvc_finish(h,&s,&r))return GGH_HVC_ERROR;
    if(r.x[0]==60)return GGH_HVC_AGAIN;
    if(r.x[0] || !r.x[1] || r.x[1]>capacity || r.x[2]>1)return GGH_HVC_ERROR;
    for(i=0;i<(size_t)r.x[1];++i)((uint8_t *)data)[i]=scratch[i];
    *received=(size_t)r.x[1];
    return GGH_HVC_OK;
}
#endif
