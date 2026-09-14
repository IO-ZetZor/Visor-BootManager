/* gui_clock.c - menu clock and date rendering (feature: clock) */
#include "gui_internal.h"

static UINTN clock_px(gui_state_t *state) {
    UINTN px = state->clock_size ? state->clock_size
                                 : (state->name_size ? state->name_size
                                                     : default_title_px(state));
    if (px < 10) px = 10;
    return px;
}

static UINTN clock_date_px(UINTN px) {
    UINTN d = px * 40 / 100;
    if (d < 9) d = 9;
    return d;
}

static const CHAR16 *MONTH_LONG[12] = {
    L"January", L"February", L"March",     L"April",   L"May",      L"June",
    L"July",    L"August",   L"September", L"October", L"November", L"December"
};

static const CHAR16 *WEEKDAY_LONG[7] = {
    L"Sunday", L"Monday", L"Tuesday", L"Wednesday",
    L"Thursday", L"Friday", L"Saturday"
};

static int day_of_week(UINTN y, UINTN m, UINTN d) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 1 || m > 12 || y < 1) return -1;
    if (m < 3) y -= 1;
    return (int)((y + y / 4 - y / 100 + y / 400 + (UINTN)t[m - 1] + d) % 7);
}

static UINTN put2(CHAR16 *buf, UINTN at, UINTN v) {
    buf[at]     = (CHAR16)(L'0' + (v / 10) % 10);
    buf[at + 1] = (CHAR16)(L'0' + v % 10);
    return at + 2;
}

static UINTN put_str(CHAR16 *buf, UINTN at, const CHAR16 *s) {
    while (*s) buf[at++] = *s++;
    return at;
}

static UINTN clock_format_time(gui_state_t *state, EFI_TIME *t, CHAR16 *buf) {
    UINTN h = t->Hour;
    const CHAR16 *suffix = NULL;

    if (!state->clock_24h) {
        suffix = (h < 12) ? L" AM" : L" PM";
        h = h % 12;
        if (h == 0) h = 12;
    }

    UINTN n = 0;

    if (!state->clock_24h && h < 10) buf[n++] = (CHAR16)(L'0' + h);
    else                             n = put2(buf, n, h);

    buf[n++] = L':';
    n = put2(buf, n, t->Minute);
    if (state->clock_seconds) {
        buf[n++] = L':';
        n = put2(buf, n, t->Second);
    }
    if (suffix) n = put_str(buf, n, suffix);

    buf[n] = 0;
    return n;
}

static UINTN clock_format_date(gui_state_t *state, EFI_TIME *t, CHAR16 *buf) {
    UINTN n = 0;
    UINTN mon = (t->Month >= 1 && t->Month <= 12) ? t->Month : 1;

    switch (state->clock_date_format) {
        case CLOCK_DATE_ISO:
            n = put2(buf, n, (UINTN)t->Year / 100);
            n = put2(buf, n, (UINTN)t->Year % 100);
            buf[n++] = L'-';
            n = put2(buf, n, t->Month);
            buf[n++] = L'-';
            n = put2(buf, n, t->Day);
            break;
        case CLOCK_DATE_DMY:
            n = put2(buf, n, t->Day);
            buf[n++] = L'/';
            n = put2(buf, n, t->Month);
            buf[n++] = L'/';
            n = put2(buf, n, (UINTN)t->Year % 100);
            break;
        case CLOCK_DATE_MDY:
            n = put2(buf, n, t->Month);
            buf[n++] = L'/';
            n = put2(buf, n, t->Day);
            buf[n++] = L'/';
            n = put2(buf, n, (UINTN)t->Year % 100);
            break;
        default: {
            int wd = day_of_week((UINTN)t->Year, t->Month, t->Day);
            if (wd >= 0) {
                n = put_str(buf, n, WEEKDAY_LONG[wd]);
                buf[n++] = L','; buf[n++] = L' ';
            }
            n = put_str(buf, n, MONTH_LONG[mon - 1]);
            buf[n++] = L' ';
            if (t->Day >= 10) n = put2(buf, n, t->Day);
            else              buf[n++] = (CHAR16)(L'0' + t->Day);
            break;
        }
    }

    buf[n] = 0;
    return n;
}

static INTN clock_key(gui_state_t *state, EFI_TIME *t) {
    INTN key = (INTN)t->Hour * 60 + (INTN)t->Minute;
    if (state->clock_seconds) key = key * 60 + (INTN)t->Second;

    if (state->clock_date) key += (INTN)t->Day * 100000;
    return key;
}

