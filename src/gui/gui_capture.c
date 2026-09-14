/* gui_capture.c - screenshot and screen recording (feature: capture) */
#include "gui_internal.h"
int  cap_overlay_live(gui_state_t *state) {
    return state->cap_mode != 0 || state->cap_status_ms > 0;
}

#define CAP_COUNTDOWN_MS   3000u
#define CAP_FRAME_MS       50u
#define CAP_RECORD_MS_MIN  1000u
#define CAP_RECORD_MS_MAX  12000u
#define CAP_MAX_WIDTH      960u
#define CAP_BUDGET_BYTES   (20u * 1024u * 1024u)
#define CAP_TOAST_MS       2600
#define CAP_SHOTS_DIR      L"\\EFI\\visor\\shots"

static UINTN cap_record_ms(gui_state_t *state) {
    UINTN ms = (state->record_seconds ? state->record_seconds : 3) * 1000;
    if (ms < CAP_RECORD_MS_MIN) ms = CAP_RECORD_MS_MIN;
    if (ms > CAP_RECORD_MS_MAX) ms = CAP_RECORD_MS_MAX;
    return ms;
}

static UINTN cap_max_frames(gui_state_t *state) {
    UINTN n = cap_record_ms(state) / CAP_FRAME_MS + 1;
    if (n > CAP_GIF_MAX_FRAMES) n = CAP_GIF_MAX_FRAMES;
    return n;
}

void cap_set_toast(gui_state_t *state, const CHAR16 *msg,
                          const CHAR16 *detail, int is_err) {
    SPrint(state->cap_status, sizeof(state->cap_status), L"%s", msg);
    if (detail) SPrint(state->cap_detail, sizeof(state->cap_detail), L"%s", detail);
    else        state->cap_detail[0] = 0;
    state->cap_status_err = is_err;
    state->cap_status_ms  = CAP_TOAST_MS;
    state->cap_last_ms    = 0;
}

static void cap_size_text(CHAR16 *out, UINTN cap, UINTN bytes) {
    if (bytes >= 1024u * 1024u) {
        UINTN whole = bytes / (1024u * 1024u);
        UINTN frac  = (bytes % (1024u * 1024u)) * 10u / (1024u * 1024u);
        SPrint(out, cap, L"%d.%d MiB", (int)whole, (int)frac);
    } else if (bytes >= 1024u) {
        UINTN whole = bytes / 1024u;
        UINTN frac  = (bytes % 1024u) * 10u / 1024u;
        SPrint(out, cap, L"%d.%d KiB", (int)whole, (int)frac);
    } else {
        SPrint(out, cap, L"%d bytes", (int)bytes);
    }
}

static EFI_STATUS cap_write_shot(const CHAR16 *name, const UINT8 *data,
                                 UINTN size) {
    if (!EFI_ERROR(cap_ensure_dir(L"\\EFI\\visor")))
        cap_ensure_dir(CAP_SHOTS_DIR);
    CHAR16 full[256];
    SPrint(full, sizeof(full), CAP_SHOTS_DIR L"\\%s", name);
    return cap_save_file(full, data, size);
}

void cap_do_screenshot(gui_state_t *state) {
    if (!state->backbuffer || !state->screen_width || !state->screen_height) return;
    UINT8 *png = NULL; UINTN pngsz = 0;
    EFI_STATUS st = cap_png_encode(state->backbuffer, state->screen_width,
                                   state->screen_height, &png, &pngsz);
    if (EFI_ERROR(st) || !png) {
        cap_set_toast(state, L"Screenshot failed", L"Out of memory", 1);
        if (png) efi_free_pool(png);
        return;
    }
    CHAR16 name[192], detail[128], size[32];
    cap_timestamp_name(name, 192, L"shot", L".png");
    st = cap_write_shot(name, png, pngsz);
    efi_free_pool(png);
    if (EFI_ERROR(st)) {
        cap_set_toast(state, L"Screenshot not saved",
                      L"Could not write to " CAP_SHOTS_DIR, 1);
        return;
    }
    cap_size_text(size, sizeof(size) / sizeof(CHAR16), pngsz);
    SPrint(detail, sizeof(detail), L"%s  -  %d x %d  -  %s", name,
           (int)state->screen_width, (int)state->screen_height, size);
    cap_set_toast(state, L"Screenshot saved", detail, 0);
}

