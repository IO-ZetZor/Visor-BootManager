/* gui_background.c - wallpaper, logo and animated background painting */
#include "gui_internal.h"

#define DEFAULT_BACKGROUND_PATH L"\\EFI\\visor\\backgrounds\\default.png"

void gui_set_background(gui_state_t *state, CHAR16 *path) {
    if (state->background && state->background->pixels) {
        if (state->background->scaled) efi_free_pool(state->background->scaled);
        efi_free_pool(state->background->pixels);
        efi_free_pool(state->background);
    }
    state->background = NULL;
    if (state->bg_anim) {
        anim_free(state->bg_anim);
        state->bg_anim = NULL;
    }
    blur_free(state);
    state->bg_gen++;
    if (state->background_path) {
        efi_free_pool(state->background_path);
    }

    state->background_path = efi_strdup(path);

    int kind = path_anim_kind(path);
    {
        CHAR16 dbg[80];
        SPrint(dbg, sizeof(dbg), L"  anim kind=%d for ", kind);
        UINTN dl = 0; while (dbg[dl]) dl++;
        UINTN pi = 0;
        while (path[pi] && dl < 78) { dbg[dl++] = path[pi++]; }
        dbg[dl] = 0;
        efi_log(dbg);
    }
    if (kind >= 1 && kind <= 3) {
        icon_t *first = NULL;
        anim_t *a = gui_load_anim(path, &first,
                                  state->screen_width, state->screen_height);
        if (a) {
            if (a->frame_count > 1) {
                state->bg_anim = a;
            } else {
                anim_free(a);
            }
            state->background = first;
        } else {
            efi_log(L"  ERROR: animation decode failed");
        }
    } else {
        state->background = gui_load_image(path);
    }

    if (!state->background && efi_strcmp(path, DEFAULT_BACKGROUND_PATH) != 0) {
        efi_log(L"  WARN: background unusable - falling back to default background");
        state->background = gui_load_image(DEFAULT_BACKGROUND_PATH);
        if (state->background)
            efi_log(L"  background: default fallback loaded");
        else
            efi_log(L"  WARN: default background missing too - using solid colour");
    }
}

#define DEFAULT_LOGO_PATH L"\\EFI\\visor\\logo.png"

void gui_set_logo(gui_state_t *state, CHAR16 *path) {
    if (state->logo && state->logo->pixels) {
        if (state->logo->scaled) efi_free_pool(state->logo->scaled);
        efi_free_pool(state->logo->pixels);
        efi_free_pool(state->logo);
    }
    state->logo = NULL;

    CHAR16 *want = (path && path[0]) ? path : DEFAULT_LOGO_PATH;
    state->logo = gui_load_image(want);

    if (!state->logo && efi_strcmp(want, DEFAULT_LOGO_PATH) != 0) {
        efi_log(L"  WARN: logo unusable - falling back to default logo");
        state->logo = gui_load_image(DEFAULT_LOGO_PATH);
    }
    if (!state->logo)
        efi_log(L"  WARN: no logo image - drawing the title alone");
}

#define ANIM_MAX_SKIP 8


/* Load the optional splash picture.  A path that cannot be decoded is logged
 * and ignored: the splash then falls back to the composed frame, so a bad
 * splash_image can never cost a boot. */
void gui_set_splash(gui_state_t *state, CHAR16 *path) {
    if (state->splash_img && state->splash_img->pixels) {
        if (state->splash_img->scaled) efi_free_pool(state->splash_img->scaled);
        efi_free_pool(state->splash_img->pixels);
        efi_free_pool(state->splash_img);
    }
    state->splash_img = NULL;
    if (!path || !path[0]) return;

    efi_log(L"  loading splash image");
    state->splash_img = gui_load_image(path);
    if (!state->splash_img) {
        efi_log(L"  WARN: splash_image failed to load/decode - using the composed frame");
    } else {
        efi_log(L"  splash image loaded");
    }
}

