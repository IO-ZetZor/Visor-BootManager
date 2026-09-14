/* stub_fade.c - stub when crossfades are compiled out */

#include "gui_internal.h"

UINTN ease_alpha(INTN frame, INTN frames) {
    (void)frame; (void)frames;
    return 255;
}

INTN ease_permille(INTN frame, INTN frames) {
    (void)frame; (void)frames;
    return 1000;
}

int gui_animation_on(gui_state_t *state) {
    (void)state;
    return 0;
}

void gui_crossfade(gui_state_t *state, UINT32 *from, UINT32 *to) {
    (void)state; (void)from; (void)to;
}

void gui_fade_in_current(gui_state_t *state) {
    (void)state;
}

void gui_fade_out(gui_state_t *state) {
    (void)state;
}
