/* stub_capture.c - stub when screenshots and recording are compiled out */

#include "capture.h"
#include "gui_internal.h"

void cap_cancel_record(gui_state_t *state) {
    (void)state;
}

void cap_do_screenshot(gui_state_t *state) {
    (void)state;
}

void cap_draw_overlay(gui_state_t *state) {
    (void)state;
}

void cap_finish_record(gui_state_t *state) {
    (void)state;
}

UINTN cap_gif_count(const cap_gif *g) {
    (void)g;
    return 0;
}

void cap_gif_free(cap_gif *g) {
    (void)g;
}

int cap_grab_due_frames(gui_state_t *state) {
    (void)state;
    return 0;
}

int cap_overlay_live(gui_state_t *state) {
    (void)state;
    return 0;
}

void cap_set_toast(gui_state_t *state, const CHAR16 *msg, const CHAR16 *detail, int is_err) {
    (void)state; (void)msg; (void)detail; (void)is_err;
}

void cap_start_record(gui_state_t *state) {
    (void)state;
}

int cap_tick(gui_state_t *state) {
    (void)state;
    return 0;
}
