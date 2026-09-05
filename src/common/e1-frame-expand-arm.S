    .syntax unified
    .arm
    .text
    .align 2

    .global e1_expand_i8_argb4444_arm
    .type e1_expand_i8_argb4444_arm, %function
e1_expand_i8_argb4444_arm:
    cmp     r3, #0
    bxeq    lr
    push    {r4-r7}
1:
    ldr     r4, [r1], #4
    uxtb    r5, r4
    uxtb    r6, r4, ror #8
    uxtb    r7, r4, ror #16
    lsr     r4, r4, #24
    ldr     r5, [r2, r5, lsl #2]
    ldr     r6, [r2, r6, lsl #2]
    ldr     r7, [r2, r7, lsl #2]
    ldr     r4, [r2, r4, lsl #2]
    pkhbt   r5, r5, r6, lsl #16
    pkhbt   r7, r7, r4, lsl #16
    str     r5, [r0], #4
    str     r7, [r0], #4
    subs    r3, r3, #4
    bne     1b
    pop     {r4-r7}
    bx      lr
    .size e1_expand_i8_argb4444_arm, .-e1_expand_i8_argb4444_arm

    .global e1_scale_i8_argb4444_arm
    .type e1_scale_i8_argb4444_arm, %function
e1_scale_i8_argb4444_arm:
    ldr     r12, [sp]
    cmp     r12, #0
    bxeq    lr
    push    {r4-r7}
1:
    ldr     r4, [r2], #4
    ldr     r6, [r2], #4
    uxth    r5, r4
    lsr     r4, r4, #16
    uxth    r7, r6
    lsr     r6, r6, #16
    ldrb    r5, [r1, r5]
    ldrb    r4, [r1, r4]
    ldrb    r7, [r1, r7]
    ldrb    r6, [r1, r6]
    ldr     r5, [r3, r5, lsl #2]
    ldr     r4, [r3, r4, lsl #2]
    ldr     r7, [r3, r7, lsl #2]
    ldr     r6, [r3, r6, lsl #2]
    pkhbt   r5, r5, r4, lsl #16
    pkhbt   r7, r7, r6, lsl #16
    str     r5, [r0], #4
    str     r7, [r0], #4
    subs    r12, r12, #4
    bne     1b
    pop     {r4-r7}
    bx      lr
    .size e1_scale_i8_argb4444_arm, .-e1_scale_i8_argb4444_arm

    .section .note.GNU-stack,"",%progbits
