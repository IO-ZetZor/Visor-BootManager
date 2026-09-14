/* stub_screensaver.c - stub when the idle screensaver is compiled out */

#include "gui_internal.h"

void ss_draw_frame(gui_state_t *state) {
    (void)state;
}

int ss_input_pending(gui_state_t *state) {
    (void)state;
    return 0;
}

int ss_tick(gui_state_t *state) {
    (void)state;
    return 0;
}

void ss_transition_to_menu(gui_state_t *state, int was_blank) {
    (void)state; (void)was_blank;
}

void ss_transition_to_saver(gui_state_t *state) {
    (void)state;
}

int ss_wake(gui_state_t *state) {
    (void)state;
    return 0;
}
