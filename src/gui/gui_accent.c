/* gui_accent.c - accent colour resolution and theme application */
#include "gui_internal.h"

static int accent_resolve(gui_state_t *state, accent_spec_t *own,
                          accent_spec_t *group, int def_role, color_t *out) {
    accent_spec_t *chain[3];
    int n = 0;
    if (own   && own->mode   != SPEC_UNSET) chain[n++] = own;
    if (group && group->mode != SPEC_UNSET) chain[n++] = group;
    if (state->sp_all.mode   != SPEC_UNSET) chain[n++] = &state->sp_all;

    for (int i = 0; i < n; i++) {
        switch (chain[i]->mode) {
        case SPEC_OFF:
            return 0;
        case SPEC_COLOR:
            *out = chain[i]->color;
            return 1;
        case SPEC_ROLE:
            if (!state->accent_valid) return 0;
            *out = state->accent_roles[chain[i]->role % GUI_ACCENT_ROLES];
            return 1;
        case SPEC_ON:
        default:
            i = n;
            break;
        }
    }

    if (!state->accent_valid) return 0;
    *out = state->accent_roles[def_role % GUI_ACCENT_ROLES];
    return 1;
}

int entry_own_color(gui_state_t *state, boot_entry_t *e, color_t *out) {
    if (!e) return 0;
    if (e->has_color) { *out = e->color; return 1; }
    if (e->color_role >= 0 && state->accent_valid) {
        *out = state->accent_roles[e->color_role % GUI_ACCENT_ROLES];
        return 1;
    }
    return 0;
}

void gui_apply_accent(gui_state_t *state) {
    state->accent_valid = 0;
    state->logo_tint_on = 0;
    state->os_icon_tint_on = 0;
    state->pwr_tint_on[0] = state->pwr_tint_on[1] = state->pwr_tint_on[2] = 0;

    if (state->accent_enabled &&
        accent_generate(state->background, state->accent_variant, state->accent_roles)) {
        state->accent_primary   = state->accent_roles[ROLE_PRIMARY];
        state->accent_secondary = state->accent_roles[ROLE_SECONDARY];
        state->accent_tertiary  = state->accent_roles[ROLE_TERTIARY];
        state->accent_valid = 1;
        efi_log(L"accent: derived Material palette from wallpaper");
    } else if (state->accent_enabled) {
        efi_log(L"accent: no usable color in wallpaper - keeping configured colors");
    }

    color_t c;
    accent_spec_t *g_text      = &state->sp_g_text;
    accent_spec_t *g_icons     = &state->sp_g_icons;
    accent_spec_t *g_underline = &state->sp_g_underline;

    if (state->accent_underline || state->sp_underline.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_underline, g_underline, ROLE_PRIMARY, &c))
            state->underline_color = c;
    }
    if (state->accent_underline || state->sp_highlight.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_highlight, g_underline, ROLE_PRIMARY, &c))
            state->highlight_color = c;
    }
    if (state->accent_text || state->sp_title.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_title, g_text, ROLE_PRIMARY, &c))
            state->title_color = c;
    }
    if (state->accent_text || state->sp_name.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_name, g_text, ROLE_ON_SURFACE, &c))
            state->name_color = c;
    }
    if (state->accent_text || state->sp_info.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_info, g_text, ROLE_ON_SURFACE_VARIANT, &c))
            state->fg_color = c;
    }
    if (state->accent_icons || state->sp_shutdown.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_shutdown, g_icons, ROLE_SECONDARY, &c)) {
            state->shutdown_color = c;
            state->pwr_tint_on[0] = 1;
        }
    }
    if (state->accent_icons || state->sp_reboot.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_reboot, g_icons, ROLE_SECONDARY, &c)) {
            state->reboot_color = c;
            state->pwr_tint_on[1] = 1;
        }
    }
    if (state->accent_icons || state->sp_firmware.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_firmware, g_icons,
                           ROLE_TERTIARY_CONTAINER, &c)) {
            state->firmware_color = c;
            state->pwr_tint_on[2] = 1;
        }
    }
    if (state->accent_logo || state->sp_logo.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_logo, NULL, ROLE_PRIMARY, &c)) {
            state->logo_tint = c;
            state->logo_tint_on = 1;
        }
    }
    if (state->accent_os_icons || state->sp_os_icons.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_os_icons, NULL, ROLE_PRIMARY, &c)) {
            state->os_icon_tint = c;
            state->os_icon_tint_on = 1;
        }
    }

    if (state->accent_clock || state->sp_clock.mode != SPEC_UNSET) {
        if (accent_resolve(state, &state->sp_clock, NULL, ROLE_CLOCK, &c))
            state->clock_color = c;
    }

    if (!state->accent_valid) return;

    if (accent_resolve(state, &state->sp_bg, NULL, ROLE_SURFACE, &c))
        state->bg_color = c;
    if (!state->blur_color_set &&
        accent_resolve(state, &state->sp_blur, NULL, ROLE_ON_PRIMARY_CONTAINER, &c))
        state->blur_color = c;
}
