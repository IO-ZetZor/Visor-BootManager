/* stub_editor.c - stub when the interactive cmdline editor is compiled out */

#include "gui_internal.h"

void draw_editor_overlay(gui_state_t *state) {
    (void)state;
}

void editor_enter(gui_state_t *state) {
    (void)state;
}

int editor_key(gui_state_t *state, EFI_INPUT_KEY *key) {
    (void)state; (void)key;
    return 0;
}

void prompt_enter(gui_state_t *state, CHAR16 *title, CHAR16 *hint) {
    (void)state; (void)title; (void)hint;
}
