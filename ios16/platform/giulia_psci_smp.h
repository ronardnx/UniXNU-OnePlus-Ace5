/*
 * Giulia PSCI SMP implementation for Qualcomm SM8650 (Snapdragon 8 Gen 3).
 * Ported from hackios (osfmk/arm64/psci_smp.c) with co-authorship.
 */
#ifndef _GIULIA_PSCI_SMP_H_
#define _GIULIA_PSCI_SMP_H_

#include <mach/mach_types.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON             0xC4000003u
#define PSCI_AFFINITY_INFO      0xC4000004u
#define PSCI_AFFINITY_OFF       1

#define PSCI_SMP_CPU_ASKED      1       /* firmware agreed to start it */
#define PSCI_SMP_CPU_IN_KERNEL  2       /* core reached arm_init_cpu */
#define PSCI_SMP_CPU_RUNNING    3       /* core joined the scheduler */
#define PSCI_SMP_CPU_OFF        4       /* core switched off */

#define SWEET_IPI_SGI           1       /* SGI 1 used for cross-CPU signals */

extern void             giulia_psci_smp_init(void);
extern kern_return_t    giulia_psci_smp_start_cpu(int cpu);
extern void             giulia_psci_smp_cpu_reached(uint32_t stage);
extern void             giulia_psci_smp_cpu_running(void);
extern void             giulia_psci_smp_cpu_off(void) __dead2;
extern void             giulia_sgi_signal(uint32_t phys_id);

__END_DECLS

#endif /* _GIULIA_PSCI_SMP_H_ */
