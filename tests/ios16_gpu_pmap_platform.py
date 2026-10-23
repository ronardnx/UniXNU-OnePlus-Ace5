"""Exercise the generated GPU pmap gate on a RAM-only page-table model.

The tested function bodies are generator._STATE / generator._GUARD, not a
second implementation. Host verification substitutes only the three AArch64
system-register reads; native codegen keeps the original reads and minimal
kernel-compatible declarations. This is not a full XNU compilation, target
execution or proof that a hardware mapping exists.
"""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_gpu_pmap_patch import _GUARD, _STATE

DECLARATIONS = '''#include "giulia_gpu_fence_pmap.h"
#include "giulia_gpu_pmap.h"
#define ARM64_BOARD_CONFIG_SWEET 1
#define PAGE_SIZE 16384u
#define PMAP_LOCK_SHARED 0
#define KERN_PROTECTION_FAILURE 2
#define VM_WIMG_MASK (0xffu << 16)
#define VM_WIMG_DEFAULT 0u
#define VM_WIMG_IO (1u << 16)
#define VM_WIMG_COPYBACK (2u << 16)
#define PT_ENTRY_NULL ((pt_entry_t *)0)
typedef uint64_t pt_entry_t;
typedef uint64_t pmap_paddr_t;
typedef uint64_t vm_map_address_t;
struct page_table_attr { uint32_t page; };
struct pmap { const struct page_table_attr *pmap_pt_attr; uint64_t tte; };
'''

SERVICES = '''extern struct pmap kernel_pmap_store;
extern const struct pmap *kernel_pmap;
extern const struct page_table_attr * const native_pt_attr;
extern uint64_t vm_first_phys, vm_last_phys;
static uint64_t pt_attr_page_size(const struct page_table_attr *a) { return a->page; }
static const struct page_table_attr *pmap_get_pt_attr(const struct pmap *p) { return p->pmap_pt_attr; }
extern void pmap_lock(const struct pmap *, int);
extern void pmap_unlock(const struct pmap *, int);
extern pt_entry_t *pmap_pte(const struct pmap *, vm_map_address_t);
'''

HOST = DECLARATIONS + SERVICES + '''#include <assert.h>
#include <stdio.h>
#include <string.h>
struct pmap kernel_pmap_store;
const struct pmap *kernel_pmap = &kernel_pmap_store;
static const struct page_table_attr host_attr = { 16384 };
const struct page_table_attr * const native_pt_attr = &host_attr;
uint64_t vm_first_phys = UINT64_C(0x8c0000000), vm_last_phys = UINT64_C(0x8c8000000);
unsigned long gPhysBase = UINT64_C(0x8c0000000), gPhysSize = UINT64_C(0x80000000);
static uint64_t host_mair = UINT64_C(0x4400), host_tcr, host_ttbr1 = UINT64_C(0x80120000);
static uint64_t locks, unlocks;
static struct { vm_map_address_t va; pt_entry_t pte; } host_leaves[64];
static unsigned host_leaf_count;
static uint64_t host_mair_value(void) { return host_mair; }
static uint64_t host_tcr_value(void) { return host_tcr; }
static uint64_t host_ttbr1_value(void) { return host_ttbr1; }
void pmap_lock(const struct pmap *p, int mode) { assert(p == kernel_pmap && mode == PMAP_LOCK_SHARED); ++locks; }
void pmap_unlock(const struct pmap *p, int mode) { assert(p == kernel_pmap && mode == PMAP_LOCK_SHARED); ++unlocks; }
pt_entry_t *pmap_pte(const struct pmap *p, vm_map_address_t va)
{
    assert(p == kernel_pmap);
    for (unsigned i = 0; i < host_leaf_count; i++) if (host_leaves[i].va == va) return &host_leaves[i].pte;
    return PT_ENTRY_NULL;
}
static pt_entry_t host_leaf(uint64_t pa, unsigned writable)
{ return pa | UINT64_C(0x60000000000407) | (writable ? 0 : UINT64_C(0x80)); }
static pt_entry_t wimg_to_pte(unsigned wimg, pmap_paddr_t pa)
{
    const unsigned index = wimg == VM_WIMG_IO ? 1u : 0u;
    return pa | ((uint64_t)index << 2) | UINT64_C(0x60000000000403);
}
static uint64_t host_tcr_for(unsigned page)
{
    const uint64_t tg1 = page == 16384 ? 1 : page == 4096 ? 2 : 3;
    return (tg1 << 30) | (UINT64_C(16) << 16);
}
static void fill(uint64_t pa, size_t bytes, unsigned writable)
{
    host_leaf_count = 0;
    for (size_t i = 0; i < bytes / 16384; i++) {
        host_leaves[i].va = UINT64_C(0xfffffff010000000) + i * 16384;
        host_leaves[i].pte = host_leaf(pa + i * 16384, writable);
        host_leaf_count++;
    }
}
'''

