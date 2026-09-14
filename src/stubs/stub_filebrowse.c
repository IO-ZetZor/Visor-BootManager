/* stub_filebrowse.c - stub when the ESP file browser is compiled out */

#include "filebrowse.h"
#include "gui_internal.h"
#include "text_menu_internal.h"

int browse_band_set(gui_state_t *state) {
    (void)state;
    return 0;
}

UINTN browse_rows(gui_state_t *state, UINTN name_px) {
    (void)state; (void)name_px;
    return 0;
}

int browse_run(gui_state_t *state, UINTN rows, UINTN cols, CHAR16 *initial) {
    (void)state; (void)rows; (void)cols; (void)initial;
    return 0;
}

void draw_browse_panel(gui_state_t *state, UINTN name_px) {
    (void)state; (void)name_px;
}

int fb_boot_apply(fb_t *s, struct gui_state *gui) {
    (void)s; (void)gui;
    return 0;
}

fb_entry_t * fb_cursor(fb_t *s) {
    (void)s;
    return 0;
}

int fb_enter(fb_t *s) {
    (void)s;
    return 0;
}

void fb_free(fb_t *s) {
    (void)s;
}

int fb_init(fb_t *s) {
    (void)s;
    return 0;
}

int fb_list(fb_t *s) {
    (void)s;
    return 0;
}

void fb_move(fb_t *s, int delta) {
    (void)s; (void)delta;
}

void fb_switch_volume(fb_t *s, int dir) {
    (void)s; (void)dir;
}

int fb_up(fb_t *s) {
    (void)s;
    return 0;
}

void list_dir(UINTN *row, UINTN rows, UINTN cols, CHAR16 *path) {
    (void)row; (void)rows; (void)cols; (void)path;
}