static int anim_build_maps(anim_t *a, UINTN dw, UINTN dh) {
    if (a->xmap && a->ymap && a->map_w == dw && a->map_h == dh) return 1;

    if (a->xmap) { efi_free_pool(a->xmap); a->xmap = NULL; }
    if (a->ymap) { efi_free_pool(a->ymap); a->ymap = NULL; }
    a->map_w = a->map_h = 0;

    if (!dw || !dh) return 0;

    UINTN *xm = efi_allocate_pool(dw * sizeof(UINTN));
    UINTN *ym = efi_allocate_pool(dh * sizeof(UINTN));
    if (!xm || !ym) {
        if (xm) efi_free_pool(xm);
        if (ym) efi_free_pool(ym);
        return 0;
    }

    for (UINTN x = 0; x < dw; x++) xm[x] = (x * a->width) / dw;
    for (UINTN y = 0; y < dh; y++) ym[y] = (y * a->height) / dh * a->width;

    a->xmap = xm;
    a->ymap = ym;
    a->map_w = dw;
    a->map_h = dh;
    return 1;
}

static UINT8 g_dim195[256];
static int g_dim195_ready = 0;

static void build_dim195(void) {
    for (int i = 0; i < 256; i++) g_dim195[i] = (UINT8)((i * 195) / 255);
    g_dim195_ready = 1;
}

static UINT32 dim_pixel(UINT32 c) {
    return 0xFF000000u |
           ((UINT32)g_dim195[(c >> 16) & 0xFF] << 16) |
           ((UINT32)g_dim195[(c >> 8) & 0xFF] << 8) |
           (UINT32)g_dim195[c & 0xFF];
}

int gui_draw_background(gui_state_t *state) {
    UINTN dst_width  = state->screen_width;
    UINTN dst_height = state->screen_height;

    anim_t *a = state->bg_anim;
    if (a && a->canvas && anim_build_maps(a, dst_width, dst_height)) {
        if (!g_dim195_ready) build_dim195();
        if (!state->backbuffer) return 0;
        UINT32 *src = a->canvas;
        UINT32 *dst = state->backbuffer;
        UINT32 fill = dim_pixel(color_to_u32(state->bg_color));

        for (UINTN y = 0; y < dst_height; y++) {
            UINTN rowbase = a->ymap[y];
            UINT32 *drow = dst + y * dst_width;
            const UINT32 *srow = src + rowbase;
            if (a->width == dst_width) {
                for (UINTN x = 0; x < dst_width; x++) {
                    UINT32 pixel = srow[x];
                    drow[x] = (pixel & 0xFF000000u) ? dim_pixel(pixel) : fill;
                }
            } else {
                const UINTN *xm = a->xmap;
                for (UINTN x = 0; x < dst_width; x++) {
                    UINT32 pixel = srow[xm[x]];
                    drow[x] = (pixel & 0xFF000000u) ? dim_pixel(pixel) : fill;
                }
            }
        }
        return 1;
    }

    if (!state->background || !state->background->pixels) {

        gui_fill_rect(state, 0, 0, state->screen_width, state->screen_height, state->bg_color);
        return 0;
    }

    icon_t *bg = state->background;

    for (UINTN y = 0; y < dst_height; y++) {
        for (UINTN x = 0; x < dst_width; x++) {
            UINTN src_x = (x * bg->width) / dst_width;
            UINTN src_y = (y * bg->height) / dst_height;
            UINT32 pixel = bg->pixels[src_y * bg->width + src_x];
            UINT32 *dest = get_pixel(state, x, y);
            if (dest) {

                *dest = (pixel & 0x00FFFFFF) | 0xFF000000;
            }
        }
    }
    return 0;
}

