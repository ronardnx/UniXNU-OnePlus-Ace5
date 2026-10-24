/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_CONTROL_H
#define GIULIA_GPU_CONTROL_H
#include "giulia_gpu_power_control.h"
#include "giulia_gpu_smmu.h"
#include "giulia_gpu_gmu.h"
#ifdef __cplusplus
extern "C" {
#endif
int giulia_gpu_power_acquire(const struct gg_power_ops *,struct gg_power_state *);
int giulia_gpu_power_release(const struct gg_power_ops *,struct gg_power_state *);
ArmSmmu4KStatus giulia_gpu_smmu_start(const struct gg_smmu_ops *,struct gg_smmu_state *,const ArmSmmu4KContextConfig *,unsigned,unsigned);
ArmSmmu4KStatus giulia_gpu_smmu_stop(const struct gg_smmu_ops *,struct gg_smmu_state *,const ArmSmmu4KContextConfig *);
/* Input is the entire digest-pinned GGFWPK1 container, not a raw GMU member. */
int giulia_gpu_gmu_load(const struct gg_gmu_ops *,struct gg_gmu_state *,const void *pack_bytes,size_t pack_length,struct gg_fw_block *,size_t);
int giulia_gpu_gmu_start(const struct gg_gmu_ops *,struct gg_gmu_state *);
void giulia_gpu_log_event(void *,const struct gg_event *);
#ifdef __cplusplus
}
#endif
#endif
