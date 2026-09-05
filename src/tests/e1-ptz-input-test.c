#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "e1-ptz-input.h"

int main(void)
{
    struct e1_ptz_input_state state = {0};
    struct e1_ptz_input_state action_state = {0};
    struct e1_ptz_input_delta delta;

    delta = e1_ptz_apply_command(&state, 3, 1000);
    assert(delta.press == E1_PTZ_KEY_UP && delta.release == 0);
    assert(state.deadline_ms == 1350);

    delta = e1_ptz_apply_command(&state, 3, 1200);
    assert(delta.press == 0 && delta.release == 0);
    assert(state.deadline_ms == 1550);

    delta = e1_ptz_apply_command(&state, 1, 1250);
    assert(delta.press == E1_PTZ_KEY_LEFT);
    assert(delta.release == E1_PTZ_KEY_UP);

    delta = e1_ptz_apply_command(&state, 12, 1300);
    assert(delta.press == E1_PTZ_ACTION_KEYS && delta.release == 0);
    assert(state.held == (E1_PTZ_KEY_LEFT | E1_PTZ_ACTION_KEYS));

    delta = e1_ptz_apply_command(&state, 9, 1400);
    assert(delta.press == 0 && delta.release == 0);
    assert(state.deadline_ms == 1650);

    delta = e1_ptz_expire(&state, 1649);
    assert(delta.press == 0 && delta.release == 0);
    delta = e1_ptz_expire(&state, 1650);
    assert(delta.press == 0);
    assert(delta.release == (E1_PTZ_KEY_LEFT | E1_PTZ_ACTION_KEYS));
    assert(state.held == 0);

    delta = e1_ptz_apply_command(&state, 11, 2000);
    assert(delta.press == E1_PTZ_KEY_ESCAPE && delta.release == 0);
    delta = e1_ptz_apply_command(&state, 0, 2010);
    assert(delta.release == E1_PTZ_KEY_ESCAPE && state.held == 0);

    /* Only a second explicit Up tap becomes Fire+Use+Confirm. */
    delta = e1_ptz_apply_command(&action_state, 3, 3000);
    assert(delta.press == E1_PTZ_KEY_UP);
    delta = e1_ptz_apply_command(&action_state, 0, 3120);
    assert(delta.release == E1_PTZ_KEY_UP);
    delta = e1_ptz_apply_command(&action_state, 3, 3600);
    assert(delta.press == (E1_PTZ_KEY_UP | E1_PTZ_ACTION_KEYS));
    delta = e1_ptz_apply_command(&action_state, 0, 3720);
    assert(delta.release == (E1_PTZ_KEY_UP | E1_PTZ_ACTION_KEYS));

    /* Too slow, or an intervening direction, remains ordinary movement. */
    delta = e1_ptz_apply_command(&action_state, 3, 5000);
    assert(delta.press == E1_PTZ_KEY_UP);
    (void)e1_ptz_apply_command(&action_state, 0, 5100);
    delta = e1_ptz_apply_command(&action_state, 3, 5801);
    assert(delta.press == E1_PTZ_KEY_UP);
    (void)e1_ptz_apply_command(&action_state, 0, 5900);
    delta = e1_ptz_apply_command(&action_state, 1, 6000);
    assert(delta.press == E1_PTZ_KEY_LEFT);
    (void)e1_ptz_apply_command(&action_state, 0, 6100);
    delta = e1_ptz_apply_command(&action_state, 3, 6200);
    assert(delta.press == E1_PTZ_KEY_UP);

    puts("e1-ptz-input-test-ok");
    return 0;
}
