/*
 * Giulia PSCI SMP implementation for Qualcomm SM8650 (Snapdragon 8 Gen 3).
 * Ported from hackios (osfmk/arm64/psci_smp.c) with adaptations for Giulia/Sweet.
 *
 * Secondary cores are powered on via standard ARM PSCI (CPU_ON 0xC4000003).
 * Physical address of LowResetVectorBase is provided as entry point.
 * Inter-processor interrupts (IPI) use GICv3 ICC_SGI1R_EL1 targeting core MPIDR.
 */
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/misc_protos.h>
#include <libkern/kernel_mach_header.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <vm/pmap.h>

#include "giulia_psci_smp.h"

#define PSCI_SMP_START_SECONDS  5
#define PSCI_SMP_STOP_SECONDS   3

extern unsigned long gVirtBase, gPhysBase;
extern void *LowResetVectorBase;

static uint32_t psci_smp_cpu_stage[MAX_CPUS];
static uint32_t psci_smp_started;

struct psci_smp_start_fault {
	uint64_t valid, mpidr, esr, elr, far, lr, sp, unused;
} psci_smp_start_fault __attribute__((aligned(64)));

static int64_t
psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = arg3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
		"x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	return (int64_t)x0;
}

static const char *
psci_error(int64_t status)
{
	static const char *const names[] = {
		"success", "not supported", "invalid parameters", "denied", "already on", "on pending",
		"internal failure", "not present", "disabled", "invalid address",
	};
	return (status <= 0 && status > -(int64_t)(sizeof(names) / sizeof(names[0]))) ? names[-status] : "unknown";
}

void
giulia_psci_smp_init(void)
{
	bzero(psci_smp_cpu_stage, sizeof(psci_smp_cpu_stage));
	psci_smp_started = 0;
	psci_smp_start_fault.valid = 0;
}

void
giulia_psci_smp_cpu_reached(uint32_t stage)
{
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], stage, release);
}

void
giulia_psci_smp_cpu_running(void)
{
	giulia_psci_smp_cpu_reached(PSCI_SMP_CPU_RUNNING);
}

static void
psci_smp_clean_segment(const char *name)
{
	unsigned long size = 0;
	vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);

	if (start != 0 && size != 0) {
		CleanPoC_DcacheRegion(start, size);
	}
}

void
giulia_sgi_signal(uint32_t phys_id)
{
	uint64_t aff0 = phys_id & 0xff;
	uint64_t aff1 = (phys_id >> 8) & 0xff;
	uint64_t sgi = ((uint64_t)SWEET_IPI_SGI << 24) | (aff1 << 16) | (1ULL << (aff0 & 15));

	__builtin_arm_dsb(DSB_ISHST);
	__builtin_arm_wsr64("ICC_SGI1R_EL1", sgi);
	__builtin_arm_isb(ISB_SY);
}

kern_return_t
giulia_psci_smp_start_cpu(int cpu)
{
	cpu_data_t *cdp = cpu_datap(cpu);
	if (cdp == NULL) {
		return KERN_FAILURE;
	}
	uint32_t phys_id = cdp->cpu_phys_id;
	uint64_t entry = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	uint64_t now, deadline;
	int64_t status;

	psci_smp_start_fault.valid = 0;
	psci_smp_clean_segment("__TEXT");
	psci_smp_clean_segment("__DATA_CONST");
	psci_smp_clean_segment("__TEXT_EXEC");
	psci_smp_clean_segment("__DATA");
	psci_smp_clean_segment("__BOOTDATA");
	for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
		if (cpu_datap(other) != NULL) {
			CleanPoC_DcacheRegion((vm_offset_t)cpu_datap(other), sizeof(cpu_data_t));
		}
	}
	__builtin_arm_dsb(DSB_SY);

	os_atomic_store(&psci_smp_cpu_stage[cpu], PSCI_SMP_CPU_ASKED, release);
	status = psci_call(PSCI_CPU_ON, phys_id, entry, 0);
	if (status != 0) {
		os_atomic_store(&psci_smp_cpu_stage[cpu], 0, release);
		printf("psci: cpu %d (MPIDR 0x%x) was not started: %lld (%s)\n",
			cpu, phys_id, status, psci_error(status));
		return KERN_FAILURE;
	}
	os_atomic_or(&psci_smp_started, 1u << cpu, relaxed);

	clock_interval_to_deadline(PSCI_SMP_START_SECONDS, NSEC_PER_SEC, &deadline);
	do {
		uint32_t stage = os_atomic_load(&psci_smp_cpu_stage[cpu], acquire);
		if (stage >= PSCI_SMP_CPU_RUNNING) {
			return KERN_SUCCESS;
		}
		FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
		if (psci_smp_start_fault.valid != 0) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
		now = mach_absolute_time();
	} while (now < deadline);

	if (psci_smp_start_fault.valid != 0) {
		struct psci_smp_start_fault *f = &psci_smp_start_fault;
		printf("psci: cpu %d (MPIDR 0x%llx) exception: ESR 0x%llx ELR 0x%llx FAR 0x%llx\n",
			cpu, f->mpidr, f->esr, f->elr, f->far);
		os_atomic_andnot(&psci_smp_started, 1u << cpu, relaxed);
	} else {
		printf("psci: cpu %d (MPIDR 0x%x) timed out waiting to join scheduler\n",
			cpu, phys_id);
	}
	return KERN_FAILURE;
}

void
giulia_psci_smp_cpu_off(void)
{
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], PSCI_SMP_CPU_OFF, release);
	__builtin_arm_dsb(DSB_SY);
	(void) psci_call(PSCI_CPU_OFF, 0, 0, 0);
	for (;;) {
		__builtin_arm_wfi();
	}
}
