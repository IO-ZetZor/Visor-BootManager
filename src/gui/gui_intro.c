/* gui_intro.c - the optional boot splash played once before the menu
 *
 * feature: intro (requires gui).  Reuses the decoders the background already
 * uses, so an intro is just another media path with a deadline of its own.
 * There is no second media pipeline here and no new codec: anything
 * gui_load_image()/gui_load_anim() can open, an intro can play.
 */
#include "gui_internal.h"
#include "intro.h"

/* A splash longer than this is a boot hazard, not a splash. */
#define INTRO_MAX_DURATION_MS  5000
/* Fade-in length, and never more than half the intro itself. */
#define INTRO_FADE_MS          250
/* Key-poll interval.  Same ballpark as the menu loop's own sleep. */
#define INTRO_POLL_US          16000
/* Cap on frames skipped in one step, so a slow decode cannot make the clip
 * jump the whole loop in a single paint.  Mirrors ANIM_MAX_SKIP. */
#define INTRO_MAX_SKIP         8
/* GIF/MJPEG delays below this are legal but unusable as a frame rate. */
#define INTRO_MIN_FRAME_MS     10

/* Where the media lands on screen, and the source column for each
 * destination column.  Built once per intro: the blit runs it every frame,
 * and deriving the column with a division instead costs a 64-bit divide per
 * screen pixel - about a million of them per frame at 1080p. */
typedef struct {
    UINTN  left, top, width, height;
    UINTN *xmap;
} intro_fit_t;

static void intro_fit_free(intro_fit_t *fit) {
    if (fit->xmap) efi_free_pool(fit->xmap);
    fit->xmap = NULL;
}

/* Aspect-fit into the screen, centred, never stretched or cropped.  The
 * products are done in UINT64 because a decoded frame's dimensions are
 * attacker-controlled and this runs before anything has validated them. */
static int intro_fit_build(intro_fit_t *fit, gui_state_t *state,
                           UINTN img_w, UINTN img_h) {
    fit->xmap = NULL;
    fit->left = fit->top = fit->width = fit->height = 0;

    UINTN sw = state->screen_width, sh = state->screen_height;
    if (!img_w || !img_h || !sw || !sh) return 0;

    UINTN dw = sw;
    UINTN dh = (UINTN)((UINT64)img_h * sw / img_w);
    if (!dh || dh > sh) {
        dh = sh;
        dw = (UINTN)((UINT64)img_w * sh / img_h);
    }
    if (!dw || !dh) return 0;

    fit->width  = dw;
    fit->height = dh;
    fit->left   = (sw - dw) / 2;
    fit->top    = (sh - dh) / 2;

    fit->xmap = efi_allocate_pool(dw * sizeof(UINTN));
    if (!fit->xmap) return 0;
    for (UINTN x = 0; x < dw; x++)
        fit->xmap[x] = (UINTN)((UINT64)x * img_w / dw);
    return 1;
}

/* The blit overwrites every pixel inside the fit rect, so only the
 * letterbox bars need clearing. */
static void intro_clear(gui_state_t *state, const intro_fit_t *fit) {
    UINTN sw = state->screen_width, sh = state->screen_height;
    UINT32 *bb = state->backbuffer;
    if (!bb) return;

    for (UINTN y = 0; y < fit->top; y++)
        for (UINTN x = 0; x < sw; x++) bb[y * sw + x] = 0xFF000000u;
    for (UINTN y = fit->top + fit->height; y < sh; y++)
        for (UINTN x = 0; x < sw; x++) bb[y * sw + x] = 0xFF000000u;
    for (UINTN y = fit->top; y < fit->top + fit->height; y++) {
        UINT32 *row = bb + y * sw;
        for (UINTN x = 0; x < fit->left; x++) row[x] = 0xFF000000u;
        for (UINTN x = fit->left + fit->width; x < sw; x++) row[x] = 0xFF000000u;
    }
}

static void intro_label(gui_state_t *state, CHAR16 *text, INTN y, UINTN size,
                        color_t color, UINTN alpha) {
    INTN pad = (INTN)(size / 2);
    UINTN maxw = state->screen_width > (UINTN)(2 * pad + 16)
               ? state->screen_width - (UINTN)(2 * pad) : state->screen_width;
    UINTN tw = text_fit_width_px(text, size, maxw, NULL);
    INTN w = (INTN)tw + pad * 2;
    if (w > (INTN)state->screen_width) w = (INTN)state->screen_width;
    if (w > 0)
        fill_round_rect(state, ((INTN)state->screen_width - w) / 2, y - pad,
                        w, (INTN)size + pad * 2, pad, COLOR_BLACK,
                        (UINT8)(alpha * 190 / 255));
    draw_text_fit_px_a(state, text,
                       ((INTN)state->screen_width - (INTN)tw) / 2, y,
                       color, size, (INTN)alpha, maxw);
}