/**
 * gui_draw_splash - paint the boot splash frame.
 *
 * Draws the wallpaper, then the logo and the title centred as one block, and
 * presents it.  This is a still frame, not an animation: it exists so the
 * handover to the kernel never shows a text screen, and so the kernel (or the
 * distro's own splash) paints over an image that already looks like a splash
 * instead of over a black screen.
 *
 * @param  state  GUI state, already wired from the configuration.
 *
 * @retval 1  the splash was painted and presented
 * @retval 0  no usable framebuffer, nothing was drawn
 */
int gui_draw_splash(gui_state_t *state) {
    if (!state || !state->backbuffer || !state->screen_width ||
        !state->screen_height)
        return 0;

    UINTN sw = state->screen_width;
    UINTN sh = state->screen_height;

    /* A user-supplied splash picture wins outright: it is theirs to design,
     * so nothing is composed on top of it.  Scaled to cover, centred. */
    if (state->splash_img && state->splash_img->pixels &&
        state->splash_img->width && state->splash_img->height) {
        UINTN iw = state->splash_img->width, ih = state->splash_img->height;
        UINTN dw = sw, dh = sh;
        if (iw * sh > ih * sw) dh = ih * sw / iw;
        else                   dw = iw * sh / ih;
        if (!dw) dw = 1;
        if (!dh) dh = 1;
        gui_fill_rect(state, 0, 0, (INTN)sw, (INTN)sh, state->bg_color);
        draw_image_sized(state, state->splash_img,
                         (sw - dw) / 2, (sh - dh) / 2, dw);
        gui_present(state);
        return 1;
    }

    gui_draw_background(state);

    UINTN base     = ui_base(state);
    UINTN title_px = state->title_size ? state->title_size : default_title_px(state);
    UINTN logo_px  = state->logo_size ? state->logo_size : base / 6;
    if (logo_px < 24) logo_px = 24;
    if (logo_px > sw / 3) logo_px = sw / 3;

    int have_logo  = state->logo && state->logo->pixels &&
                     state->logo_mode != LOGO_MODE_OFF;
    int have_title = state->show_title;
    CHAR16 *title  = (state->title && state->title[0]) ? state->title : L"Visor";
    UINTN title_maxw = sw * 3 / 4;
    UINTN title_w  = have_title ? text_fit_width_px(title, title_px, title_maxw, NULL) : 0;

    UINTN gap   = title_px / 3;
    UINTN block = (have_logo ? logo_px : 0) +
                  ((have_logo && have_title) ? gap : 0) +
                  (have_title ? title_px : 0);
    INTN  y = (INTN)((sh > block ? sh - block : 0) / 2);

    if (have_logo) {
        draw_image_sized(state, state->logo, (sw - logo_px) / 2, (UINTN)y, logo_px);
        y += (INTN)(logo_px + gap);
    }
    if (have_title)
        draw_text_fit_px_a(state, title, (INTN)((sw - title_w) / 2), y,
                           state->title_color, title_px, 255, title_maxw);

    gui_present(state);
    return 1;
}

int anim_tick(gui_state_t *state) {
    anim_t *a = state->bg_anim;
    if (!a || a->frame_count < 2) return 0;

    UINT64 now = efi_get_tick();

    if (!a->next_ms) {
        a->next_ms = now + a->cur_delay;
        return 0;
    }
    if (now < a->next_ms) return 0;

    UINTN step = a->cur_delay ? a->cur_delay : 10;
    UINTN late = (UINTN)(now - a->next_ms);
    UINTN skip = 1 + late / step;
    int   clamped = skip > ANIM_MAX_SKIP;
    if (clamped) skip = ANIM_MAX_SKIP;

    if (!anim_advance_n(a, skip)) {
        a->next_ms = 0;
        a->frame_count = 1;
        return 0;
    }

    UINTN hold = a->cur_delay;
    if (hold < 10) hold = 10;

    if (clamped)
        a->next_ms = now + hold;
    else
        a->next_ms += (UINT64)(skip - 1) * step + hold;

    state->bg_gen++;
    state->scene_valid = 0;
    return 1;
}
