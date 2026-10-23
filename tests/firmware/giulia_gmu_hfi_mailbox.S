/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Terminal-only NMI mailbox. Link at 0x3a40 after checking the pinned
 * firmware's ITCM reservation ends at 0x3a3c. Install only while idle.
 * All general registers touched here are hardware-stacked or preserved.
 * No clock, voltage, firmware control or arbitrary RAM access is provided.
 * Mailbox in an unused DTCM gap: result, operation, address, words, sequence,
 * response_sequence, VTOR, data[48]. Operations: 0 identify, 1 read, 2 write.
 * Only the existing pinned HFI allocation may be accessed.
 */
.syntax unified
.cpu cortex-m3
.thumb
.section .text,"ax",%progbits
.global mailbox_handler
.thumb_func
mailbox_handler:
    push {r4-r7, lr}
    ldr r0, =0x10006ec0
    ldr r1, =0xe000ed08
    ldr r1, [r1]
    str r1, [r0, #24]
    ldr r1, [r0, #4]
    cbz r1, success
    cmp r1, #2
    bhi refused
    ldr r2, [r0, #8]
    tst r2, #3
    bne refused
    ldr r3, [r0, #12]
    cmp r3, #1
    blo refused
    cmp r3, #48
    bhi refused
    ldr r4, =0x60010000
    cmp r2, r4
    blo refused
    ldr r5, =0x60022000
    add.w r4, r2, r3, lsl #2
    cmp r4, r2
    blo refused
    cmp r4, r5
    bhi refused
    add r4, r0, #28
    dsb
copy:
    cmp r1, #1
    beq read_word
    ldr r5, [r4]
    str r5, [r2]
    dsb
read_word:
    ldr r5, [r2], #4
    str r5, [r4], #4
    subs r3, r3, #1
    bne copy
success:
    ldr r1, =0xa750600d
    b finish
refused:
    ldr r1, =0xa750bad0
finish:
    dsb
    ldr r2, [r0, #16]
    str r2, [r0, #20]
    dsb
    str r1, [r0]
    dsb
    pop {r4-r7, pc}
.balign 4
.ltorg
