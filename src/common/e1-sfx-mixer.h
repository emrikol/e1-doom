#ifndef E1_SFX_MIXER_H
#define E1_SFX_MIXER_H

#include <stddef.h>
#include <stdint.h>

#include "e1-audio-contract.h"

#define E1_SFX_MAX_VOICES 16U

struct e1_sfx_voice {
    const uint8_t *samples;
    uint32_t sample_count;
    uint64_t position_q16;
    uint32_t step_q16;
    uint16_t volume;
    uint16_t separation;
    uint8_t active;
    uint8_t paused;
};

int e1_sfx_start_voice(struct e1_sfx_voice *voice, const uint8_t *samples,
                       uint32_t sample_count, uint32_t source_rate,
                       unsigned int pitch, unsigned int volume,
                       unsigned int separation);
void e1_sfx_mix_frame(struct e1_sfx_voice *voices, size_t voice_count,
                      int16_t output[E1_AUDIO_SAMPLES_PER_FRAME]);

#endif