_TEST_BODY = '''
static void fresh(void)
{
    memset(&giulia_gpu_pmap_state, 0, sizeof(giulia_gpu_pmap_state));
    kernel_pmap_store.pmap_pt_attr = &host_attr;
    kernel_pmap_store.tte = host_ttbr1;
    host_mair = UINT64_C(0x4400);
    host_tcr = host_tcr_for(16384);
    gPhysBase = UINT64_C(0x8c0000000); gPhysSize = UINT64_C(0x80000000);
    vm_first_phys = UINT64_C(0x8c0000000); vm_last_phys = UINT64_C(0x8c8000000);
    locks = unlocks = 0;
    fill(UINT64_C(0xd4f6c000), 0x1c000, 1);
}
static void boot_phase(void)
{
    fresh();
    assert(!giulia_gpu_fence_cpu_policy_ready());
    /* A wrong MAIR, TG1 or page fails the latch but keeps the boot values. */
    giulia_gpu_fence_cpu_boot_validate(host_tcr, UINT64_C(0xff44), 16384);
    giulia_gpu_fence_cpu_boot_retired();
    assert(!giulia_gpu_fence_cpu_policy_ready() &&
           giulia_gpu_pmap_state.boot_mair == UINT64_C(0xff44));
    giulia_gpu_fence_cpu_boot_validate(host_tcr_for(4096), UINT64_C(0x4400), 16384);
    giulia_gpu_fence_cpu_boot_retired();
    assert(!giulia_gpu_fence_cpu_policy_ready());
    fresh();
    giulia_gpu_fence_cpu_boot_validate(host_tcr, UINT64_C(0x4400), 16384);
    giulia_gpu_fence_cpu_boot_map_check(UINT64_C(0x9c000000), 0x100000, 16384);
    giulia_gpu_fence_cpu_boot_retired();
    assert(giulia_gpu_fence_cpu_policy_ready());
}
static void audit_ok(void)
{
    struct ggfpm_audit audit;
    const uint64_t va = UINT64_C(0xfffffff010000000), pa = UINT64_C(0xd4f6c000);
    fresh();
    giulia_gpu_fence_cpu_boot_validate(host_tcr, UINT64_C(0x4400), 16384);
    giulia_gpu_fence_cpu_boot_retired();
    assert(giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 1, &audit) == 1);
    assert(audit.abi == GGFPM_ABI && audit.bytes == 0x1c000 && audit.span == 0x1c000);
    assert(audit.cpu_page_bytes == 16384 && audit.writable == 1);
    assert(audit.virtual_address == va && audit.physical == pa);
    assert(audit.mair == UINT64_C(0x4400) && audit.tcr == host_tcr);
    assert(audit.ttbr1 == host_ttbr1);
    assert(audit.first_descriptor == host_leaf(pa, 1));
    assert(audit.last_descriptor == host_leaf(pa + 6 * 16384, 1));
    assert(locks == 1 && unlocks == 1);
}
static void audit_refusals(void)
{
    struct ggfpm_audit audit, unchanged, saved;
    const uint64_t va = UINT64_C(0xfffffff010000000), pa = UINT64_C(0xd4f6c000);
    fresh();
    giulia_gpu_fence_cpu_boot_validate(host_tcr, UINT64_C(0x4400), 16384);
    giulia_gpu_fence_cpu_boot_retired();
    assert(giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 1, &audit) == 1);
    /* A corrupted middle leaf must fail even though the endpoints stay good. */
    host_leaves[3].pte = host_leaf(pa + 3 * 16384, 1) & ~UINT64_C(0x400);
    memset(&unchanged, 0x5a, sizeof(unchanged)); saved = unchanged;
    assert(giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 1, &unchanged) == 0);
    assert(!memcmp(&unchanged, &saved, sizeof(unchanged)) && locks == unlocks);
    fill(pa, 0x1c000, 1);
    /* A wrong PA, a wrong permission, a missing leaf and a short span fail. */
    assert(giulia_gpu_fence_cpu_audit(va, pa + 16384, 0x1c000, 1, &unchanged) == 0);
    fill(pa, 0x1c000, 0);
    assert(giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 1, &unchanged) == 0);
    fill(pa, 0x1c000, 1);
    host_leaf_count = 6;
    assert(giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 1, &unchanged) == 0);
    fill(pa, 0x1c000, 1);
    assert(giulia_gpu_fence_cpu_audit(va, pa, 0x1c001, 1, &unchanged) == 0);
    assert(giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 0, &unchanged) == 0);
    /* A policy that pulls the carveout into the physical aperture or managed
     * RAM can never authorize the audit. */
    fill(pa, 0x1c000, 1);
    gPhysBase = UINT64_C(0xd0000000); gPhysSize = UINT64_C(0x10000000);
    assert(giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 1, &unchanged) == 0);
    gPhysBase = UINT64_C(0x8c0000000); gPhysSize = UINT64_C(0x80000000);
    vm_first_phys = pa;
    vm_last_phys = pa + 0x1c000;
    assert(giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 1, &unchanged) == 0);
    /* A TTBR1 swap after the boot latch is not accepted. */
    vm_first_phys = UINT64_C(0x8c0000000); vm_last_phys = UINT64_C(0x8c8000000);
    host_ttbr1 = UINT64_C(0x80124000);
    assert(!giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 1, &unchanged));
    host_ttbr1 = UINT64_C(0x80120000);
    fill(pa, 0x1c000, 1);
    assert(giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 1, &audit) == 1);
}
static void boot_block(void)
{
    struct ggfpm_audit audit;
    const uint64_t va = UINT64_C(0xfffffff010000000), pa = UINT64_C(0xd4f6c000);
    fresh();
    giulia_gpu_fence_cpu_boot_validate(host_tcr, UINT64_C(0x4400), 16384);
    /* Any bootstrap mapping of the carveout itself blocks the gate. */
    giulia_gpu_fence_cpu_boot_map_check(GGPM_CARVEOUT_PHYSICAL, GGPM_CARVEOUT_BYTES, 16384);
    giulia_gpu_fence_cpu_boot_retired();
    assert(!giulia_gpu_fence_cpu_policy_ready());
    assert(!giulia_gpu_fence_cpu_audit(va, pa, 0x1c000, 1, &audit));
    /* Latch survives re-validation and retirement. */
    giulia_gpu_fence_cpu_boot_validate(host_tcr, UINT64_C(0x4400), 16384);
    giulia_gpu_fence_cpu_boot_retired();
    assert(!giulia_gpu_fence_cpu_policy_ready());
}
static void guard(void)
{
    fresh();
    /* Only attr index 1 is accepted for the fixed carveout, whatever the
     * caller asked for; unrelated physical ranges are untouched. */
    assert(giulia_gpu_pmap_guard_enter(GGPM_CARVEOUT_PHYSICAL, VM_WIMG_IO) == 0);
    assert(giulia_gpu_pmap_guard_enter(GGPM_CARVEOUT_PHYSICAL, VM_WIMG_COPYBACK) == 1);
    assert(giulia_gpu_pmap_guard_enter(GGPM_CARVEOUT_PHYSICAL, VM_WIMG_DEFAULT) == 1);
    assert(giulia_gpu_pmap_guard_enter(GGPM_CARVEOUT_END - 0x1000, VM_WIMG_COPYBACK) == 1);
    assert(giulia_gpu_pmap_guard_enter(GGPM_CARVEOUT_END, VM_WIMG_COPYBACK) == 0);
    assert(giulia_gpu_pmap_guard_enter(UINT64_C(0x9c000000), VM_WIMG_COPYBACK) == 0);
}
int main(void)
{
    boot_phase();
    audit_ok();
    audit_refusals();
    boot_block();
    guard();
    puts("Native GPU pmap gate RAM shim: boot latch, TTBR1/TCR/MAIR policy, "
         "full-envelope audit, alias windows, boot block and NC-only guard passed");
    return 0;
}
'''

