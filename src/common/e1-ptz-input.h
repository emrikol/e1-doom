#ifndef E1_PTZ_INPUT_H
#define E1_PTZ_INPUT_H

#include <stdint.h>

#define E1_PTZ_INPUT_TIMEOUT_MS UINT64_C(350)
#define E1_PTZ_ACTION_DOUBLE_TAP_MS UINT64_C(700)

enum e1_ptz_key_mask {
    E1_PTZ_KEY_UP = 1U << 0,
    E1_PTZ_KEY_DOWN = 1U << 1,
    E1_PTZ_KEY_LEFT = 1U << 2,
    E1_PTZ_KEY_RIGHT = 1U << 3,
    E1_PTZ_KEY_FIRE = 1U << 4,
    E1_PTZ_KEY_USE = 1U << 5,
    E1_PTZ_KEY_ENTER = 1U << 6,
    E1_PTZ_KEY_ESCAPE = 1U << 7,
};

#define E1_PTZ_DIRECTION_KEYS \
    (E1_PTZ_KEY_UP | E1_PTZ_KEY_DOWN | E1_PTZ_KEY_LEFT | E1_PTZ_KEY_RIGHT)
#define E1_PTZ_ACTION_KEYS \
    (E1_PTZ_KEY_FIRE | E1_PTZ_KEY_USE | E1_PTZ_KEY_ENTER)

struct e1_ptz_input_state {
    uint32_t held;
    uint64_t deadline_ms;
    uint64_t last_up_release_ms;
};

struct e1_ptz_input_delta {
    uint32_t press;
    uint32_t release;
};

static inline struct e1_ptz_input_delta e1_ptz_set_held(
    struct e1_ptz_input_state *state, uint32_t next, uint64_t now_ms)
{
    struct e1_ptz_input_delta delta;

    delta.press = next & ~state->held;
    delta.release = state->held & ~next;
    state->held = next;
    state->deadline_ms = next != 0U ? now_ms + E1_PTZ_INPUT_TIMEOUT_MS : 0U;
    return delta;
}

static inline struct e1_ptz_input_delta e1_ptz_apply_command(
    struct e1_ptz_input_state *state, int32_t command, uint64_t now_ms)
{
    uint32_t next = state->held;

    switch (command) {
    case 0: /* PTZ stop/release */
        if ((state->held & E1_PTZ_KEY_UP) != 0U &&
            (state->held & E1_PTZ_ACTION_KEYS) == 0U) {
            state->last_up_release_ms = now_ms;
        } else if ((state->held & E1_PTZ_ACTION_KEYS) != 0U) {
            state->last_up_release_ms = 0;
        }
        next = 0;
        break;
    case 1: /* left */
        next = (next & ~E1_PTZ_DIRECTION_KEYS) | E1_PTZ_KEY_LEFT;
        break;
    case 2: /* right */
        state->last_up_release_ms = 0;
        next = (next & ~E1_PTZ_DIRECTION_KEYS) | E1_PTZ_KEY_RIGHT;
        break;
    case 3: /* up */
        next = (next & ~E1_PTZ_DIRECTION_KEYS) | E1_PTZ_KEY_UP;
        if ((state->held & E1_PTZ_KEY_UP) == 0U &&
            state->last_up_release_ms != 0U &&
            now_ms >= state->last_up_release_ms &&
            now_ms - state->last_up_release_ms <=
                E1_PTZ_ACTION_DOUBLE_TAP_MS) {
            next |= E1_PTZ_ACTION_KEYS;
            state->last_up_release_ms = 0;
        }
        break;
    case 4: /* down */
        state->last_up_release_ms = 0;
        next = (next & ~E1_PTZ_DIRECTION_KEYS) | E1_PTZ_KEY_DOWN;
        break;
    case 11: /* zoom decrement: optional Escape/back */
        next |= E1_PTZ_KEY_ESCAPE;
        break;
    case 12: /* zoom increment: Fire+Use, Enter in menus */
        next |= E1_PTZ_ACTION_KEYS;
        break;
    default:
        return (struct e1_ptz_input_delta){0, 0};
    }
    if (command == 1) {
        state->last_up_release_ms = 0;
    }
    return e1_ptz_set_held(state, next, now_ms);
}

static inline struct e1_ptz_input_delta e1_ptz_expire(
    struct e1_ptz_input_state *state, uint64_t now_ms)
{
    if (state->held != 0U && now_ms >= state->deadline_ms) {
        return e1_ptz_set_held(state, 0, now_ms);
    }
    return (struct e1_ptz_input_delta){0, 0};
}

#endif
