#ifndef E1_MELT_H
#define E1_MELT_H

#include <stddef.h>
#include <stdint.h>

#define E1_MELT_COLUMNS 320U
#define E1_MELT_LOGICAL_ROWS 200
#define E1_MELT_TRANSPARENT UINT16_C(0x0000)
#define E1_MELT_RATE_HZ UINT64_C(35)
#define E1_MELT_MAX_TICKS_PER_UPDATE 200U

struct e1_melt_state {
    int16_t row[E1_MELT_COLUMNS];
};

static inline unsigned int e1_melt_clock_ticks(uint64_t *last_ms,
                                                uint64_t *remainder,
                                                uint64_t now_ms)
{
    uint64_t elapsed;
    uint64_t scaled;
    unsigned int ticks;

    if (*last_ms == 0U) {
        *last_ms = now_ms;
        return 0;
    }
    if (now_ms <= *last_ms) {
        return 0;
    }
    elapsed = now_ms - *last_ms;
    *last_ms = now_ms;
    if (elapsed > (UINT64_C(1000) * E1_MELT_MAX_TICKS_PER_UPDATE) /
                      E1_MELT_RATE_HZ) {
        *remainder = 0;
        return E1_MELT_MAX_TICKS_PER_UPDATE;
    }
    scaled = elapsed * E1_MELT_RATE_HZ + *remainder;
    ticks = (unsigned int)(scaled / UINT64_C(1000));
    *remainder = scaled % UINT64_C(1000);
    return ticks;
}

static inline uint32_t e1_melt_random(uint32_t *seed)
{
    *seed = *seed * UINT32_C(1664525) + UINT32_C(1013904223);
    return *seed >> 24U;
}

static inline void e1_melt_init(struct e1_melt_state *state, uint32_t seed)
{
    unsigned int column;

    state->row[0] = (int16_t)-(int)(e1_melt_random(&seed) % 16U);
    for (column = 1; column < E1_MELT_COLUMNS; ++column) {
        int row = state->row[column - 1] +
                  (int)(e1_melt_random(&seed) % 3U) - 1;
        if (row > 0) {
            row = 0;
        } else if (row == -16) {
            row = -15;
        }
        state->row[column] = (int16_t)row;
    }
}

static inline int e1_melt_advance(struct e1_melt_state *state,
                                  unsigned int ticks)
{
    unsigned int tick;
    unsigned int column;

    for (tick = 0; tick < ticks; ++tick) {
        for (column = 0; column < E1_MELT_COLUMNS; ++column) {
            int row = state->row[column];

            if (row < 0) {
                ++row;
            } else if (row < E1_MELT_LOGICAL_ROWS) {
                int delta = row < 16 ? row + 1 : 8;
                row += delta;
                if (row > E1_MELT_LOGICAL_ROWS) {
                    row = E1_MELT_LOGICAL_ROWS;
                }
            }
            state->row[column] = (int16_t)row;
        }
    }
    for (column = 0; column < E1_MELT_COLUMNS; ++column) {
        if (state->row[column] < E1_MELT_LOGICAL_ROWS) {
            return 0;
        }
    }
    return 1;
}

static inline unsigned int e1_melt_scaled_row(int row, unsigned int height)
{
    if (row <= 0) {
        return 0;
    }
    if (row >= E1_MELT_LOGICAL_ROWS) {
        return height;
    }
    return (unsigned int)((uint64_t)(unsigned int)row * height /
                          E1_MELT_LOGICAL_ROWS);
}

static inline void e1_melt_compose_entry(
    uint16_t *output, const uint16_t *doom,
    unsigned int width, unsigned int height,
    const struct e1_melt_state *state)
{
    unsigned int x;
    unsigned int y;

    for (x = 0; x < width; ++x) {
        unsigned int column = (unsigned int)((uint64_t)x * E1_MELT_COLUMNS /
                                             width);
        unsigned int reveal = e1_melt_scaled_row(state->row[column], height);

        for (y = 0; y < height; ++y) {
            size_t offset = (size_t)y * width + x;
            output[offset] = y < reveal ? doom[offset] : E1_MELT_TRANSPARENT;
        }
    }
}

static inline void e1_melt_compose_exit(
    uint16_t *output, const uint16_t *snapshot,
    unsigned int width, unsigned int height,
    const struct e1_melt_state *state)
{
    unsigned int x;
    unsigned int y;

    for (x = 0; x < width; ++x) {
        unsigned int column = (unsigned int)((uint64_t)x * E1_MELT_COLUMNS /
                                             width);
        unsigned int drop = e1_melt_scaled_row(state->row[column], height);

        for (y = 0; y < height; ++y) {
            size_t output_offset = (size_t)y * width + x;
            output[output_offset] = y < drop ? E1_MELT_TRANSPARENT :
                snapshot[(size_t)(y - drop) * width + x];
        }
    }
}

#endif
