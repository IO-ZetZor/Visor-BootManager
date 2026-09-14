/* gui_fade.c - fades and crossfades (feature: animations) */
#include "gui_internal.h"

static int fade_speed_value(gui_state_t *state) {
    int sp = state ? state->fade_speed : 0;
    if (sp < 0) sp = 0;
    if (sp > 10) sp = 10;

    return sp;
}

#define FADE_MIN_DURATION_MS      150
#define FADE_MAX_DURATION_MS      500

static UINTN fade_duration_ms(gui_state_t *state) {
    int sp = fade_speed_value(state);
    return FADE_MAX_DURATION_MS
         - (UINTN)sp * (FADE_MAX_DURATION_MS - FADE_MIN_DURATION_MS) / 10;
}

int gui_animation_on(gui_state_t *state) {
    return state && state->animation;
}

INTN ease_permille(INTN frame, INTN frames) {
    if (frames <= 0 || frame >= frames) return 1000;
    if (frame <= 0) return 0;

    INTN t = frame * 1000 / frames;
    return (t * t * (3000 - 2 * t) + 500000) / 1000000;
}

UINTN ease_alpha(INTN frame, INTN frames) {
    return (UINTN)(ease_permille(frame, frames) * 255 / 1000);
}

void fade_write_black(gui_state_t *state, UINTN px) {
    for (UINTN i = 0; i < px; i++)
        state->backbuffer[i] = 0xFF000000u;
}

static void fade_write_snapshot(gui_state_t *state, UINT32 *snapshot, UINTN px) {
    for (UINTN i = 0; i < px; i++)
        state->backbuffer[i] = snapshot[i];
}

void fade_write_scaled(gui_state_t *state, UINT32 *snapshot,
                              UINTN px, UINTN alpha) {
    if (alpha == 0) {
        fade_write_black(state, px);
        return;
    }
    if (alpha >= 255) {
        fade_write_snapshot(state, snapshot, px);
        return;
    }

    UINT32 a = (UINT32)((alpha * 256 + 127) / 255);
    for (UINTN i = 0; i < px; i++) {
        UINT32 p = snapshot[i];
        UINT32 rb = (((p & 0x00FF00FFu) * a + 0x00800080u) >> 8) & 0x00FF00FFu;
        UINT32 g  = (((p & 0x0000FF00u) * a + 0x00008000u) >> 8) & 0x0000FF00u;
        state->backbuffer[i] = 0xFF000000u | rb | g;
    }
}

static void gui_fade_from_snapshot(gui_state_t *state, UINT32 *snapshot,
                                   int fade_in) {
    if (!state || !state->backbuffer || !snapshot) return;
    UINTN px = state->screen_width * state->screen_height;
    if (!gui_animation_on(state)) {
        if (fade_in)
            fade_write_snapshot(state, snapshot, px);
        else
            fade_write_black(state, px);
        gui_present(state);
        return;
    }

    arch_clock_init();
    UINT64 duration_us = (UINT64)fade_duration_ms(state) * 1000;
    UINT64 start = arch_now_us();

    for (;;) {
        UINT64 frame_start = arch_now_us();
        UINT64 elapsed = frame_start - start;
        int done = elapsed >= duration_us;

        UINTN e = done ? 1000 : (UINTN)ease_permille((INTN)elapsed, (INTN)duration_us);
        UINTN a = e * 255 / 1000;
        if (!fade_in) a = 255 - a;

        fade_write_scaled(state, snapshot, px, a);
        gui_present(state);
        if (done) break;

        UINT64 spent = arch_now_us() - frame_start;
        if (spent < FADE_MIN_FRAME_US) BS->Stall(FADE_MIN_FRAME_US - spent);
    }
}

void gui_fade_in_current(gui_state_t *state) {
    if (!state || !state->backbuffer) return;
    UINTN px = state->screen_width * state->screen_height;
    if (!gui_animation_on(state)) {
        gui_present(state);
        return;
    }
    UINT32 *snapshot = efi_allocate_pool(px * sizeof(UINT32));
    if (!snapshot) return;
    for (UINTN i = 0; i < px; i++) snapshot[i] = state->backbuffer[i];
    gui_fade_from_snapshot(state, snapshot, 1);
    efi_free_pool(snapshot);
}

void gui_fade_out(gui_state_t *state) {
    if (!state || !state->backbuffer) return;
    UINTN px = state->screen_width * state->screen_height;
    if (!gui_animation_on(state)) {
        fade_write_black(state, px);
        gui_present(state);
        return;
    }
    UINT32 *snapshot = efi_allocate_pool(px * sizeof(UINT32));
    if (!snapshot) return;
    for (UINTN i = 0; i < px; i++) snapshot[i] = state->backbuffer[i];
    gui_fade_from_snapshot(state, snapshot, 0);
    efi_free_pool(snapshot);
}

static void fade_write_cross(gui_state_t *state, UINT32 *from, UINT32 *to,
                             UINTN px, UINTN alpha) {
    if (alpha == 0)   { fade_write_snapshot(state, from, px); return; }
    if (alpha >= 255) { fade_write_snapshot(state, to, px);   return; }

    UINT32 a = (UINT32)((alpha * 256 + 127) / 255);
    UINT32 ia = 256 - a;
    for (UINTN i = 0; i < px; i++) {
        UINT32 p = to[i], q = from[i];
        UINT32 rb = ((((p & 0x00FF00FFu) * a) + ((q & 0x00FF00FFu) * ia)) >> 8)
                    & 0x00FF00FFu;
        UINT32 g  = ((((p & 0x0000FF00u) * a) + ((q & 0x0000FF00u) * ia)) >> 8)
                    & 0x0000FF00u;
        state->backbuffer[i] = 0xFF000000u | rb | g;
    }
}

void gui_crossfade(gui_state_t *state, UINT32 *from, UINT32 *to) {
    UINTN px = state->screen_width * state->screen_height;
    if (!gui_animation_on(state)) {
        fade_write_snapshot(state, to, px);
        gui_present(state);
        return;
    }

    arch_clock_init();
    UINT64 duration_us = (UINT64)fade_duration_ms(state) * 1000;
    UINT64 start = arch_now_us();

    for (;;) {
        UINT64 frame_start = arch_now_us();
        UINT64 elapsed = frame_start - start;
        int done = elapsed >= duration_us;

        UINTN e = done ? 1000
                       : (UINTN)ease_permille((INTN)elapsed, (INTN)duration_us);
        fade_write_cross(state, from, to, px, e * 255 / 1000);
        gui_present(state);
        if (done) break;

        UINT64 spent = arch_now_us() - frame_start;
        if (spent < FADE_MIN_FRAME_US) BS->Stall(FADE_MIN_FRAME_US - spent);
    }
}
