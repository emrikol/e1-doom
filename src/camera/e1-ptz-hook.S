/* Thumb entry trampoline for the decoded PTZ actuator dispatcher located by
 * its semantic instruction signature. At entry r1 is the channel, r2 is the
 * decoded command, and r3 is the speed. A pass-through reproduces the
 * displaced eight-byte prologue and continues at the runtime-resolved
 * address. */

    .syntax unified
    .thumb
    .section .text.e1_ptz_hook_entry,"ax",%progbits
    .balign 4
    .global e1_ptz_hook_entry
    .type e1_ptz_hook_entry,%function
    .hidden e1_ptz_continuation
    .thumb_func
e1_ptz_hook_entry:
    sub     sp, sp, #24
    str     r0, [sp, #0]
    str     r1, [sp, #4]
    str     r2, [sp, #8]
    str     r3, [sp, #12]
    str     r12, [sp, #16]
    str     lr, [sp, #20]
    mov     r0, r1
    mov     r1, r2
    mov     r2, r3
    bl      e1_ptz_hook_route
    cmp     r0, #0
    bne     .Lconsume

    ldr     r0, [sp, #0]
    ldr     r1, [sp, #4]
    ldr     r2, [sp, #8]
    ldr     r3, [sp, #12]
    ldr     r12, [sp, #16]
    ldr     lr, [sp, #20]
    add     sp, sp, #24

    /* Stable displaced prologue from the resolved actuator dispatcher. */
    push.w  {r4, r5, r6, r7, r8, r9, r10, r11, lr}
    mov     r4, r0
    mov     r5, r2
    ldr.w   r12, .Lcontinuation_offset
.Lcontinuation_pc:
    add     r12, pc
    ldr.w   r12, [r12]
    bx      r12

.Lconsume:
    ldr     lr, [sp, #20]
    add     sp, sp, #24
    movs    r0, #0
    bx      lr

    .balign 4
.Lcontinuation_offset:
    .word   e1_ptz_continuation - (.Lcontinuation_pc + 4)
    .size e1_ptz_hook_entry,.-e1_ptz_hook_entry

    .section .note.GNU-stack,"",%progbits
