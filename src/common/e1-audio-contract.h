#ifndef E1_AUDIO_CONTRACT_H
#define E1_AUDIO_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#define E1_AUDIO_SAMPLE_RATE UINT32_C(16000)
#define E1_AUDIO_SAMPLES_PER_FRAME UINT32_C(1024)
#define E1_AUDIO_PCM_BYTES_PER_FRAME \
    (E1_AUDIO_SAMPLES_PER_FRAME * (uint32_t)sizeof(int16_t))

struct e1_adts_contract {
    unsigned int mpeg_id;
    unsigned int object_type;
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int header_size;
    unsigned int frame_size;
    unsigned int buffer_fullness;
    unsigned int raw_data_blocks;
};

static inline void e1_fill_tone_1khz_s16(int16_t *samples, size_t count)
{
    static const int16_t wave[16] = {
        0, 5740, 10607, 13858, 15000, 13858, 10607, 5740,
        0, -5740, -10607, -13858, -15000, -13858, -10607, -5740,
    };
    size_t index;

    for (index = 0; index < count; ++index) {
        samples[index] = wave[index & 15U];
    }
}

static inline int e1_parse_stock_adts(const uint8_t *data, size_t available,
                                      struct e1_adts_contract *contract)
{
    static const unsigned int sample_rates[13] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350,
    };
    unsigned int frequency_index;
    unsigned int frame_size;
    unsigned int fullness;

    if (data == NULL || contract == NULL || available < 7U ||
        data[0] != UINT8_C(0xff) || data[1] != UINT8_C(0xf9)) {
        return -1;
    }
    frequency_index = (data[2] >> 2) & 15U;
    if (frequency_index >= 13U) {
        return -1;
    }
    frame_size = ((unsigned int)(data[3] & 3U) << 11) |
                 ((unsigned int)data[4] << 3) | (data[5] >> 5);
    fullness = ((unsigned int)(data[5] & 31U) << 6) | (data[6] >> 2);
    contract->mpeg_id = (data[1] >> 3) & 1U;
    contract->object_type = (data[2] >> 6) + 1U;
    contract->sample_rate = sample_rates[frequency_index];
    contract->channels = ((unsigned int)(data[2] & 1U) << 2) |
                         (data[3] >> 6);
    contract->header_size = 7U;
    contract->frame_size = frame_size;
    contract->buffer_fullness = fullness;
    contract->raw_data_blocks = data[6] & 3U;
    if (contract->mpeg_id != 1U || contract->object_type != 2U ||
        contract->sample_rate != E1_AUDIO_SAMPLE_RATE ||
        contract->channels != 1U || frame_size <= 7U ||
        frame_size > available || contract->raw_data_blocks != 0U) {
        return -1;
    }
    return 0;
}

#endif
