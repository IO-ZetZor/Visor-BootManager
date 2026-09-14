/* stub_pointer.c - stub when mouse support is compiled out */

#include "gui_internal.h"

void cursor_backing_restore(gui_state_t *state, INTN ox, INTN oy) {
    (void)state; (void)ox; (void)oy;
}

void cursor_move(gui_state_t *state) {
    (void)state;
}

void cursor_overlay(gui_state_t *state) {
    (void)state;
}

int poll_pointer(gui_state_t *state, int *menu_redraw) {
    (void)state; (void)menu_redraw;
    return 0;
}
