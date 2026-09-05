#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "e1-frame-expand.h"

#define PIXELS 640U
#define SOURCE_PIXELS 426U

static uint8_t source[PIXELS] __attribute__((aligned(16)));
static uint16_t expected[PIXELS] __attribute__((aligned(16)));
static uint16_t actual[PIXELS] __attribute__((aligned(16)));
static uint16_t x_map[PIXELS] __attribute__((aligned(16)));
static uint32_t palette[256] __attribute__((aligned(16)));

int main(void)
{
    uint32_t random = UINT32_C(0x12345678);
    unsigned int iteration;
    unsigned int index;

    for (index = 0; index < PIXELS; ++index) {
        x_map[index] = (uint16_t)((uint64_t)index * SOURCE_PIXELS / PIXELS);
    }

    for (iteration = 0; iteration < 1000U; ++iteration) {
        for (index = 0; index < 256U; ++index) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            palette[index] = random & UINT32_C(0xffff);
        }
        for (index = 0; index < PIXELS; ++index) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            source[index] = (uint8_t)(random >> 24U);
            expected[index] = (uint16_t)palette[source[index]];
            actual[index] = UINT16_C(0xa55a);
        }

        e1_expand_i8_argb4444_arm(actual, source, palette, PIXELS);
        for (index = 0; index < PIXELS; ++index) {
            assert(actual[index] == expected[index]);
        }

        for (index = 0; index < PIXELS; ++index) {
            expected[index] = (uint16_t)palette[source[x_map[index]]];
            actual[index] = UINT16_C(0xa55a);
        }
        e1_scale_i8_argb4444_arm(actual, source, x_map, palette, PIXELS);
        for (index = 0; index < PIXELS; ++index) {
            assert(actual[index] == expected[index]);
        }
    }

    puts("e1-frame-expand-arm-test-ok iterations=1000 pixels=640 scale=426:640");
    return 0;
}
