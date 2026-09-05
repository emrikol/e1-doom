#ifndef E1_FRAMEBUFFER_H
#define E1_FRAMEBUFFER_H

#include <stdint.h>

#define E1_FRAME_MAGIC UINT32_C(0x4531444d)
#define E1_FRAME_VERSION UINT32_C(1)
#define E1_FRAME_PATH "/mnt/tmp/e1-doom/state/framebuffer.shm"
#define E1_FRAME_MAX_WIDTH 640U
#define E1_FRAME_MAX_HEIGHT 300U
#define E1_FRAME_MAX_PIXELS (E1_FRAME_MAX_WIDTH * E1_FRAME_MAX_HEIGHT)
#define E1_FRAME_PALETTE_BYTES (256U * 3U)
#define E1_STREAM_DEFAULT_FRAME_RATE 10U

struct e1_frame_slot {
    volatile uint32_t sequence;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t palette[E1_FRAME_PALETTE_BYTES];
    uint8_t pixels[E1_FRAME_MAX_PIXELS];
};

struct e1_framebuffer {
    uint32_t magic;
    uint32_t version;
    uint32_t structure_size;
    volatile uint32_t active_slot;
    volatile uint32_t frame_sequence;
    uint32_t reserved[3];
    struct e1_frame_slot slot[2];
};

static inline void e1_frame_barrier(void)
{
    __sync_synchronize();
}

/* Count producer frames that a consumer skipped between two stable sequence
 * observations.  Unsigned subtraction intentionally handles wraparound. */
static inline uint32_t e1_frame_sequence_drops(uint32_t previous,
                                               uint32_t current)
{
    uint32_t delta;

    if (previous == 0U) {
        return 0U;
    }
    delta = current - previous;
    if (delta <= 1U || delta >= UINT32_C(0x80000000)) {
        return 0U;
    }
    return delta - 1U;
}

#endif
