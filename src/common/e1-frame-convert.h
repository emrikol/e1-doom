#ifndef E1_FRAME_CONVERT_H
#define E1_FRAME_CONVERT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "e1-frame-expand.h"
#include "e1-framebuffer.h"

#define E1_OSG_WIDTH 640U
#define E1_OSG_HEIGHT 360U

static inline int e1_build_scale_maps(unsigned int source_width,
                                      unsigned int source_height,
                                      uint16_t x_map[E1_OSG_WIDTH],
                                      uint16_t y_map[E1_OSG_HEIGHT])
{
    unsigned int x;
    unsigned int y;

    if (!((source_width == 640U && source_height == 300U) ||
          (source_width == 426U && source_height == 200U))) {
        return -1;
    }
    for (x = 0; x < E1_OSG_WIDTH; ++x) {
        x_map[x] = (uint16_t)((uint64_t)x * source_width / E1_OSG_WIDTH);
    }
    for (y = 0; y < E1_OSG_HEIGHT; ++y) {
        y_map[y] = (uint16_t)((uint64_t)y * source_height / E1_OSG_HEIGHT);
    }
    return 0;
}

static inline uint16_t e1_palette_to_argb4444(const uint8_t *palette,
                                               uint8_t index)
{
    const uint8_t *rgb = palette + (size_t)index * 3U;
    return (uint16_t)(UINT16_C(0xf000) |
                      ((uint16_t)(rgb[0] >> 4U) << 8U) |
                      ((uint16_t)(rgb[1] >> 4U) << 4U) |
                      (uint16_t)(rgb[2] >> 4U));
}

static inline int e1_convert_indexed_argb4444(
    uint16_t output[E1_OSG_WIDTH * E1_OSG_HEIGHT],
    const uint8_t *pixels, unsigned int width, unsigned int height,
    unsigned int pitch, const uint8_t palette[E1_FRAME_PALETTE_BYTES],
    const uint16_t x_map[E1_OSG_WIDTH],
    const uint16_t y_map[E1_OSG_HEIGHT])
{
    uint32_t converted_palette[256];
    unsigned int palette_index;
    unsigned int y;

    if (output == NULL || pixels == NULL || palette == NULL ||
        x_map == NULL || y_map == NULL || pitch < width ||
        !((width == 640U && height == 300U) ||
          (width == 426U && height == 200U))) {
        return -1;
    }
    for (palette_index = 0; palette_index < 256U; ++palette_index) {
        converted_palette[palette_index] =
            e1_palette_to_argb4444(palette, (uint8_t)palette_index);
    }
    for (y = 0; y < E1_OSG_HEIGHT; ++y) {
        const uint8_t *source = pixels + (size_t)y_map[y] * pitch;
        uint16_t *destination = output + (size_t)y * E1_OSG_WIDTH;

        if (y > 0U && y_map[y] == y_map[y - 1U]) {
            memcpy(destination, destination - E1_OSG_WIDTH,
                   E1_OSG_WIDTH * sizeof(*destination));
            continue;
        }
        if (width == E1_OSG_WIDTH) {
            e1_expand_i8_argb4444_arm(destination, source,
                                      converted_palette, E1_OSG_WIDTH);
            continue;
        }
        e1_scale_i8_argb4444_arm(destination, source, x_map,
                                 converted_palette, E1_OSG_WIDTH);
    }
    return 0;
}

#endif
