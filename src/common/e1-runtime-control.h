#ifndef E1_RUNTIME_CONTROL_H
#define E1_RUNTIME_CONTROL_H

#include <stdint.h>

#define E1_SECRET_GAP_MS UINT64_C(1500)
#define E1_SECRET_TOTAL_MS UINT64_C(10000)
#define E1_STOP_ONLY_SECRET_TAPS 8U
#define E1_EXIT_HOLD_MS UINT64_C(3000)

enum e1_runtime_mode {
    E1_RUNTIME_CAMERA_ARMED = 0,
    E1_RUNTIME_ENTRY_MELT = 1,
    E1_RUNTIME_DOOM = 2,
    E1_RUNTIME_EXIT_MELT = 3,
};

enum e1_runtime_action {
    E1_RUNTIME_ACTION_NONE = 0,
    E1_RUNTIME_ACTION_ENTER = 1U << 0,
    E1_RUNTIME_ACTION_EXIT = 1U << 1,
    E1_RUNTIME_ACTION_FORWARD = 1U << 2,
};

struct e1_runtime_control {
    enum e1_runtime_mode mode;
    unsigned int secret_index;
    uint64_t secret_first_ms;
    uint64_t secret_last_ms;
    unsigned int stop_only_index;
    uint64_t stop_only_first_ms;
    uint64_t stop_only_last_ms;
    uint64_t exit_down_first_ms;
    int camera_control_down;
    int raw_direction_since_stop;
    int secret_complete;
    int exit_down;
};

static inline void e1_runtime_reset_secret(struct e1_runtime_control *state)
{
    state->secret_index = 0;
    state->secret_first_ms = 0;
    state->secret_last_ms = 0;
    state->secret_complete = 0;
}

static inline void e1_runtime_reset_stop_only(
    struct e1_runtime_control *state)
{
    state->stop_only_index = 0;
    state->stop_only_first_ms = 0;
    state->stop_only_last_ms = 0;
}

static inline unsigned int e1_runtime_request_enter(
    struct e1_runtime_control *state)
{
    if (state->mode != E1_RUNTIME_CAMERA_ARMED) {
        return E1_RUNTIME_ACTION_NONE;
    }
    state->mode = E1_RUNTIME_ENTRY_MELT;
    state->camera_control_down = 0;
    state->raw_direction_since_stop = 0;
    e1_runtime_reset_secret(state);
    e1_runtime_reset_stop_only(state);
    return E1_RUNTIME_ACTION_ENTER;
}

static inline unsigned int e1_runtime_request_exit(
    struct e1_runtime_control *state);

/* The current iPhone UI emits only a stop command for a short tap; direction
 * is not present anywhere at the camera.  Preserve the exact directional
 * matcher when a direction is observed, but let eight genuinely stop-only
 * taps toggle the toy into or out of Doom. */
static inline unsigned int e1_runtime_observe_raw_ptz(
    struct e1_runtime_control *state, int32_t command, uint64_t now_ms)
{
    if (state->mode != E1_RUNTIME_CAMERA_ARMED &&
        state->mode != E1_RUNTIME_DOOM) {
        state->raw_direction_since_stop = 0;
        e1_runtime_reset_stop_only(state);
        return E1_RUNTIME_ACTION_NONE;
    }
    if (state->stop_only_index != 0U &&
        (now_ms - state->stop_only_last_ms > E1_SECRET_GAP_MS ||
         now_ms - state->stop_only_first_ms > E1_SECRET_TOTAL_MS)) {
        e1_runtime_reset_stop_only(state);
    }
    if (command != 0) {
        state->raw_direction_since_stop = 1;
        e1_runtime_reset_stop_only(state);
        return E1_RUNTIME_ACTION_NONE;
    }
    if (state->raw_direction_since_stop) {
        state->raw_direction_since_stop = 0;
        e1_runtime_reset_stop_only(state);
        return E1_RUNTIME_ACTION_NONE;
    }
    if (state->stop_only_index == 0U) {
        state->stop_only_first_ms = now_ms;
    }
    state->stop_only_last_ms = now_ms;
    ++state->stop_only_index;
    if (state->stop_only_index == E1_STOP_ONLY_SECRET_TAPS) {
        return state->mode == E1_RUNTIME_CAMERA_ARMED
                   ? e1_runtime_request_enter(state)
                   : e1_runtime_request_exit(state);
    }
    return E1_RUNTIME_ACTION_NONE;
}

static inline unsigned int e1_runtime_request_exit(
    struct e1_runtime_control *state)
{
    if (state->mode != E1_RUNTIME_DOOM) {
        return E1_RUNTIME_ACTION_NONE;
    }
    state->mode = E1_RUNTIME_EXIT_MELT;
    state->camera_control_down = 0;
    state->raw_direction_since_stop = 0;
    state->exit_down = 0;
    state->exit_down_first_ms = 0;
    e1_runtime_reset_stop_only(state);
    return E1_RUNTIME_ACTION_EXIT;
}

static inline unsigned int e1_runtime_camera_command(
    struct e1_runtime_control *state, int32_t command, uint64_t now_ms)
{
    static const int32_t secret[] = {3, 3, 4, 4, 1, 2, 1, 2};

