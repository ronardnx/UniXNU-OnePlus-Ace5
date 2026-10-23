/* Vendor-derived GMU register preparation over a RAM model; no device access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_gmu_boot.h"

struct fixture;
static struct gg_gmu_ops make_ops(struct fixture *f);

struct fixture {
    uint32_t regs[0x24000];
    unsigned calls, writes, fail_at, corrupt_reg, corrupt_once;
    struct gg_gmu_state s;
    struct ggb_config c;
};
static int fconfigure(void *p);
static int fhfi(void *p);

static int event(struct fixture *f) { return (unsigned)++f->calls == f->fail_at; }
static int gr(void *p, uint32_t reg, uint32_t *v)
{
    struct fixture *f = (struct fixture *)p;
    assert(reg < 0x24000 && v);
    if (event(f)) return 81;
    *v = f->regs[reg];
    if (f->corrupt_reg == reg && f->corrupt_once) {
        f->corrupt_once = 0;
        *v ^= 1u;
    }
    return 0;
}
static int gw(void *p, uint32_t reg, uint32_t v)
{
    struct fixture *f = (struct fixture *)p;
    int e = event(f);
    ++f->writes;
    assert(reg < 0x24000);
    if (e) return 82;
    f->regs[reg] = v;
    if (reg == GGM_RESET && v == 0) f->regs[GGM_FW_READY] = 0x100;
    if (reg == GGM_HFI_INIT && v) f->regs[GGM_HFI_STATUS] = 1;
    return 0;
}
static int gb(void *p) { return event((struct fixture *)p) ? 83 : 0; }
static int wait_us(void *p, unsigned us) { (void)p; assert(us == 10); return 0; }
static int pre(void *p) { (void)p; return 1; }
static int ram(void *p, uint32_t a, const uint8_t *d, size_t n)
{ (void)p; (void)a; (void)d; (void)n; return 1; }
static int healthy(void *p) { (void)p; return 1; }
static void trace(void *p, const struct gg_event *e) { (void)p; (void)e; }

static struct gg_gmu_ops make_ops(struct fixture *f)
{
    struct gg_gmu_ops o = { f, gr, gw, gb, wait_us, pre, ram, fconfigure, fhfi, healthy, trace };
    return o;
}
static int fconfigure(void *p)
{
    struct fixture *f = (struct fixture *)p;
    struct gg_gmu_ops o = make_ops(f);
    return ggb_configure(&o, &f->s, &f->c);
}
static int fhfi(void *p)
{
    struct fixture *f = (struct fixture *)p;
    struct gg_gmu_ops o = make_ops(f);
    return ggb_prepare_hfi(&o, &f->s, &f->c);
}
static void fresh(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->regs[GGM_RESET] = 1;
    f->c.hfi_iova = 0x60010000u;
    f->c.vrb_iova = 0x60040000u;
    f->c.log_iova = 0x60030000u;
    f->c.log_bytes = 0x10000u;
    f->c.chip_rev = 0x07090100u;
}
static void check_reg(const struct fixture *f, uint32_t reg, uint32_t value)
{
    assert(f->regs[reg] == value);
}
static void config_bounds(void)
{
    struct ggb_config c;
    c.hfi_iova = 0x60010001u; c.vrb_iova = 0x60040000u; c.log_iova = 0x60030000u;
    c.log_bytes = 0x10000u; c.chip_rev = 1;
    assert(!ggb_config_valid(&c));
    c.hfi_iova = 0x60010000u; c.log_bytes = 0x10001u;
    assert(!ggb_config_valid(&c));
    c.log_bytes = 4096u * 257u;
    assert(!ggb_config_valid(&c));
    c.log_bytes = 0x10000u; c.chip_rev = 0;
    assert(!ggb_config_valid(&c));
    c.chip_rev = 1;
    assert(ggb_config_valid(&c));
}
static void configure_sequence(void)
{
    struct fixture f;
    struct gg_gmu_ops o;
    fresh(&f);
    f.s.attempted = 1;
    o = make_ops(&f);
    assert(ggb_configure(&o, &f.s, &f.c) == GGM_OK);
    check_reg(&f, GGB_FAL_INTF, 1);
    check_reg(&f, GGB_FALNEXT_INTF, 1);
    check_reg(&f, GGB_FW_INIT_RESULT, 0);
    check_reg(&f, GGB_BOOT_CONFIG, 2);
    check_reg(&f, GGB_QTBL_ADDR, 0x60010000u);
    check_reg(&f, GGB_QTBL_INFO, 1);
    check_reg(&f, GGB_AHB_FENCE_RANGE, GGB_AHB_FENCE_VALUE);
    check_reg(&f, GGB_CM3_CFG, 0x4052u);
    check_reg(&f, GGB_GENERAL_11, 0x60040000u);
    check_reg(&f, GGB_GENERAL_10, 0x07090100u);
    check_reg(&f, GGB_GENERAL_8, 0x6003000fu);
    check_reg(&f, GGB_GENERAL_9, 0);
    assert(f.writes == 12);
    /* A configure call without an attempted load is refused before any write. */
    fresh(&f);
    o = make_ops(&f);
    assert(ggb_configure(&o, &f.s, &f.c) == GGM_BUSY && !f.writes);
    /* A quarantined state never writes. */
    fresh(&f);
    f.s.attempted = 1; f.s.quarantined = 1;
    o = make_ops(&f);
    assert(ggb_configure(&o, &f.s, &f.c) == GGM_BUSY && !f.writes);
}
static void configure_readback(void)
{
    struct fixture f;
    struct gg_gmu_ops o;
    fresh(&f);
    f.s.attempted = 1;
    f.corrupt_reg = GGB_QTBL_ADDR;
    f.corrupt_once = 1;
    o = make_ops(&f);
    assert(ggb_configure(&o, &f.s, &f.c) == GGM_READBACK);
    assert(f.s.diag.failure.sequence);
    assert(f.s.diag.failure.error == GGM_READBACK);
    /* An I/O failure on the first write is reported, not ignored. */
    fresh(&f);
    f.s.attempted = 1;
    f.fail_at = 1;
    o = make_ops(&f);
    assert(ggb_configure(&o, &f.s, &f.c) == GGM_READBACK);
}
static void hfi_handoff(void)
{
    struct fixture f;
    struct gg_gmu_ops o;
    fresh(&f);
    f.s.attempted = 1; f.s.fw_ready = 1;
    o = make_ops(&f);
    /* The table base/info must match exactly before HFI_INIT is allowed. */
    f.regs[GGB_QTBL_ADDR] = 0x60020000u;
    f.regs[GGB_QTBL_INFO] = 1;
    assert(ggb_prepare_hfi(&o, &f.s, &f.c) == GGM_READBACK);
    f.regs[GGB_QTBL_ADDR] = 0x60010000u;
    assert(ggb_prepare_hfi(&o, &f.s, &f.c) == GGM_OK);
    f.regs[GGB_QTBL_INFO] = 2;
    assert(ggb_prepare_hfi(&o, &f.s, &f.c) == GGM_READBACK);
    f.regs[GGB_QTBL_INFO] = 1;
    f.regs[GGB_HFI_STATUS] = 1;
    assert(ggb_prepare_hfi(&o, &f.s, &f.c) == GGM_READBACK);
    f.regs[GGB_HFI_STATUS] = 0;
    f.s.fw_ready = 0;
    assert(ggb_prepare_hfi(&o, &f.s, &f.c) == GGM_BUSY);
    f.s.fw_ready = 1;
    f.fail_at = f.calls + 1;
    assert(ggb_prepare_hfi(&o, &f.s, &f.c) == GGM_READBACK);
}
static void start_flow(void)
{
    struct fixture f;
    struct gg_gmu_ops o;
    fresh(&f);
    f.s.loaded = 1;
    f.s.attempted = 1;
    f.s.retain_resources = 1;
    o = make_ops(&f);
    assert(gg_gmu_start(&o, &f.s) == GGM_OK);
    assert(f.s.fw_ready && f.s.hfi_ready && !f.s.quarantined);
    assert(f.regs[GGM_HFI_STATUS] == 1);
    check_reg(&f, GGM_RESET, 0);
    check_reg(&f, GGB_QTBL_ADDR, 0x60010000u);
}
int main(void)
{
    config_bounds();
    configure_sequence();
    configure_readback();
    hfi_handoff();
    start_flow();
    puts("GPU GMU boot: vendor register sequence, readback, HFI table identity, "
         "bounded refusals and start handoff passed (RAM model only)");
    return 0;
}