void cap_start_record(gui_state_t *state) {
    if (!state->backbuffer || !state->screen_width || !state->screen_height) return;
    state->cap_mode = 2;
    state->cap_start_ms = efi_get_tick();
    state->cap_frames = 0;
    state->cap_truncated = 0;
    state->cap_next_due_ms = 0;
    state->cap_gif = NULL;
    state->cap_sec_prev = CAP_COUNTDOWN_MS / 1000 + 1;
    state->cap_status[0] = 0;
    state->cap_detail[0] = 0;
    state->cap_status_ms = -1;

    state->cap_gif = cap_gif_new(state->screen_width, state->screen_height,
                                 CAP_MAX_WIDTH, cap_max_frames(state),
                                 CAP_BUDGET_BYTES,
                                 CAP_FRAME_MS / 10);
    if (!state->cap_gif) {
        state->cap_mode = 0;
        cap_set_toast(state, L"Cannot start recording",
                      L"Not enough memory for the encoder", 1);
    }
}

void cap_cancel_record(gui_state_t *state) {
    if (state->cap_gif) { cap_gif_free(state->cap_gif); state->cap_gif = NULL; }
    state->cap_mode = 0;
    cap_set_toast(state, L"Recording cancelled", NULL, 0);
}

void cap_finish_record(gui_state_t *state) {
    cap_gif *g = state->cap_gif;
    UINTN frames = cap_gif_count(g);
    UINTN gw = cap_gif_width(g), gh = cap_gif_height(g);
    state->cap_gif = NULL;
    state->cap_mode = 0;

    UINT8 *data = NULL; UINTN sz = 0;
    EFI_STATUS st = g ? cap_gif_close(g, &data, &sz) : EFI_DEVICE_ERROR;
    if (EFI_ERROR(st) || !data) {
        cap_set_toast(state, L"Recording failed",
                      frames ? L"Could not assemble the GIF"
                             : L"No frames were captured", 1);
        return;
    }

    CHAR16 name[192], detail[128], size[32];
    cap_timestamp_name(name, 192, L"rec", L".gif");
    st = cap_write_shot(name, data, sz);
    efi_free_pool(data);

    if (EFI_ERROR(st)) {
        cap_set_toast(state, L"Recording not saved",
                      L"Could not write to " CAP_SHOTS_DIR, 1);
        return;
    }
    cap_size_text(size, sizeof(size) / sizeof(CHAR16), sz);
    SPrint(detail, sizeof(detail), L"%s  -  %d frames  -  %d x %d  -  %s",
           name, (int)frames, (int)gw, (int)gh, size);
    cap_set_toast(state,
                  state->cap_truncated ? L"Recording saved (cut short)"
                                       : L"Recording saved",
                  detail, state->cap_truncated ? 1 : 0);
}

int cap_grab_due_frames(gui_state_t *state) {
    if (!state->cap_gif) return 0;
    UINT64 now = efi_get_tick();
    UINTN  span = cap_record_ms(state);

    for (int burst = 0; burst < 3; burst++) {
        if (state->cap_next_due_ms > now - state->cap_start_ms) break;
        if (state->cap_next_due_ms >= span) return 0;

        int r = cap_gif_frame(state->cap_gif, state->backbuffer, now);
        if (r == CAP_FRAME_ERROR) return 0;
        state->cap_frames = cap_gif_count(state->cap_gif);

        UINT64 el = now - state->cap_start_ms;
        do { state->cap_next_due_ms += CAP_FRAME_MS; }
        while (state->cap_next_due_ms <= el);

        if (r == CAP_FRAME_FULL) {
            state->cap_truncated = 1;
            return 0;
        }
    }
    return 1;
}

