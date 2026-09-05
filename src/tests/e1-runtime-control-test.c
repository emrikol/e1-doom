#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "e1-runtime-control.h"

static unsigned int tap(struct e1_runtime_control *state, int32_t command,
                        uint64_t *now)
{
    unsigned int action;

    action = e1_runtime_apply_ptz(state, command, *now);
    assert(action == E1_RUNTIME_ACTION_NONE);
    *now += 100;
    action = e1_runtime_apply_ptz(state, 0, *now);
    *now += 100;
    return action;
}

int main(void)
{
    const int32_t secret[] = {3, 3, 4, 4, 1, 2, 1, 2};
    struct e1_runtime_control state = {
        .mode = E1_RUNTIME_CAMERA_ARMED,
    };
    uint64_t now = 1000;
    unsigned int index;

    /* Dashboard lifecycle requests use the same guarded transition states. */
    {
        struct e1_runtime_control dashboard = {
            .mode = E1_RUNTIME_CAMERA_ARMED,
        };

        assert(e1_runtime_request_exit(&dashboard) == E1_RUNTIME_ACTION_NONE);
        assert(e1_runtime_request_enter(&dashboard) == E1_RUNTIME_ACTION_ENTER);
        assert(dashboard.mode == E1_RUNTIME_ENTRY_MELT);
        assert(e1_runtime_request_enter(&dashboard) == E1_RUNTIME_ACTION_NONE);
        assert(e1_runtime_entry_complete(&dashboard) == 0);
        assert(e1_runtime_request_exit(&dashboard) == E1_RUNTIME_ACTION_EXIT);
        assert(dashboard.mode == E1_RUNTIME_EXIT_MELT);
        assert(e1_runtime_request_exit(&dashboard) == E1_RUNTIME_ACTION_NONE);
        assert(e1_runtime_exit_complete(&dashboard) == 0);
    }

    /* Repeats during one held press are not extra secret-code taps. */
    assert(e1_runtime_apply_ptz(&state, 3, now) == E1_RUNTIME_ACTION_NONE);
    assert(e1_runtime_apply_ptz(&state, 3, now + 20) == E1_RUNTIME_ACTION_NONE);
    assert(state.secret_index == 1);
    assert(e1_runtime_apply_ptz(&state, 0, now + 100) ==
           E1_RUNTIME_ACTION_NONE);
    e1_runtime_reset_secret(&state);
    state.camera_control_down = 0;

    for (index = 0; index < sizeof(secret) / sizeof(secret[0]); ++index) {
        unsigned int action = tap(&state, secret[index], &now);
        if (index + 1U < sizeof(secret) / sizeof(secret[0])) {
            assert(action == E1_RUNTIME_ACTION_NONE);
            assert(state.mode == E1_RUNTIME_CAMERA_ARMED);
        } else {
            /* The final stop passes through the camera before entry starts. */
            assert(action == E1_RUNTIME_ACTION_ENTER);
            assert(state.mode == E1_RUNTIME_ENTRY_MELT);
        }
    }
    assert(e1_runtime_entry_complete(&state) == 0);
    assert(state.mode == E1_RUNTIME_DOOM);

    assert(e1_runtime_apply_ptz(&state, 12, now) ==
           E1_RUNTIME_ACTION_FORWARD);
    assert(e1_runtime_apply_ptz(&state, 0, now + 100) ==
           E1_RUNTIME_ACTION_FORWARD);
    assert(e1_runtime_apply_ptz(&state, 4, now + 200) ==
           E1_RUNTIME_ACTION_FORWARD);
    assert(e1_runtime_tick(&state, now + 3199) == E1_RUNTIME_ACTION_NONE);
    assert(e1_runtime_tick(&state, now + 3200) == E1_RUNTIME_ACTION_EXIT);
    assert(state.mode == E1_RUNTIME_EXIT_MELT);
    assert(e1_runtime_apply_ptz(&state, 0, now + 3300) ==
           E1_RUNTIME_ACTION_FORWARD);
    assert(!state.exit_down);
    assert(e1_runtime_exit_complete(&state) == 0);
    assert(state.mode == E1_RUNTIME_CAMERA_ARMED);

    /* A stop event that crosses the threshold cannot race the worker tick. */
    state.mode = E1_RUNTIME_DOOM;
    assert(e1_runtime_apply_ptz(&state, 4, UINT64_C(10000)) ==
           E1_RUNTIME_ACTION_FORWARD);
    assert(e1_runtime_apply_ptz(&state, 0, UINT64_C(13001)) ==
           (E1_RUNTIME_ACTION_FORWARD | E1_RUNTIME_ACTION_EXIT));
    assert(state.mode == E1_RUNTIME_EXIT_MELT);
    assert(!state.exit_down);
    assert(e1_runtime_exit_complete(&state) == 0);

    state.mode = E1_RUNTIME_DOOM;
    assert(e1_runtime_apply_ptz(&state, 4, UINT64_C(20000)) ==
           E1_RUNTIME_ACTION_FORWARD);
    assert(e1_runtime_apply_ptz(&state, 0, UINT64_C(22999)) ==
           E1_RUNTIME_ACTION_FORWARD);
    assert(state.mode == E1_RUNTIME_DOOM);
    state.mode = E1_RUNTIME_CAMERA_ARMED;

    /* Wrong input and the per-tap deadline reset the matcher. */
    now = 20000;
    assert(tap(&state, 3, &now) == E1_RUNTIME_ACTION_NONE);
    assert(tap(&state, 4, &now) == E1_RUNTIME_ACTION_NONE);
    assert(state.secret_index == 0);
    assert(tap(&state, 3, &now) == E1_RUNTIME_ACTION_NONE);
    now += E1_SECRET_GAP_MS + 1;
    assert(e1_runtime_tick(&state, now) == E1_RUNTIME_ACTION_NONE);
    assert(state.secret_index == 0);

    /* Exact timestamps from the first pipelined camera integration trace. */
    {
        static const struct {
            int32_t command;
            uint64_t time_ms;
        } trace[] = {
            {3, 7882920}, {0, 7883252}, {3, 7883256}, {0, 7883258},
            {4, 7883258}, {0, 7883362}, {4, 7883362}, {0, 7883472},
            {1, 7883472}, {0, 7883582}, {2, 7883582}, {0, 7883910},
            {1, 7883910}, {0, 7884131}, {2, 7884131}, {0, 7884131},
        };
        struct e1_runtime_control traced = {
            .mode = E1_RUNTIME_CAMERA_ARMED,
        };
        unsigned int action = 0;

        for (index = 0; index < sizeof(trace) / sizeof(trace[0]); ++index) {
            action |= e1_runtime_apply_ptz(&traced, trace[index].command,
                                           trace[index].time_ms);
        }
        assert(action == E1_RUNTIME_ACTION_ENTER);
        assert(traced.mode == E1_RUNTIME_ENTRY_MELT);
    }

    /* The iPhone emits one directionless stop for each short tap.  Eight
     * stop-only taps activate the toy, while real direction+stop pairs never
     * leak into that fallback counter. */
    {
        struct e1_runtime_control stop_only = {
            .mode = E1_RUNTIME_CAMERA_ARMED,
        };

        for (index = 0; index < E1_STOP_ONLY_SECRET_TAPS; ++index) {
            unsigned int action = e1_runtime_observe_raw_ptz(
                &stop_only, 0, UINT64_C(30000) + index * 400U);
            assert(action == (index + 1U == E1_STOP_ONLY_SECRET_TAPS
                                  ? E1_RUNTIME_ACTION_ENTER
                                  : E1_RUNTIME_ACTION_NONE));
        }
        assert(stop_only.mode == E1_RUNTIME_ENTRY_MELT);

        stop_only.mode = E1_RUNTIME_CAMERA_ARMED;
        assert(e1_runtime_observe_raw_ptz(&stop_only, 3, UINT64_C(40000)) ==
               E1_RUNTIME_ACTION_NONE);
        assert(e1_runtime_observe_raw_ptz(&stop_only, 0, UINT64_C(40100)) ==
               E1_RUNTIME_ACTION_NONE);
        assert(stop_only.stop_only_index == 0U);
        assert(e1_runtime_observe_raw_ptz(&stop_only, 0, UINT64_C(42000)) ==
               E1_RUNTIME_ACTION_NONE);
        assert(stop_only.stop_only_index == 1U);
        assert(e1_runtime_tick(&stop_only,
                               UINT64_C(42000) + E1_SECRET_GAP_MS + 1U) ==
               E1_RUNTIME_ACTION_NONE);
        assert(stop_only.stop_only_index == 0U);

        stop_only.mode = E1_RUNTIME_DOOM;
        for (index = 0; index < E1_STOP_ONLY_SECRET_TAPS; ++index) {
            unsigned int action = e1_runtime_observe_raw_ptz(
                &stop_only, 0, UINT64_C(50000) + index * 400U);
            assert(action == (index + 1U == E1_STOP_ONLY_SECRET_TAPS
                                  ? E1_RUNTIME_ACTION_EXIT
                                  : E1_RUNTIME_ACTION_NONE));
        }
        assert(stop_only.mode == E1_RUNTIME_EXIT_MELT);
    }

    puts("e1-runtime-control-test-ok");
    return 0;
}
