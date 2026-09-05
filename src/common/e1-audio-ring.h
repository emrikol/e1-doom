#ifndef E1_AUDIO_RING_H
#define E1_AUDIO_RING_H

#include <stdint.h>

#include "e1-audio-contract.h"

#define E1_AUDIO_RING_MAGIC UINT32_C(0x45314155)
#define E1_AUDIO_RING_VERSION UINT32_C(4)
#define E1_AUDIO_RING_SLOTS UINT32_C(8)
#define E1_AUDIO_PREBUFFER_FRAMES UINT32_C(3)

struct e1_audio_slot {
    volatile uint32_t sequence;
    volatile uint32_t frame_number;
    int16_t samples[E1_AUDIO_SAMPLES_PER_FRAME];
};

struct e1_audio_ring {
    uint32_t magic;
    uint32_t version;
    uint32_t structure_size;
    uint32_t sample_rate;
    uint32_t samples_per_frame;
    uint32_t slot_count;
    volatile uint32_t published_frame;
    volatile uint32_t consumed_frame;
    volatile uint32_t producer_drops;
    volatile uint32_t mixed_frames;
    volatile uint32_t nonzero_frames;
    volatile uint32_t last_nonzero_frame;
    volatile uint32_t peak_abs;
    struct e1_audio_slot slot[E1_AUDIO_RING_SLOTS];
};

static inline void e1_audio_barrier(void)
{
    __sync_synchronize();
}

static inline int e1_audio_ring_valid(const struct e1_audio_ring *ring)
{
    return ring != 0 && ring->magic == E1_AUDIO_RING_MAGIC &&
           ring->version == E1_AUDIO_RING_VERSION &&
           ring->structure_size == sizeof(*ring) &&
           ring->sample_rate == E1_AUDIO_SAMPLE_RATE &&
           ring->samples_per_frame == E1_AUDIO_SAMPLES_PER_FRAME &&
           ring->slot_count == E1_AUDIO_RING_SLOTS;
}

/* Copy the next chronological producer frame.  The first read deliberately
 * starts three frames behind the newest publication, giving the stock AAC
 * callback a 192 ms jitter cushion.  A successful read acknowledges the frame
 * through consumed_frame so the independent mixer refills only what the native
 * encoder has taken.  If the producer laps the consumer, skip only frames that
 * are no longer retained.  The function makes at most two attempts, never
 * waits, and clears output on a true underrun or concurrent write. */
int e1_audio_ring_read_next(struct e1_audio_ring *ring,
                            uint32_t *last_frame,
                            int16_t output[E1_AUDIO_SAMPLES_PER_FRAME],
                            uint32_t *dropped_frames);

#endif
