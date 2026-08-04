#include "gui.h"
#include "font.h"
#include "efi_helpers.h"
#include "arch.h"
#include "accent.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;

icon_t* png_load(UINT8 *data, UINTN size);

static const font_t *g_font = &jetbrains_font;

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
    while (o < f->unpacked_size) out[o++] = 0;
    g_glyph_cov = out;
}

static void glyph_cache_flush(void);

void gui_set_font(const char *name) {
    if (name && name[0]) {
        int is_jb = (name[0] == 'j' || name[0] == 'J');
        if (!is_jb)
            efi_log(L"WARN: font= ignored - only the built-in 'jetbrains' font is available");
    }
    g_font = &jetbrains_font;
    glyph_cache_flush();
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

static UINTN clamp_uintn(UINTN v, UINTN lo, UINTN hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static UINTN ui_base(gui_state_t *state) {
    UINTN a = state->screen_width < state->screen_height
            ? state->screen_width : state->screen_height;
    return a ? a : 800;
}

static UINTN default_icon_size(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 9, 64, 128);
}

static UINTN default_icon_spacing(gui_state_t *state, UINTN icon_size) {
    UINTN by_res = ui_base(state) / 12;
    UINTN by_icon = icon_size * 7 / 10;
    UINTN s = by_res > by_icon ? by_res : by_icon;
    return clamp_uintn(s, 48, 104);
}

static UINTN default_name_px(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 38, 18, 28);
}

static UINTN default_title_px(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 11, 48, 86);
}

static UINTN default_power_px(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 32, 20, 28);
}

static UINTN default_power_icon_size(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 18, 40, 56);
}

static UINTN default_aux_text_px(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 34, 20, 30);
}

static void gui_fill_rect(gui_state_t *state, UINTN x, UINTN y,
                          UINTN w, UINTN h, color_t color);

static int add_overflow_uintn(UINTN a, UINTN b, UINTN *out) {
    if (~(UINTN)0 - a < b) return 1;
    *out = a + b;
    return 0;
}

static int mul_overflow_uintn(UINTN a, UINTN b, UINTN *out) {
    if (a && b > ~(UINTN)0 / a) return 1;
    *out = a * b;
    return 0;
}

