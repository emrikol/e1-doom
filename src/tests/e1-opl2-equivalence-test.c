#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "e1-opl2-noise.h"
#include "opl3.h"

void OPL3_ReferenceGenerate(opl3_chip *chip, Bit16s *buf);
void OPL3_ReferenceGenerateResampled(opl3_chip *chip, Bit16s *buf);
void OPL3_ReferenceReset(opl3_chip *chip, Bit32u samplerate);
void OPL3_ReferenceWriteReg(opl3_chip *chip, Bit16u reg, Bit8u value);

static uint32_t scalar_noise_advance_18(uint32_t noise)
{
    unsigned int clock;

    for (clock = 0; clock < 18U; ++clock) {
        uint32_t feedback = ((noise >> 14U) ^ noise) & UINT32_C(1);
        noise = (noise >> 1U) | (feedback << 22U);
    }
    return noise;
}

static void write_both(opl3_chip *fast, opl3_chip *reference,
                       Bit16u reg, Bit8u value)
{
    OPL3_WriteReg(fast, reg, value);
    OPL3_ReferenceWriteReg(reference, reg, value);
}

static void configure_voice(opl3_chip *fast, opl3_chip *reference,
                            unsigned int channel, unsigned int mod_offset,
                            unsigned int carrier_offset)
{
    write_both(fast, reference, (Bit16u)(0x20U + mod_offset), 0x21U);
    write_both(fast, reference, (Bit16u)(0x20U + carrier_offset), 0x01U);
    write_both(fast, reference, (Bit16u)(0x40U + mod_offset), 0x10U);
    write_both(fast, reference, (Bit16u)(0x40U + carrier_offset), 0x00U);
    write_both(fast, reference, (Bit16u)(0x60U + mod_offset), 0xf2U);
    write_both(fast, reference, (Bit16u)(0x60U + carrier_offset), 0xf2U);
    write_both(fast, reference, (Bit16u)(0x80U + mod_offset), 0x73U);
    write_both(fast, reference, (Bit16u)(0x80U + carrier_offset), 0x73U);
    write_both(fast, reference, (Bit16u)(0xe0U + mod_offset), 0x01U);
    write_both(fast, reference, (Bit16u)(0xe0U + carrier_offset), 0x00U);
    write_both(fast, reference, (Bit16u)(0xc0U + channel), 0x0eU);
}

int main(void)
{
    static const unsigned int operator_offsets[9][2] = {
        {0x00U, 0x03U}, {0x01U, 0x04U}, {0x02U, 0x05U},
        {0x08U, 0x0bU}, {0x09U, 0x0cU}, {0x0aU, 0x0dU},
        {0x10U, 0x13U}, {0x11U, 0x14U}, {0x12U, 0x15U}
    };
    opl3_chip fast;
    opl3_chip reference;
    uint32_t noise;
    unsigned int channel;
    unsigned int sample;

    for (noise = 0; noise < UINT32_C(0x800000); ++noise) {
        assert(e1_opl2_noise_advance_18(noise) ==
               scalar_noise_advance_18(noise));
    }

    OPL3_Reset(&fast, 16000U);
    OPL3_ReferenceReset(&reference, 16000U);
    write_both(&fast, &reference, 0x01U, 0x20U);

    for (channel = 0; channel < 9U; ++channel) {
        configure_voice(&fast, &reference, channel,
                        operator_offsets[channel][0],
                        operator_offsets[channel][1]);
        write_both(&fast, &reference, (Bit16u)(0xa0U + channel),
                   (Bit8u)(0x40U + channel * 7U));
        write_both(&fast, &reference, (Bit16u)(0xb0U + channel),
                   (Bit8u)(0x31U + ((channel & 3U) << 2U)));
    }

    for (sample = 0; sample < 100000U; ++sample) {
        Bit16s fast_output[2];
        Bit16s reference_output[2];

        if ((sample % 4096U) == 0U) {
            unsigned int selected = (sample / 4096U) % 9U;
            write_both(&fast, &reference, (Bit16u)(0xa0U + selected),
                       (Bit8u)(0x30U + ((sample / 4096U) & 0x7fU)));
        }
        if (sample == 20000U) {
            write_both(&fast, &reference, 0xbdU, 0x3fU);
        }
        if (sample == 60000U) {
            write_both(&fast, &reference, 0xbdU, 0x20U);
        }

        OPL3_GenerateResampled(&fast, fast_output);
        OPL3_ReferenceGenerateResampled(&reference, reference_output);
        assert(memcmp(fast_output, reference_output,
                      sizeof(fast_output)) == 0);
        assert(fast.noise == reference.noise);
    }

    /* Drive every voice through release, extended silence, and re-keying. */
    write_both(&fast, &reference, 0xbdU, 0x00U);
    for (channel = 0; channel < 9U; ++channel) {
        write_both(&fast, &reference, (Bit16u)(0xb0U + channel), 0x01U);
    }
    for (sample = 0; sample < 200000U; ++sample) {
        Bit16s fast_output[2];
        Bit16s reference_output[2];

        if (sample == 150000U) {
            for (channel = 0; channel < 9U; ++channel) {
                write_both(&fast, &reference,
                           (Bit16u)(0xb0U + channel),
                           (Bit8u)(0x31U + ((channel & 3U) << 2U)));
            }
        }
        OPL3_Generate(&fast, fast_output);
        OPL3_ReferenceGenerate(&reference, reference_output);
        assert(memcmp(fast_output, reference_output,
                      sizeof(fast_output)) == 0);
        assert(fast.noise == reference.noise);
    }

    puts("e1-opl2-equivalence-test-ok states=8388608 samples=300000");
    return 0;
}
