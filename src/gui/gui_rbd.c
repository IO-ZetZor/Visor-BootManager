/* gui_rbd.c - Red Boot of Death screen (feature: rbd) */
#include "gui_internal.h"

#define RBD_BLOOM_MS   700
#define RBD_TEXT_MS    600
#define RBD_HOLD_MS    800
#define RBD_SINK_MS    600

UINT32 rbd_scene_duration_ms(void) {
    return RBD_BLOOM_MS + RBD_TEXT_MS + RBD_HOLD_MS + RBD_SINK_MS;
}

#define RBD_TITLE      L"Have we been here before..?"

static void rbd_paint_scene(gui_state_t *state, UINT32 *scene) {
    UINTN w = state->screen_width, h = state->screen_height;
    INTN cx = (INTN)w / 2, cy = (INTN)h / 2;
    UINTN maxd2 = (UINTN)(cx * cx + cy * cy);
    if (!maxd2) maxd2 = 1;

    for (UINTN y = 0; y < h; y++) {
        INTN dy = (INTN)y - cy;
        UINTN dy2 = (UINTN)(dy * dy);
        UINT32 *row = scene + y * w;
        for (UINTN x = 0; x < w; x++) {
            INTN dx = (INTN)x - cx;
            UINTN d2 = dy2 + (UINTN)(dx * dx);
            UINTN t = d2 >= maxd2 ? 0 : 1000 - d2 * 1000 / maxd2;
            t = t * t / 1000;
            UINT32 r = (UINT32)(45 + 105 * t / 1000);
            UINT32 g = (UINT32)(20 +  50 * t / 1000);
            UINT32 b = (UINT32)(90 + 145 * t / 1000);
            row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

static void rbd_draw_text(gui_state_t *state, UINTN title_px, INTN alpha) {
    if (alpha <= 0) return;

    INTN cy = (INTN)state->screen_height / 2;
    INTN ty = cy - (INTN)title_px;

    UINTN tw = text_width_px(RBD_TITLE, title_px);
    INTN tx = tw < state->screen_width ? (INTN)(state->screen_width - tw) / 2 : 0;

    draw_text_px_a(state, RBD_TITLE, tx, ty, (color_t){0xE8, 0xE4, 0xF8},
                   title_px, alpha);
}

void gui_rbd_screen(gui_state_t *state) {
    if (!state || !state->backbuffer) return;
    UINTN px = state->screen_width * state->screen_height;
    if (!px) return;

    UINTN title_px = clamp_uintn(ui_base(state) / 9, 30, 104);

    UINT32 *scene = efi_allocate_pool(px * sizeof(UINT32));
    if (!scene) {
        gui_fill_rect(state, 0, 0, state->screen_width, state->screen_height,
                      (color_t){0x4A, 0x22, 0x8A});
        rbd_draw_text(state, title_px, 255);
        gui_present(state);
        BS->Stall((RBD_BLOOM_MS + RBD_HOLD_MS) * 1000);
        fade_write_black(state, px);
        gui_present(state);
        return;
    }

    rbd_paint_scene(state, scene);

    arch_clock_init();
    UINT64 total_us = (UINT64)(RBD_BLOOM_MS + RBD_TEXT_MS + RBD_HOLD_MS +
                               RBD_SINK_MS) * 1000ULL;
    UINT64 start = arch_now_us();

    for (;;) {
        UINT64 frame_start = arch_now_us();
        UINT64 el = frame_start - start;
        int done = el >= total_us;
        UINTN ms = (UINTN)(el / 1000);

        UINTN red_alpha = 255;
        INTN  text_alpha = 255;

        if (ms < RBD_BLOOM_MS) {
            red_alpha = ease_alpha((INTN)ms, RBD_BLOOM_MS);
            text_alpha = 0;
        } else if (ms < RBD_BLOOM_MS + RBD_TEXT_MS) {
            text_alpha = (INTN)ease_alpha((INTN)(ms - RBD_BLOOM_MS), RBD_TEXT_MS);
        } else if (ms >= RBD_BLOOM_MS + RBD_TEXT_MS + RBD_HOLD_MS) {
            UINTN into = ms - (RBD_BLOOM_MS + RBD_TEXT_MS + RBD_HOLD_MS);
            UINTN gone = ease_alpha((INTN)into, RBD_SINK_MS);
            red_alpha  = gone < 255 ? 255 - gone : 0;
            text_alpha = (INTN)red_alpha;
        }
        if (done) { red_alpha = 0; text_alpha = 0; }

        fade_write_scaled(state, scene, px, red_alpha);
        rbd_draw_text(state, title_px, text_alpha);
        gui_present(state);
        if (done) break;

        if (efi_key_pending()) {
            fade_write_black(state, px);
            gui_present(state);
            break;
        }

        UINT64 spent = arch_now_us() - frame_start;
        if (spent < FADE_MIN_FRAME_US) BS->Stall(FADE_MIN_FRAME_US - spent);
    }

    efi_free_pool(scene);
    state->scene_valid = 0;
    state->blur_valid = 0;
}
