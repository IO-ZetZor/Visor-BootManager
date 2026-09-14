/* stub_gui.c - stub when the framebuffer menu is compiled out */

#include "gui.h"
#include "gui_internal.h"

EFI_STATUS gui_init(gui_state_t *state) {
    (void)state;
    return EFI_UNSUPPORTED;
}

icon_t * gui_load_icon(CHAR16 *path) {
    (void)path;
    return NULL;
}

EFI_STATUS gui_prompt_password(gui_state_t *state, CHAR16 *title, CHAR16 *hint, CHAR16 **out) {
    (void)state; (void)title; (void)hint; (void)out;
    return EFI_UNSUPPORTED;
}

boot_entry_t * gui_run(gui_state_t *state) {
    (void)state;
    return NULL;
}

void gui_set_background(gui_state_t *state, CHAR16 *path) {
    (void)state; (void)path;
}

void gui_set_font(const char *name) {
    (void)name;
}

void gui_set_logo(gui_state_t *state, CHAR16 *path) {
    (void)state; (void)path;
}

EFI_STATUS gui_set_mode(gui_state_t *state, UINTN want_w, UINTN want_h, int want_max) {
    (void)state; (void)want_w; (void)want_h; (void)want_max;
    return EFI_UNSUPPORTED;
}

void gui_shutdown(gui_state_t *state) {
    (void)state;
}
