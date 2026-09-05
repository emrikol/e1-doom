#include "e1-audio-ring.h"

#include <string.h>

int e1_audio_ring_read_next(struct e1_audio_ring *ring,
                            uint32_t *last_frame,
                            int16_t output[E1_AUDIO_SAMPLES_PER_FRAME],
                            uint32_t *dropped_frames)
{
    unsigned int attempt;

    if (ring == NULL || last_frame == NULL || output == NULL ||
        dropped_frames == NULL) {
        return -1;
    }
    for (attempt = 0; attempt < 2U; ++attempt) {
        const struct e1_audio_slot *slot;
        uint32_t published;
        uint32_t oldest;
        uint32_t target;
        uint32_t sequence_before;
        uint32_t sequence_after;
        uint32_t slot_frame;

        published = ring->published_frame;
        e1_audio_barrier();
        if (published == 0U || published <= *last_frame) {
            memset(output, 0, E1_AUDIO_PCM_BYTES_PER_FRAME);
            return 0;
        }
        oldest = published > E1_AUDIO_RING_SLOTS
                     ? published - E1_AUDIO_RING_SLOTS + 1U
                     : 1U;
        if (*last_frame == 0U) {
            target = published > E1_AUDIO_PREBUFFER_FRAMES
                         ? published - E1_AUDIO_PREBUFFER_FRAMES
                         : 1U;
        } else {
            target = *last_frame + 1U;
            if (target < oldest) {
                *dropped_frames += oldest - target;
                target = oldest;
            }
        }
        slot = &ring->slot[(target - 1U) & (E1_AUDIO_RING_SLOTS - 1U)];
        sequence_before = slot->sequence;
        e1_audio_barrier();
        if ((sequence_before & 1U) != 0U) {
            continue;
        }
        slot_frame = slot->frame_number;
        memcpy(output, slot->samples, E1_AUDIO_PCM_BYTES_PER_FRAME);
        e1_audio_barrier();
        sequence_after = slot->sequence;
        if (sequence_before == sequence_after &&
            (sequence_after & 1U) == 0U && slot_frame == target) {
            *last_frame = target;
            ring->consumed_frame = target;
            e1_audio_barrier();
            return 1;
        }
    }
    memset(output, 0, E1_AUDIO_PCM_BYTES_PER_FRAME);
    return -1;
}
