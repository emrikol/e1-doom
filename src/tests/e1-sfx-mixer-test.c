#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "e1-sfx-mixer.h"

static void test_resample_and_finish(void)
{
    uint8_t source[4410];
    int16_t output[E1_AUDIO_SAMPLES_PER_FRAME];
    struct e1_sfx_voice voice;
    unsigned int frame;
    size_t index;
    int nonzero = 0;

    for (index = 0; index < sizeof(source); ++index) {
        source[index] = (uint8_t)(128 + ((index & 31U) < 16U ? 80 : -80));
    }
    assert(e1_sfx_start_voice(&voice, source, sizeof(source), 44100, 128,
                              127, 128) == 0);
    assert(voice.step_q16 > UINT32_C(0x20000));
    for (frame = 0; frame < 4; ++frame) {
        e1_sfx_mix_frame(&voice, 1, output);
        for (index = 0; index < E1_AUDIO_SAMPLES_PER_FRAME; ++index) {
            nonzero |= output[index] != 0;
        }
    }
    assert(nonzero);
    assert(!voice.active);
}

static void test_mix_clip_pause_and_pan_downmix(void)
{
    uint8_t source[4096];
    int16_t left[E1_AUDIO_SAMPLES_PER_FRAME];
    int16_t right[E1_AUDIO_SAMPLES_PER_FRAME];
    int16_t mixed[E1_AUDIO_SAMPLES_PER_FRAME];
    struct e1_sfx_voice voices[E1_SFX_MAX_VOICES];
    size_t index;

    memset(source, 255, sizeof(source));
    memset(voices, 0, sizeof(voices));
    assert(e1_sfx_start_voice(&voices[0], source, sizeof(source), 16000, 128,
                              100, 0) == 0);
    e1_sfx_mix_frame(voices, 1, left);
    assert(e1_sfx_start_voice(&voices[0], source, sizeof(source), 16000, 128,
                              100, 255) == 0);
    e1_sfx_mix_frame(voices, 1, right);
    assert(memcmp(left, right, sizeof(left)) == 0);

    for (index = 0; index < E1_SFX_MAX_VOICES; ++index) {
        assert(e1_sfx_start_voice(&voices[index], source, sizeof(source),
                                  16000, 128, 127, 128) == 0);
    }
    voices[0].paused = 1;
    e1_sfx_mix_frame(voices, E1_SFX_MAX_VOICES, mixed);
    assert(mixed[0] == INT16_MAX);
    assert(voices[0].position_q16 == 0);
}

int main(void)
{
    test_resample_and_finish();
    test_mix_clip_pause_and_pan_downmix();
    puts("e1-sfx-mixer-test: ok");
    return 0;
}
