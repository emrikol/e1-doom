#ifndef E1_OPL2_NOISE_H
#define E1_OPL2_NOISE_H

#include <stdint.h>

/*
 * Nuked OPL advances this 23-bit LFSR once per operator.  In OPL2 mode the
 * upper 18 OPL3 operators are permanently silent, so jump over their clocks
 * without evaluating 18 envelopes, phases, and waveforms.
 *
 * For clocks 0..8, feedback[k] = bit[k] ^ bit[k + 14].  For clocks 9..17,
 * feedback[k] = feedback[k - 9] ^ bit[k].  Packing those feedback bits and
 * shifting the five surviving input bits gives the exact state after 18
 * scalar clocks.
 */
static inline uint32_t e1_opl2_noise_advance_18(uint32_t noise)
{
#if defined(__arm__) && !defined(__thumb__)
    uint32_t feedback_low;
    uint32_t feedback_high;

    __asm__ volatile(
        "eor %[low], %[noise], %[noise], lsr #14\n\t"
        "ubfx %[low], %[low], #0, #9\n\t"
        "ubfx %[high], %[noise], #9, #9\n\t"
        "eor %[high], %[high], %[low]\n\t"
        "orr %[low], %[low], %[high], lsl #9\n\t"
        "lsr %[noise], %[noise], #18\n\t"
        "orr %[noise], %[noise], %[low], lsl #5"
        : [noise] "+r"(noise), [low] "=&r"(feedback_low),
          [high] "=&r"(feedback_high)
        :
        : "cc");
    return noise;
#else
    uint32_t feedback_low = (noise ^ (noise >> 14U)) & UINT32_C(0x1ff);
    uint32_t feedback_high = feedback_low ^ ((noise >> 9U) & UINT32_C(0x1ff));

    return (noise >> 18U) |
           ((feedback_low | (feedback_high << 9U)) << 5U);
#endif
}

#endif
