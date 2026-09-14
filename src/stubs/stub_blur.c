/* stub_blur.c - stub when the blur feature is compiled out */

#include "gui_internal.h"

void blur_free(gui_state_t *state) {
    (void)state;
}

void build_blur_cache(gui_state_t *state) {
    (void)state;
}

void draw_frost(gui_state_t *state, INTN x, INTN y, INTN w, INTN h, INTN a) {
    (void)state; (void)x; (void)y; (void)w; (void)h; (void)a;
}
