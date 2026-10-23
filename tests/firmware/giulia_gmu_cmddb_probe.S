/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * FAILED EXPERIMENT: the target entered USB 900E when the command DB read
 * began, even through read-only translation. DO NOT REPLAY on device.
 * Kept only to reproduce and investigate the failed experiment offline.
 * Standalone read-only CM3 probe for the DT-reserved command DB region.
 * Runs with stock GMU stopped, SID 5 in explicit bypass mode. No external
 * store instructions exist. Host requests contain PA/count/sequence in DTCM.
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
again:
    ldr r1, [r0, #16]
    ldr r2, [r0, #20]
    cmp r1, r2
    beq again
    ldr r2, [r0, #8]
    ldr r3, [r0, #12]
    tst r2, #3
    bne refused
    cmp r3, #1
    blo refused
    cmp r3, #48
    bhi refused
    ldr r4, =0x81c60000
    cmp r2, r4
    blo refused
    ldr r5, =0x81c80000
    add.w r4, r2, r3, lsl #2
    cmp r4, r2
    blo refused
    cmp r4, r5
    bhi refused
    add r4, r0, #28
copy:
    ldr r5, [r2], #4
    str r5, [r4], #4
    subs r3, r3, #1
    bne copy
    ldr r2, =0xa750600d
    b done
refused:
    ldr r2, =0xa750bad0
done:
    dsb
    str r2, [r0]
    dsb
    str r1, [r0, #20]
    dsb
    b again
.thumb_func
fault_handler:
    cpsid i
    ldr r0, =0x10004000
    ldr r1, =0xa750dead
    str r1, [r0]
    dsb
halt:
    b halt
.balign 4
.ltorg