/* The splash is the same UI as the menu, so it wears the configured title at
 * the configured size rather than a hardcoded one. */
static void intro_overlay(gui_state_t *state, UINTN alpha, int manual) {
    UINTN sh = state->screen_height;
    color_t white = COLOR_WHITE;
    color_t dim   = {0x9A, 0xA3, 0xB0};

    if (state->show_title) {
        CHAR16 *title = (state->title && state->title[0]) ? state->title : L"Visor";
        UINTN size = state->title_size ? state->title_size : default_title_px(state);
        intro_label(state, title, (INTN)(sh / 8), size, white, alpha);
    }
    /* "press any key" is only true when a key is what ends the intro. */
    if (manual)
        intro_label(state, L"Press any key to continue", (INTN)(sh * 8 / 10),
                    default_aux_text_px(state), dim, alpha);
}

/* One 256-entry scale table, rebuilt whenever the fade moves.  Media that
 * carries no alpha of its own - which is most of what a splash is - has
 * a == alpha for every pixel, so the three multiply-divides per channel
 * become three lookups.  Worth it: the general path costs 2.2 ms per 720p
 * frame against 0.9 ms for this one.  See tools/intro_blit_host.c.
 *
 * The flag is narrowed as we paint, so a clip that turns out to use alpha
 * part-way through drops back to the general path instead of being wrong. */
static UINT8 intro_scale[256];

static void intro_build_scale(UINTN alpha) {
    for (UINTN c = 0; c < 256; c++)
        intro_scale[c] = (UINT8)(c * alpha / 255);
}

/* The blit overwrites every pixel inside the fit rect, so only the letterbox
 * bars need clearing, and opaque media at full fade needs no arithmetic at
 * all - just the alpha byte masked back in. */