int cap_tick(gui_state_t *state) {
    UINT64 now = efi_get_tick();

    if (state->cap_mode == 0) {
        if (state->cap_status_ms < 0) return 0;
        if (!state->cap_last_ms) state->cap_last_ms = now;
        INTN dt = (INTN)(now - state->cap_last_ms);
        state->cap_last_ms = now;
        if (dt > 0) state->cap_status_ms -= dt;
        if (state->cap_status_ms <= 0) {
            state->cap_status_ms = -1;
            return 2;
        }
        return state->cap_status_ms < CAP_TOAST_FADE_MS ? 2 : 0;
    }

    UINT64 el = now - state->cap_start_ms;

    if (state->cap_mode == 2) {
        if (state->cap_gif && state->backbuffer)
            cap_gif_sample(state->cap_gif, state->backbuffer);

        if (el >= CAP_COUNTDOWN_MS) {
            state->cap_mode = 3;
            state->cap_start_ms = now;
            state->cap_frames = 0;
            state->cap_next_due_ms = 0;
            state->cap_sec_prev = cap_record_ms(state) / 1000 + 1;
            return 2;
        }
        UINTN sec = (CAP_COUNTDOWN_MS - (UINT64)el + 999) / 1000;
        if (sec != state->cap_sec_prev) {
            state->cap_sec_prev = sec;
            return 2;
        }
        return 2;
    }

    if (state->cap_mode == 3) {
        UINTN span = cap_record_ms(state);
        if (el >= span || state->cap_truncated ||
            cap_gif_is_full(state->cap_gif)) {
            state->cap_mode = 4;
            return 2;
        }
        return 0;
    }

    return 0;
}

static void cap_draw_rec_badge(gui_state_t *state) {
    UINT64 el = efi_get_tick() - state->cap_start_ms;
    UINTN span = cap_record_ms(state);
    UINTN rem_ms = el >= span ? 0 : span - (UINTN)el;
    UINTN rem = (rem_ms + 999) / 1000;

    UINTN px = 21;
    CHAR16 label[16], count[24];
    SPrint(label, sizeof(label), L"REC");
    SPrint(count, sizeof(count), L"%ds  %d fr", (int)rem, (int)state->cap_frames);

    INTN dot = 10;
    INTN lw = (INTN)text_width_px(label, px);
    INTN cw = (INTN)text_width_px(count, px * 5 / 6);
    INTN bw = 15 + dot + 9 + lw + 12 + cw + 15;
    INTN bh = (INTN)px + 26;
    INTN bx = (INTN)state->screen_width - bw - 24;
    INTN by = 16;
    if (bx < 8) bx = 8;

    fill_round_rect(state, bx + 1, by + 3, bw, bh, bh / 2, COLOR_BLACK, 70);
    fill_round_rect(state, bx, by, bw, bh, bh / 2, COLOR_BLACK, 205);
    fill_round_rect(state, bx, by, bw, 1, 0, COLOR_WHITE, 30);

    INTN pulse = ((el / 250) & 1) ? 150 : 255;
    card_dot(state, bx + 15, by + (bh - dot) / 2, dot, COLOR_RED, pulse);

    INTN tx = bx + 15 + dot + 9;
    INTN ty = by + 7;
    draw_text_px_a(state, label, tx, ty, COLOR_RED, px, 255);
    draw_text_px_a(state, count, tx + lw + 12, ty + 1, COLOR_GRAY,
                   px * 5 / 6, 225);

    card_bar(state, bx + 15, by + bh - 9, bw - 30, 4,
             el > span ? span : (UINTN)el, span, COLOR_RED);
}

static void cap_draw_countdown(gui_state_t *state) {
    UINT64 el = efi_get_tick() - state->cap_start_ms;
    UINTN left_ms = el >= CAP_COUNTDOWN_MS ? 0 : CAP_COUNTDOWN_MS - (UINTN)el;
    UINTN sec = (left_ms + 999) / 1000;

    card_t c;
    card_metrics(state, &c);

    CHAR16 big[8], sub[96];
    SPrint(big, sizeof(big), L"%d", (int)sec);
    SPrint(sub, sizeof(sub), L"%d fps  -  %ds  -  up to %d px wide",
           (int)(1000 / CAP_FRAME_MS), (int)(cap_record_ms(state) / 1000),
           (int)(state->screen_width < CAP_MAX_WIDTH
                 ? state->screen_width : CAP_MAX_WIDTH));

    UINTN big_px = c.title_px * 5 / 2;
    INTN bw = 460;
    if (bw > (INTN)state->screen_width - 80) bw = (INTN)state->screen_width - 80;
    INTN bh = c.pad + (INTN)c.title_px + 11
            + (INTN)big_px + 14 + 6 + 14
            + (INTN)c.small_px + 8
            + card_hints_h(&c) + c.pad / 2;

    card_open(state, &c, bw, bh);
    card_title(state, &c, L"Recording starts in", COLOR_RED, COLOR_WHITE);

    draw_text_centered_px(state, big, c.x, (UINTN)c.w, c.cy, COLOR_RED, big_px);
    c.cy += (INTN)big_px + 14;

    card_bar(state, c.x + c.pad, c.cy, c.w - 2 * c.pad, 6,
             (UINTN)el > CAP_COUNTDOWN_MS ? CAP_COUNTDOWN_MS : (UINTN)el,
             CAP_COUNTDOWN_MS, COLOR_RED);
    c.cy += 6 + 14;

    draw_text_centered_px(state, sub, c.x, (UINTN)c.w, c.cy, COLOR_GRAY,
                          c.small_px);
    c.cy += (INTN)c.small_px + 8;

    {
        const key_hint_t hints[] = { { L"F10", L"cancel" } };
        card_hints(state, &c, hints, 1);
    }
}

