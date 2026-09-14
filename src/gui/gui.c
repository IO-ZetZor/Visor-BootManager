/* gui.c - framebuffer setup, mode selection and presentation */
#include "gui_internal.h"

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
    state->sound_start = NULL;
    state->sound_poll = NULL;
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
    state->show_clock = 0;
    state->accent_clock = 1;
    state->clock_color = COLOR_WHITE;
    state->clock_size = 0;
    state->clock_24h = 1;
    state->clock_seconds = 0;
    state->clock_position = CLOCK_POS_TOPRIGHT;
    state->clock_date = 0;
    state->clock_date_format = CLOCK_DATE_LONG;
    state->clock_blur = 0;
    state->clock_shadow = 1;
    state->clock_last_key = -1;
    state->clock_x = state->clock_y = 0;
    state->clock_w = state->clock_h = 0;
    state->clock_drawn = 0;
    state->clock_dirty = 0;
    state->screensaver = 0;
    state->ss_delay_ms = 60000;
    state->ss_blank_ms = 600000;
    state->ss_keep_clock = 1;
    state->ss_level = SS_LEVEL_AWAKE;
    state->ss_last_input_ms = 0;
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
    state->bg_anim = NULL;
    state->browse = NULL;

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
    state->blur_line  = NULL;
    state->blur_w = state->blur_h = 0;
    state->blur_shift = 2;
    state->blur_gen = 0;
    state->blur_valid = 0;
    state->bg_gen = 0;
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
    blur_free(state);

    state->backbuffer  = efi_allocate_pool(px * sizeof(UINT32));
    state->scene_cache = efi_allocate_pool(px * sizeof(UINT32));
    state->scene_valid = 0;
    if (!state->backbuffer) return EFI_OUT_OF_RESOURCES;

    gui_fill_rect(state, 0, 0, state->screen_width, state->screen_height, state->bg_color);

    gui_fb_set_wc(state);
    gui_present(state);
    return EFI_SUCCESS;
}
