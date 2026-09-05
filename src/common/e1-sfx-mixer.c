#include "e1-sfx-mixer.h"

#include <limits.h>
#include <string.h>

static int16_t clip_s16(int64_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

int e1_sfx_start_voice(struct e1_sfx_voice *voice, const uint8_t *samples,
                       uint32_t sample_count, uint32_t source_rate,
                       unsigned int pitch, unsigned int volume,
                       unsigned int separation)
{
    uint64_t numerator;
    uint64_t denominator = (uint64_t)E1_AUDIO_SAMPLE_RATE * 128U;

    if (voice == NULL || samples == NULL || sample_count < 2U ||
        source_rate < 4000U || source_rate > 96000U || pitch == 0U) {
        return -1;
    }
    if (pitch > 255U) {
        pitch = 255U;
    }
    if (volume > 127U) {
        volume = 127U;
    }
    if (separation > 255U) {
        separation = 255U;
    }
    numerator = (uint64_t)source_rate * pitch * UINT64_C(65536);
    memset(voice, 0, sizeof(*voice));
    voice->samples = samples;
    voice->sample_count = sample_count;
    voice->step_q16 = (uint32_t)(numerator / denominator);
    if (voice->step_q16 == 0U) {
        voice->step_q16 = 1U;
    }
    voice->volume = (uint16_t)volume;
    voice->separation = (uint16_t)separation;
    voice->active = volume != 0U;
    return voice->active ? 0 : -1;
}

void e1_sfx_mix_frame(struct e1_sfx_voice *voices, size_t voice_count,
                      int16_t output[E1_AUDIO_SAMPLES_PER_FRAME])
{
    int32_t mixed[E1_AUDIO_SAMPLES_PER_FRAME] = {0};
    size_t voice_index;
    size_t output_index;

    for (voice_index = 0; voice_index < voice_count; ++voice_index) {
        struct e1_sfx_voice *voice = &voices[voice_index];

        if (!voice->active || voice->paused) {
            continue;
        }
        for (output_index = 0; output_index < E1_AUDIO_SAMPLES_PER_FRAME;
             ++output_index) {
            uint64_t source_index;
            uint32_t fraction;
            int32_t first;
            int32_t second;
            int32_t sample;

            source_index = voice->position_q16 >> 16;
            if (source_index >= voice->sample_count) {
                voice->active = 0;
                break;
            }
            fraction = voice->position_q16 & UINT32_C(0xffff);
            first = ((int32_t)voice->samples[source_index] - 128) << 8;
            second = source_index + 1U < voice->sample_count
                         ? ((int32_t)voice->samples[source_index + 1U] - 128)
                               << 8
                         : first;
            sample = first +
                     (int32_t)(((int64_t)(second - first) * fraction) >> 16);

            /* In a mono downmix, the complementary stereo pan gains sum to
             * 255 exactly. Avoid calculating both channels only to add them
             * back together. */
            mixed[output_index] += sample * (int32_t)voice->volume / 127;
            voice->position_q16 += voice->step_q16;
            if ((voice->position_q16 >> 16) >= voice->sample_count) {
                voice->active = 0;
            }
        }
    }
    for (output_index = 0; output_index < E1_AUDIO_SAMPLES_PER_FRAME;
         ++output_index) {
        output[output_index] = clip_s16(mixed[output_index]);
    }
}
