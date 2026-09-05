/* Thumb entry trampoline for the final stock PCM-to-AAC function located by
 * its semantic instruction signature. The C route may replace the 2,048-byte
 * PCM block before continuing in the untouched stock encoder. */

    .syntax unified
    .thumb
    .section .text.e1_audio_hook_entry,"ax",%progbits
    .balign 4
    .global e1_audio_hook_entry
    .type e1_audio_hook_entry,%function
    .hidden e1_audio_continuation
    .thumb_func
e1_audio_hook_entry:
    sub     sp, sp, #24
    str     r0, [sp, #0]
    str     r1, [sp, #4]
    str     r2, [sp, #8]
    str     r3, [sp, #12]
    str     r12, [sp, #16]
    str     lr, [sp, #20]
    mov     r0, r1
    mov     r1, r2
    bl      e1_audio_pcm_hook_route
    ldr     r0, [sp, #0]
    ldr     r1, [sp, #4]
    ldr     r2, [sp, #8]
    ldr     r3, [sp, #12]
    ldr     r12, [sp, #16]
    ldr     lr, [sp, #20]
    add     sp, sp, #24

    /* Stable displaced prologue from the resolved PCM-to-AAC function. */
    push    {r4, lr}
    add.w   r0, r0, #0x66000
    cmp     r2, #0
    ldr.w   r12, .Lcontinuation_offset
.Lcontinuation_pc:
    add     r12, pc
    ldr.w   r12, [r12]
    bx      r12

    .balign 4
.Lcontinuation_offset:
    .word   e1_audio_continuation - (.Lcontinuation_pc + 4)
    .size e1_audio_hook_entry,.-e1_audio_hook_entry

    .section .note.GNU-stack,"",%progbits
