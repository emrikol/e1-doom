#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "e1-audio-ring.h"

static void initialize(struct e1_audio_ring *ring)
{
    memset(ring, 0, sizeof(*ring));
    ring->magic = E1_AUDIO_RING_MAGIC;
    ring->version = E1_AUDIO_RING_VERSION;
    ring->structure_size = sizeof(*ring);
    ring->sample_rate = E1_AUDIO_SAMPLE_RATE;
    ring->samples_per_frame = E1_AUDIO_SAMPLES_PER_FRAME;
    ring->slot_count = E1_AUDIO_RING_SLOTS;
}

static void publish(struct e1_audio_ring *ring, uint32_t frame, int16_t value)
{
    struct e1_audio_slot *slot =
        &ring->slot[(frame - 1U) & (E1_AUDIO_RING_SLOTS - 1U)];
    size_t index;

    ++slot->sequence;
    slot->frame_number = frame;
    for (index = 0; index < E1_AUDIO_SAMPLES_PER_FRAME; ++index) {
        slot->samples[index] = value;
    }
    ++slot->sequence;
    ring->published_frame = frame;
}

int main(void)
{
    struct e1_audio_ring ring;
    int16_t output[E1_AUDIO_SAMPLES_PER_FRAME];
    uint32_t last = 0;
    uint32_t dropped = 0;

    initialize(&ring);
    assert(e1_audio_ring_valid(&ring));
    memset(output, 1, sizeof(output));
    assert(e1_audio_ring_read_next(&ring, &last, output, &dropped) == 0);
    assert(output[0] == 0 && output[E1_AUDIO_SAMPLES_PER_FRAME - 1U] == 0);

    publish(&ring, 1, 1234);
    publish(&ring, 2, 2222);
    publish(&ring, 3, 3333);
    publish(&ring, 4, 4444);
    assert(e1_audio_ring_read_next(&ring, &last, output, &dropped) == 1);
    assert(last == 1 && ring.consumed_frame == 1 && dropped == 0 &&
           output[0] == 1234);
    assert(e1_audio_ring_read_next(&ring, &last, output, &dropped) == 1);
    assert(last == 2 && dropped == 0 && output[0] == 2222);
    assert(e1_audio_ring_read_next(&ring, &last, output, &dropped) == 1);
    assert(last == 3 && dropped == 0 && output[0] == 3333);
    assert(e1_audio_ring_read_next(&ring, &last, output, &dropped) == 1);
    assert(last == 4 && dropped == 0 && output[0] == 4444);
    assert(e1_audio_ring_read_next(&ring, &last, output, &dropped) == 0);

    publish(&ring, 5, 5555);
    assert(e1_audio_ring_read_next(&ring, &last, output, &dropped) == 1);
    assert(last == 5 && dropped == 0 && output[17] == 5555);

    /* The consumer skips only the one frame overwritten by an eight-slot
     * producer lap, then continues in chronological order. */
    publish(&ring, 6, 6000);
    publish(&ring, 7, 7000);
    publish(&ring, 8, 8000);
    publish(&ring, 9, 9000);
    publish(&ring, 10, 10000);
    publish(&ring, 11, 11000);
    publish(&ring, 12, 12000);
    publish(&ring, 13, 13000);
    publish(&ring, 14, 14000);
    assert(e1_audio_ring_read_next(&ring, &last, output, &dropped) == 1);
    assert(last == 7 && ring.consumed_frame == 7 && dropped == 1 &&
           output[17] == 7000);

    ring.slot[(8U - 1U) & (E1_AUDIO_RING_SLOTS - 1U)].sequence |= 1U;
    assert(e1_audio_ring_read_next(&ring, &last, output, &dropped) == -1);
    assert(last == 7 && output[0] == 0);

    puts("e1-audio-ring-test: ok");
    return 0;
}
