#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "e1-melt.h"

#define WIDTH 640U
#define HEIGHT 360U
#define PIXELS ((size_t)WIDTH * HEIGHT)

int main(void)
{
    struct e1_melt_state melt;
    uint16_t *doom = malloc(PIXELS * sizeof(*doom));
    uint16_t *snapshot = malloc(PIXELS * sizeof(*snapshot));
    uint16_t *output = malloc(PIXELS * sizeof(*output));
    unsigned int column;
    unsigned int ticks = 0;
    size_t index;
    uint64_t clock_last = UINT64_C(5000);
    uint64_t clock_remainder = UINT64_C(17);

    assert(doom != NULL && snapshot != NULL && output != NULL);
    assert(e1_melt_clock_ticks(&clock_last, &clock_remainder,
                               UINT64_C(4999)) == 0U);
    assert(clock_last == UINT64_C(5000));
    assert(clock_remainder == UINT64_C(17));
    assert(e1_melt_clock_ticks(&clock_last, &clock_remainder,
                               UINT64_C(5000)) == 0U);
    assert(clock_last == UINT64_C(5000));
    assert(clock_remainder == UINT64_C(17));
    assert(e1_melt_clock_ticks(&clock_last, &clock_remainder,
                               UINT64_C(5100)) == 3U);
    assert(clock_last == UINT64_C(5100));
    assert(clock_remainder == UINT64_C(517));
    assert(e1_melt_clock_ticks(&clock_last, &clock_remainder,
                               UINT64_C(20000)) ==
           E1_MELT_MAX_TICKS_PER_UPDATE);
    assert(clock_last == UINT64_C(20000));
    assert(clock_remainder == 0U);
    for (index = 0; index < PIXELS; ++index) {
        doom[index] = (uint16_t)(UINT16_C(0xf000) | (index & 0x07ffU));
        snapshot[index] =
            (uint16_t)(UINT16_C(0xf800) | (index & 0x07ffU));
    }

    e1_melt_init(&melt, UINT32_C(0x4531444d));
    for (column = 0; column < E1_MELT_COLUMNS; ++column) {
        assert(melt.row[column] >= -15 && melt.row[column] <= 0);
        if (column != 0) {
            int difference = melt.row[column] - melt.row[column - 1];
            assert(difference >= -1 && difference <= 1);
        }
    }
    e1_melt_compose_entry(output, doom, snapshot, WIDTH, HEIGHT, &melt);
    for (index = 0; index < PIXELS; ++index) {
        assert(output[index] == snapshot[index]);
    }

    for (column = 0; column < E1_MELT_COLUMNS; ++column) {
        melt.row[column] = 100;
    }
    e1_melt_compose_entry(output, doom, snapshot, WIDTH, HEIGHT, &melt);
    assert(output[179U * WIDTH] == doom[179U * WIDTH]);
    assert(output[180U * WIDTH] == snapshot[0]);
    assert(output[359U * WIDTH] == snapshot[179U * WIDTH]);
    e1_melt_compose_exit(output, doom, WIDTH, HEIGHT, &melt);
    assert(output[179U * WIDTH] == E1_MELT_TRANSPARENT);
    assert(output[180U * WIDTH] == doom[0]);
    assert(output[359U * WIDTH] == doom[179U * WIDTH]);

    e1_melt_init(&melt, UINT32_C(0x4531444d));
    while (!e1_melt_advance(&melt, 1)) {
        assert(++ticks < 100U);
    }
    e1_melt_compose_entry(output, doom, snapshot, WIDTH, HEIGHT, &melt);
    for (index = 0; index < PIXELS; ++index) {
        assert(output[index] == doom[index]);
    }
    e1_melt_compose_exit(output, doom, WIDTH, HEIGHT, &melt);
    for (index = 0; index < PIXELS; ++index) {
        assert(output[index] == E1_MELT_TRANSPARENT);
    }

    free(output);
    free(snapshot);
    free(doom);
    puts("e1-melt-test-ok");
    return 0;
}