static int clock_layout(gui_state_t *state, EFI_TIME *t,
                        CHAR16 *time_buf, CHAR16 *date_buf,
                        INTN *out_x, INTN *out_y, INTN *out_w, INTN *out_h,
                        INTN *time_x, INTN *date_x, INTN *date_y) {
    UINTN px = clock_px(state);
    UINTN dpx = clock_date_px(px);

    clock_format_time(state, t, time_buf);
    date_buf[0] = 0;
    if (state->clock_date) clock_format_date(state, t, date_buf);

    UINTN tw = text_width_px(time_buf, px);
    UINTN dw = date_buf[0] ? text_width_px(date_buf, dpx) : 0;
    UINTN gap = date_buf[0] ? (px / 5 + 2) : 0;

    UINTN bw = tw > dw ? tw : dw;
    UINTN bh = px + (date_buf[0] ? gap + dpx : 0);

    UINTN margin = clamp_uintn(ui_base(state) / 26, 28, 46);
    int pos = state->clock_position;

    int right  = (pos == CLOCK_POS_TOPRIGHT || pos == CLOCK_POS_BOTTOMRIGHT);
    int left   = (pos == CLOCK_POS_TOPLEFT  || pos == CLOCK_POS_BOTTOMLEFT);
    int bottom = (pos == CLOCK_POS_BOTTOMRIGHT || pos == CLOCK_POS_BOTTOMLEFT ||
                  pos == CLOCK_POS_BOTTOMCENTER);

    INTN x;
    if (right)     x = (INTN)state->screen_width - (INTN)bw - (INTN)margin;
    else if (left) x = (INTN)margin;
    else           x = ((INTN)state->screen_width - (INTN)bw) / 2;

    INTN y;
    if (pos == CLOCK_POS_CENTER)
        y = ((INTN)state->screen_height - (INTN)bh) / 2;
    else if (bottom)
        y = (INTN)state->screen_height - (INTN)bh - (INTN)margin;
    else

        y = (INTN)(state->screen_height / 14);

    if (x < (INTN)margin) x = (INTN)margin;
    if (y < 0) y = 0;

    if (!bottom && pos != CLOCK_POS_CENTER) {
        INTN hx, hy, hw, hh;
        if (header_box(state, &hx, &hy, &hw, &hh)) {
            INTN pad = (INTN)(px / 2);
            int overlap_x = (x - pad) < (hx + hw) && (x + (INTN)bw + pad) > hx;
            int overlap_y = y < (hy + hh) && (y + (INTN)bh) > hy;
            if (overlap_x && overlap_y)
                y = hy + hh + (INTN)(px / 2) + 8;
        }
    }

    *time_x = x + ((INTN)bw - (INTN)tw) / 2;
    *date_x = x + ((INTN)bw - (INTN)dw) / 2;
    *date_y = y + (INTN)px + (INTN)gap;

    *out_x = x; *out_y = y; *out_w = (INTN)bw; *out_h = (INTN)bh;
    return 1;
}

int draw_clock_ex(gui_state_t *state, int frost) {
    if (!state->show_clock) return 0;

    EFI_TIME t;
    if (EFI_ERROR(RT->GetTime(&t, NULL))) return 0;

    CHAR16 time_buf[24], date_buf[48];
    INTN x, y, w, h, tx, dx, dy;
    if (!clock_layout(state, &t, time_buf, date_buf, &x, &y, &w, &h, &tx, &dx, &dy))
        return 0;

    UINTN px = clock_px(state);
    UINTN dpx = clock_date_px(px);
    int backing = frost && state->clock_blur;

    if (backing) {
        INTN pad = (INTN)(px / 2);
        draw_frost(state, x - pad, y - pad / 2, w + pad * 2, h + pad, 255);
    }

    if (state->clock_shadow) {
        INTN off = (INTN)(px / 14); if (off < 1) off = 1;
        draw_text_px_a(state, time_buf, tx + off, y + off, COLOR_BLACK, px, 115);
        if (date_buf[0])
            draw_text_px_a(state, date_buf, dx + off, dy + off, COLOR_BLACK, dpx, 115);
    }

    draw_text_px(state, time_buf, tx, y, state->clock_color, px);
    if (date_buf[0])
        draw_text_px(state, date_buf, dx, dy, state->clock_color, dpx);

    INTN pad = backing ? (INTN)(px / 2) : 2;
    INTN rx = x - pad, ry = y - pad / 2 - 2;
    INTN rw = w + pad * 2, rh = h + pad + 4;
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }

    state->clock_x = rx; state->clock_y = ry;
    state->clock_w = rw; state->clock_h = rh;
    state->clock_drawn = 1;
    state->clock_last_key = clock_key(state, &t);
    return 1;
}

int draw_clock(gui_state_t *state) {
    return draw_clock_ex(state, 1);
}

int clock_needs_tick(gui_state_t *state) {
    if (!state->show_clock) return 0;
    EFI_TIME t;
    if (EFI_ERROR(RT->GetTime(&t, NULL))) return 0;
    return clock_key(state, &t) != state->clock_last_key;
}
