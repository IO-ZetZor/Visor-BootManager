/* gui_screensaver.c - idle dim/blank screensaver (feature: screensaver) */
#include "gui_internal.h"

int ss_wake(gui_state_t *state) {
    state->ss_last_input_ms = efi_get_tick();
    if (!state->screensaver) return 0;
    if (state->ss_level == SS_LEVEL_AWAKE) return 0;

    state->ss_level = SS_LEVEL_AWAKE;
    state->scene_valid = 0;
    return 1;
}

int ss_tick(gui_state_t *state) {
    if (!state->screensaver) return 0;

    if (state->timeout_active && state->timeout > 0) {
        state->ss_last_input_ms = efi_get_tick();
        return 0;
    }

    if (state->editing || state->browse) {
        state->ss_last_input_ms = efi_get_tick();
        return 0;
    }

    UINT64 now = efi_get_tick();
    if (!state->ss_last_input_ms) state->ss_last_input_ms = now;
    UINT64 idle = now - state->ss_last_input_ms;

    if (state->ss_level == SS_LEVEL_AWAKE && idle >= state->ss_delay_ms) {
        state->ss_level = SS_LEVEL_DIM;
        return 1;
    }
    if (state->ss_level == SS_LEVEL_DIM && state->ss_blank_ms &&
        idle >= state->ss_blank_ms) {
        state->ss_level = SS_LEVEL_BLANK;
        return 1;
    }
    return 0;
}

int ss_input_pending(gui_state_t *state) {
    int hit = 0;

    EFI_INPUT_KEY k;
    while (!EFI_ERROR(uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2,
                                       ST->ConIn, &k)))
        hit = 1;

    if (state->mouse_enabled && state->has_pointer) {
        if (state->spp) {
            EFI_SIMPLE_POINTER_PROTOCOL *sp = state->spp;
            EFI_SIMPLE_POINTER_STATE st;
            while (!EFI_ERROR(sp->GetState(sp, &st))) {
                if (st.RelativeMovementX || st.RelativeMovementY ||
                    st.RelativeMovementZ || st.LeftButton || st.RightButton)
                    hit = 1;
            }
        }
        if (state->app) {
            EFI_ABSOLUTE_POINTER_PROTOCOL *ap = state->app;
            EFI_ABSOLUTE_POINTER_STATE st;
            while (!EFI_ERROR(ap->GetState(ap, &st)))
                hit = 1;
        }
    }
    return hit;
}

void ss_draw_frame(gui_state_t *state) {
    if (!gui_draw_background(state))
        fill_rect_alpha(state, 0, 0, state->screen_width, state->screen_height,
                        COLOR_BLACK, 60);

    state->clock_drawn = 0;
    state->clock_dirty = 0;

    if (state->ss_keep_clock) draw_clock_ex(state, 0);
}

void ss_transition_to_saver(gui_state_t *state) {
    UINTN px = state->screen_width * state->screen_height;
    if (!state->backbuffer) return;

    state->cursor_saved = 0;

    UINT32 *from = efi_allocate_pool(px * sizeof(UINT32));
    UINT32 *to   = efi_allocate_pool(px * sizeof(UINT32));
    if (!from || !to) {

        if (from) efi_free_pool(from);
        if (to)   efi_free_pool(to);
        ss_draw_frame(state);
        gui_present(state);
        return;
    }

    CopyMem(from, state->backbuffer, px * sizeof(UINT32));
    ss_draw_frame(state);
    CopyMem(to, state->backbuffer, px * sizeof(UINT32));

    gui_crossfade(state, from, to);
    efi_free_pool(from);
    efi_free_pool(to);
}

void ss_transition_to_menu(gui_state_t *state, int was_blank) {
    UINTN px = state->screen_width * state->screen_height;
    if (!state->backbuffer) return;

    state->cursor_saved = 0;

    UINT32 *from = was_blank ? NULL : efi_allocate_pool(px * sizeof(UINT32));
    if (from) CopyMem(from, state->backbuffer, px * sizeof(UINT32));

    state->scene_valid = 0;
    gui_draw_menu(state, 0);

    if (from) {
        UINT32 *to = efi_allocate_pool(px * sizeof(UINT32));
        if (to) {
            CopyMem(to, state->backbuffer, px * sizeof(UINT32));
            gui_crossfade(state, from, to);
            efi_free_pool(to);
        } else {
            gui_present(state);
        }
        efi_free_pool(from);
    } else {

        gui_fade_in_current(state);
    }

    if (state->cursor_active) cursor_overlay(state);
}
