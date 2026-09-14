/* stub_accent.c - stub when accent-colour theming is compiled out */

#include "accent.h"
#include "gui_internal.h"

int accent_role_from_str(const CHAR16 *s) {
    (void)s;
    return -1;
}

int accent_variant_from_str(const CHAR16 *s) {
    (void)s;
    return -1;
}

int entry_own_color(gui_state_t *state, boot_entry_t *e, color_t *out) {
    (void)state; (void)e; (void)out;
    return 0;
}

void gui_apply_accent(gui_state_t *state) {
    (void)state;
}
