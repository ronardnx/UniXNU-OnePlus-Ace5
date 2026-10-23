/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included ONLY in Darwin 22 arm/pmap/pmap.c by the opt-in patch generator.
 * Only owned module allocations are eligible for executable access.
 * pmap_enter(), ml_static_protect() and normal pmap_protect() stay unchanged.
 */
#include <pexpert/pexpert.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include "native_hotload.h"
static LCK_GRP_DECLARE(nlh_memory_group, "native_hotload_memory");
static LCK_MTX_DECLARE(nlh_memory_lock, &nlh_memory_group);
static struct { vm_offset_t address; vm_size_t size; } nlh_memory[NLH_MAX_MODULES * 2];
static struct {
    vm_offset_t start, end;
    thread_t owner;
} nlh_execute_request;

int nlh_memory_enabled(void);
int nlh_memory_create(unsigned, const void *, unsigned);
void *nlh_memory_address(unsigned);
void nlh_memory_destroy(unsigned);

#if (DEVELOPMENT || DEBUG) && !XNU_MONITOR && !defined(KERNEL_INTEGRITY_KTRR) && !defined(KERNEL_INTEGRITY_CTRR) && !__arm64e__ && !KASAN
const char nlh_memory_build[] = "NLH memory abi=2 executable=1";
#else
const char nlh_memory_build[] = "NLH memory abi=2 executable=0";
#endif

int nlh_memory_enabled(void)
{
#if (DEVELOPMENT || DEBUG) && !XNU_MONITOR && !defined(KERNEL_INTEGRITY_KTRR) && !defined(KERNEL_INTEGRITY_CTRR) && !__arm64e__ && !KASAN
    int enabled = 0;
    PE_parse_boot_argn("unixnu_hotload", &enabled, sizeof(enabled));
    return enabled == 1;
#else
    return 0;
#endif
}

/* Called inside the existing pmap/PVH locks. No sleeping, allocation or
 * second lock here. Only the synchronous seal operation knows this cookie. */
static bool nlh_execute_allowed(pmap_t pmap, vm_map_address_t start,
                               vm_map_address_t end, vm_prot_t prot, void *args)
{
    return args == &nlh_execute_request && pmap == kernel_pmap &&
        prot == (VM_PROT_READ | VM_PROT_EXECUTE) &&
        nlh_execute_request.owner == current_thread() &&
        start >= nlh_execute_request.start && end <= nlh_execute_request.end && start < end;
}

int nlh_memory_create(unsigned slot, const void *code, unsigned length)
{
    if (!nlh_memory_enabled()) return ENOTSUP;
    if (slot >= NLH_MAX_MODULES * 2 || !code || !length || length > NLH_MAX_CODE) return EINVAL;
    lck_mtx_lock(&nlh_memory_lock);
    if (nlh_memory[slot].address) { lck_mtx_unlock(&nlh_memory_lock); return EBUSY; }
    vm_size_t size = round_page(length);
    vm_offset_t address = 0;
    /* An owned wired object, initially RW/NX; never use KMA_DATA for code. */
    kern_return_t kr = kmem_alloc(kernel_map, &address, size, KMA_ZERO, VM_KERN_MEMORY_OSKEXT);
    if (kr != KERN_SUCCESS) { lck_mtx_unlock(&nlh_memory_lock); return ENOMEM; }
    memcpy((void *)address, code, length);
    /* First remove write permission in VM and pmap. Then seal only this range.
     * Normal protect cannot grant execute on ARM64. The private cookie below
     * permits PXN removal inside the existing locked PTE/TLB update path. */
    kr = vm_map_protect(kernel_map, address, address + size,
        VM_PROT_READ | VM_PROT_EXECUTE, FALSE);
    if (kr != KERN_SUCCESS) {
        kmem_free(kernel_map, address, size);
        lck_mtx_unlock(&nlh_memory_lock);
        return EACCES;
    }
    flush_dcache64(address, (unsigned)size, FALSE);
    invalidate_icache64(address, (unsigned)size, FALSE);
    nlh_execute_request.start = address;
    nlh_execute_request.end = address + size;
    nlh_execute_request.owner = current_thread();
    pmap_protect_options(kernel_pmap, address, address + size,
        VM_PROT_READ | VM_PROT_EXECUTE, 0, &nlh_execute_request);
    bzero(&nlh_execute_request, sizeof(nlh_execute_request));
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
    /* Verify effective permissions, including table-level execute overrides.
     * A callback must never be the first test of executable mapping support. */
    bool sealed = true;
    pmap_lock(kernel_pmap, PMAP_LOCK_SHARED);
    for (vm_offset_t at = address; at < address + size; at += PAGE_SIZE) {
        uint64_t prot = pmap_get_arm64_prot(kernel_pmap, at);
        if (prot != (ARM_PTE_AP(AP_RONA) | ARM_PTE_NX)) { sealed = false; break; }
    }
    pmap_unlock(kernel_pmap, PMAP_LOCK_SHARED);
    if (!sealed) {
        pmap_protect(kernel_pmap, address, address + size, VM_PROT_READ);
        kmem_free(kernel_map, address, size);
        lck_mtx_unlock(&nlh_memory_lock);
        return EACCES;
    }
    nlh_memory[slot].address = address;
    nlh_memory[slot].size = size;
    lck_mtx_unlock(&nlh_memory_lock);
    return 0;
}

void *nlh_memory_address(unsigned slot)
{
    return slot < NLH_MAX_MODULES * 2 ? (void *)nlh_memory[slot].address : NULL;
}

void nlh_memory_destroy(unsigned slot)
{
    if (slot >= NLH_MAX_MODULES * 2) return;
    lck_mtx_lock(&nlh_memory_lock);
    if (nlh_memory[slot].address) {
        /* The runtime has already closed admission and drained all calls. */
        pmap_protect(kernel_pmap, nlh_memory[slot].address,
            nlh_memory[slot].address + nlh_memory[slot].size, VM_PROT_READ);
        kmem_free(kernel_map, nlh_memory[slot].address, nlh_memory[slot].size);
        bzero(&nlh_memory[slot], sizeof(nlh_memory[slot]));
    }
    lck_mtx_unlock(&nlh_memory_lock);
}
