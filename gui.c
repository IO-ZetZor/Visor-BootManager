
#include "gui.h"
#include "font.h"
#include "efi_helpers.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;

icon_t* png_load(UINT8 *data, UINTN size);

static const font_t *g_font = &jetbrains_font;

/* partraschebestgirlever */

static const unsigned char *g_glyph_cov = 0;
static const font_t        *g_cov_for   = 0;

static void font_ensure_decoded(void) {
    if (g_cov_for == g_font && g_glyph_cov) return;
    const font_t *f = g_font;
    unsigned char *out = efi_allocate_pool(f->unpacked_size);
    g_cov_for = f;
    if (!out) { g_glyph_cov = 0; return; }

    const unsigned char *in     = f->pixels;
    const unsigned char *in_end = in + f->packed_size;
    UINTN o = 0;
    while (in < in_end && o < f->unpacked_size) {
        signed char n = (signed char)*in++;
        if (n >= 0) {
            UINTN cnt = (UINTN)n + 1;
            while (cnt-- && in < in_end && o < f->unpacked_size) out[o++] = *in++;
        } else if (n != -128) {
            if (in >= in_end) break;
            UINTN cnt = (UINTN)(1 - (int)n);
            unsigned char v = *in++;
            while (cnt-- && o < f->unpacked_size) out[o++] = v;
        }
    }
    g_glyph_cov = out;
}

void gui_set_font(const char *name) {

    (void)name;
    g_font = &jetbrains_font;
}