NATIVE = DECLARATIONS + SERVICES + '''extern pt_entry_t wimg_to_pte(unsigned int, pmap_paddr_t);
'''


def build_glue(host: bool) -> str:
    state = _STATE
    guard = _GUARD
    if host:
        for expression, replacement in (
                ('__builtin_arm_rsr64("MAIR_EL1")', 'host_mair_value()'),
                ('__builtin_arm_rsr64("TCR_EL1")', 'host_tcr_value()'),
                ('__builtin_arm_rsr64("TTBR1_EL1")', 'host_ttbr1_value()')):
            assert state.count(expression) == 1, f'Review changed native register read: {expression}'
            state = state.replace(expression, replacement)
        guard = guard.replace('static int\ngiulia_gpu_pmap_guard_enter',
                              'int\ngiulia_gpu_pmap_guard_enter')
    return state + guard


def run() -> None:
    with tempfile.TemporaryDirectory(prefix='giulia-gpu-pmap-platform-') as directory:
        work = Path(directory)
        host = work / 'pmap-host.c'
        native = work / 'pmap-native.c'
        executable = work / 'pmap-host'
        assembly = work / 'pmap-native.s'
        host.write_text(HOST + build_glue(True) + _TEST_BODY, encoding='utf-8')
        # The guard is a static helper in the real pmap; keep a reference so an
        # otherwise unused-function diagnostic cannot mask a real error.
        native.write_text(NATIVE + build_glue(False) + '''
void *giulia_gpu_pmap_native_keep(void)
{ return (void *)giulia_gpu_pmap_guard_enter; }
''', encoding='utf-8')
        common = ['-std=c11', '-Wall', '-Wextra', '-Werror', *include_flags(ROOT)]
        subprocess.run([os.environ.get('CC', 'cc'), *common,
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g',
            str(host), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        subprocess.run(['clang', '--target=arm64-apple-darwin22', '-ffreestanding',
            *common, '-O2', '-S', str(native), '-o', str(assembly)], check=True)
        text = assembly.read_text(encoding='utf-8')
        for name in ('policy_ready', 'audit', 'boot_validate', 'boot_map_check', 'boot_retired'):
            assert re.search(r'^_giulia_gpu_fence_cpu_' + name + r':', text, re.M), name
        for register in ('MAIR_EL1', 'TCR_EL1', 'TTBR1_EL1'):
            assert re.search(r'\bmrs\s+x\d+,\s*' + register + r'\b', text), register
        assert re.search(r'\bldr\s+x\d+,\s*\[x\d+\]', text)
        print('Native GPU pmap gate emits all five Darwin/AArch64 exports, MRS reads '
              'of MAIR/TCR/TTBR1 and 64-bit descriptor loads; RAM model only')


if __name__ == '__main__':
    run()
