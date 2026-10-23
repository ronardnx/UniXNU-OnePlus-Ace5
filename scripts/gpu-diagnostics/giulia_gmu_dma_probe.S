/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Terminal-only CM3 diagnostic. See docs/GPU_TERMINAL_BRINGUP.md.
 * Uses an exclusively owned, mapped word at 0x60000000 and restores it.
 */
.syntax unified
.cpu cortex-m3
.thumb
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
    ldr r2, =0x4000
    ldr r3, [r2]
    str r3, [r0, #4]
    ldr r3, [r2, #4]
    str r3, [r0, #8]
    ldr r2, =0x60000000
    ldr r4, [r2]
    str r4, [r0, #24]
    ldr r3, =0xa750cafe
    str r3, [r2]
    dsb
    ldr r3, [r2]
    str r3, [r0, #12]
    str r4, [r2]
    dsb
    ldr r3, [r2]
    str r3, [r0, #28]
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
    str r3, [r0, #16]
    ldr r3, [r2, #16]
    str r3, [r0, #20]
    ldr r1, =0xa750dead
    str r1, [r0]
    dsb
    b idle
.ltorg