static void cap_draw_saving(gui_state_t *state) {
    card_t c;
    card_metrics(state, &c);
    CHAR16 sub[96], size[32];
    cap_size_text(size, sizeof(size) / sizeof(CHAR16),
                  cap_gif_bytes(state->cap_gif));
    SPrint(sub, sizeof(sub), L"%d frames  -  %s",
           (int)cap_gif_count(state->cap_gif), size);

    INTN bw = 420;
    if (bw > (INTN)state->screen_width - 80) bw = (INTN)state->screen_width - 80;
    INTN bh = c.pad + (INTN)c.title_px + 11
            + (INTN)c.small_px + 12 + 6 + c.pad;
    card_open(state, &c, bw, bh);
    card_title(state, &c, L"Saving recording", COLOR_ORANGE, COLOR_WHITE);
    card_line(state, &c, sub, COLOR_GRAY, c.small_px, 225);
    card_space(&c, 6);
    card_bar(state, c.x + c.pad, c.cy, c.w - 2 * c.pad, 6, 1, 1, COLOR_ORANGE);
}

static void cap_draw_toast(gui_state_t *state) {
    card_t c;
    card_metrics(state, &c);

    color_t dot = state->cap_status_err ? COLOR_RED : COLOR_GREEN;
    UINTN px = c.body_px;
    UINTN dpx = c.small_px;
    int have_detail = state->cap_detail[0] != 0;

    INTN d = 10;
    INTN tw = (INTN)text_width_px(state->cap_status, px);
    INTN dw = have_detail ? (INTN)text_width_px(state->cap_detail, dpx) : 0;
    INTN inner = tw > dw ? tw : dw;
    INTN bw = 17 + d + 11 + inner + 17;
    INTN bh = 13 + (INTN)px + (have_detail ? 4 + (INTN)dpx : 0) + 13;

    INTN maxw = (INTN)state->screen_width - 48;
    if (bw > maxw) bw = maxw;

    INTN bx = ((INTN)state->screen_width - bw) / 2;
    INTN by = (INTN)state->screen_height - bh - 46;
    if (by < 0) by = 0;

    INTN a = 255;
    if (state->cap_status_ms < CAP_TOAST_FADE_MS)
        a = (INTN)state->cap_status_ms * 255 / CAP_TOAST_FADE_MS;
    if (a < 0) a = 0;

    INTN r = bh / 2;
    fill_round_rect(state, bx + 1, by + 3, bw, bh, r, COLOR_BLACK, (UINT8)(a * 70 / 255));
    fill_round_rect(state, bx, by, bw, bh, r, COLOR_BLACK, (UINT8)(a * 215 / 255));
    fill_round_rect(state, bx, by, bw, 1, 0, COLOR_WHITE, (UINT8)(a * 30 / 255));

    INTN ty = by + 13;
    card_dot(state, bx + 17, ty + ((INTN)px - d) / 2 + 1, d, dot, a);
    draw_text_px_a(state, state->cap_status, bx + 17 + d + 11, ty,
                   COLOR_WHITE, px, a);
    if (have_detail)
        draw_text_px_a(state, state->cap_detail, bx + 17 + d + 11,
                       ty + (INTN)px + 4, COLOR_GRAY, dpx, a * 215 / 255);
}

void cap_draw_overlay(gui_state_t *state) {
    if (state->cap_mode == 2)                cap_draw_countdown(state);
    else if (state->cap_mode == 3)           cap_draw_rec_badge(state);
    else if (state->cap_mode == 4)           cap_draw_saving(state);
    else if (state->cap_status_ms > 0)       cap_draw_toast(state);
}