    if (state->secret_index != 0U &&
        (now_ms - state->secret_last_ms > E1_SECRET_GAP_MS ||
         now_ms - state->secret_first_ms > E1_SECRET_TOTAL_MS)) {
        e1_runtime_reset_secret(state);
    }
    if (command == 0) {
        state->camera_control_down = 0;
        if (state->secret_complete) {
            state->mode = E1_RUNTIME_ENTRY_MELT;
            e1_runtime_reset_secret(state);
            return E1_RUNTIME_ACTION_ENTER;
        }
        return E1_RUNTIME_ACTION_NONE;
    }
    if (state->camera_control_down) {
        return E1_RUNTIME_ACTION_NONE;
    }
    state->camera_control_down = 1;

    if (state->secret_index < sizeof(secret) / sizeof(secret[0]) &&
        command == secret[state->secret_index]) {
        if (state->secret_index == 0U) {
            state->secret_first_ms = now_ms;
        }
        state->secret_last_ms = now_ms;
        ++state->secret_index;
        if (state->secret_index == sizeof(secret) / sizeof(secret[0])) {
            state->secret_complete = 1;
        }
    } else {
        e1_runtime_reset_secret(state);
        if (command == secret[0]) {
            state->secret_index = 1;
            state->secret_first_ms = now_ms;
            state->secret_last_ms = now_ms;
        }
    }
    return E1_RUNTIME_ACTION_NONE;
}

static inline unsigned int e1_runtime_apply_ptz(
    struct e1_runtime_control *state, int32_t command, uint64_t now_ms)
{
    if (state->mode == E1_RUNTIME_CAMERA_ARMED) {
        return e1_runtime_camera_command(state, command, now_ms);
    }
    if (state->mode == E1_RUNTIME_EXIT_MELT) {
        if (state->exit_down && command == 0) {
            state->exit_down = 0;
            state->exit_down_first_ms = 0;
            return E1_RUNTIME_ACTION_FORWARD;
        }
        return E1_RUNTIME_ACTION_NONE;
    }
    if (state->mode != E1_RUNTIME_DOOM) {
        return E1_RUNTIME_ACTION_NONE;
    }

    if (command == 4) {
        if (!state->exit_down) {
            state->exit_down = 1;
            state->exit_down_first_ms = now_ms;
        }
    } else if (command == 0 && state->exit_down) {
        int qualified = now_ms >= state->exit_down_first_ms &&
                        now_ms - state->exit_down_first_ms >= E1_EXIT_HOLD_MS;

        state->exit_down = 0;
        state->exit_down_first_ms = 0;
        if (qualified) {
            state->mode = E1_RUNTIME_EXIT_MELT;
            return E1_RUNTIME_ACTION_FORWARD | E1_RUNTIME_ACTION_EXIT;
        }
    } else if (state->exit_down) {
        state->exit_down = 0;
        state->exit_down_first_ms = 0;
    }
    return E1_RUNTIME_ACTION_FORWARD;
}

static inline unsigned int e1_runtime_tick(struct e1_runtime_control *state,
                                            uint64_t now_ms)
{
    if (state->mode == E1_RUNTIME_CAMERA_ARMED &&
        state->secret_index != 0U &&
        (now_ms - state->secret_last_ms > E1_SECRET_GAP_MS ||
         now_ms - state->secret_first_ms > E1_SECRET_TOTAL_MS)) {
        e1_runtime_reset_secret(state);
    }
    if ((state->mode == E1_RUNTIME_CAMERA_ARMED ||
         state->mode == E1_RUNTIME_DOOM) &&
        state->stop_only_index != 0U &&
        (now_ms - state->stop_only_last_ms > E1_SECRET_GAP_MS ||
         now_ms - state->stop_only_first_ms > E1_SECRET_TOTAL_MS)) {
        e1_runtime_reset_stop_only(state);
    }
    if (state->mode == E1_RUNTIME_DOOM && state->exit_down &&
        now_ms >= state->exit_down_first_ms &&
        now_ms - state->exit_down_first_ms >= E1_EXIT_HOLD_MS) {
        state->mode = E1_RUNTIME_EXIT_MELT;
        return E1_RUNTIME_ACTION_EXIT;
    }
    return E1_RUNTIME_ACTION_NONE;
}

static inline int e1_runtime_entry_complete(struct e1_runtime_control *state)
{
    if (state->mode != E1_RUNTIME_ENTRY_MELT) {
        return -1;
    }
    state->mode = E1_RUNTIME_DOOM;
    return 0;
}

static inline int e1_runtime_exit_complete(struct e1_runtime_control *state)
{
    if (state->mode != E1_RUNTIME_EXIT_MELT) {
        return -1;
    }
    state->mode = E1_RUNTIME_CAMERA_ARMED;
    state->camera_control_down = 0;
    state->raw_direction_since_stop = 0;
    state->exit_down = 0;
    state->exit_down_first_ms = 0;
    e1_runtime_reset_secret(state);
    e1_runtime_reset_stop_only(state);
    return 0;
}

#endif