static UINT16 rd16le(const UINT8 *p) {
    return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static UINT32 rd32le(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static void wipe16(CHAR16 *s, UINTN n) {
    volatile CHAR16 *p = (volatile CHAR16*)s;
    while (n--) *p++ = 0;
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

    UINT8 *fb = (UINT8*)state->gop->Mode->FrameBufferBase;
    UINTN  ppsl = state->pixels_per_scanline;
    UINTN  w = state->screen_width;

    if (state->pixel_format == PixelBlueGreenRedReserved8BitPerColor) {

        if (ppsl == w) {
            CopyMem(fb + (UINTN)y * w * sizeof(UINT32),
                    &state->backbuffer[(UINTN)y * w],
                    (UINTN)h * w * sizeof(UINT32));
        } else {
            for (INTN row = y; row < y + h; row++)
                CopyMem(fb + (UINTN)row * ppsl * sizeof(UINT32),
                        &state->backbuffer[(UINTN)row * w],
                        w * sizeof(UINT32));
        }
        return;
    }

    for (INTN row = y; row < y + h; row++) {
        UINT32 *dst = (UINT32*)(fb + (UINTN)row * ppsl * sizeof(UINT32));
        UINT32 *src = &state->backbuffer[(UINTN)row * w];
        for (UINTN x = 0; x < w; x++) {
            UINT32 p = src[x];
            dst[x] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
        }
    }
}

static int gui_has_linear_fb(gui_state_t *state) {
    return state->gop && state->gop->Mode->FrameBufferBase &&
           (state->pixel_format == PixelBlueGreenRedReserved8BitPerColor ||
            state->pixel_format == PixelRedGreenBlueReserved8BitPerColor);
}

void gui_present(gui_state_t *state) {
    if (!state->backbuffer) return;

    if (state->fb_fast) {
        blit_rows(state, 0, (INTN)state->screen_height);
        return;
    }

    EFI_STATUS s = uefi_call_wrapper(state->gop->Blt, 10,
        state->gop,
        (EFI_GRAPHICS_OUTPUT_BLT_PIXEL*)state->backbuffer,
        EfiBltBufferToVideo,
        0, 0, 0, 0,
        state->screen_width, state->screen_height,
        0);
    if (!EFI_ERROR(s)) return;

    if (gui_has_linear_fb(state))
        blit_rows(state, 0, (INTN)state->screen_height);
}

void gui_present_band(gui_state_t *state, INTN y, INTN h) {
    if (!state->backbuffer) return;
    if (y < 0) { h += y; y = 0; }
    if (h <= 0) return;
    if (y + h > (INTN)state->screen_height) h = (INTN)state->screen_height - y;
    if (h <= 0) return;

    if (state->fb_fast) {
        blit_rows(state, y, h);
        return;
    }

    EFI_STATUS s = uefi_call_wrapper(state->gop->Blt, 10,
        state->gop,
        (EFI_GRAPHICS_OUTPUT_BLT_PIXEL*)state->backbuffer,
        EfiBltBufferToVideo,
        0, (UINTN)y, 0, (UINTN)y,
        state->screen_width, (UINTN)h,
        state->screen_width * sizeof(UINT32));
    if (!EFI_ERROR(s)) return;

    if (gui_has_linear_fb(state))
        blit_rows(state, y, h);
}

static int fade_speed_value(gui_state_t *state) {
    int sp = state ? state->fade_speed : 0;
    if (sp < 0) sp = 0;
    if (sp > 10) sp = 10;

    return sp;
}

#define FADE_MIN_DURATION_MS      150
#define FADE_MAX_DURATION_MS      500

#define FADE_MIN_FRAME_US         1500

static UINTN fade_duration_ms(gui_state_t *state) {
    int sp = fade_speed_value(state);
    return FADE_MAX_DURATION_MS
         - (UINTN)sp * (FADE_MAX_DURATION_MS - FADE_MIN_DURATION_MS) / 10;
}

static int gui_animation_on(gui_state_t *state) {
    return state && state->animation;
}

static INTN ease_permille(INTN frame, INTN frames) {
    if (frames <= 0 || frame >= frames) return 1000;
    if (frame <= 0) return 0;

    INTN t = frame * 1000 / frames;
    return (t * t * (3000 - 2 * t) + 500000) / 1000000;
}

static UINTN ease_alpha(INTN frame, INTN frames) {
    return (UINTN)(ease_permille(frame, frames) * 255 / 1000);
}

static void fade_write_black(gui_state_t *state, UINTN px) {
    for (UINTN i = 0; i < px; i++)
        state->backbuffer[i] = 0xFF000000u;
}

static void fade_write_snapshot(gui_state_t *state, UINT32 *snapshot, UINTN px) {
    for (UINTN i = 0; i < px; i++)
        state->backbuffer[i] = snapshot[i];
}

static void fade_write_scaled(gui_state_t *state, UINT32 *snapshot,
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

static void gui_fade_in_current(gui_state_t *state) {
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

#define FB_FAST_THRESHOLD_US 20000

static void gui_fb_set_wc(gui_state_t *state) {
    state->fb_fast = 0;
    if (!gui_has_linear_fb(state)) return;
    UINT64 base = (UINT64)state->gop->Mode->FrameBufferBase;
    UINT64 size = (UINT64)state->gop->Mode->FrameBufferSize;
    if (!base || !size) return;

    const CHAR16 *method = arch_fb_make_wc(base, size);

    arch_clock_init();
    UINT64 t0 = arch_now_us();
    blit_rows(state, 0, (INTN)state->screen_height);
    UINT64 dt = arch_now_us() - t0;
    state->fb_fast = (dt < FB_FAST_THRESHOLD_US) ? 1 : 0;

    CHAR16 g[160];
    SPrint(g, sizeof(g), L"gfx: fb WC method=%s present=%dus -> %s",
           method, (int)dt, state->fb_fast ? L"DIRECT(fast)" : L"BLT(fallback)");
    efi_log(g);
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
    state->animation = 1;
    state->anim_speed = 0;
    state->fade_speed = 0;
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
    state->hotplug_poll = NULL;
    state->hotplug_ctx = NULL;
    state->hp_last_ms = 0;
    state->hp_anim = 0;
    state->hp_frame = 0;
    state->hp_first = 0;
    state->hp_shift = 0;
    state->hp_removal = 0;
    state->band_n = 0;
    for (int i = 0; i < 4; i++) { state->band_y[i] = 0; state->band_h[i] = 0; }
    state->prev_ul_y = 0;
    state->title = NULL;
    state->show_title = 1;
    state->logo = NULL;
    state->logo_mode = LOGO_MODE_TITLE;
    state->logo_size = 0;
    state->logo_gap = 0;
    state->accent_logo = 0;
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

    state->version_mode = 0;
    state->ver_fading = 0;
    state->ver_frame = 0;
    state->ver_dir = 0;
    state->ver_what = 0;
    state->ver_next = 0;
    state->snap_mode = 0;
    state->snap_scroll = 0;

    state->editor_enabled = 1;
    state->editing = 0;
    state->edit_secret = 0;
    state->edit_title = NULL;
    state->edit_len = 0;
    state->edit_cursor = 0;
    state->override_cmdline = NULL;

    state->mouse_enabled = 1;
    state->pointer_speed = 4;
    state->spp = NULL;
    state->app = NULL;
    state->has_pointer = 0;
    state->cursor_active = 0;
    state->cursor_x = (INTN)state->screen_width / 2;
    state->cursor_y = (INTN)state->screen_height / 2;
    state->hit_n = 0;
    {
        EFI_GUID spg = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
        EFI_GUID apg = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
        EFI_SIMPLE_POINTER_PROTOCOL *sp = NULL;
        EFI_ABSOLUTE_POINTER_PROTOCOL *ap = NULL;
        if (!EFI_ERROR(BS->LocateProtocol(&spg, NULL, (void**)&sp)) && sp) {
            sp->Reset(sp, FALSE);
            state->spp = sp;
            state->has_pointer = 1;
        }
        if (!EFI_ERROR(BS->LocateProtocol(&apg, NULL, (void**)&ap)) && ap) {
            ap->Reset(ap, FALSE);
            state->app = ap;
            state->has_pointer = 1;
        }
    }

    state->backbuffer = efi_allocate_pool(
        state->screen_width * state->screen_height * sizeof(UINT32));
    if (!state->backbuffer) return EFI_OUT_OF_RESOURCES;

    state->scene_cache = efi_allocate_pool(
        state->screen_width * state->screen_height * sizeof(UINT32));
    state->blur_cache = NULL;
    state->scene_valid = 0;

    gui_fill_rect(state, 0, 0, state->screen_width, state->screen_height, state->bg_color);

    gui_fb_set_wc(state);
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

    gui_fb_set_wc(state);
    gui_present(state);
    return EFI_SUCCESS;
}

static void gui_fill_rect(gui_state_t *state, UINTN x, UINTN y, UINTN w, UINTN h, color_t color) {
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
    if (size == 0 || size > 4096) return NULL;
    if (icon->scaled && icon->scaled_size == size) return icon->scaled;
    if (icon->scaled) { efi_free_pool(icon->scaled); icon->scaled = NULL; icon->scaled_size = 0; }

    UINTN px, bytes;
    if (mul_overflow_uintn(size, size, &px) ||
        mul_overflow_uintn(px, sizeof(UINT32), &bytes))
        return NULL;
    UINT32 *out = efi_allocate_pool(bytes);
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

static void draw_image_tinted_a(gui_state_t *state, icon_t *icon,
                                UINTN x, UINTN y, UINTN size,
                                color_t tint, INTN master) {
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

            UINT32 *dest = get_pixel(state, x + i, y + j);
            if (!dest) continue;
            UINT8 br = (*dest >> 16) & 0xFF, bg = (*dest >> 8) & 0xFF, bb = *dest & 0xFF;
            UINT8 nr = (UINT8)((tint.r * cov + br * (255 - cov)) / 255);
            UINT8 ng = (UINT8)((tint.g * cov + bg * (255 - cov)) / 255);
            UINT8 nb = (UINT8)((tint.b * cov + bb * (255 - cov)) / 255);
            *dest = (0xFFu << 24) | (nr << 16) | (ng << 8) | nb;
        }
    }
}

static INTN scale_metric(INTN v, UINTN dh, UINTN size) {
    INTN num = v * (INTN)dh;
    INTN half = (INTN)size / 2;
    return num >= 0 ? (num + half) / (INTN)size
                    : -((-num + half) / (INTN)size);
}

#define FIXQ         16
#define FIXONE       (1 << FIXQ)
#define GLYPH_PAD    1
#define GLYPH_PHASES 4
#define GLYPH_SLOTS  512

typedef struct {
    UINT16  cp;
    UINT16  px;
    UINT8   phase;
    UINT8   valid;
    UINT16  w, h;
    INT16   ox, oy;
    UINT8  *cov;
} glyph_entry_t;

static glyph_entry_t g_glyphs[GLYPH_SLOTS];

static void glyph_cache_flush(void) {
    for (UINTN i = 0; i < GLYPH_SLOTS; i++) {
        if (g_glyphs[i].cov) efi_free_pool(g_glyphs[i].cov);
        g_glyphs[i].cov = NULL;
        g_glyphs[i].valid = 0;
    }
}

static void cov_axis(const UINT8 *src, UINTN sn, UINTN sstride,
                     UINT8 *dst, UINTN dn, UINTN dstride,
                     UINTN lines, UINTN line_src, UINTN line_dst,
                     UINTN step, UINTN phase)
{
    UINTN width = step < FIXONE ? FIXONE : step;
    UINTN half  = width / 2;
    INTN  poff  = (INTN)(((UINT64)phase * step) >> FIXQ);

    for (UINTN i = 0; i < dn; i++) {
        INTN c  = ((INTN)i - GLYPH_PAD) * (INTN)step + (INTN)(step / 2) - poff;
        INTN lo = c - (INTN)half;
        INTN hi = c + (INTN)half;
        INTN k0 = lo >> FIXQ;
        INTN k1 = (hi + FIXONE - 1) >> FIXQ;
        if (k0 < 0) k0 = 0;
        if (k1 > (INTN)sn) k1 = (INTN)sn;

        for (UINTN l = 0; l < lines; l++) {
            const UINT8 *s = src + l * line_src;
            UINT64 acc = 0;
            for (INTN k = k0; k < k1; k++) {
                INTN a = (k << FIXQ), b = a + FIXONE;
                if (a < lo) a = lo;
                if (b > hi) b = hi;
                if (b <= a) continue;
                acc += (UINT64)s[(UINTN)k * sstride] * (UINT64)(b - a);
            }
            UINT64 v = (acc + (width >> 1)) / width;
            if (v > 255) v = 255;
            dst[l * line_dst + i * dstride] = (UINT8)v;
        }
    }
}

static glyph_entry_t *glyph_get(CHAR16 cp, UINTN dh, UINTN phase) {
    const font_t *f = g_font;
    if (cp < f->first || cp > f->last) cp = '?';
    UINTN slot = ((UINTN)cp * 2654435761u + dh * 97u + phase) & (GLYPH_SLOTS - 1);
    glyph_entry_t *e = &g_glyphs[slot];
    if (e->valid && e->cp == cp && e->px == dh && e->phase == phase) return e;

    const glyph_t *g = &f->glyphs[cp - f->first];
    if (e->cov) efi_free_pool(e->cov);
    e->cov = NULL;
    e->valid = 1;
    e->cp = (UINT16)cp;
    e->px = (UINT16)dh;
    e->phase = (UINT8)phase;
    e->w = e->h = 0;
    e->ox = e->oy = 0;
    if (!g->w || !g->h || !g_glyph_cov || !dh) return e;

    UINTN size = f->size;
    UINTN step = ((UINTN)size << FIXQ) / dh;
    UINTN dw = ((UINTN)g->w * dh + size - 1) / size + 2 * GLYPH_PAD;
    UINTN dhh = ((UINTN)g->h * dh + size - 1) / size + 2 * GLYPH_PAD;
    if (!dw || !dhh || dw > 4096 || dhh > 4096) return e;

    UINT8 *tmp = efi_allocate_pool(dw * (UINTN)g->h);
    UINT8 *out = efi_allocate_pool(dw * dhh);
    if (!tmp || !out) {
        if (tmp) efi_free_pool(tmp);
        if (out) efi_free_pool(out);
        return e;
    }

    const UINT8 *cov = (const UINT8 *)g_glyph_cov + g->pixel_offset;
    UINTN ph = (phase << FIXQ) / GLYPH_PHASES;

    /* horizontal */
    cov_axis(cov, g->w, 1, tmp, dw, 1, g->h, g->w, dw, step, ph);
    /* vertical */
    cov_axis(tmp, g->h, dw, out, dhh, dw, dw, 1, 1, step, 0);

    efi_free_pool(tmp);
    e->cov = out;
    e->w = (UINT16)dw;
    e->h = (UINT16)dhh;
    e->ox = -GLYPH_PAD;
    e->oy = -GLYPH_PAD;
    return e;
}

static void blend_glyph(gui_state_t *state, const glyph_entry_t *e, UINT32 rgb,
                        INTN dx, INTN dyTop, INTN master) {
    if (!e->cov || !e->w || !e->h) return;
    UINT8 fr = (rgb >> 16) & 0xFF, fg = (rgb >> 8) & 0xFF, fb = rgb & 0xFF;
    for (UINTN j = 0; j < e->h; j++) {
        INTN py = dyTop + (INTN)j;
        if (py < 0 || py >= (INTN)state->screen_height) continue;
        const UINT8 *row = e->cov + j * e->w;
        for (UINTN i = 0; i < e->w; i++) {
            UINT8 a = row[i];
            if (!a) continue;
            if (master < 255) a = (UINT8)((UINTN)a * (UINTN)master / 255);
            if (!a) continue;
            INTN px = dx + (INTN)i;
            if (px < 0 || px >= (INTN)state->screen_width) continue;
            UINT32 *p = get_pixel(state, (UINTN)px, (UINTN)py);
            if (!p) continue;
            UINT8 br = (*p >> 16) & 0xFF, bg = (*p >> 8) & 0xFF, bb = *p & 0xFF;
            UINT8 r = (fr * a + br * (255 - a)) / 255;
            UINT8 gg = (fg * a + bg * (255 - a)) / 255;
            UINT8 b = (fb * a + bb * (255 - a)) / 255;
            *p = (0xFFu << 24) | (r << 16) | (gg << 8) | b;
        }
    }
}

static UINTN text_width_px(CHAR16 *text, UINTN dh) {
    if (!text) return 0;
    UINTN pen = 0;
    UINTN size = g_font->size;
    while (*text) {
        CHAR16 c = *text++;
        if (c < g_font->first || c > g_font->last) c = '?';
        const glyph_t *g = &g_font->glyphs[c - g_font->first];
        pen += (UINTN)g->advance_q6 * dh / size;
    }
    return (pen + 32) / 64;
}

static void draw_text_px_a(gui_state_t *state, CHAR16 *text, INTN x, INTN y,
                           color_t color, UINTN dh, INTN master) {
    if (!text || master <= 0 || !dh) return;
    if (master > 255) master = 255;
    font_ensure_decoded();
    UINT32 rgb = color_to_u32(color) & 0x00FFFFFF;
    UINTN size = g_font->size;
    INTN baseline = y + scale_metric((INTN)g_font->ascent, dh, size);
    INTN pen64 = x * 64;
    while (*text) {
        CHAR16 c = *text++;
        if (c < g_font->first || c > g_font->last) c = '?';
        const glyph_t *g = &g_font->glyphs[c - g_font->first];

        INTN gpen = pen64 + ((INTN)g->left * (INTN)dh * 64) / (INTN)size;
        INTN gx    = gpen >> 6;
        INTN frac  = gpen - (gx << 6);
        UINTN phase = (UINTN)((frac * GLYPH_PHASES) >> 6);
        if (phase >= GLYPH_PHASES) phase = GLYPH_PHASES - 1;

        INTN gyTop = baseline - scale_metric((INTN)g->top, dh, size);
        glyph_entry_t *e = glyph_get(c, dh, phase);
        if (e) blend_glyph(state, e, rgb, gx + e->ox, gyTop + e->oy, master);

        pen64 += ((INTN)g->advance_q6 * (INTN)dh) / (INTN)size;
    }
}

static void draw_text_px(gui_state_t *state, CHAR16 *text, INTN x, INTN y,
                         color_t color, UINTN dh) {
    draw_text_px_a(state, text, x, y, color, dh, 255);
}

static void draw_text_centered_px(gui_state_t *state, CHAR16 *text, INTN x, UINTN w,
                                  INTN y, color_t color, UINTN dh) {
    UINTN tw = text_width_px(text, dh);
    INTN tx = (tw < w) ? x + (INTN)(w - tw) / 2 : x;
    draw_text_px(state, text, tx, y, color, dh);
}

icon_t* gui_load_image(CHAR16 *path) {
    efi_log(L"  image: opening file");
    efi_log(path);
    efi_file_buffer_t *buf = efi_load_file(path);
    if (!buf) { efi_log(L"  ERROR: image file not found or unreadable"); return NULL; }
    { CHAR16 d[64]; SPrint(d, sizeof(d), L"  image: read %d bytes", (int)buf->size); efi_log(d); }

    UINT8 *data = (UINT8*)buf->data;
    if (buf->size < 2) {
        efi_log(L"  ERROR: image file is too small");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    UINT8 png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    INTN is_png = 1;
    if (buf->size < sizeof(png_sig)) is_png = 0;
    for (int i = 0; is_png && i < 8; i++) {
        if (data[i] != png_sig[i]) {
            is_png = 0;
            break;
        }
    }

    if (is_png) {
        icon_t *icon = png_load(data, buf->size);
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        if (!icon) efi_log(L"  ERROR: PNG decode failed");
        return icon;
    }

    if (data[0] != 'B' || data[1] != 'M') {
        efi_log(L"  ERROR: image is neither PNG nor BMP");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    if (buf->size < 54) {
        efi_log(L"  ERROR: BMP header is truncated");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    UINT32 width = rd32le(data + 18);
    UINT32 height = rd32le(data + 22);
    UINT16 bpp = rd16le(data + 28);
    UINT32 compression = rd32le(data + 30);
    UINT32 data_offset = rd32le(data + 10);

    if (width == 0 || height == 0 || width > 8192 || height > 8192 ||
        (UINT64)width * height > 16u * 1024u * 1024u ||
        compression != 0 || (bpp != 24 && bpp != 32) || data_offset >= buf->size) {
        efi_log(L"  ERROR: unsupported or invalid BMP");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    UINTN bytes_per_px = bpp / 8;
    UINTN row_raw = 0, row_stride = 0, pixel_array_bytes = 0;
    UINTN pixel_array_end = 0, pixel_count = 0, pixel_bytes = 0;
    if (mul_overflow_uintn((UINTN)width, bytes_per_px, &row_raw) ||
        add_overflow_uintn(row_raw, 3, &row_stride) ||
        mul_overflow_uintn((UINTN)height, row_stride & ~(UINTN)3, &pixel_array_bytes) ||
        add_overflow_uintn((UINTN)data_offset, pixel_array_bytes, &pixel_array_end) ||
        pixel_array_end > buf->size ||
        mul_overflow_uintn((UINTN)width, (UINTN)height, &pixel_count) ||
        mul_overflow_uintn(pixel_count, sizeof(UINT32), &pixel_bytes)) {
        efi_log(L"  ERROR: BMP dimensions or pixel data are invalid");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }
    row_stride &= ~(UINTN)3;

    icon_t *icon = efi_allocate_pool(sizeof(icon_t));
    if (!icon) {
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }
    icon->width = width;
    icon->height = height;
    icon->scaled_size = 0;
    icon->scaled = NULL;
    icon->pixels = efi_allocate_pool(pixel_bytes);
    if (!icon->pixels) {
        efi_free_pool(icon);
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    UINT8 *src = data + data_offset;

    for (UINTN y = 0; y < height; y++) {
        UINT8 *row = src + ((UINTN)height - 1 - y) * row_stride;
        for (UINTN x = 0; x < width; x++) {
            UINT8 *pixel = row + x * bytes_per_px;

            icon->pixels[y * width + x] = (0xFF << 24) | (pixel[2] << 16) | (pixel[1] << 8) | pixel[0];
        }
    }

    efi_free_pool(buf->data);
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

static int entry_own_color(gui_state_t *state, boot_entry_t *e, color_t *out) {
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

    if (!state->accent_valid) return;

    if (accent_resolve(state, &state->sp_bg, NULL, ROLE_SURFACE, &c))
        state->bg_color = c;
    if (!state->blur_color_set &&
        accent_resolve(state, &state->sp_blur, NULL, ROLE_ON_PRIMARY_CONTAINER, &c))
        state->blur_color = c;
}

static void gui_draw_background(gui_state_t *state) {
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

static void layout_power(gui_state_t *state) {
    UINTN th      = default_power_px(state);
    UINTN line_h  = th + 18;
    UINTN margin  = clamp_uintn(ui_base(state) / 26, 28, 46);

    icon_t *icon[POWER_ACTION_COUNT] = {
        state->shutdown_icon, state->reboot_icon, state->firmware_icon
    };
    UINTN isz       = state->power_icon_size ? state->power_icon_size
                                             : default_power_icon_size(state);
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
            UINTN tw = text_width_px(POWER_ACTIONS[i].label, th);
            INTN  x  = right_side ? (INTN)(state->screen_width - margin - tw) : (INTN)margin;
            state->pwr_x[i] = x; state->pwr_y[i] = y;
            state->pwr_w[i] = (INTN)tw; state->pwr_h[i] = (INTN)th;
            y += line_h;
        }
    }
    state->pwr_y0 = top;
    state->pwr_y1 = top + (INTN)block_h;
}

static void draw_power_actions(gui_state_t *state, int focus_idx, int live) {
    UINTN th = default_power_px(state);
    icon_t *icon[POWER_ACTION_COUNT] = {
        state->shutdown_icon, state->reboot_icon, state->firmware_icon
    };
    color_t key_color[POWER_ACTION_COUNT] = {
        state->shutdown_color, state->reboot_color, state->firmware_color
    };
    color_t dim = { 0xC0, 0xC0, 0xC8 };
    if (state->accent_valid)
        dim = state->accent_roles[ROLE_ON_SURFACE_VARIANT];

    for (UINTN i = 0; i < POWER_ACTION_COUNT; i++) {
        int focused = ((int)i == focus_idx);
        if (state->power_icons && icon[i]) {
            if (live && !(focused && state->blur)) continue;
            if (state->pwr_tint_on[i])
                draw_image_tinted_a(state, icon[i], state->pwr_x[i], state->pwr_y[i],
                                    (UINTN)state->pwr_w[i], key_color[i], 255);
            else
                draw_image_sized(state, icon[i], state->pwr_x[i], state->pwr_y[i],
                                 (UINTN)state->pwr_w[i]);
        } else {
            if (live && !focused) continue;
            CHAR16 *label = POWER_ACTIONS[i].label;
            INTN x = state->pwr_x[i], y = state->pwr_y[i];

            CHAR16 first[2] = { label[0], 0 };
            draw_text_px(state, first, x, y, key_color[i], th);
            draw_text_px(state, label + 1,
                         x + (INTN)text_width_px(first, th) + (INTN)(th / 5),
                         y, focused ? key_color[i] : dim, th);
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

static UINTN entry_icon_size(boot_entry_t *e, UINTN fallback) {
    return (e && e->icon_size) ? e->icon_size : fallback;
}

static UINTN entry_slot_width(gui_state_t *state, boot_entry_t *e,
                              UINTN icon_size, UINTN name_px) {
    UINTN w = icon_size;
    if (state->show_names && e && e->name) {
        UINTN nw = text_width_px(e->name, name_px);
        if (nw > w) w = nw;
    }
    return w;
}

static void calc_row_layout(gui_state_t *state, UINTN start, UINTN n,
                            UINTN sel_local, UINTN is, UINTN isp, UINTN name_px,
                            UINTN *total_w, INTN *sel_left, UINTN *sel_ei) {
    UINTN x = 0;
    boot_entry_t *e = entry_at(state, start);

    *total_w = 0;
    *sel_left = 0;
    *sel_ei = is;

    for (UINTN i = 0; i < n && e; i++) {
        if (i) x += isp;
        UINTN ei = entry_icon_size(e, is);
        UINTN slot_w = entry_slot_width(state, e, ei, name_px);
        if (i == sel_local) {
            *sel_left = (INTN)x + (INTN)(slot_w - ei) / 2;
            *sel_ei = ei;
        }
        x += slot_w;
        e = e->next;
    }
    *total_w = x;
}

static UINTN visible_row_width(gui_state_t *state) {
    UINTN per_page = state->per_page ? state->per_page : 3;
    UINTN page_start = (state->selected / per_page) * per_page;
    UINTN n = state->entry_count > page_start ? state->entry_count - page_start : 0;
    if (n > per_page) n = per_page;
    if (!n) return 0;
    UINTN is  = state->icon_size    ? state->icon_size    : default_icon_size(state);
    UINTN isp = state->icon_spacing ? state->icon_spacing : default_icon_spacing(state, is);
    UINTN name_px = state->name_size ? state->name_size : default_name_px(state);
    UINTN w = 0, sei = is;
    INTN  sl = 0;
    calc_row_layout(state, page_start, n, 0, is, isp, name_px, &w, &sl, &sei);
    return w;
}

static void gui_entries_added(gui_state_t *state, boot_entry_t *head,
                              UINTN count, UINTN first) {
    UINTN old_count = state->entry_count;
    state->entries = head;
    state->entry_count = count;
    state->hp_anim = 0;
    state->hp_removal = 0;
    if (state->selected >= count) state->selected = 0;

    UINTN per_page = state->per_page ? state->per_page : 3;
    UINTN page_start = (state->selected / per_page) * per_page;
    UINTN old_n = (old_count > page_start) ? old_count - page_start : 0;
    if (old_n > per_page) old_n = per_page;
    UINTN new_n = count - page_start;
    if (new_n > per_page) new_n = per_page;

    if (!gui_animation_on(state) || new_n <= old_n || first < page_start)
        return;

    UINTN is  = state->icon_size    ? state->icon_size    : default_icon_size(state);
    UINTN isp = state->icon_spacing ? state->icon_spacing : default_icon_spacing(state, is);
    UINTN name_px = state->name_size ? state->name_size : default_name_px(state);
    UINTN old_w = 0, new_w = 0, sei = is;
    INTN  sl = 0;
    if (old_n)
        calc_row_layout(state, page_start, old_n, 0, is, isp, name_px,
                        &old_w, &sl, &sei);
    calc_row_layout(state, page_start, new_n, 0, is, isp, name_px,
                    &new_w, &sl, &sei);

    state->hp_first = first;
    state->hp_frame = 0;
    state->hp_anim  = 1;
    state->hp_shift = old_n ? (INTN)(new_w - old_w) / 2 : 0;
}

static void gui_entries_removed(gui_state_t *state, boot_entry_t *head,
                                UINTN count, UINTN gap, UINTN old_w) {
    state->entries = head;
    state->entry_count = count;
    state->hp_anim = 0;
    state->hp_removal = 0;
    if (state->selected >= count && count) state->selected = count - 1;

    if (!gui_animation_on(state) || !count) return;

    UINTN per_page = state->per_page ? state->per_page : 3;
    UINTN page_start = (state->selected / per_page) * per_page;

    if (gap < page_start || gap >= page_start + per_page) return;

    UINTN new_w = visible_row_width(state);
    if (!new_w || new_w >= old_w) return;

    state->hp_first   = gap;
    state->hp_frame   = 0;
    state->hp_anim    = 1;
    state->hp_removal = 1;
    state->hp_shift   = (INTN)(old_w - new_w) / 2;
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
    calc_row_layout(state, start, n, sel_local, is, isp, name_px,
                    &total_w, &sel_left, &sel_ei);
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
        if (i) x += isp;
        UINTN ei = entry_icon_size(e, is);
        UINTN slot_w = entry_slot_width(state, e, ei, name_px);
        UINTN icon_x = x + (slot_w - ei) / 2;
        UINTN iy = (icon_cy > ei / 2) ? icon_cy - ei / 2 : 0;
        if (e->icon) {
            if (state->os_icon_tint_on)
                draw_image_tinted_a(state, e->icon, icon_x, iy, ei, state->os_icon_tint, master);
            else
                draw_image_sized_a(state, e->icon, icon_x, iy, ei, master);
        } else {
            color_t ph = e->type == 0 ? COLOR_GREEN : COLOR_RED;
            fill_round_rect(state, (INTN)icon_x, (INTN)iy, (INTN)ei, (INTN)ei,
                            12, ph, (UINT8)master);
        }
        if (state->show_names) {
            color_t name_col;
            if (!entry_own_color(state, e, &name_col)) {
                if (i == sel_local) name_col = state->name_color;
                else name_col = (color_t){ state->name_color.r * 7 / 10,
                                           state->name_color.g * 7 / 10,
                                           state->name_color.b * 7 / 10 };
            }
            UINTN nw = text_width_px(e->name, name_px);
            INTN  nx = (INTN)x + (INTN)slot_w / 2 - (INTN)nw / 2;
            draw_text_px_a(state, e->name, nx, (INTN)name_y, name_col, name_px, master);
        }
        x += slot_w;
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
        draw_text_px_a(state, lt, lx, cy, cc, csz, master);
    }
    if ((page + 1) * per_page < state->entry_count) {
        CHAR16 gt[] = L">";
        draw_text_px_a(state, gt, (INTN)(start_x + total_w) + gap, cy, cc, csz, master);
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

    color_t name_col;
    if (!entry_own_color(state, e, &name_col)) name_col = state->name_color;
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
        draw_text_px_a(state, e->name, cx - (INTN)nw / 2, (INTN)name_y, name_col, name_px, master);
    if (tbuf[0])
        draw_text_px_a(state, tbuf, cx - (INTN)pw / 2, (INTN)path_y, dim, path_px, master);
}

static void apply_deploy(boot_entry_t *e) {
    if (!e || e->deploy_count == 0) return;
    UINTN s = e->deploy_sel;
    if (s >= e->deploy_count) { s = 0; e->deploy_sel = 0; }
    e->kernel_path = e->deployments[s].kernel;
    e->initrd_path = e->deployments[s].initrd;
    e->cmdline     = e->deployments[s].cmdline;
}

static int v_cycle_next(gui_state_t *state) {
    boot_entry_t *se = entry_at(state, state->selected);
    if (!se) return 0;
    int cur = state->version_mode ? 1 : (state->snap_mode ? 2 : 0);
    int has_dep  = se->deploy_count > 1;
    int has_snap = se->snap_count > 0;
    for (int next = cur + 1; next <= 3; next++) {
        int n = next % 3;
        if (n == 1 && !has_dep) continue;
        if (n == 2 && !has_snap) continue;
        return n;
    }
    return 0;
}

static const CHAR16* v_panel_name(int panel) {
    if (panel == 1) return L"deployments";
    if (panel == 2) return L"snapshots";
    return L"entry";
}

static void v_log_press(gui_state_t *state, boot_entry_t *entry) {
    int panel = state->version_mode ? 1 : (state->snap_mode ? 2 : 0);
    CHAR16 line[192];
    SPrint(line, sizeof(line),
           L"input: V pressed focus=%s selected=%d panel=%s deployments=%d snapshots=%d",
           state->focus == FOCUS_ENTRIES ? L"entries" : L"power",
           (int)state->selected, v_panel_name(panel),
           entry ? (int)entry->deploy_count : 0,
           entry ? (int)entry->snap_count : 0);
    efi_log(line);
    if (entry && entry->name) efi_log(entry->name);
}

static void v_cycle_engage(gui_state_t *state, int what) {
    boot_entry_t *se = entry_at(state, state->selected);
    int cur = state->version_mode ? 1 : (state->snap_mode ? 2 : 0);
    CHAR16 line[96];
    SPrint(line, sizeof(line), L"input: V panel %s -> %s",
           v_panel_name(cur), v_panel_name(what));
    efi_log(line);
    if (what == cur) return;
    int anim = gui_animation_on(state);

    if (what == 1 && se) { se->deploy_sel = se->deploy_default; apply_deploy(se); }
    if (what == 2 && se) { se->snap_sel = 0; state->snap_scroll = 0; }

    if (!anim) {
        state->version_mode = (what == 1);
        state->snap_mode    = (what == 2);
        state->ver_fading = 0; state->ver_next = 0;
        return;
    }
    if (cur == 0) {
        state->version_mode = (what == 1);
        state->snap_mode    = (what == 2);
        state->ver_what = what;
        state->ver_dir = 1; state->ver_frame = 0; state->ver_fading = 1;
        state->ver_next = 0;
    } else {
        state->ver_what = cur;
        state->ver_dir = -1; state->ver_frame = 0; state->ver_fading = 1;
        state->ver_next = what;
    }
}

static const CHAR16* deploy_role_str(int role) {
    if (role == DEPLOY_CURRENT)  return L"current";
    if (role == DEPLOY_ROLLBACK) return L"rollback";
    if (role == DEPLOY_PINNED)   return L"pinned";
    return L"older";
}

static void draw_version_info(gui_state_t *state, boot_entry_t *e,
                              UINTN top_y, UINTN name_px, INTN master) {
    if (!e || e->deploy_count == 0 || master <= 0) return;
    if (master > 255) master = 255;
    UINTN sel = e->deploy_sel; if (sel >= e->deploy_count) sel = 0;
    deployment_t *d = &e->deployments[sel];

    UINTN path_px = (name_px * 4) / 5; if (path_px < 10) path_px = 10;
    color_t name_col;
    if (!entry_own_color(state, e, &name_col)) name_col = state->name_color;
    color_t line_col = { state->name_color.r * 7 / 10,
                         state->name_color.g * 7 / 10,
                         state->name_color.b * 7 / 10 };
    if (d->role == DEPLOY_ROLLBACK) line_col = (color_t){ 0xE0, 0xAF, 0x68 };
    else if (d->role == DEPLOY_PINNED) line_col = (color_t){ 0x7A, 0xA2, 0xF7 };

    CHAR16 line[176];
    SPrint(line, sizeof(line), L"<  %s   %s   %d/%d  >",
           d->version ? d->version : L"?", deploy_role_str(d->role),
           (int)(sel + 1), (int)e->deploy_count);

    UINTN name_y = top_y;
    UINTN line_y = top_y + name_px + 6;
    UINTN nw = text_width_px(e->name, name_px);
    UINTN lw = text_width_px(line, path_px);
    UINTN block_w = nw > lw ? nw : lw;
    UINTN block_h = name_px + 6 + path_px;
    INTN  cx = (INTN)state->screen_width / 2;

    if (state->blur) {
        INTN fpad = 16;
        draw_frost(state, cx - (INTN)block_w / 2 - fpad, (INTN)top_y - fpad,
                   (INTN)block_w + 2 * fpad, (INTN)block_h + 2 * fpad, master);
    }
    draw_text_px_a(state, e->name, cx - (INTN)nw / 2, (INTN)name_y, name_col, name_px, master);
    draw_text_px_a(state, line, cx - (INTN)lw / 2, (INTN)line_y, line_col, path_px, master);
}

static void snap_metrics(gui_state_t *state, boot_entry_t *e, UINTN name_px,
                         INTN avail_top, UINTN *head_h, UINTN *row_h,
                         UINTN *rows, INTN *bottom) {
    UINTN path_px = (name_px * 4) / 5; if (path_px < 10) path_px = 10;
    *row_h  = path_px + 12;
    *head_h = name_px + 14;
    *bottom = (INTN)state->screen_height - 48;
    INTN avail = *bottom - avail_top - (INTN)*head_h;
    INTN fit = avail > (INTN)*row_h ? avail / (INTN)*row_h : 1;
    UINTN r = (UINTN)fit;
    if (e->snap_count < r) r = e->snap_count;
    if (r > 8) r = 8;
    if (r < 1) r = 1;
    *rows = r;
}

static void chop_to_width(CHAR16 *s, UINTN px, UINTN maxw) {
    if (text_width_px(s, px) <= maxw) return;
    UINTN len = 0; while (s[len]) len++;
    while (len > 3 && text_width_px(s, px) > maxw) {
        s[len - 3] = '.'; s[len - 2] = '.'; s[len - 1] = 0;
        len--;
    }
}

static void draw_snap_info(gui_state_t *state, boot_entry_t *e,
                           UINTN name_px, INTN master, INTN expand_pm,
                           INTN avail_top) {
    if (!e || e->snap_count == 0 || master <= 0) return;
    if (master > 255) master = 255;
    if (expand_pm < 0) expand_pm = 0;
    if (expand_pm > 1000) expand_pm = 1000;

    UINTN path_px = (name_px * 4) / 5; if (path_px < 10) path_px = 10;
    UINTN head_h, row_h, rows; INTN bottom;
    snap_metrics(state, e, name_px, avail_top, &head_h, &row_h, &rows, &bottom);

    if (e->snap_sel >= e->snap_count) e->snap_sel = e->snap_count - 1;
    if (e->snap_sel < state->snap_scroll) state->snap_scroll = e->snap_sel;
    if (e->snap_sel >= state->snap_scroll + rows)
        state->snap_scroll = e->snap_sel - rows + 1;
    if (state->snap_scroll + rows > e->snap_count)
        state->snap_scroll = e->snap_count - rows;

    UINTN list_h     = rows * row_h;
    UINTN list_shown = (UINTN)((INT64)list_h * expand_pm / 1000);
    UINTN block_h    = head_h + list_shown;
    INTN  top        = bottom - (INTN)block_h;

    UINTN maxw = state->screen_width * 8 / 10;
    CHAR16 head[144], line[192];
    SPrint(head, sizeof(head), L"%s   snapshot %d/%d",
           e->name, (int)(e->snap_sel + 1), (int)e->snap_count);
    chop_to_width(head, name_px, maxw);
    UINTN block_w = text_width_px(head, name_px);
    for (UINTN i = 0; i < rows && state->snap_scroll + i < e->snap_count; i++) {
        snapshot_t *s = &e->snapshots[state->snap_scroll + i];
        SPrint(line, sizeof(line), L"  #%s   %s   %s", s->id,
               s->date ? s->date : L"", s->desc ? s->desc : L"");
        chop_to_width(line, path_px, maxw);
        UINTN w = text_width_px(line, path_px);
        if (w > block_w) block_w = w;
    }

    INTN cx = (INTN)state->screen_width / 2;
    if (state->blur) {
        INTN fpad = 16;
        draw_frost(state, cx - (INTN)block_w / 2 - fpad, top - fpad,
                   (INTN)block_w + 2 * fpad, (INTN)block_h + 2 * fpad, master);
    }

    color_t name_col;
    if (!entry_own_color(state, e, &name_col)) name_col = state->name_color;
    color_t dim = { state->name_color.r * 6 / 10,
                    state->name_color.g * 6 / 10,
                    state->name_color.b * 6 / 10 };
    color_t sel_col = state->underline_color;

    UINTN hw = text_width_px(head, name_px);
    draw_text_px_a(state, head, cx - (INTN)hw / 2, top, name_col, name_px, master);

    INTN lx = cx - (INTN)block_w / 2;
    INTN y  = top + (INTN)head_h;
    for (UINTN i = 0; i < rows && state->snap_scroll + i < e->snap_count; i++) {
        if (y + (INTN)row_h > top + (INTN)block_h + 1) break;
        UINTN gi = state->snap_scroll + i;
        snapshot_t *s = &e->snapshots[gi];
        int selr = (gi == e->snap_sel);
        SPrint(line, sizeof(line), L"%s#%s   %s   %s",
               selr ? L"> " : L"  ", s->id,
               s->date ? s->date : L"", s->desc ? s->desc : L"");
        chop_to_width(line, path_px, maxw);
        draw_text_px_a(state, line, lx, y, selr ? sel_col : dim, path_px, master);
        y += (INTN)row_h;
    }
}

static void draw_header(gui_state_t *state) {
    int mode = state->logo ? state->logo_mode : LOGO_MODE_OFF;
    int with_logo = (mode != LOGO_MODE_OFF);
    int with_text = state->show_title && mode != LOGO_MODE_ONLY;
    if (!with_logo && !with_text) return;

    CHAR16 *title = (state->title && state->title[0]) ? state->title : L"Visor";
    UINTN title_px = state->title_size ? state->title_size : default_title_px(state);
    UINTN tw = with_text ? text_width_px(title, title_px) : 0;

    UINTN lsz = 0, gap = 0;
    if (with_logo) {
        if (state->logo_size) {
            lsz = state->logo_size;
        } else if (mode == LOGO_MODE_TITLE && with_text) {
            lsz = title_px * 3 / 2;
        } else {
            lsz = title_px * 2;
        }
        gap = state->logo_gap ? state->logo_gap : title_px / 2;

        UINTN room = state->screen_height / 3;
        UINTN over = (mode == LOGO_MODE_ABOVE && with_text) ? gap + title_px : 0;
        if (lsz + over > room) lsz = (room > over) ? room - over : title_px;
        if (lsz > state->screen_width / 3) lsz = state->screen_width / 3;
        if (lsz < 8) lsz = 8;
    }

    UINTN bw, bh;
    if (with_logo && with_text && mode == LOGO_MODE_ABOVE) {
        bw = (lsz > tw) ? lsz : tw;
        bh = lsz + gap + title_px;
    } else if (with_logo && with_text) {
        bw = lsz + gap + tw;
        bh = (lsz > title_px) ? lsz : title_px;
    } else if (with_logo) {
        bw = lsz;
        bh = lsz;
    } else {
        bw = tw;
        bh = title_px;
    }

    INTN bx = (bw < state->screen_width) ? (INTN)(state->screen_width - bw) / 2 : 0;
    INTN by = (INTN)(state->screen_height / 14);

    if (state->blur_title) {
        INTN pad = 18;
        draw_frost(state, bx - pad, by - pad,
                   (INTN)bw + 2 * pad, (INTN)bh + 2 * pad, 255);
    }

    if (with_logo) {
        INTN lx, ly;
        if (with_text && mode == LOGO_MODE_ABOVE) {
            lx = bx + (INTN)(bw - lsz) / 2;
            ly = by;
        } else {
            lx = bx;
            ly = by + (INTN)(bh - lsz) / 2;
        }
        if (state->logo_tint_on)
            draw_image_tinted_a(state, state->logo, (UINTN)lx, (UINTN)ly, lsz,
                                state->logo_tint, 255);
        else
            draw_image_sized(state, state->logo, (UINTN)lx, (UINTN)ly, lsz);
    }

    if (with_text) {
        INTN tx, ty;
        if (with_logo && mode == LOGO_MODE_ABOVE) {
            tx = bx + (INTN)(bw - tw) / 2;
            ty = by + (INTN)(lsz + gap);
        } else if (with_logo) {
            tx = bx + (INTN)(lsz + gap);
            ty = by + (INTN)(bh - title_px) / 2;
        } else {
            tx = bx;
            ty = by;
        }
        draw_text_px(state, title, tx, ty, state->title_color, title_px);
    }
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

        draw_header(state);

        draw_power_actions(state, -1, 0);

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
        UINTN msg_px = default_aux_text_px(state);
        draw_text_centered_px(state, msg, 0, state->screen_width,
                              (INTN)state->screen_height / 2, state->fg_color, msg_px);
        if (!building && state->scene_valid)
            scene_restore_band(state, state->pwr_y0 - 6,
                               state->pwr_y1 - state->pwr_y0 + 12);
        draw_power_actions(state, state->focus == FOCUS_POWER ? (int)state->power_sel : -1, 1);
        return;
    }

    UINTN is      = state->icon_size    ? state->icon_size    : default_icon_size(state);
    UINTN isp     = state->icon_spacing ? state->icon_spacing : default_icon_spacing(state, is);

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

    UINTN icon_cy = state->icon_y ? state->icon_y : state->screen_height / 2;
    UINTN row_top = (icon_cy > max_ei / 2) ? icon_cy - max_ei / 2 : 0;

    UINTN name_px = state->name_size ? state->name_size : default_name_px(state);
    UINTN ul_th   = state->underline_thickness ? state->underline_thickness : 4;
    INTN  pad     = 16;

    UINTN total_w = 0, sel_ei = is;
    INTN  sel_left = 0;
    calc_row_layout(state, page_start, page_n, sel_local, is, isp, name_px,
                    &total_w, &sel_left, &sel_ei);
    UINTN start_x = (state->screen_width > total_w) ? (state->screen_width - total_w) / 2 : 0;
    sel_left += (INTN)start_x;

    UINTN ul_y    = row_top + max_ei + 10;
    UINTN name_y  = ul_y + ul_th + 8;
    UINTN ul_len  = state->underline_length ? state->underline_length
                                            : (sel_ei + 2 * pad - 20);

    int ci_version = state->version_mode ||
                     (state->ver_fading && state->ver_what == 1);
    int ci_snap    = state->snap_mode ||
                     (state->ver_fading && state->ver_what == 2);
    boot_entry_t *ci_e = entry_at(state, state->selected);
    INTN snap_avail_top = (INTN)(name_y + name_px) + pad;
    UINTN ci_block_h;
    if (ci_snap && ci_e && ci_e->snap_count > 0) {
        UINTN sh_h, sr_h, sr; INTN sbot;
        snap_metrics(state, ci_e, name_px, snap_avail_top, &sh_h, &sr_h, &sr, &sbot);
        ci_block_h = sh_h + sr * sr_h;
    } else if (ci_version) {
        ci_block_h = name_px + 6 + ((name_px * 4) / 5);
    } else {
        ci_block_h = center_info_block_h(state, name_px);
    }
    UINTN ci_margin  = 48;
    UINTN ci_top = (state->screen_height > ci_block_h + ci_margin)
                   ? state->screen_height - ci_margin - ci_block_h : name_y;
    INTN  ci_band_lo = (INTN)ci_top - pad - 2;
    INTN  ci_band_hi = (INTN)(ci_top + ci_block_h) + pad + 2;
    int ci_active = state->center_info || ci_version || ci_snap;

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

    int animate = gui_animation_on(state);
    int N = state->anim_frames; if (N < 2) N = 2;

    int first = !state->anim_init;
    if (!animate) {
        state->page_anim = 0;
        state->anim_active = 0;
        state->anim_cross = 0;
    }

    if (animate && !first && page != state->prev_page && !state->page_anim) {
        state->page_anim = 1;
        state->page_frame = 0;
        state->page_old = state->prev_page;
        state->page_old_sel = state->prev_selected;
        state->hp_anim = 0;
        state->hp_removal = 0;
    }

    if (state->page_anim) {
        state->page_frame++;
        INTN fin = ease_alpha(state->page_frame, N); if (fin > 255) fin = 255;
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
            if (animate) {
                for (int k = 0; k < 9; k++) {
                    state->anim_from[k] = state->anim_cur[k];
                    state->anim_to[k]   = tgt[k];
                }
                state->anim_frame  = 0;
                state->anim_active = 1;
                int zc = ((state->focus == FOCUS_POWER) != (state->prev_focus == FOCUS_POWER));
                state->anim_cross = state->blur ? zc : 0;
            } else {
                for (int k = 0; k < 9; k++)
                    state->anim_cur[k] = state->anim_from[k] = state->anim_to[k] = tgt[k];
                state->anim_frame = 0;
                state->anim_active = 0;
                state->anim_cross = 0;
            }
        }
    }
    if (state->anim_active) {
        state->anim_frame++;
        if (state->anim_frame >= N) {
            for (int k = 0; k < 9; k++) state->anim_cur[k] = state->anim_to[k];
            state->anim_active = 0;
            state->anim_cross = 0;
        } else if (!state->anim_cross) {
            INTN e = ease_permille(state->anim_frame, N);
            for (int k = 0; k < 9; k++)
                state->anim_cur[k] = state->anim_from[k]
                                   + (state->anim_to[k] - state->anim_from[k]) * e / 1000;
        }
    }

    int cross = state->anim_active && state->anim_cross;
    INTN fin = cross ? ease_alpha(state->anim_frame, N) : 255;
    INTN fout = 255 - fin;

    INTN ilo[6], ihi[6]; int ni = 0;
    ilo[ni] = (INTN)row_top - pad - 2;
    ihi[ni] = (INTN)(name_y + name_px + pad) + 2; ni++;
    if (ci_active) {
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

    INTN hp_off = 0, hp_a = 255, hp_pm = 1000;
    if (state->hp_anim) {
        hp_pm  = ease_permille(state->hp_frame, N);
        hp_off = state->hp_shift * (1000 - hp_pm) / 1000;
        hp_a   = (INTN)ease_alpha(state->hp_frame, N);
    }

    boot_entry_t *entry = entry_at(state, page_start);
    UINTN x = start_x;
    state->hit_n = 0;
    for (UINTN i = 0; i < page_n && entry; i++) {
        if (i) x += isp;
        UINTN ei = entry_icon_size(entry, is);
        UINTN slot_w = entry_slot_width(state, entry, ei, name_px);
        UINTN icon_x = x + (slot_w - ei) / 2;
        UINTN iy = (icon_cy > ei / 2) ? icon_cy - ei / 2 : 0;

        int  hp_new = state->hp_anim && !state->hp_removal &&
                      page_start + i >= state->hp_first;
        INTN dx = 0;
        if (state->hp_anim) {
            if (state->hp_removal)
                dx = (page_start + i < state->hp_first) ? -hp_off : hp_off;
            else if (!hp_new)
                dx = hp_off;
        }
        UINTN ei_d = ei;
        INTN  ix = (INTN)icon_x + dx, iyy = (INTN)iy;
        if (hp_new) {
            ei_d = ei * (UINTN)(700 + 300 * hp_pm / 1000) / 1000;
            ix  += (INTN)(ei - ei_d) / 2;
            iyy += (INTN)(ei - ei_d) / 2;
        }

        if (state->hit_n < 32) {
            int h = state->hit_n++;
            state->hit_x[h] = (INTN)x;
            state->hit_y[h] = (INTN)iy;
            state->hit_w[h] = (INTN)slot_w;
            state->hit_h[h] = (INTN)((name_y + name_px > iy) ? (name_y + name_px - iy) : ei);
            state->hit_idx[h] = page_start + i;
        }

        if (entry->icon) {
            if (state->os_icon_tint_on)
                draw_image_tinted_a(state, entry->icon, ix, iyy, ei_d,
                                    state->os_icon_tint, hp_new ? hp_a : 255);
            else if (hp_new || dx)
                draw_image_sized_a(state, entry->icon, ix, iyy, ei_d,
                                   hp_new ? hp_a : 255);
            else
                draw_image_sized(state, entry->icon, icon_x, iy, ei);
        } else {
            color_t placeholder = entry->type == 0 ? COLOR_GREEN : COLOR_RED;
            fill_round_rect(state, ix, iyy, ei_d, ei_d,
                            12, placeholder, (UINT8)(hp_new ? hp_a : 255));
        }

        if (state->show_names) {
            color_t name_col;
            if (entry_own_color(state, entry, &name_col)) {
            } else if (page_start + i == state->selected && state->focus == FOCUS_ENTRIES) {
                name_col = state->name_color;
            } else {
                name_col = (color_t){ state->name_color.r * 7 / 10,
                                      state->name_color.g * 7 / 10,
                                      state->name_color.b * 7 / 10 };
            }
            UINTN nw = text_width_px(entry->name, name_px);
            INTN  nx = (INTN)x + dx + (INTN)slot_w / 2 - (INTN)nw / 2;
            if (hp_new)
                draw_text_px_a(state, entry->name, nx, (INTN)name_y, name_col,
                               name_px, hp_a);
            else
                draw_text_px(state, entry->name, nx, (INTN)name_y, name_col, name_px);
        }

        x += slot_w;
        entry = entry->next;
    }

    draw_chevrons(state, page, per_page, start_x, total_w, isp, max_ei, icon_cy, 255);

    draw_power_actions(state, pfocus, 1);

    if (ci_active && state->entry_count > 0) {
        boot_entry_t *se = ci_e;
        INTN vm = 255, xp = 1000;
        if (state->ver_fading) {
            int N = state->anim_frames; if (N < 2) N = 2;
            INTN f = ease_alpha(state->ver_frame, N); if (f > 255) f = 255;
            vm = state->ver_dir > 0 ? f : 255 - f;
            xp = vm * 1000 / 255;
        }
        if (ci_snap && se && se->snap_count > 0) {
            draw_snap_info(state, se, name_px, vm, xp, snap_avail_top);
        } else if (ci_version && se && se->deploy_count > 0) {
            draw_version_info(state, se, ci_top, name_px, vm);
        } else if (state->center_info) {
            draw_center_info(state, se, ci_top, name_px, 255);
        }
    }

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
            UINTN countdown_px = default_aux_text_px(state);
            UINTN cw = text_width_px(buf, countdown_px);
            INTN cx = (state->power_position == POWER_POS_BOTTOMLEFT)
                      ? (INTN)state->screen_width - 30 - (INTN)cw : 30;
            draw_text_px(state, buf, cx,
                         (INTN)state->screen_height - 30 - (INTN)countdown_px,
                         state->fg_color, countdown_px);
        }
    }
}

#define CUR_W 18
#define CUR_H 24

static void draw_cursor(gui_state_t *state) {
    INTN cx = state->cursor_x, cy = state->cursor_y;
    for (INTN j = 0; j <= 20; j++) {
        INTN w = (j <= 14) ? j + 1 : (20 - j) * 3;
        if (w < 1) w = 1;
        fill_rect_alpha(state, cx - 1, cy + j, w + 2, 1, COLOR_BLACK, 220);
    }
    for (INTN j = 0; j <= 20; j++) {
        INTN w = (j <= 14) ? j + 1 : (20 - j) * 3;
        if (w < 1) w = 1;
        fill_rect_alpha(state, cx, cy + j, w, 1, COLOR_WHITE, 255);
    }
}

static void cursor_backing_save(gui_state_t *state, INTN ox, INTN oy) {
    for (INTN j = 0; j < CUR_H; j++)
        for (INTN i = 0; i < CUR_W; i++) {
            UINT32 *p = get_pixel(state, (UINTN)(ox + i), (UINTN)(oy + j));
            state->cursor_save[j * CUR_W + i] = p ? *p : 0;
        }
}

static void cursor_backing_restore(gui_state_t *state, INTN ox, INTN oy) {
    for (INTN j = 0; j < CUR_H; j++)
        for (INTN i = 0; i < CUR_W; i++) {
            UINT32 *p = get_pixel(state, (UINTN)(ox + i), (UINTN)(oy + j));
            if (p) *p = state->cursor_save[j * CUR_W + i];
        }
}

static void cursor_overlay(gui_state_t *state) {
    cursor_backing_save(state, state->cursor_x - 1, state->cursor_y);
    state->cur_prev_x = state->cursor_x;
    state->cur_prev_y = state->cursor_y;
    state->cursor_saved = 1;
    draw_cursor(state);
    gui_present_band(state, state->cursor_y, CUR_H);
}

static void cursor_move(gui_state_t *state) {
    INTN old_y = state->cur_prev_y;
    if (state->cursor_saved)
        cursor_backing_restore(state, state->cur_prev_x - 1, state->cur_prev_y);
    cursor_backing_save(state, state->cursor_x - 1, state->cursor_y);
    state->cur_prev_x = state->cursor_x;
    state->cur_prev_y = state->cursor_y;
    state->cursor_saved = 1;
    draw_cursor(state);
    INTN ny = state->cursor_y;
    INTN lo = old_y < ny ? old_y : ny;
    INTN hi = (old_y > ny ? old_y : ny) + CUR_H;
    if (hi - lo <= 2 * CUR_H)
        gui_present_band(state, lo, hi - lo);
    else {
        gui_present_band(state, old_y, CUR_H);
        gui_present_band(state, ny, CUR_H);
    }
}

static void draw_editor_overlay(gui_state_t *state) {
    UINTN W = state->screen_width, H = state->screen_height;
    fill_rect_alpha(state, 0, 0, (INTN)W, (INTN)H, COLOR_BLACK, 150);

    UINTN th = state->name_size ? state->name_size : 20;
    if (th < 18) th = 18;
    UINTN bw = W * 8 / 10, bx = (W - bw) / 2;
    UINTN bh = state->edit_hint ? th * 5 : th * 4;
    UINTN by = (H - bh) / 2;

    if (state->blur)
        draw_frost(state, (INTN)bx, (INTN)by, (INTN)bw, (INTN)bh, 255);
    else
        fill_round_rect(state, (INTN)bx, (INTN)by, (INTN)bw, (INTN)bh,
                        state->box_radius ? (INTN)state->box_radius : 14,
                        COLOR_BLACK, 215);

    CHAR16 *title = state->edit_title ? state->edit_title
                                      : L"Edit boot options   (Enter = boot, Esc = cancel)";
    draw_text_px_a(state, title, (INTN)bx + 20, (INTN)by + 12, state->underline_color, th * 3 / 4, 255);

    UINTN tx = bx + 20, ty = by + 12 + th;
    int mask = state->edit_secret && !state->edit_reveal;
    CHAR16 secret_buf[512];
    CHAR16 *shown = state->edit_buf;
    if (mask) {
        for (UINTN i = 0; i < state->edit_len && i < 511; i++) secret_buf[i] = '*';
        secret_buf[state->edit_len < 511 ? state->edit_len : 511] = 0;
        shown = secret_buf;
    }
    draw_text_px_a(state, shown, (INTN)tx, (INTN)ty, COLOR_WHITE, th, 255);

    if (state->edit_hint)
        draw_text_px_a(state, state->edit_hint, (INTN)tx, (INTN)(ty + th + th / 4),
                       COLOR_WHITE, th * 5 / 8, 170);

    CHAR16 tmp[512];
    UINTN k = 0;
    for (; k < state->edit_cursor && k < 511; k++) tmp[k] = mask ? '*' : state->edit_buf[k];
    tmp[k] = 0;
    UINTN caret = tx + text_width_px(tmp, th);
    fill_rect_alpha(state, (INTN)caret, (INTN)ty, 2, (INTN)th,
                    state->underline_color, 255);
}

static void editor_enter(gui_state_t *state) {
    boot_entry_t *e = entry_at(state, state->selected);
    state->edit_secret = 0;
    state->edit_reveal = 0;
    state->edit_title = NULL;
    state->edit_hint = NULL;
    state->edit_len = 0;
    if (e && e->cmdline)
        while (e->cmdline[state->edit_len] && state->edit_len < 511) {
            state->edit_buf[state->edit_len] = e->cmdline[state->edit_len];
            state->edit_len++;
        }
    state->edit_buf[state->edit_len] = 0;
    state->edit_cursor = state->edit_len;
    state->editing = 1;
}

static void prompt_enter(gui_state_t *state, CHAR16 *title, CHAR16 *hint) {
    state->edit_secret = 1;
    state->edit_reveal = 0;
    state->edit_title = title;
    state->edit_hint = hint;
    state->edit_len = 0;
    state->edit_cursor = 0;
    state->edit_buf[0] = 0;
    state->editing = 1;
}

static int editor_key(gui_state_t *state, EFI_INPUT_KEY *key) {
    if (key->UnicodeChar == 0x0D) {
        state->edit_buf[state->edit_len] = 0;
        if (!state->edit_secret) {
            if (state->override_cmdline) efi_free_pool(state->override_cmdline);
            state->override_cmdline = efi_strdup(state->edit_buf);
        }
        state->editing = 0;
        return 1;
    }
    if (key->UnicodeChar == 0x1B || (key->UnicodeChar == 0x00 && key->ScanCode == 0x17)) {
        state->editing = 0;
        return -1;
    }
    if (key->UnicodeChar == 0x08) {
        if (state->edit_cursor > 0) {
            for (UINTN i = state->edit_cursor - 1; i + 1 < state->edit_len; i++)
                state->edit_buf[i] = state->edit_buf[i + 1];
            state->edit_len--;
            state->edit_cursor--;
            state->edit_buf[state->edit_len] = 0;
        }
        return 0;
    }
    if (key->UnicodeChar == 0x00) {
        if (key->ScanCode == 0x04 && state->edit_cursor > 0) state->edit_cursor--;
        else if (key->ScanCode == 0x03 && state->edit_cursor < state->edit_len) state->edit_cursor++;
        else if (key->ScanCode == 0x0C && state->edit_secret)
            state->edit_reveal = !state->edit_reveal;
        return 0;
    }
    CHAR16 c = key->UnicodeChar;
    if (c >= 0x20 && state->edit_len < 510) {
        for (UINTN i = state->edit_len; i > state->edit_cursor; i--)
            state->edit_buf[i] = state->edit_buf[i - 1];
        state->edit_buf[state->edit_cursor] = c;
        state->edit_len++;
        state->edit_cursor++;
        state->edit_buf[state->edit_len] = 0;
    }
    return 0;
}

static int point_in(INTN px, INTN py, INTN x, INTN y, INTN w, INTN h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static int poll_pointer(gui_state_t *state, int *menu_redraw) {
    if (!state->mouse_enabled || !state->has_pointer) return 0;
    static int prev_btn = 0;
    int moved = 0, btn = 0, scroll = 0;

    if (state->app) {
        EFI_ABSOLUTE_POINTER_PROTOCOL *ap = state->app;
        EFI_ABSOLUTE_POINTER_STATE st;
        while (!EFI_ERROR(ap->GetState(ap, &st)) && ap->Mode) {
            UINT64 minx = ap->Mode->AbsoluteMinX, maxx = ap->Mode->AbsoluteMaxX;
            UINT64 miny = ap->Mode->AbsoluteMinY, maxy = ap->Mode->AbsoluteMaxY;
            if (maxx > minx)
                state->cursor_x = (INTN)((st.CurrentX - minx) * (state->screen_width - 1) / (maxx - minx));
            if (maxy > miny)
                state->cursor_y = (INTN)((st.CurrentY - miny) * (state->screen_height - 1) / (maxy - miny));
            moved = 1;
            if (st.ActiveButtons & EFI_ABSP_TouchActive) btn = 1;
        }
    }
    if (state->spp) {
        EFI_SIMPLE_POINTER_PROTOCOL *sp = state->spp;
        EFI_SIMPLE_POINTER_STATE st;
        INTN dx = 0, dy = 0;
        while (!EFI_ERROR(sp->GetState(sp, &st))) {
            UINT64 rx = sp->Mode ? sp->Mode->ResolutionX : 0;
            UINT64 ry = sp->Mode ? sp->Mode->ResolutionY : 0;
            INTN mx = st.RelativeMovementX, my = st.RelativeMovementY;
            if (rx > 1) mx = mx / (INTN)rx;
            if (ry > 1) my = my / (INTN)ry;
            dx += mx; dy += my;
            if (st.RelativeMovementZ > 0) scroll = 1;
            else if (st.RelativeMovementZ < 0) scroll = -1;
            if (st.LeftButton) btn = 1;
        }
        if (dx || dy) {
            UINTN speed = state->pointer_speed;
            if (speed < 1) speed = 1;
            if (speed > 20) speed = 20;
            state->cursor_x += dx * (INTN)speed;
            state->cursor_y += dy * (INTN)speed;
            moved = 1;
        }
    }

    if (state->cursor_x < 0) state->cursor_x = 0;
    if (state->cursor_y < 0) state->cursor_y = 0;
    if (state->cursor_x >= (INTN)state->screen_width)  state->cursor_x = (INTN)state->screen_width - 1;
    if (state->cursor_y >= (INTN)state->screen_height) state->cursor_y = (INTN)state->screen_height - 1;

    if (state->version_mode || state->snap_mode) {
        boot_entry_t *se = entry_at(state, state->selected);
        if (state->snap_mode) {
            if (se && se->snap_count > 0) {
                if (scroll > 0 && se->snap_sel + 1 < se->snap_count) { se->snap_sel++; *menu_redraw = 1; }
                else if (scroll < 0 && se->snap_sel > 0) { se->snap_sel--; *menu_redraw = 1; }
            }
        } else if (se && se->deploy_count > 1) {
            if (scroll > 0 && se->deploy_sel + 1 < se->deploy_count) { se->deploy_sel++; apply_deploy(se); *menu_redraw = 1; }
            else if (scroll < 0 && se->deploy_sel > 0) { se->deploy_sel--; apply_deploy(se); *menu_redraw = 1; }
        }
        if (moved) state->cursor_active = 1;
        prev_btn = btn;
        return moved ? 2 : 0;
    }

    if (scroll > 0 && state->selected + 1 < state->entry_count) {
        state->selected++; state->focus = FOCUS_ENTRIES; *menu_redraw = 1;
    } else if (scroll < 0 && state->selected > 0) {
        state->selected--; state->focus = FOCUS_ENTRIES; *menu_redraw = 1;
    }

    if (moved) {
        state->cursor_active = 1;
        if (state->timeout_active) { state->timeout_active = 0; *menu_redraw = 1; }
        int hovered = 0;
        for (int i = 0; i < state->hit_n; i++) {
            if (point_in(state->cursor_x, state->cursor_y,
                         state->hit_x[i], state->hit_y[i], state->hit_w[i], state->hit_h[i])) {
                if (state->hit_idx[i] != state->selected || state->focus != FOCUS_ENTRIES) {
                    state->selected = state->hit_idx[i];
                    state->focus = FOCUS_ENTRIES;
                    *menu_redraw = 1;
                }
                hovered = 1;
                break;
            }
        }
        if (!hovered)
            for (int i = 0; i < 3; i++) {
                if (state->pwr_w[i] <= 0) continue;
                if (point_in(state->cursor_x, state->cursor_y,
                             state->pwr_x[i], state->pwr_y[i], state->pwr_w[i], state->pwr_h[i])) {
                    if (state->focus != FOCUS_POWER || state->power_sel != (UINTN)i) {
                        state->focus = FOCUS_POWER;
                        state->power_sel = (UINTN)i;
                        *menu_redraw = 1;
                    }
                    break;
                }
            }
    }

    int clicked = (btn && !prev_btn);
    prev_btn = btn;
    if (clicked) {
        for (int i = 0; i < state->hit_n; i++) {
            if (point_in(state->cursor_x, state->cursor_y,
                         state->hit_x[i], state->hit_y[i], state->hit_w[i], state->hit_h[i])) {
                state->selected = state->hit_idx[i];
                state->focus = FOCUS_ENTRIES;
                state->action = VISOR_ACTION_BOOT;
                return 1;
            }
        }
        for (int i = 0; i < 3; i++) {
            if (state->pwr_w[i] <= 0) continue;
            if (point_in(state->cursor_x, state->cursor_y,
                         state->pwr_x[i], state->pwr_y[i], state->pwr_w[i], state->pwr_h[i])) {
                state->action = VISOR_ACTION_SHUTDOWN + i;
                return 1;
            }
        }
    }
    return moved ? 2 : 0;
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
    int  intro_fade = 1;

    while (state->running) {
        if (need_redraw) {
            INTN ghost_y = -1;
            if (state->cursor_saved) {
                cursor_backing_restore(state, state->cur_prev_x - 1, state->cur_prev_y);
                ghost_y = state->cur_prev_y;
                state->cursor_saved = 0;
            }
            gui_draw_menu(state, !full_redraw);
            if (state->editing) draw_editor_overlay(state);
            if (intro_fade && full_redraw && !state->editing) {
                gui_fade_in_current(state);
                intro_fade = 0;
            } else if (full_redraw || state->editing) {
                gui_present(state);
            } else {
                for (int b = 0; b < state->band_n; b++)
                    gui_present_band(state, state->band_y[b], state->band_h[b]);
                if (ghost_y >= 0) gui_present_band(state, ghost_y, CUR_H);
            }
            if (state->editing) intro_fade = 0;
            need_redraw = state->anim_active || state->page_anim || state->hp_anim;
            full_redraw = 0;
            if (state->cursor_active && !state->editing)
                cursor_overlay(state);
        }

        if (!state->editing) {
            int menu_rd = 0;
            int pr = poll_pointer(state, &menu_rd);
            if (pr == 1) state->running = 0;
            else if (menu_rd) need_redraw = 1;
            else if (pr == 2 && state->cursor_active && !need_redraw)
                cursor_move(state);
        }

        status = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (!EFI_ERROR(status)) {

            if (state->timeout_active) { state->timeout_active = 0; need_redraw = 1; full_redraw = 1; }

            if (state->editing) {
                editor_key(state, &key);
                need_redraw = 1; full_redraw = 1;
                continue;
            }

            if (state->version_mode || state->snap_mode) {
                boot_entry_t *se = entry_at(state, state->selected);
                int snap = state->snap_mode;
                CHAR16 vu = key.UnicodeChar;
                if (vu >= 'a' && vu <= 'z') vu -= 32;
                if (vu == 'V') {
                    v_log_press(state, se);
                    v_cycle_engage(state, v_cycle_next(state));
                    need_redraw = 1; full_redraw = 1;
                } else if (key.UnicodeChar == 0x0D) {
                    if (snap && se && se->snap_count > 0) {
                        snapshot_t *s = &se->snapshots[se->snap_sel];
                        state->override_cmdline = s->cmdline ? efi_strdup(s->cmdline) : NULL;
                        if (s->kernel) state->override_kernel_path = efi_strdup(s->kernel);
                        if (s->initrd) { state->override_initrd_path = efi_strdup(s->initrd);
                                         state->override_initrd_set = 1; }
                    }
                    state->running = 0;
                } else if (key.UnicodeChar == 0x1B ||
                           (key.UnicodeChar == 0x00 && key.ScanCode == 0x17)) {
                    if (!snap && se) { se->deploy_sel = se->deploy_default; apply_deploy(se); }
                    if (state->center_info || !gui_animation_on(state)) {
                        state->version_mode = 0; state->snap_mode = 0;
                        state->ver_fading = 0; state->ver_next = 0;
                    } else {
                        state->ver_what = snap ? 2 : 1;
                        state->ver_dir = -1; state->ver_frame = 0; state->ver_fading = 1;
                        state->ver_next = 0;
                    }
                    need_redraw = 1; full_redraw = 1;
                } else if (key.UnicodeChar == 0x00 &&
                           (key.ScanCode == 0x04 || (snap && key.ScanCode == 0x01))) {
                    if (snap) {
                        if (se && se->snap_sel > 0) { se->snap_sel--; need_redraw = 1; full_redraw = 1; }
                    } else if (se && se->deploy_sel > 0) {
                        se->deploy_sel--; apply_deploy(se); need_redraw = 1; full_redraw = 1;
                    }
                } else if (key.UnicodeChar == 0x00 &&
                           (key.ScanCode == 0x03 || (snap && key.ScanCode == 0x02))) {
                    if (snap) {
                        if (se && se->snap_sel + 1 < se->snap_count) { se->snap_sel++; need_redraw = 1; full_redraw = 1; }
                    } else if (se && se->deploy_sel + 1 < se->deploy_count) {
                        se->deploy_sel++; apply_deploy(se); need_redraw = 1; full_redraw = 1;
                    }
                } else if (vu == 'S') { state->action = VISOR_ACTION_SHUTDOWN; state->running = 0; }
                else if (vu == 'R') { state->action = VISOR_ACTION_REBOOT; state->running = 0; }
                else if (vu == 'F') { state->action = VISOR_ACTION_FIRMWARE; state->running = 0; }
                continue;
            }

            CHAR16 uc = key.UnicodeChar;
            if (uc >= 'a' && uc <= 'z') uc -= 32;

            if (uc == 'V') {
                boot_entry_t *se = entry_at(state, state->selected);
                v_log_press(state, se);
                if (state->focus == FOCUS_ENTRIES && se &&
                    (se->deploy_count > 1 || se->snap_count > 0)) {
                    v_cycle_engage(state, v_cycle_next(state));
                    need_redraw = 1; full_redraw = 1;
                } else if (state->focus != FOCUS_ENTRIES)
                    efi_log(L"input: V ignored because power actions have focus");
                else if (!se)
                    efi_log(L"input: V ignored because no boot entry is selected");
                else
                    efi_log(L"input: V ignored because the entry has no alternate deployments or snapshots");
            }
            else if (uc == 'E') {
                if (state->focus == FOCUS_ENTRIES && state->editor_enabled && state->entry_count > 0) {
                    editor_enter(state);
                    need_redraw = 1; full_redraw = 1;
                }
            }
            else if (uc == 'S') { state->action = VISOR_ACTION_SHUTDOWN; state->running = 0; }
            else if (uc == 'R') { state->action = VISOR_ACTION_REBOOT; state->running = 0; }
            else if (uc == 'F') { state->action = VISOR_ACTION_FIRMWARE; state->running = 0; }
            else if (key.UnicodeChar == 0x1B) {
                efi_log(L"input: Esc pressed at the menu - opening options/rescue console");
                state->action = VISOR_ACTION_RESCUE;
                state->running = 0;
            }
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
                        if (state->entry_count) {
                            if (state->selected > 0) state->selected--;
                            else state->selected = state->entry_count - 1;
                        }
                        need_redraw = 1;
                        break;
                    case 0x03:
                        state->focus = FOCUS_ENTRIES;
                        if (state->entry_count) {
                            if (state->selected + 1 < state->entry_count) state->selected++;
                            else state->selected = 0;
                        }
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
                        efi_log(L"input: Esc pressed at the menu - opening options/rescue console");
                        state->action = VISOR_ACTION_RESCUE;
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

        if (state->ver_fading) {
            int N = state->anim_frames; if (N < 2) N = 2;
            state->ver_frame++;
            need_redraw = 1; full_redraw = 1;
            if (state->ver_frame >= N) {
                state->ver_fading = 0;
                state->ver_frame = 0;
                if (state->ver_dir < 0) {
                    state->version_mode = 0;
                    state->snap_mode = 0;
                    if (state->ver_next) {
                        int nx = state->ver_next;
                        state->ver_next = 0;
                        state->version_mode = (nx == 1);
                        state->snap_mode    = (nx == 2);
                        state->ver_what = nx;
                        state->ver_dir = 1; state->ver_fading = 1;
                    }
                }
            }
        }

        if (state->hp_anim) {
            int N = state->anim_frames; if (N < 2) N = 2;
            state->hp_frame++;
            need_redraw = 1;
            if (state->hp_frame >= N) {
                state->hp_anim = 0;
                state->hp_frame = 0;
                state->hp_removal = 0;
            }
        }

        if (state->hotplug_poll && !state->editing &&
            !state->page_anim && !state->hp_anim && !state->anim_active) {
            UINT64 now = efi_get_tick();
            if (!state->hp_last_ms) state->hp_last_ms = now;
            if (now - state->hp_last_ms >= 1200) {
                state->hp_last_ms = now;
                UINTN pre_w = visible_row_width(state);
                boot_entry_t *head = state->entries;
                UINTN cnt = state->entry_count;
                UINTN first = state->entry_count;
                int mask = state->hotplug_poll(state->hotplug_ctx,
                                               &head, &cnt, &first);
                if (mask == 1 && cnt > state->entry_count) {
                    gui_entries_added(state, head, cnt, first);
                } else if (mask == 2) {
                    gui_entries_removed(state, head, cnt, first, pre_w);
                } else if (mask) {

                    state->entries = head;
                    state->entry_count = cnt;
                    state->hp_anim = 0;
                    state->hp_removal = 0;
                    if (state->selected >= cnt && cnt)
                        state->selected = cnt - 1;
                }
                if (mask) {
                    need_redraw = 1;
                    full_redraw = 1;
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

        efi_sleep((state->anim_active || state->page_anim ||
                   state->ver_fading || state->hp_anim) ? 6
                  : (state->cursor_active ? 12 : 30));
    }

    if (state->action != VISOR_ACTION_BOOT) return NULL;

    boot_entry_t *selected = state->entries;
    for (UINTN i = 0; i < state->selected && selected; i++) {
        selected = selected->next;
    }
    return selected;
}

EFI_STATUS gui_prompt_password(gui_state_t *state, CHAR16 *title, CHAR16 *hint,
                               CHAR16 **out) {
    EFI_STATUS status;
    EFI_INPUT_KEY key;
    int running = 1;
    int accepted = 0;

    if (!state || !out) return EFI_INVALID_PARAMETER;
    *out = NULL;

    prompt_enter(state, title ? title : L"Password   (Enter = boot, Esc = cancel)",
                 hint ? hint : L"F2 shows what you typed - use it to check your keyboard layout");

    while (running) {
        gui_draw_menu(state, 0);
        draw_editor_overlay(state);
        gui_present(state);

        status = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_ERROR(status)) {
            efi_sleep(30);
            continue;
        }

        int r = editor_key(state, &key);
        if (r == 1) { accepted = 1; running = 0; }
        else if (r < 0) { running = 0; }
    }

    state->edit_secret = 0;
    state->edit_reveal = 0;
    state->edit_title = NULL;
    state->edit_hint = NULL;
    state->editing = 0;

    if (!accepted) {
        wipe16(state->edit_buf, 512);
        state->edit_len = state->edit_cursor = 0;
        return EFI_ABORTED;
    }
    *out = efi_strdup(state->edit_buf);
    wipe16(state->edit_buf, 512);
    state->edit_len = state->edit_cursor = 0;
    return *out ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
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
    free_icon(state->logo);
    state->logo = NULL;
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
    glyph_cache_flush();
}
