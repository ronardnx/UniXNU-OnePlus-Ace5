/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Terminal-only CM3 diagnostic. See docs/GPU_TERMINAL_BRINGUP.md.
 * Vector table at GMU ITCM 0; result mailbox at GMU DTCM 0x10004000.
 */
.syntax unified
.cpu cortex-m3
.thumb
#ifndef PROBE_IOVA
#define PROBE_IOVA 0x4000
#endif
.section .text,"ax",%progbits
.global vectors
vectors:
    .word 0x10007f80
    .word reset_handler
    .rept 14
    .word fault_handler
    .endr
.thumb_func
reset_handler:
    cpsid i
    ldr r0, =0x10004000
    ldr r1, =0xa7500001
    str r1, [r0]
    dsb
    ldr r2, =PROBE_IOVA
    ldr r3, [r2]
    str r3, [r0, #4]
    ldr r3, [r2, #4]
    str r3, [r0, #8]
    ldr r1, =0xa7500002
    str r1, [r0]
    dsb
idle:
    wfi
    b idle
.thumb_func
fault_handler:
    cpsid i
    ldr r0, =0x10004000
    ldr r2, =0xe000ed28
    ldr r3, [r2]
    str r3, [r0, #12]
    ldr r3, [r2, #16]
    str r3, [r0, #16]
    ldr r1, =0xa750dead
    str r1, [r0]
    dsb
    b idle
.ltorg
