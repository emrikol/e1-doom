#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "e1-frame-convert.h"

static void run_profile(unsigned int width, unsigned int height)
{
    uint8_t *pixels = calloc((size_t)width, height);
    uint16_t *output = calloc(E1_OSG_WIDTH * E1_OSG_HEIGHT,
                              sizeof(*output));
    uint8_t palette[E1_FRAME_PALETTE_BYTES] = {0};
    uint16_t x_map[E1_OSG_WIDTH];
    uint16_t y_map[E1_OSG_HEIGHT];

    assert(pixels != NULL);
    assert(output != NULL);
    palette[1 * 3] = 255;
    palette[2 * 3 + 1] = 255;
    palette[3 * 3 + 2] = 255;
    palette[4 * 3] = 0xab;
    palette[4 * 3 + 1] = 0xcd;
    palette[4 * 3 + 2] = 0xef;

    pixels[0] = 1;
    pixels[width - 1] = 2;
    pixels[(size_t)(height - 1) * width] = 3;
    pixels[(size_t)(height - 1) * width + width - 1] = 4;

    assert(e1_build_scale_maps(width, height, x_map, y_map) == 0);
    assert(x_map[0] == 0);
    assert(y_map[0] == 0);
    assert(x_map[E1_OSG_WIDTH - 1] == width - 1);
    assert(y_map[E1_OSG_HEIGHT - 1] == height - 1);
    if (width == 640U) {
        assert(x_map[639] == 639);
        assert(y_map[6] == 5); /* exact 6:5 aspect correction */
    } else {
        assert(x_map[639] == 425);
        assert(y_map[359] == 199);
    }

    assert(e1_convert_indexed_argb4444(output, pixels, width, height, width,
                                       palette, x_map, y_map) == 0);
    assert(output[0] == UINT16_C(0xff00));
    assert(output[E1_OSG_WIDTH - 1] == UINT16_C(0xf0f0));
    assert(output[(E1_OSG_HEIGHT - 1) * E1_OSG_WIDTH] == UINT16_C(0xf00f));
    assert(output[E1_OSG_WIDTH * E1_OSG_HEIGHT - 1] == UINT16_C(0xface));

    free(output);
    free(pixels);
}

int main(void)
{
    uint16_t x_map[E1_OSG_WIDTH];
    uint16_t y_map[E1_OSG_HEIGHT];

    run_profile(426, 200);
    run_profile(640, 300);
    assert(e1_build_scale_maps(320, 200, x_map, y_map) == -1);
    assert(e1_frame_sequence_drops(0, 20) == 0);
    assert(e1_frame_sequence_drops(20, 20) == 0);
    assert(e1_frame_sequence_drops(20, 21) == 0);
    assert(e1_frame_sequence_drops(20, 24) == 3);
    assert(e1_frame_sequence_drops(UINT32_MAX - 1U, 1U) == 2);
    assert(e1_frame_sequence_drops(24, 20) == 0);
    puts("e1-frame-convert-test-ok");
    return 0;
}
