/* stub_clock.c - stub when the on-screen clock is compiled out */

#include "gui_internal.h"

int clock_needs_tick(gui_state_t *state) {
    (void)state;
    return 0;
}

int draw_clock(gui_state_t *state) {
    (void)state;
    return 0;
}

int draw_clock_ex(gui_state_t *state, int frost) {
    (void)state; (void)frost;
    return 0;
}