static UINTN isqrt_(UINTN n) {
    if (n == 0) return 0;
    UINTN x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

static UINT32 color_to_u32(color_t c) {
    return (0xFF << 24) | (c.r << 16) | (c.g << 8) | c.b;
}

static UINT32 blend_color(color_t c1, color_t c2, UINT8 alpha) __attribute__((unused));
static UINT32 blend_color(color_t c1, color_t c2, UINT8 alpha) {
    UINT8 r = (c1.r * (255 - alpha) + c2.r * alpha) / 255;
    UINT8 g = (c1.g * (255 - alpha) + c2.g * alpha) / 255;
    UINT8 b = (c1.b * (255 - alpha) + c2.b * alpha) / 255;
    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

static UINT32* get_pixel(gui_state_t *state, UINTN x, UINTN y) {
    if (x >= state->screen_width || y >= state->screen_height || !state->backbuffer)
        return NULL;
    return &state->backbuffer[y * state->screen_width + x];
}

static void blit_rows(gui_state_t *state, INTN y, INTN h) {
    if (state->pixel_format != PixelRedGreenBlueReserved8BitPerColor &&
        state->pixel_format != PixelBlueGreenRedReserved8BitPerColor)
        return;

    int swap = (state->pixel_format == PixelRedGreenBlueReserved8BitPerColor);
    UINT8 *fb = (UINT8*)state->gop->Mode->FrameBufferBase;
    for (INTN row = y; row < y + h; row++) {
        UINT32 *dst = (UINT32*)(fb + (UINTN)row * state->pixels_per_scanline * sizeof(UINT32));
        UINT32 *src = &state->backbuffer[(UINTN)row * state->screen_width];
        if (swap)
            for (UINTN x = 0; x < state->screen_width; x++) {
                UINT32 p = src[x];
                dst[x] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
            }
        else
            for (UINTN x = 0; x < state->screen_width; x++) dst[x] = src[x];
    }
}

void gui_present(gui_state_t *state) {
    if (!state->backbuffer) return;

    EFI_STATUS s = uefi_call_wrapper(state->gop->Blt, 10,
        state->gop,
        (EFI_GRAPHICS_OUTPUT_BLT_PIXEL*)state->backbuffer,
        EfiBltBufferToVideo,
        0, 0, 0, 0,
        state->screen_width, state->screen_height,
        0);
    if (!EFI_ERROR(s)) return;

    blit_rows(state, 0, (INTN)state->screen_height);
}

void gui_present_band(gui_state_t *state, INTN y, INTN h) {
    if (!state->backbuffer) return;
    if (y < 0) { h += y; y = 0; }
    if (h <= 0) return;
    if (y + h > (INTN)state->screen_height) h = (INTN)state->screen_height - y;
    if (h <= 0) return;

    EFI_STATUS s = uefi_call_wrapper(state->gop->Blt, 10,
        state->gop,
        (EFI_GRAPHICS_OUTPUT_BLT_PIXEL*)state->backbuffer,
        EfiBltBufferToVideo,
        0, (UINTN)y, 0, (UINTN)y,
        state->screen_width, (UINTN)h,
        state->screen_width * sizeof(UINT32));
    if (!EFI_ERROR(s)) return;

    blit_rows(state, y, h);
}

EFI_STATUS gui_init(gui_state_t *state) {

    EFI_STATUS status = BS->HandleProtocol(
        ST->ConsoleOutHandle,
        &gEfiGraphicsOutputProtocolGuid,
        (void**)&state->gop
    );

    if (EFI_ERROR(status)) {

        UINTN count;
        EFI_HANDLE *handles = efi_locate_handle_buffer(&gEfiGraphicsOutputProtocolGuid, &count);
        if (!handles) {
            return EFI_NOT_FOUND;
        }
        for (UINTN i = 0; i < count; i++) {
            status = BS->HandleProtocol(handles[i], &gEfiGraphicsOutputProtocolGuid, (void**)&state->gop);
            if (!EFI_ERROR(status)) break;
        }
        efi_free_pool(handles);
    }

    if (EFI_ERROR(status) || !state->gop) {
        return EFI_NOT_FOUND;
    }

    state->screen_width = state->gop->Mode->Info->HorizontalResolution;
    state->screen_height = state->gop->Mode->Info->VerticalResolution;
    state->bpp = 32;
    state->pixel_format = state->gop->Mode->Info->PixelFormat;

    {
        CHAR16 g[160];
        SPrint(g, sizeof(g),
               L"   GOP %dx%d pxfmt=%d ppsl=%d fb=%lx",
               (int)state->screen_width, (int)state->screen_height,
               (int)state->pixel_format,
               (int)state->gop->Mode->Info->PixelsPerScanLine,
               (UINT64)state->gop->Mode->FrameBufferBase);
        efi_log(g);
    }

    state->pixels_per_scanline = state->gop->Mode->Info->PixelsPerScanLine;
    if (state->pixels_per_scanline < state->screen_width) {
        state->pixels_per_scanline = state->screen_width;
    }

    state->bg_color = COLOR_BLACK;
    state->fg_color = COLOR_WHITE;
    state->highlight_color = COLOR_BLUE;
    state->blur = 0;
    state->blur_title = 0;
    state->blur_color = COLOR_WHITE;
    state->anim_speed = 0;
    state->anim_cross = 0;
    state->anim_frames = 12;

    state->selected = 0;
    state->per_page = 3;
    state->prev_page = 0;
    state->prev_selected = 0;
    state->page_anim = 0;
    state->page_frame = 0;
    state->page_old = 0;
    state->page_old_sel = 0;
    state->entries = NULL;
    state->entry_count = 0;
    state->timeout = 0;
    state->timeout_active = 1;
    state->running = 1;
    state->action = VISOR_ACTION_BOOT;
    state->focus = FOCUS_ENTRIES;
    state->prev_focus = FOCUS_ENTRIES;
    state->power_sel = 0;
    for (int i = 0; i < 9; i++) { state->anim_cur[i] = state->anim_from[i] = state->anim_to[i] = 0; }
    state->prev_box_y0 = 0;
    state->prev_box_y1 = 0;
    state->anim_frame = 0;
    state->anim_active = 0;
    state->anim_init = 0;
    state->anim_power = 0;
    state->band_n = 0;
    for (int i = 0; i < 4; i++) { state->band_y[i] = 0; state->band_h[i] = 0; }
    state->prev_ul_y = 0;
    state->title = NULL;
    state->show_title = 1;
    state->show_names = 1;
    state->center_info = 0;
    state->box_radius = 0;
    state->title_color = COLOR_WHITE;
    state->name_color = COLOR_WHITE;
    state->title_size = 0;
    state->name_size = 0;

    state->icon_size = 0;
    state->icon_spacing = 0;
    state->icon_y = 0;

    state->underline_color = COLOR_BLUE;
    state->underline_thickness = 0;
    state->underline_length = 0;

    state->power_position = POWER_POS_BOTTOMRIGHT;
    state->shutdown_color = COLOR_BLUE;
    state->reboot_color = COLOR_BLUE;
    state->firmware_color = COLOR_BLUE;

    state->power_icons = 0;
    state->power_icon_size = 0;
    state->shutdown_icon = NULL;
    state->reboot_icon = NULL;
    state->firmware_icon = NULL;

    state->background = NULL;
    state->background_path = NULL;

    state->backbuffer = efi_allocate_pool(
        state->screen_width * state->screen_height * sizeof(UINT32));
    if (!state->backbuffer) return EFI_OUT_OF_RESOURCES;

    state->scene_cache = efi_allocate_pool(
        state->screen_width * state->screen_height * sizeof(UINT32));
    state->blur_cache = NULL;
    state->scene_valid = 0;

    gui_fill_rect(state, 0, 0, state->screen_width, state->screen_height, state->bg_color);
    gui_present(state);

    return EFI_SUCCESS;
}

EFI_STATUS gui_set_mode(gui_state_t *state, UINTN want_w, UINTN want_h, int want_max) {
    if (!state->gop) return EFI_NOT_FOUND;

    UINT32 maxmode = state->gop->Mode->MaxMode;
    UINT32 cur = state->gop->Mode->Mode;
    UINT32 best = cur;
    int found = 0;
    UINTN best_px = 0;

    for (UINT32 m = 0; m < maxmode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        UINTN sz = 0;
        if (EFI_ERROR(state->gop->QueryMode(state->gop, m, &sz, &info)) || !info)
            continue;
        UINTN mw = info->HorizontalResolution, mh = info->VerticalResolution;
        if (want_max) {
            UINTN px = mw * mh;
            if (px > best_px) { best_px = px; best = m; found = 1; }
        } else if (mw == want_w && mh == want_h) {
            best = m; found = 1; break;
        }
    }

    if (!found) {
        efi_log(L"WARN: requested resolution not available - keeping current mode");
        return EFI_NOT_FOUND;
    }

    if (best != cur) {
        if (EFI_ERROR(state->gop->SetMode(state->gop, best))) {
            efi_log(L"WARN: SetMode failed - keeping current mode");
            return EFI_DEVICE_ERROR;
        }
    }

    state->screen_width  = state->gop->Mode->Info->HorizontalResolution;
    state->screen_height = state->gop->Mode->Info->VerticalResolution;
    state->pixel_format  = state->gop->Mode->Info->PixelFormat;
    state->pixels_per_scanline = state->gop->Mode->Info->PixelsPerScanLine;
    if (state->pixels_per_scanline < state->screen_width)
        state->pixels_per_scanline = state->screen_width;

    {
        CHAR16 g[96];
        SPrint(g, sizeof(g), L"   GOP mode set to %dx%d (pxfmt=%d ppsl=%d)",
               (int)state->screen_width, (int)state->screen_height,
               (int)state->pixel_format, (int)state->pixels_per_scanline);
        efi_log(g);
    }

    UINTN px = state->screen_width * state->screen_height;
    if (state->backbuffer)  efi_free_pool(state->backbuffer);
    if (state->scene_cache) efi_free_pool(state->scene_cache);
    if (state->blur_cache)  { efi_free_pool(state->blur_cache); state->blur_cache = NULL; }

    state->backbuffer  = efi_allocate_pool(px * sizeof(UINT32));
    state->scene_cache = efi_allocate_pool(px * sizeof(UINT32));
    state->scene_valid = 0;
    if (!state->backbuffer) return EFI_OUT_OF_RESOURCES;

    gui_fill_rect(state, 0, 0, state->screen_width, state->screen_height, state->bg_color);
    gui_present(state);
    return EFI_SUCCESS;
}

void gui_fill_rect(gui_state_t *state, UINTN x, UINTN y, UINTN w, UINTN h, color_t color) {
    UINT32 pixel = color_to_u32(color);
    for (UINTN j = y; j < y + h && j < state->screen_height; j++) {
        for (UINTN i = x; i < x + w && i < state->screen_width; i++) {
            UINT32 *p = get_pixel(state, i, j);
            if (p) *p = pixel;
        }
    }
}

static void fill_rect_alpha(gui_state_t *state, INTN x, INTN y, INTN w, INTN h,
                            color_t color, UINT8 alpha) {
    for (INTN j = y; j < y + h; j++) {
        if (j < 0 || j >= (INTN)state->screen_height) continue;
        for (INTN i = x; i < x + w; i++) {
            if (i < 0 || i >= (INTN)state->screen_width) continue;
            UINT32 *p = get_pixel(state, i, j);
            if (!p) continue;
            UINT8 br = (*p >> 16) & 0xFF, bg = (*p >> 8) & 0xFF, bb = *p & 0xFF;
            UINT8 r = (color.r * alpha + br * (255 - alpha)) / 255;
            UINT8 g = (color.g * alpha + bg * (255 - alpha)) / 255;
            UINT8 b = (color.b * alpha + bb * (255 - alpha)) / 255;
            *p = (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

static void fill_round_rect(gui_state_t *state, INTN x, INTN y, INTN w, INTN h,
                            INTN r, color_t color, UINT8 alpha) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (INTN j = 0; j < h; j++) {
        INTN inset = 0;
        if (j < r)              { INTN dy = r - 1 - j; inset = r - (INTN)isqrt_(r*r - dy*dy); }
        else if (j >= h - r)    { INTN dy = j - (h - r); inset = r - (INTN)isqrt_(r*r - dy*dy); }
        fill_rect_alpha(state, x + inset, y + j, w - 2 * inset, 1, color, alpha);
    }
}

static UINT32* icon_build_scaled(icon_t *icon, UINTN size) {
    if (icon->scaled && icon->scaled_size == size) return icon->scaled;
    if (icon->scaled) { efi_free_pool(icon->scaled); icon->scaled = NULL; icon->scaled_size = 0; }

    UINT32 *out = efi_allocate_pool(size * size * sizeof(UINT32));
    if (!out) return NULL;

    UINTN iw = icon->width, ih = icon->height;
    for (UINTN j = 0; j < size; j++) {
        UINTN sy0 = j * ih / size;
        UINTN sy1 = (j + 1) * ih / size;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > ih) sy1 = ih;
        for (UINTN i = 0; i < size; i++) {
            UINTN sx0 = i * iw / size;
            UINTN sx1 = (i + 1) * iw / size;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > iw) sx1 = iw;

            UINT64 ar = 0, ag = 0, ab = 0, aa = 0; UINTN n = 0;
            for (UINTN sy = sy0; sy < sy1; sy++) {
                const UINT32 *row = icon->pixels + sy * iw;
                for (UINTN sx = sx0; sx < sx1; sx++) {
                    UINT32 p = row[sx];
                    UINT32 a = (p >> 24) & 0xFF;
                    ar += ((p >> 16) & 0xFF) * a;
                    ag += ((p >> 8) & 0xFF) * a;
                    ab += (p & 0xFF) * a;
                    aa += a;
                    n++;
                }
            }
            UINT8 cov = (n == 0) ? 0 : (UINT8)(aa / n);
            UINT8 sr, sg, sb;
            if (aa == 0) { sr = sg = sb = 0; }
            else { sr = (UINT8)(ar / aa); sg = (UINT8)(ag / aa); sb = (UINT8)(ab / aa); }
            out[j * size + i] = ((UINT32)cov << 24) | ((UINT32)sr << 16)
                              | ((UINT32)sg << 8) | sb;
        }
    }
    icon->scaled = out;
    icon->scaled_size = size;
    return out;
}

static void draw_image_sized_a(gui_state_t *state, icon_t *icon,
                               UINTN x, UINTN y, UINTN size, INTN master) {
    if (!icon || !icon->pixels || icon->width == 0 || icon->height == 0 || size == 0)
        return;
    if (master <= 0) return;
    if (master > 255) master = 255;

    UINT32 *sc = icon_build_scaled(icon, size);
    if (!sc) return;

    for (UINTN j = 0; j < size && (y + j) < state->screen_height; j++) {
        for (UINTN i = 0; i < size && (x + i) < state->screen_width; i++) {
            UINT32 p = sc[j * size + i];
            UINTN cov = (p >> 24) & 0xFF;
            cov = cov * (UINTN)master / 255;
            if (cov == 0) continue;
            UINT8 sr = (p >> 16) & 0xFF, sg = (p >> 8) & 0xFF, sb = p & 0xFF;

            UINT32 *dest = get_pixel(state, x + i, y + j);
            if (!dest) continue;
            UINT8 br = (*dest >> 16) & 0xFF, bg = (*dest >> 8) & 0xFF, bb = *dest & 0xFF;
            UINT8 nr = (UINT8)((sr * cov + br * (255 - cov)) / 255);
            UINT8 ng = (UINT8)((sg * cov + bg * (255 - cov)) / 255);
            UINT8 nb = (UINT8)((sb * cov + bb * (255 - cov)) / 255);
            *dest = (0xFFu << 24) | (nr << 16) | (ng << 8) | nb;
        }
    }
}

static void draw_image_sized(gui_state_t *state, icon_t *icon,
                             UINTN x, UINTN y, UINTN size) {
    draw_image_sized_a(state, icon, x, y, size, 255);
}

void gui_draw_image(gui_state_t *state, icon_t *icon, UINTN x, UINTN y) {
    draw_image_sized(state, icon, x, y, ICON_SIZE);
}

static UINTN px_height(UINTN scale) { return scale * 16; }

static UINT8 sample_cov(const unsigned char *cov, UINTN w, UINTN h,
                        UINTN sx256, UINTN sy256) {
    UINTN x0 = sx256 >> 8, y0 = sy256 >> 8;
    if (x0 >= w) x0 = w ? w - 1 : 0;
    if (y0 >= h) y0 = h ? h - 1 : 0;
    UINTN x1 = (x0 + 1 < w) ? x0 + 1 : x0;
    UINTN y1 = (y0 + 1 < h) ? y0 + 1 : y0;
    UINTN fx = sx256 & 0xFF, fy = sy256 & 0xFF;
    UINTN c00 = cov[y0 * w + x0], c10 = cov[y0 * w + x1];
    UINTN c01 = cov[y1 * w + x0], c11 = cov[y1 * w + x1];
    UINTN top = c00 + ((c10 - c00) * fx >> 8);
    UINTN bot = c01 + ((c11 - c01) * fx >> 8);
    return (UINT8)(top + (((INTN)bot - (INTN)top) * (INTN)fy >> 8));
}

static UINTN blend_glyph(gui_state_t *state, const glyph_t *g, UINT32 rgb,
                         INTN dx, INTN dyTop, UINTN size_px, UINTN dh, INTN master) {
    if (g->w == 0 || g->h == 0) return g->advance * dh / size_px;
    if (!g_glyph_cov) return g->advance * dh / size_px;
    const unsigned char *cov = g_glyph_cov + g->pixel_offset;
    UINTN dw = (UINTN)g->w * dh / size_px;
    UINTN ddh = (UINTN)g->h * dh / size_px;
    if (dw == 0 || ddh == 0) return g->advance * dh / size_px;

    UINT8 fr = (rgb >> 16) & 0xFF, fg = (rgb >> 8) & 0xFF, fb = rgb & 0xFF;
    for (UINTN j = 0; j < ddh; j++) {
        UINTN sy = (j * g->h * 256) / ddh;
        INTN py = dyTop + (INTN)j;
        if (py < 0 || py >= (INTN)state->screen_height) continue;
        for (UINTN i = 0; i < dw; i++) {
            UINTN sx = (i * g->w * 256) / dw;
            UINT8 a = sample_cov(cov, g->w, g->h, sx, sy);
            if (master < 255) a = (UINT8)((UINTN)a * (UINTN)master / 255);
            if (!a) continue;
            INTN px = dx + (INTN)i;
            if (px < 0 || px >= (INTN)state->screen_width) continue;
            UINT32 *p = get_pixel(state, px, py);
            if (!p) continue;
            UINT8 br = (*p >> 16) & 0xFF, bg = (*p >> 8) & 0xFF, bb = *p & 0xFF;
            UINT8 r = (fr * a + br * (255 - a)) / 255;
            UINT8 gg = (fg * a + bg * (255 - a)) / 255;
            UINT8 b = (fb * a + bb * (255 - a)) / 255;
            *p = (0xFFu << 24) | (r << 16) | (gg << 8) | b;
        }
    }
    return g->advance * dh / size_px;
}

static UINTN text_width_px(CHAR16 *text, UINTN dh) {
    if (!text) return 0;
    UINTN w = 0;
    while (*text) {
        CHAR16 c = *text++;
        if (c < g_font->first || c > g_font->last) c = '?';
        const glyph_t *g = &g_font->glyphs[c - g_font->first];
        w += (UINTN)g->advance * dh / g_font->size;
    }
    return w;
}

static void draw_text_px_a(gui_state_t *state, CHAR16 *text, UINTN x, UINTN y,
                           color_t color, UINTN dh, INTN master) {
    if (!text || master <= 0) return;
    if (master > 255) master = 255;
    font_ensure_decoded();
    UINT32 rgb = color_to_u32(color) & 0x00FFFFFF;
    UINTN size = g_font->size;
    UINTN baseline = y + (UINTN)g_font->ascent * dh / size;
    INTN pen = (INTN)x;
    while (*text) {
        CHAR16 c = *text++;
        if (c < g_font->first || c > g_font->last) c = '?';
        const glyph_t *g = &g_font->glyphs[c - g_font->first];
        INTN gx = pen + (INTN)((INTN)g->left * (INTN)dh / (INTN)size);
        INTN gyTop = (INTN)baseline - (INTN)((INTN)g->top * (INTN)dh / (INTN)size);
        pen += (INTN)blend_glyph(state, g, rgb, gx, gyTop, size, dh, master);
    }
}

static void draw_text_px(gui_state_t *state, CHAR16 *text, UINTN x, UINTN y,
                         color_t color, UINTN dh) {
    draw_text_px_a(state, text, x, y, color, dh, 255);
}

static UINTN text_width(CHAR16 *text, UINTN scale) {
    return text_width_px(text, px_height(scale));
}
static void draw_text_scaled(gui_state_t *state, CHAR16 *text, UINTN x, UINTN y,
                             color_t color, UINTN scale) {
    draw_text_px(state, text, x, y, color, px_height(scale));
}
static void draw_text_centered(gui_state_t *state, CHAR16 *text, UINTN x, UINTN w,
                               UINTN y, color_t color, UINTN scale) {
    UINTN tw = text_width_px(text, px_height(scale));
    UINTN tx = (tw < w) ? x + (w - tw) / 2 : x;
    draw_text_px(state, text, tx, y, color, px_height(scale));
}

void gui_draw_text_small(gui_state_t *state, CHAR16 *text, UINTN x, UINTN y,
                         color_t color) {
    draw_text_px(state, text, x, y, color, 16);
}
void gui_draw_text_large(gui_state_t *state, CHAR16 *text, UINTN x, UINTN y,
                         color_t color) {
    draw_text_px(state, text, x, y, color, 32);
}

icon_t* gui_load_image(CHAR16 *path) {
    efi_log(L"  image: opening file");
    efi_log(path);
    efi_file_buffer_t *buf = efi_load_file(path);
    if (!buf) { efi_log(L"  ERROR: image file not found or unreadable"); return NULL; }
    { CHAR16 d[64]; SPrint(d, sizeof(d), L"  image: read %d bytes", (int)buf->size); efi_log(d); }

    UINT8 *data = (UINT8*)buf->data;

    UINT8 png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    INTN is_png = 1;
    for (int i = 0; i < 8; i++) {
        if (data[i] != png_sig[i]) {
            is_png = 0;
            break;
        }
    }

    if (is_png) {
        icon_t *icon = png_load(data, buf->size);
        efi_free_pool(buf);
        if (!icon) efi_log(L"  ERROR: PNG decode failed");
        return icon;
    }

    if (data[0] != 'B' || data[1] != 'M') {
        efi_log(L"  ERROR: image is neither PNG nor BMP");
        efi_free_pool(buf);
        return NULL;
    }

    UINT32 width = *(UINT32*)(data + 18);
    UINT32 height = *(UINT32*)(data + 22);
    UINT16 bpp = *(UINT16*)(data + 28);
    UINT32 compression = *(UINT32*)(data + 30);
    UINT32 data_offset = *(UINT32*)(data + 10);

    if (compression != 0 || (bpp != 24 && bpp != 32)) {
        efi_free_pool(buf);
        return NULL;
    }

    icon_t *icon = efi_allocate_pool(sizeof(icon_t));
    if (!icon) { efi_free_pool(buf); return NULL; }
    icon->width = width;
    icon->height = height;
    icon->scaled_size = 0;
    icon->scaled = NULL;
    icon->pixels = efi_allocate_pool(width * height * sizeof(UINT32));
    if (!icon->pixels) { efi_free_pool(icon); efi_free_pool(buf); return NULL; }

    UINT8 *src = data + data_offset;
    UINTN row_padding = (4 - (width * (bpp / 8)) % 4) % 4;

    for (UINTN y = 0; y < height; y++) {
        for (UINTN x = 0; x < width; x++) {
            UINT8 *pixel;
            if (bpp == 32) {
                pixel = src + ((height - 1 - y) * (width * 4 + row_padding * height) + x * 4);
            } else {

                pixel = src + ((height - 1 - y) * (width * 3 + row_padding) + x * 3);
            }

            icon->pixels[y * width + x] = (0xFF << 24) | (pixel[2] << 16) | (pixel[1] << 8) | pixel[0];
        }
    }

    efi_free_pool(buf);
    return icon;
}

icon_t* gui_load_icon(CHAR16 *path) {
    return gui_load_image(path);
}

#define DEFAULT_BACKGROUND_PATH L"\\EFI\\visor\\backgrounds\\default.png"

void gui_set_background(gui_state_t *state, CHAR16 *path) {
    if (state->background && state->background->pixels) {
        if (state->background->scaled) efi_free_pool(state->background->scaled);
        efi_free_pool(state->background->pixels);
        efi_free_pool(state->background);
    }
    if (state->background_path) {
        efi_free_pool(state->background_path);
    }

    state->background_path = efi_strdup(path);
    state->background = gui_load_image(path);

    if (!state->background && efi_strcmp(path, DEFAULT_BACKGROUND_PATH) != 0) {
        efi_log(L"  WARN: background unusable - falling back to default background");
        state->background = gui_load_image(DEFAULT_BACKGROUND_PATH);
        if (state->background)
            efi_log(L"  background: default fallback loaded");
        else
            efi_log(L"  WARN: default background missing too - using solid colour");
    }
}

void gui_draw_background(gui_state_t *state) {
    if (!state->background || !state->background->pixels) {

        gui_fill_rect(state, 0, 0, state->screen_width, state->screen_height, state->bg_color);
        return;
    }

    icon_t *bg = state->background;
    UINTN dst_width = state->screen_width;
    UINTN dst_height = state->screen_height;

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
}

static const struct { CHAR16 *label; int action; } POWER_ACTIONS[] = {
    { L"Shutdown", VISOR_ACTION_SHUTDOWN },
    { L"Reboot",   VISOR_ACTION_REBOOT   },
    { L"Firmware", VISOR_ACTION_FIRMWARE },
};
#define POWER_ACTION_COUNT 3

#define POWER_SCALE 2

static void layout_power(gui_state_t *state) {
    UINTN scale   = POWER_SCALE;
    UINTN th      = px_height(scale);
    UINTN line_h  = th + 18;
    UINTN margin  = 30;

    icon_t *icon[POWER_ACTION_COUNT] = {
        state->shutdown_icon, state->reboot_icon, state->firmware_icon
    };
    UINTN isz       = state->power_icon_size ? state->power_icon_size : 40;
    UINTN icon_line = isz + 16;

    UINTN block_h = 0;
    for (UINTN i = 0; i < POWER_ACTION_COUNT; i++)
        block_h += (state->power_icons && icon[i]) ? icon_line : line_h;

    int right_side = (state->power_position == POWER_POS_BOTTOMRIGHT ||
                      state->power_position == POWER_POS_TOPRIGHT);
    int top_side   = (state->power_position == POWER_POS_TOPRIGHT ||
                      state->power_position == POWER_POS_TOPLEFT);

    INTN top = top_side ? (INTN)margin
                        : (INTN)(state->screen_height - margin - block_h);
    INTN y = top;

    for (UINTN i = 0; i < POWER_ACTION_COUNT; i++) {
        if (state->power_icons && icon[i]) {
            INTN x = right_side ? (INTN)(state->screen_width - margin - isz) : (INTN)margin;
            state->pwr_x[i] = x; state->pwr_y[i] = y;
            state->pwr_w[i] = (INTN)isz; state->pwr_h[i] = (INTN)isz;
            y += icon_line;
        } else {
            UINTN tw = text_width(POWER_ACTIONS[i].label, scale);
            INTN  x  = right_side ? (INTN)(state->screen_width - margin - tw) : (INTN)margin;
            state->pwr_x[i] = x; state->pwr_y[i] = y;
            state->pwr_w[i] = (INTN)tw; state->pwr_h[i] = (INTN)th;
            y += line_h;
        }
    }
    state->pwr_y0 = top;
    state->pwr_y1 = top + (INTN)block_h;
}

static void draw_power_actions(gui_state_t *state, int focus_idx) {
    UINTN scale = POWER_SCALE;
    icon_t *icon[POWER_ACTION_COUNT] = {
        state->shutdown_icon, state->reboot_icon, state->firmware_icon
    };
    color_t key_color[POWER_ACTION_COUNT] = {
        state->shutdown_color, state->reboot_color, state->firmware_color
    };
    color_t dim = { 0xC0, 0xC0, 0xC8 };

    for (UINTN i = 0; i < POWER_ACTION_COUNT; i++) {
        int focused = ((int)i == focus_idx);
        if (state->power_icons && icon[i]) {
            draw_image_sized(state, icon[i], state->pwr_x[i], state->pwr_y[i],
                             (UINTN)state->pwr_w[i]);
        } else {
            CHAR16 *label = POWER_ACTIONS[i].label;
            INTN x = state->pwr_x[i], y = state->pwr_y[i];

            CHAR16 first[2] = { label[0], 0 };
            draw_text_scaled(state, first, x, y, key_color[i], scale);
            draw_text_scaled(state, label + 1, x + (8 + 2) * scale, y,
                             focused ? key_color[i] : dim, scale);
        }
    }
}

static void scene_restore_band(gui_state_t *state, INTN y, INTN h) {
    if (!state->scene_cache) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > (INTN)state->screen_height) h = (INTN)state->screen_height - y;
    UINTN W = state->screen_width;
    for (INTN row = y; row < y + h; row++) {
        UINT32 *d = state->backbuffer + (UINTN)row * W;
        UINT32 *s = state->scene_cache + (UINTN)row * W;
        for (UINTN i = 0; i < W; i++) d[i] = s[i];
    }
}

#define A_CARDX 0
#define A_CARDA 1
#define A_ULX   2
#define A_ULY   3
#define A_ULW   4
#define A_BOXX  5
#define A_BOXY  6
#define A_BOXW  7
#define A_BOXH  8

#define FROST_RADIUS 16

static void box_blur_pass(gui_state_t *state, UINT32 *src, UINT32 *dst, INTN rad) {
    UINTN W = state->screen_width, H = state->screen_height;
    for (UINTN y = 0; y < H; y++) {
        UINT32 *s = src + y * W;
        UINT32 *d = dst + y * W;
        UINT32 sr = 0, sg = 0, sb = 0;
        INTN win = 2 * rad + 1;
        for (INTN i = -rad; i <= rad; i++) {
            UINTN xi = (i < 0) ? 0 : ((UINTN)i >= W ? W - 1 : (UINTN)i);
            UINT32 p = s[xi];
            sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
        }
        for (UINTN x = 0; x < W; x++) {
            d[x] = (0xFFu << 24) | (((sr / win) & 0xFF) << 16)
                 | (((sg / win) & 0xFF) << 8) | ((sb / win) & 0xFF);
            UINTN xa = x + rad + 1; if (xa >= W) xa = W - 1;
            INTN xrm = (INTN)x - rad; UINTN xr = (xrm < 0) ? 0 : (UINTN)xrm;
            UINT32 pa = s[xa], pr = s[xr];
            sr += ((pa >> 16) & 0xFF) - ((pr >> 16) & 0xFF);
            sg += ((pa >> 8) & 0xFF) - ((pr >> 8) & 0xFF);
            sb += (pa & 0xFF) - (pr & 0xFF);
        }
    }
}

static void box_blur_vpass(gui_state_t *state, UINT32 *src, UINT32 *dst, INTN rad) {
    UINTN W = state->screen_width, H = state->screen_height;
    for (UINTN x = 0; x < W; x++) {
        UINT32 sr = 0, sg = 0, sb = 0;
        INTN win = 2 * rad + 1;
        for (INTN i = -rad; i <= rad; i++) {
            UINTN yi = (i < 0) ? 0 : ((UINTN)i >= H ? H - 1 : (UINTN)i);
            UINT32 p = src[yi * W + x];
            sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
        }
        for (UINTN y = 0; y < H; y++) {
            dst[y * W + x] = (0xFFu << 24) | (((sr / win) & 0xFF) << 16)
                           | (((sg / win) & 0xFF) << 8) | ((sb / win) & 0xFF);
            UINTN ya = y + rad + 1; if (ya >= H) ya = H - 1;
            INTN yrm = (INTN)y - rad; UINTN yr = (yrm < 0) ? 0 : (UINTN)yrm;
            UINT32 pa = src[ya * W + x], pr = src[yr * W + x];
            sr += ((pa >> 16) & 0xFF) - ((pr >> 16) & 0xFF);
            sg += ((pa >> 8) & 0xFF) - ((pr >> 8) & 0xFF);
            sb += (pa & 0xFF) - (pr & 0xFF);
        }
    }
}

static void build_blur_cache(gui_state_t *state) {
    if (!state->blur_cache) {
        state->blur_cache = efi_allocate_pool(
            state->screen_width * state->screen_height * sizeof(UINT32));
    }
    if (!state->blur_cache || !state->scene_cache) return;
    box_blur_pass(state, state->backbuffer, state->scene_cache, 14);
    box_blur_vpass(state, state->scene_cache, state->blur_cache, 14);
    box_blur_pass(state, state->blur_cache, state->scene_cache, 14);
    box_blur_vpass(state, state->scene_cache, state->blur_cache, 14);
}

static void draw_frost(gui_state_t *state, INTN x, INTN y, INTN w, INTN h, INTN a) {
    if (a <= 0 || w <= 0 || h <= 0) return;
    if (a > 255) a = 255;
    int clear = (state->blur == 2);
    color_t tint = state->blur_color;
    INTN r = state->box_radius ? (INTN)state->box_radius : FROST_RADIUS;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    UINTN W = state->screen_width;
    INTN base_fill = clear ? (a * 255 / 255) : (a * 220 / 255);
    INTN tint_a = clear ? 0 : 34;
    INTN lift   = clear ? 0 : 8;
    INTN feather = 10;
    for (INTN j = 0; j < h; j++) {
        INTN inset = 0;
        if (j < r)            { INTN dy = r - 1 - j; INTN q = r*r - dy*dy; inset = r - (INTN)isqrt_(q > 0 ? q : 0); }
        else if (j >= h - r)  { INTN dy = j - (h - r); INTN q = r*r - dy*dy; inset = r - (INTN)isqrt_(q > 0 ? q : 0); }
        INTN yy = y + j;
        if (yy < 0 || yy >= (INTN)state->screen_height) continue;
        INTN edy = (j < h - 1 - j) ? j : (h - 1 - j);
        for (INTN i = inset; i < w - inset; i++) {
            INTN xx = x + i;
            UINT32 *p = get_pixel(state, xx, yy);
            if (!p) continue;
            INTN edx = (i - inset < (w - inset - 1) - i) ? (i - inset) : ((w - inset - 1) - i);
            INTN ed = (edx < edy) ? edx : edy;
            INTN fill_a = base_fill;
            if (ed < feather) fill_a = base_fill * ed / feather;
            if (fill_a <= 0) continue;
            UINT32 src;
            if (state->blur_cache) src = state->blur_cache[(UINTN)yy * W + (UINTN)xx];
            else                   src = color_to_u32(state->bg_color);
            INTN sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
            sr += lift; sg += lift; sb += lift;
            sr = (sr * (255 - tint_a) + tint.r * tint_a) / 255;
            sg = (sg * (255 - tint_a) + tint.g * tint_a) / 255;
            sb = (sb * (255 - tint_a) + tint.b * tint_a) / 255;
            if (sr > 255) sr = 255;
            if (sg > 255) sg = 255;
            if (sb > 255) sb = 255;
            UINT8 br = (*p >> 16) & 0xFF, bgc = (*p >> 8) & 0xFF, bb = *p & 0xFF;
            UINT8 nr = (UINT8)((sr * fill_a + br * (255 - fill_a)) / 255);
            UINT8 ng = (UINT8)((sg * fill_a + bgc * (255 - fill_a)) / 255);
            UINT8 nb = (UINT8)((sb * fill_a + bb * (255 - fill_a)) / 255);
            *p = (0xFFu << 24) | (nr << 16) | (ng << 8) | nb;
        }
    }
}

static boot_entry_t* entry_at(gui_state_t *state, UINTN idx) {
    boot_entry_t *e = state->entries;
    for (UINTN i = 0; i < idx && e; i++) e = e->next;
    return e;
}

static void draw_page(gui_state_t *state, UINTN start, UINTN n, UINTN sel_local,
                      UINTN is, UINTN isp, UINTN max_ei, UINTN icon_cy,
                      UINTN name_px, UINTN ul_th, UINTN ul_len_cfg, INTN pad,
                      INTN master) {
    if (master <= 0 || n == 0) return;
    if (master > 255) master = 255;

    UINTN row_top = (icon_cy > max_ei / 2) ? icon_cy - max_ei / 2 : 0;
    UINTN ul_y    = row_top + max_ei + 10;
    UINTN name_y  = ul_y + ul_th + 8;

    UINTN total_w = 0, sel_ei = is;
    INTN  sel_left = 0;
    {
        boot_entry_t *t = entry_at(state, start);
        UINTN x = 0;
        for (UINTN i = 0; i < n && t; i++) {
            UINTN ei = t->icon_size ? t->icon_size : is;
            total_w += ei + (i + 1 < n ? isp : 0);
            x += (i ? isp : 0);
            if (i == sel_local) { sel_left = (INTN)x; sel_ei = ei; }
            x += ei;
            t = t->next;
        }
    }
    UINTN start_x = (state->screen_width > total_w) ? (state->screen_width - total_w) / 2 : 0;
    sel_left += (INTN)start_x;

    INTN  ecard_top = (INTN)icon_cy - (INTN)sel_ei / 2 - pad;
    INTN  ecard_bot = (INTN)name_y + (INTN)name_px + pad / 2;
    UINTN ul_len = ul_len_cfg ? ul_len_cfg : (sel_ei + 2 * pad - 20);
    INTN  ulx = sel_left + (INTN)sel_ei / 2 - (INTN)ul_len / 2;
    UINTN ul_rad = ul_th / 2; if (ul_rad > 2) ul_rad = 2;

    if (sel_local < n) {
        if (state->blur) {
            draw_frost(state, sel_left - pad, ecard_top,
                       (INTN)sel_ei + 2 * pad, ecard_bot - ecard_top, master);
        } else {
            fill_round_rect(state, sel_left - pad, ecard_top,
                            (INTN)sel_ei + 2 * pad, ecard_bot - ecard_top,
                            state->box_radius ? (INTN)state->box_radius : 14,
                            COLOR_WHITE, (UINT8)(38 * master / 255));
        }
        fill_round_rect(state, ulx, (INTN)ul_y, (INTN)ul_len, (INTN)ul_th,
                        (INTN)ul_rad, state->underline_color,
                        (UINT8)(230 * master / 255));
    }

    boot_entry_t *e = entry_at(state, start);
    UINTN x = start_x;
    for (UINTN i = 0; i < n && e; i++) {
        UINTN ei = e->icon_size ? e->icon_size : is;
        UINTN iy = (icon_cy > ei / 2) ? icon_cy - ei / 2 : 0;
        if (e->icon) {
            draw_image_sized_a(state, e->icon, x, iy, ei, master);
        } else {
            color_t ph = e->type == 0 ? COLOR_GREEN : COLOR_RED;
            fill_round_rect(state, (INTN)x, (INTN)iy, (INTN)ei, (INTN)ei,
                            12, ph, (UINT8)master);
        }
        if (state->show_names) {
            color_t name_col;
            if (e->has_color) name_col = e->color;
            else if (i == sel_local) name_col = state->name_color;
            else name_col = (color_t){ state->name_color.r * 7 / 10,
                                       state->name_color.g * 7 / 10,
                                       state->name_color.b * 7 / 10 };
            UINTN nw = text_width_px(e->name, name_px);
            INTN  nx = (INTN)x + (INTN)ei / 2 - (INTN)nw / 2;
            draw_text_px_a(state, e->name, (UINTN)nx, name_y, name_col, name_px, master);
        }
        x += ei + isp;
        e = e->next;
    }
}

static void draw_chevrons(gui_state_t *state, UINTN page, UINTN per_page,
                          UINTN start_x, UINTN total_w, UINTN isp,
                          UINTN max_ei, UINTN icon_cy, INTN master) {
    UINTN csz = max_ei / 2; if (csz < 18) csz = 18;
    INTN  cy  = (INTN)icon_cy - (INTN)csz / 2;
    INTN  gap = (INTN)(isp ? isp : 24);
    color_t cc = { state->name_color.r * 7 / 10,
                   state->name_color.g * 7 / 10,
                   state->name_color.b * 7 / 10 };
    if (page > 0) {
        CHAR16 lt[] = L"<";
        UINTN cw = text_width_px(lt, csz);
        INTN lx = (INTN)start_x - gap - (INTN)cw;
        if (lx < 0) lx = 0;
        draw_text_px_a(state, lt, (UINTN)lx, (UINTN)cy, cc, csz, master);
    }
    if ((page + 1) * per_page < state->entry_count) {
        CHAR16 gt[] = L">";
        draw_text_px_a(state, gt, start_x + total_w + (UINTN)gap, (UINTN)cy, cc, csz, master);
    }
}

static UINTN center_info_block_h(gui_state_t *state, UINTN name_px) {
    UINTN path_px = (name_px * 4) / 5; if (path_px < 10) path_px = 10;
    return state->show_names ? path_px : (name_px + 6 + path_px);
}

static void draw_center_info(gui_state_t *state, boot_entry_t *e,
                             UINTN top_y, UINTN name_px, INTN master) {
    if (!e) return;
    if (master <= 0) return;
    if (master > 255) master = 255;

    int   want_name = !state->show_names;
    UINTN path_px = (name_px * 4) / 5; if (path_px < 10) path_px = 10;
    UINTN name_y  = top_y;
    UINTN path_y  = want_name ? top_y + name_px + 6 : top_y;

    color_t name_col = e->has_color ? e->color : state->name_color;
    color_t dim = { state->name_color.r * 7 / 10,
                    state->name_color.g * 7 / 10,
                    state->name_color.b * 7 / 10 };

    CHAR16 *path = e->kernel_path ? e->kernel_path : L"";
    UINTN plen = 0; while (path[plen]) plen++;
    UINTN maxw = state->screen_width * 9 / 10;

    CHAR16 tbuf[208];
    tbuf[0] = 0;
    UINTN off = 0;
    while (1) {
        UINTN k = 0;
        if (off > 0) { tbuf[k++] = '.'; tbuf[k++] = '.'; tbuf[k++] = '.'; }
        for (UINTN i = off; i < plen && k < 207; i++) tbuf[k++] = path[i];
        tbuf[k] = 0;
        if (off >= plen || text_width_px(tbuf, path_px) <= maxw) break;
        off += 4;
    }

    UINTN nw = want_name ? text_width_px(e->name, name_px) : 0;
    UINTN pw = text_width_px(tbuf, path_px);
    UINTN block_w = nw > pw ? nw : pw;
    UINTN block_h = (want_name ? name_px + 6 : 0) + path_px;
    INTN  cx = (INTN)state->screen_width / 2;

    if (state->blur) {
        INTN fpad = 16;
        draw_frost(state, cx - (INTN)block_w / 2 - fpad, (INTN)top_y - fpad,
                   (INTN)block_w + 2 * fpad, (INTN)block_h + 2 * fpad, master);
    }
    if (want_name)
        draw_text_px_a(state, e->name, (UINTN)(cx - (INTN)nw / 2), name_y, name_col, name_px, master);
    if (tbuf[0])
        draw_text_px_a(state, tbuf, (UINTN)(cx - (INTN)pw / 2), path_y, dim, path_px, master);
}

void gui_draw_menu(gui_state_t *state, int partial) {

    layout_power(state);

    UINTN px = state->screen_width * state->screen_height;
    int building = (!state->scene_cache) || (!state->scene_valid);
    if (building) {
        gui_draw_background(state);
        fill_rect_alpha(state, 0, 0, state->screen_width, state->screen_height,
                        COLOR_BLACK, 60);

        if (state->blur || state->blur_title)
            build_blur_cache(state);

        if (state->show_title) {
            CHAR16 *title = (state->title && state->title[0]) ? state->title : L"Visor";
            UINTN title_px = state->title_size ? state->title_size
                                               : state->screen_height / 12;
            UINTN tw = text_width_px(title, title_px);
            UINTN tx = (tw < state->screen_width) ? (state->screen_width - tw) / 2 : 0;
            UINTN ty = state->screen_height / 14;
            if (state->blur_title) {
                INTN pad = 18;
                draw_frost(state, (INTN)tx - pad, (INTN)ty - pad,
                           (INTN)tw + 2 * pad, (INTN)title_px + 2 * pad, 255);
            }
            draw_text_px(state, title, tx, ty, state->title_color, title_px);
        }

        draw_power_actions(state, -1);

        if (state->scene_cache) {
            for (UINTN i = 0; i < px; i++) state->scene_cache[i] = state->backbuffer[i];
            state->scene_valid = 1;
        }
    } else if (partial) {
        for (int b = 0; b < state->band_n; b++)
            scene_restore_band(state, state->band_y[b], state->band_h[b]);
    } else {
        for (UINTN i = 0; i < px; i++) state->backbuffer[i] = state->scene_cache[i];
    }

    if (state->entry_count == 0) {
        CHAR16 msg[] = L"No boot entries found";
        draw_text_centered(state, msg, 0, state->screen_width,
                           state->screen_height / 2, state->fg_color, 2);
        draw_power_actions(state, state->focus == FOCUS_POWER ? (int)state->power_sel : -1);
        return;
    }

    UINTN is      = state->icon_size    ? state->icon_size    : ICON_SIZE;
    UINTN isp     = state->icon_spacing ? state->icon_spacing : ICON_SPACING + 40;

    UINTN per_page = state->per_page ? state->per_page : 3;
    UINTN page = state->selected / per_page;
    UINTN page_start = page * per_page;
    UINTN page_n = state->entry_count - page_start;
    if (page_n > per_page) page_n = per_page;
    UINTN sel_local = state->selected - page_start;

    UINTN max_ei = is;
    {
        boot_entry_t *e = state->entries;
        for (UINTN i = 0; i < state->entry_count && e; i++) {
            UINTN ei = e->icon_size ? e->icon_size : is;
            if (ei > max_ei) max_ei = ei;
            e = e->next;
        }
    }

    UINTN total_w = 0, sel_ei = is;
    INTN  sel_left = 0;
    {
        UINTN x = 0;
        boot_entry_t *e = entry_at(state, page_start);
        for (UINTN i = 0; i < page_n && e; i++) {
            UINTN ei = e->icon_size ? e->icon_size : is;
            total_w += ei + (i + 1 < page_n ? isp : 0);
            x += (i ? isp : 0);
            if (i == sel_local) { sel_left = (INTN)x; sel_ei = ei; }
            x += ei;
            e = e->next;
        }
    }

    UINTN start_x = (state->screen_width > total_w) ? (state->screen_width - total_w) / 2 : 0;
    sel_left += (INTN)start_x;

    UINTN icon_cy = state->icon_y ? state->icon_y : state->screen_height / 2;
    UINTN row_top = (icon_cy > max_ei / 2) ? icon_cy - max_ei / 2 : 0;

    UINTN name_px = state->name_size ? state->name_size : 16;
    UINTN ul_th   = state->underline_thickness ? state->underline_thickness : 4;
    INTN  pad     = 16;
    UINTN ul_y    = row_top + max_ei + 10;
    UINTN name_y  = ul_y + ul_th + 8;
    UINTN ul_len  = state->underline_length ? state->underline_length
                                            : (sel_ei + 2 * pad - 20);

    UINTN ci_block_h = center_info_block_h(state, name_px);
    UINTN ci_margin  = 48;
    UINTN ci_top = (state->screen_height > ci_block_h + ci_margin)
                   ? state->screen_height - ci_margin - ci_block_h : name_y;
    INTN  ci_band_lo = (INTN)ci_top - pad - 2;
    INTN  ci_band_hi = (INTN)(ci_top + ci_block_h) + pad + 2;

    INTN sel_top  = (INTN)icon_cy - (INTN)sel_ei / 2;
    INTN ecard_top = sel_top - pad;
    INTN ecard_bot = (INTN)name_y + (INTN)name_px + pad / 2;

    INTN tgt[9];
    tgt[A_CARDX] = sel_left;
    tgt[A_CARDA] = (state->focus == FOCUS_ENTRIES) ? 38 : 0;
    if (state->focus == FOCUS_POWER) {
        UINTN ps = state->power_sel;
        tgt[A_ULX] = state->pwr_x[ps];
        tgt[A_ULY] = state->pwr_y[ps] + state->pwr_h[ps] + 4;
        tgt[A_ULW] = state->pwr_w[ps];
        INTN bpad = 10;
        tgt[A_BOXX] = state->pwr_x[ps] - bpad;
        tgt[A_BOXY] = state->pwr_y[ps] - bpad;
        tgt[A_BOXW] = state->pwr_w[ps] + 2 * bpad;
        tgt[A_BOXH] = state->pwr_h[ps] + 2 * bpad;
    } else {
        tgt[A_ULX] = sel_left + (INTN)sel_ei / 2 - (INTN)ul_len / 2;
        tgt[A_ULY] = (INTN)ul_y;
        tgt[A_ULW] = (INTN)ul_len;
        tgt[A_BOXX] = sel_left - pad;
        tgt[A_BOXY] = ecard_top;
        tgt[A_BOXW] = (INTN)sel_ei + 2 * pad;
        tgt[A_BOXH] = ecard_bot - ecard_top;
    }

    int N = state->anim_frames; if (N < 2) N = 2;

    int first = !state->anim_init;
    if (!first && page != state->prev_page && !state->page_anim) {
        state->page_anim = 1;
        state->page_frame = 0;
        state->page_old = state->prev_page;
        state->page_old_sel = state->prev_selected;
    }

    if (state->page_anim) {
        state->page_frame++;
        INTN fin = state->page_frame * 255 / N; if (fin > 255) fin = 255;
        INTN fout = 255 - fin;

        state->band_n = 1;
        state->band_y[0] = (INTN)row_top - pad - 2;
        state->band_h[0] = (INTN)(name_y + name_px + pad) + 2 - state->band_y[0];
        if (state->center_info) {
            state->band_y[1] = ci_band_lo;
            state->band_h[1] = ci_band_hi - ci_band_lo;
            state->band_n = 2;
        }

        UINTN old_start = state->page_old * per_page;
        UINTN old_n = state->entry_count - old_start;
        if (old_n > per_page) old_n = per_page;
        UINTN old_sel_local = (state->page_old_sel >= old_start)
                              ? state->page_old_sel - old_start : old_n;

        draw_page(state, old_start, old_n, old_sel_local, is, isp, max_ei, icon_cy,
                  name_px, ul_th, state->underline_length, pad, fout);
        draw_page(state, page_start, page_n, sel_local, is, isp, max_ei, icon_cy,
                  name_px, ul_th, state->underline_length, pad, fin);

        draw_chevrons(state, page, per_page, start_x, total_w, isp, max_ei, icon_cy, fin);

        if (state->center_info && state->entry_count > 0)
            draw_center_info(state, entry_at(state, state->selected),
                             ci_top, name_px, 255);

        if (state->page_frame >= N) {
            state->page_anim = 0;
            for (int k = 0; k < 9; k++)
                state->anim_cur[k] = state->anim_from[k] = state->anim_to[k] = tgt[k];
            state->anim_active = 0;
            state->anim_cross = 0;
            state->prev_ul_y   = state->anim_cur[A_ULY];
            state->prev_box_y0 = state->anim_cur[A_BOXY] - 6;
            state->prev_box_y1 = state->anim_cur[A_BOXY] + state->anim_cur[A_BOXH] + 6;
            state->prev_page = page;
            state->prev_selected = state->selected;
        }
        state->prev_focus = state->focus;
        return;
    }

    if (!state->anim_init) {
        for (int k = 0; k < 9; k++) state->anim_cur[k] = state->anim_to[k] = tgt[k];
        state->anim_init = 1;
        state->anim_active = 0;
        state->anim_cross = 0;
    } else {
        int changed = 0;
        for (int k = 0; k < 9; k++) if (tgt[k] != state->anim_to[k]) changed = 1;
        if (changed) {
            for (int k = 0; k < 9; k++) {
                state->anim_from[k] = state->anim_cur[k];
                state->anim_to[k]   = tgt[k];
            }
            state->anim_frame  = 0;
            state->anim_active = 1;
            int zc = ((state->focus == FOCUS_POWER) != (state->prev_focus == FOCUS_POWER));
            state->anim_cross = state->blur ? zc : 0;
        }
    }
    if (state->anim_active) {
        state->anim_frame++;
        if (state->anim_frame >= N) {
            for (int k = 0; k < 9; k++) state->anim_cur[k] = state->anim_to[k];
            state->anim_active = 0;
            state->anim_cross = 0;
        } else if (!state->anim_cross) {
            INTN t = state->anim_frame * 1000 / N;
            INTN e = (t < 500)
                   ? (2 * t * t) / 1000
                   : 1000 - ((2000 - 2 * t) * (2000 - 2 * t)) / 2000;
            for (int k = 0; k < 9; k++)
                state->anim_cur[k] = state->anim_from[k]
                                   + (state->anim_to[k] - state->anim_from[k]) * e / 1000;
        }
    }

    int cross = state->anim_active && state->anim_cross;
    INTN fin = cross ? (state->anim_frame * 255 / N) : 255;
    INTN fout = 255 - fin;

    INTN ilo[6], ihi[6]; int ni = 0;
    ilo[ni] = (INTN)row_top - pad - 2;
    ihi[ni] = (INTN)(name_y + name_px + pad) + 2; ni++;
    if (state->center_info) {
        ilo[ni] = ci_band_lo; ihi[ni] = ci_band_hi; ni++;
    }

    if (cross) {
        ilo[ni] = state->anim_from[A_BOXY] - 6;
        ihi[ni] = state->anim_from[A_BOXY] + state->anim_from[A_BOXH] + 6; ni++;
        ilo[ni] = state->anim_to[A_BOXY] - 6;
        ihi[ni] = state->anim_to[A_BOXY] + state->anim_to[A_BOXH] + 6; ni++;
    } else {
        if (state->blur) {
            INTN cb0 = state->anim_cur[A_BOXY] - 6;
            INTN cb1 = state->anim_cur[A_BOXY] + state->anim_cur[A_BOXH] + 6;
            if (state->prev_box_y0 < cb0) cb0 = state->prev_box_y0;
            if (state->prev_box_y1 > cb1) cb1 = state->prev_box_y1;
            ilo[ni] = cb0; ihi[ni] = cb1; ni++;
        }
        INTN uy = state->anim_cur[A_ULY];
        INTN ulo = (uy < state->prev_ul_y) ? uy : state->prev_ul_y;
        INTN uhi = (uy > state->prev_ul_y) ? uy : state->prev_ul_y;
        ilo[ni] = ulo - 4; ihi[ni] = uhi + (INTN)ul_th + 6; ni++;
        if (state->focus == FOCUS_POWER || state->prev_focus == FOCUS_POWER) {
            ilo[ni] = state->pwr_y0 - 6; ihi[ni] = state->pwr_y1 + 6; ni++;
        }
    }

    state->prev_ul_y = state->anim_cur[A_ULY];
    state->prev_box_y0 = state->anim_cur[A_BOXY] - 6;
    state->prev_box_y1 = state->anim_cur[A_BOXY] + state->anim_cur[A_BOXH] + 6;

    for (int a = 0; a < ni; a++)
        for (int b = a + 1; b < ni; b++)
            if (ilo[b] < ilo[a]) { INTN t0 = ilo[a]; ilo[a] = ilo[b]; ilo[b] = t0;
                                   INTN t1 = ihi[a]; ihi[a] = ihi[b]; ihi[b] = t1; }
    INTN bl[6], bh[6]; int nb = 0;
    for (int a = 0; a < ni; a++) {
        if (nb && ilo[a] <= bh[nb - 1] + 2) {
            if (ihi[a] > bh[nb - 1]) bh[nb - 1] = ihi[a];
        } else { bl[nb] = ilo[a]; bh[nb] = ihi[a]; nb++; }
    }
    if (nb > 4) { bh[0] = bh[nb - 1]; nb = 1; }
    state->band_n = nb;
    for (int a = 0; a < nb; a++) { state->band_y[a] = bl[a]; state->band_h[a] = bh[a] - bl[a]; }

    int pfocus = (state->focus == FOCUS_POWER) ? (int)state->power_sel : -1;

    UINTN ul_rad = ul_th / 2; if (ul_rad > 2) ul_rad = 2;
    if (state->blur) {
        if (cross) {
            draw_frost(state, state->anim_from[A_BOXX], state->anim_from[A_BOXY],
                       state->anim_from[A_BOXW], state->anim_from[A_BOXH], fout);
            draw_frost(state, state->anim_to[A_BOXX], state->anim_to[A_BOXY],
                       state->anim_to[A_BOXW], state->anim_to[A_BOXH], fin);
            fill_round_rect(state, state->anim_from[A_ULX], state->anim_from[A_ULY],
                            state->anim_from[A_ULW], (INTN)ul_th, (INTN)ul_rad,
                            state->underline_color, (UINT8)(230 * fout / 255));
            fill_round_rect(state, state->anim_to[A_ULX], state->anim_to[A_ULY],
                            state->anim_to[A_ULW], (INTN)ul_th, (INTN)ul_rad,
                            state->underline_color, (UINT8)(230 * fin / 255));
        } else {
            draw_frost(state, state->anim_cur[A_BOXX], state->anim_cur[A_BOXY],
                       state->anim_cur[A_BOXW], state->anim_cur[A_BOXH], 255);
            fill_round_rect(state, state->anim_cur[A_ULX], state->anim_cur[A_ULY],
                            state->anim_cur[A_ULW], (INTN)ul_th, (INTN)ul_rad,
                            state->underline_color, 230);
        }
    } else {
        INTN carda = state->anim_cur[A_CARDA];
        if (carda > 0) {
            INTN cx = state->anim_cur[A_CARDX];
            fill_round_rect(state, cx - pad, ecard_top,
                            sel_ei + 2 * pad, ecard_bot - ecard_top,
                            state->box_radius ? (INTN)state->box_radius : 14,
                            COLOR_WHITE, (UINT8)carda);
        }
        fill_round_rect(state, state->anim_cur[A_ULX], state->anim_cur[A_ULY],
                        state->anim_cur[A_ULW], (INTN)ul_th, (INTN)ul_rad,
                        state->underline_color, 230);
    }

    boot_entry_t *entry = entry_at(state, page_start);
    UINTN x = start_x;
    for (UINTN i = 0; i < page_n && entry; i++) {
        UINTN ei = entry->icon_size ? entry->icon_size : is;
        UINTN iy = (icon_cy > ei / 2) ? icon_cy - ei / 2 : 0;

        if (entry->icon) {
            draw_image_sized(state, entry->icon, x, iy, ei);
        } else {
            color_t placeholder = entry->type == 0 ? COLOR_GREEN : COLOR_RED;
            fill_round_rect(state, (INTN)x, (INTN)iy, ei, ei,
                            12, placeholder, 255);
        }

        if (state->show_names) {
            color_t name_col;
            if (entry->has_color) {
                name_col = entry->color;
            } else if (page_start + i == state->selected && state->focus == FOCUS_ENTRIES) {
                name_col = state->name_color;
            } else {
                name_col = (color_t){ state->name_color.r * 7 / 10,
                                      state->name_color.g * 7 / 10,
                                      state->name_color.b * 7 / 10 };
            }
            UINTN nw = text_width_px(entry->name, name_px);
            INTN  nx = (INTN)x + (INTN)ei / 2 - (INTN)nw / 2;
            draw_text_px(state, entry->name, nx, name_y, name_col, name_px);
        }

        x += ei + isp;
        entry = entry->next;
    }

    draw_chevrons(state, page, per_page, start_x, total_w, isp, max_ei, icon_cy, 255);

    if (pfocus >= 0)
        draw_power_actions(state, pfocus);

    if (state->center_info && state->entry_count > 0)
        draw_center_info(state, entry_at(state, state->selected), ci_top, name_px, 255);

    state->prev_focus = state->focus;
    state->prev_page = page;
    state->prev_selected = state->selected;

    if (partial) return;

    if (state->timeout_active && state->timeout > 0) {
        UINT64 elapsed = efi_get_tick() - state->timeout_start;
        INTN remaining = state->timeout - (INTN)(elapsed / 1000);
        if (remaining > 0) {
            CHAR16 buf[40];
            SPrint(buf, sizeof(buf), L"Booting in %ds", (int)remaining);
            UINTN cw = text_width(buf, 2);
            UINTN cx = (state->power_position == POWER_POS_BOTTOMLEFT)
                       ? state->screen_width - 30 - cw : 30;
            draw_text_scaled(state, buf, cx, state->screen_height - 30 - 16,
                             state->fg_color, 2);
        }
    }
}

boot_entry_t* gui_run(gui_state_t *state) {
    EFI_STATUS status;
    EFI_INPUT_KEY key;

    state->timeout_start = efi_get_tick();
    state->action = VISOR_ACTION_BOOT;

    BS->SetWatchdogTimer(0, 0, 0, NULL);

    if (state->timeout == 0) {
        state->running = 0;
    }

    INTN last_remaining = -2;
    int  need_redraw = 1;
    int  full_redraw = 1;

    while (state->running) {
        if (need_redraw) {
            gui_draw_menu(state, !full_redraw);
            if (full_redraw)
                gui_present(state);
            else
                for (int b = 0; b < state->band_n; b++)
                    gui_present_band(state, state->band_y[b], state->band_h[b]);
            need_redraw = state->anim_active || state->page_anim;
            full_redraw = 0;
        }

        status = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (!EFI_ERROR(status)) {

            if (state->timeout_active) { state->timeout_active = 0; need_redraw = 1; full_redraw = 1; }

            CHAR16 uc = key.UnicodeChar;
            if (uc >= 'a' && uc <= 'z') uc -= 32;

            if (uc == 'S') { state->action = VISOR_ACTION_SHUTDOWN; state->running = 0; }
            else if (uc == 'R') { state->action = VISOR_ACTION_REBOOT; state->running = 0; }
            else if (uc == 'F') { state->action = VISOR_ACTION_FIRMWARE; state->running = 0; }
            else if (key.UnicodeChar == 0x0D) {

                if (state->focus == FOCUS_POWER)
                    state->action = VISOR_ACTION_SHUTDOWN + (int)state->power_sel;
                state->running = 0;
            }
            else if (key.UnicodeChar == 0x00) {
                int power_top = (state->power_position == POWER_POS_TOPLEFT ||
                                 state->power_position == POWER_POS_TOPRIGHT);
                switch (key.ScanCode) {
                    case 0x04:
                        state->focus = FOCUS_ENTRIES;
                        if (state->selected > 0) state->selected--;
                        else state->selected = state->entry_count - 1;
                        need_redraw = 1;
                        break;
                    case 0x03:
                        state->focus = FOCUS_ENTRIES;
                        if (state->selected < state->entry_count - 1) state->selected++;
                        else state->selected = 0;
                        need_redraw = 1;
                        break;
                    case 0x02:
                        if (power_top) {
                            if (state->focus == FOCUS_POWER) {
                                if (state->power_sel < POWER_ACTION_COUNT - 1)
                                    state->power_sel++;
                                else
                                    state->focus = FOCUS_ENTRIES;
                            }
                        } else {
                            if (state->focus == FOCUS_ENTRIES) {
                                state->focus = FOCUS_POWER;
                                state->power_sel = 0;
                            } else if (state->power_sel < POWER_ACTION_COUNT - 1) {
                                state->power_sel++;
                            }
                        }
                        need_redraw = 1;
                        break;
                    case 0x01:
                        if (power_top) {
                            if (state->focus == FOCUS_ENTRIES) {
                                state->focus = FOCUS_POWER;
                                state->power_sel = POWER_ACTION_COUNT - 1;
                            } else if (state->power_sel > 0) {
                                state->power_sel--;
                            }
                        } else {
                            if (state->focus == FOCUS_POWER) {
                                if (state->power_sel > 0) state->power_sel--;
                                else state->focus = FOCUS_ENTRIES;
                            }
                        }
                        need_redraw = 1;
                        break;
                    case 0x17:
                        state->running = 0;
                        break;
                }
            }
            else if (key.UnicodeChar >= '1' && key.UnicodeChar <= '9') {
                UINTN idx = key.UnicodeChar - '1';
                if (idx < state->entry_count) {
                    state->focus = FOCUS_ENTRIES;
                    state->selected = idx;
                    state->running = 0;
                }
            }
        }

        if (state->timeout_active && state->timeout > 0) {
            UINT64 elapsed = efi_get_tick() - state->timeout_start;
            INTN remaining = state->timeout - (INTN)(elapsed / 1000);
            if (remaining <= 0) {
                state->running = 0;
            } else if (remaining != last_remaining) {
                last_remaining = remaining;
                need_redraw = 1;
                full_redraw = 1;
            }
        }

        efi_sleep((state->anim_active || state->page_anim) ? 6 : 30);
    }

    if (state->action != VISOR_ACTION_BOOT) return NULL;

    boot_entry_t *selected = state->entries;
    for (UINTN i = 0; i < state->selected && selected; i++) {
        selected = selected->next;
    }
    return selected;
}

static void free_icon(icon_t *ic) {
    if (!ic) return;
    if (ic->scaled) efi_free_pool(ic->scaled);
    if (ic->pixels) efi_free_pool(ic->pixels);
    efi_free_pool(ic);
}

void gui_shutdown(gui_state_t *state) {

    boot_entry_t *entry = state->entries;
    while (entry) {
        if (entry->icon) { free_icon(entry->icon); entry->icon = NULL; }
        entry = entry->next;
    }

    free_icon(state->background);
    state->background = NULL;
    free_icon(state->shutdown_icon);
    state->shutdown_icon = NULL;
    free_icon(state->reboot_icon);
    state->reboot_icon = NULL;
    free_icon(state->firmware_icon);
    state->firmware_icon = NULL;

    if (state->background_path) {
        efi_free_pool(state->background_path);
    }

    gui_fill_rect(state, 0, 0, state->screen_width, state->screen_height, COLOR_BLACK);
    gui_present(state);

    if (state->backbuffer) {
        efi_free_pool(state->backbuffer);
        state->backbuffer = NULL;
    }
    if (state->scene_cache) {
        efi_free_pool(state->scene_cache);
        state->scene_cache = NULL;
    }
    if (state->blur_cache) {
        efi_free_pool(state->blur_cache);
        state->blur_cache = NULL;
    }
}
