#ifndef E1_FRAME_EXPAND_H
#define E1_FRAME_EXPAND_H

#include <stddef.h>
#include <stdint.h>

#if defined(__arm__) && !defined(__thumb__)
void e1_expand_i8_argb4444_arm(uint16_t *destination,
                               const uint8_t *source,
                               const uint32_t palette[256],
                               unsigned int count);
void e1_scale_i8_argb4444_arm(uint16_t *destination,
                              const uint8_t *source,
                              const uint16_t *x_map,
                              const uint32_t palette[256],
                              unsigned int count);
#else
static inline void e1_expand_i8_argb4444_arm(
    uint16_t *destination, const uint8_t *source,
    const uint32_t palette[256], unsigned int count)
{
    unsigned int pixel;

    for (pixel = 0; pixel < count; ++pixel) {
        destination[pixel] = (uint16_t)palette[source[pixel]];
    }
}

static inline void e1_scale_i8_argb4444_arm(
    uint16_t *destination, const uint8_t *source, const uint16_t *x_map,
    const uint32_t palette[256], unsigned int count)
{
    unsigned int pixel;

    for (pixel = 0; pixel < count; ++pixel) {
        destination[pixel] = (uint16_t)palette[source[x_map[pixel]]];
    }
}
#endif

#endif
