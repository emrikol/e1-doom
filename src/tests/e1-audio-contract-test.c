#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "e1-audio-contract.h"

static void test_tone(void)
{
    int16_t samples[E1_AUDIO_SAMPLES_PER_FRAME];
    int64_t sum = 0;
    size_t index;

    e1_fill_tone_1khz_s16(samples, E1_AUDIO_SAMPLES_PER_FRAME);
    for (index = 0; index < E1_AUDIO_SAMPLES_PER_FRAME; ++index) {
        assert(samples[index] == samples[index & 15U]);
        sum += samples[index];
    }
    assert(sum == 0);
    assert(samples[4] == 15000);
    assert(samples[12] == -15000);
}

static void test_stock_adts(void)
{
    uint8_t frame[256];
    struct e1_adts_contract contract;

    memset(frame, 0, sizeof(frame));
    /* MPEG-2 AAC-LC, 16 kHz, mono, 256-byte frame, VBR fullness. */
    frame[0] = 0xff;
    frame[1] = 0xf9;
    frame[2] = 0x60;
    frame[3] = 0x40;
    frame[4] = 0x20;
    frame[5] = 0x1f;
    frame[6] = 0xfc;
    assert(e1_parse_stock_adts(frame, sizeof(frame), &contract) == 0);
    assert(contract.mpeg_id == 1);
    assert(contract.object_type == 2);
    assert(contract.sample_rate == E1_AUDIO_SAMPLE_RATE);
    assert(contract.channels == 1);
    assert(contract.frame_size == sizeof(frame));
    assert(contract.header_size == 7);
    assert(contract.buffer_fullness == 0x7ff);
    assert(contract.raw_data_blocks == 0);

    /* The camera's live FDK-AAC writer uses legal fixed-buffer fullness
     * values around 87--101 rather than the VBR sentinel. */
    frame[5] = 0x01;
    frame[6] = 0x5c;
    assert(e1_parse_stock_adts(frame, sizeof(frame), &contract) == 0);
    assert(contract.buffer_fullness == 87);

    frame[1] = 0xf1;
    assert(e1_parse_stock_adts(frame, sizeof(frame), &contract) != 0);
    frame[1] = 0xf9;
    frame[5] = 0x1f;
    frame[6] = 0xfc;
    frame[2] = 0x64;
    assert(e1_parse_stock_adts(frame, sizeof(frame), &contract) != 0);
    frame[2] = 0x60;
    frame[3] = 0x80;
    assert(e1_parse_stock_adts(frame, sizeof(frame), &contract) != 0);
}

int main(void)
{
    test_tone();
    test_stock_adts();
    puts("e1-audio-contract-test: ok");
    return 0;
}