static void intro_paint(gui_state_t *state, const icon_t *img,
                        const intro_fit_t *fit, UINTN alpha, int manual,
                        int *opaque) {
    if (!state->backbuffer) return;
    const UINT32 *src = img->pixels;
    UINTN sw = state->screen_width;
    UINTN iw = img->width, ih = img->height;

    intro_clear(state, fit);
    if (alpha < 255 && *opaque) intro_build_scale(alpha);

    for (UINTN row = 0; row < fit->height; row++) {
        UINTN srow = (UINTN)((UINT64)row * ih / fit->height);
        const UINT32 *s = src + srow * iw;
        UINT32 *d = state->backbuffer + (fit->top + row) * sw + fit->left;
        if (alpha >= 255) {
            for (UINTN x = 0; x < fit->width; x++) {
                UINT32 p = s[fit->xmap[x]];
                d[x] = (p & 0x00FFFFFF) | 0xFF000000u;
            }
        } else if (*opaque) {
            for (UINTN x = 0; x < fit->width; x++) {
                UINT32 p = s[fit->xmap[x]];
                if (!(p >> 24)) *opaque = 0;
                d[x] = 0xFF000000u
                     | ((UINT32)intro_scale[(p >> 16) & 255] << 16)
                     | ((UINT32)intro_scale[(p >> 8) & 255] << 8)
                     | (UINT32)intro_scale[p & 255];
            }
        } else {
            /* Otherwise resolve the media's own alpha and the intro's fade
             * together, against black - the letterbox colour. */
            for (UINTN x = 0; x < fit->width; x++) {
                UINT32 p = s[fit->xmap[x]];
                UINTN a = (UINTN)((p >> 24) * alpha) / 255;
                UINT32 r = (UINT32)((UINTN)((p >> 16) & 255) * a / 255);
                UINT32 g = (UINT32)((UINTN)((p >> 8) & 255) * a / 255);
                UINT32 b = (UINT32)((UINTN)(p & 255) * a / 255);
                d[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }

    intro_overlay(state, alpha, manual);
    gui_present(state);
}

int gui_show_intro(gui_state_t *state, CHAR16 *path, UINTN duration_ms,
                   int loop, int auto_continue) {
    if (!state || !state->backbuffer || !state->screen_width ||
        !state->screen_height || !path || !path[0] || !duration_ms)
        return INTRO_BYPASSED;

    if (duration_ms > INTRO_MAX_DURATION_MS) duration_ms = INTRO_MAX_DURATION_MS;

    anim_t *anim = NULL;
    icon_t frame = {0};
    icon_t *image = NULL;
    intro_fit_t fit = {0};
    int result = INTRO_BYPASSED;

    if (path_anim_kind(path)) {
        anim = gui_load_anim(path, NULL, state->screen_width, state->screen_height);
        if (!anim) {
            efi_log(L"intro: clip unavailable, skipping intro");
            return INTRO_BYPASSED;
        }
        /* intro_loop, not the clip's own loop count, decides where the intro
         * ends - so force the decoders to wrap and let ourselves stop. */
        anim->loops = 0;
        frame.width  = anim->width;
        frame.height = anim->height;
        frame.pixels = anim->canvas;
        image = &frame;
    } else {
        image = gui_load_image(path);
        if (!image) {
            efi_log(L"intro: image unavailable, skipping intro");
            return INTRO_BYPASSED;
        }
    }
    if (!image->pixels || !image->width || !image->height ||
        (anim && !anim->frame_count))
        goto cleanup;

    if (!intro_fit_build(&fit, state, image->width, image->height))
        goto cleanup;

    arch_clock_init();
    UINT64 start = arch_now_us();
    UINT64 deadline_us = (UINT64)duration_ms * 1000;
    /* Fade in over a quarter second, but never eat more than half the intro:
     * a short intro should still be watchable. */
    UINTN fade_us = duration_ms * 1000 / 2;
    if (fade_us > INTRO_FADE_MS * 1000) fade_us = INTRO_FADE_MS * 1000;

    /* animation=0 means no motion at all: hold the first frame. */
    int playing = (anim && state->animation);
    UINT64 next_frame_us = 0;
    if (playing) {
        UINTN hold = anim->cur_delay ? anim->cur_delay : INTRO_MIN_FRAME_MS;
        next_frame_us = (UINT64)hold * 1000;
    }
    UINTN last_alpha = 256;
    int opaque = 1;   /* narrowed on the first paint if media uses alpha */

    for (;;) {
        EFI_INPUT_KEY key;
        if (!EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key))) {
            result = INTRO_SKIPPED;
            break;
        }

        UINT64 elapsed = arch_now_us() - start;
        if (auto_continue && elapsed >= deadline_us) {
            result = INTRO_FINISHED;
            break;
        }

        int frame_changed = 0;
        if (playing && elapsed >= next_frame_us) {
            if (!loop && anim->cur >= anim->frame_count - 1) {
                /* Non-looping clip: the last frame is the one on screen now,
                 * so there is nothing left to advance to. */
                playing = 0;
            } else {
                UINTN hold = anim->cur_delay ? anim->cur_delay : INTRO_MIN_FRAME_MS;
                UINTN late = (UINTN)(elapsed - next_frame_us);
                UINTN skip = 1 + (UINTN)(late / ((UINT64)hold * 1000));
                if (skip > INTRO_MAX_SKIP) skip = INTRO_MAX_SKIP;
                /* Do not overshoot the end of a one-shot clip while catching
                 * up, or the last frame never gets shown. */
                if (!loop) {
                    UINTN left = anim->frame_count - 1 - anim->cur;
                    if (left && skip > left) skip = left;
                }
                if (anim_advance_n(anim, skip)) {
                    frame.pixels = anim->canvas;
                    frame_changed = 1;
                    hold = anim->cur_delay ? anim->cur_delay : INTRO_MIN_FRAME_MS;
                    if (hold < INTRO_MIN_FRAME_MS) hold = INTRO_MIN_FRAME_MS;
                    next_frame_us = elapsed + (UINT64)hold * 1000;
                } else {
                    /* Truncated clip, or a decoder that stopped early. */
                    playing = 0;
                }
            }
            if (!playing && auto_continue) {
                result = INTRO_FINISHED;
                break;
            }
        }

        UINTN alpha = 255;
        if (gui_animation_on(state) && elapsed < fade_us)
            alpha = (UINTN)(elapsed * 255 / fade_us);

        if (alpha != last_alpha || frame_changed) {
            intro_paint(state, image, &fit, alpha, !auto_continue, &opaque);
            last_alpha = alpha;
        }

        /* Keep any boot audio engine fed while the splash is up. */
        if (state->sound_poll) state->sound_poll();
        BS->Stall(INTRO_POLL_US);
    }

    /* The splash owned the whole backbuffer.  Nothing cached from before it
     * may be reused - the menu's first draw has to repaint from scratch. */
    state->scene_valid = 0;

cleanup:
    intro_fit_free(&fit);
    if (anim) {
        anim_free(anim);
    } else if (image) {
        if (image->scaled) efi_free_pool(image->scaled);
        if (image->pixels) efi_free_pool(image->pixels);
        efi_free_pool(image);
    }
    return result;
}
