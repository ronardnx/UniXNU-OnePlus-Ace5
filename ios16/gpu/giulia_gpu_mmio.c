/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#ifndef UINT64_C
#define UINT64_C(x) x##ULL
#endif
#ifndef UINT32_C
#define UINT32_C(x) x##U
#endif
#include "../vendor/ArmSmmu4K/ArmSmmu4KMmio.c"
#endif
