/* gui_editor.c - in-menu cmdline editor overlay (feature: editor) */
#include "gui_internal.h"

/* The cmdline buffer is 512 characters.  At the sizes this editor is used
 * with that is far more visual lines than can fit on screen, so a fixed table
 * is enough and keeps the draw path allocation-free. */
#define EDIT_LINE_MAX 512

typedef struct {
    UINT16 start;
    UINT16 end;
} edit_line_t;

/* Last cmdline layout, kept so Up/Down can move between wrapped lines without
 * walking the font metrics again.  The GUI is single-threaded and the overlay
 * is always drawn before a key is handled, so this is only ever one frame
 * stale.  Passwords never use it - they are horizontal-scroll only. */
static edit_line_t g_edit_lines[EDIT_LINE_MAX];
static UINTN g_edit_line_count;
static UINTN g_edit_line_px;
static int g_edit_lines_valid;

static UINTN edit_advance_q6(CHAR16 *text, UINTN from, UINTN to, UINTN px) {
    UINT64 w = 0;
    for (UINTN i = from; i < to; i++)
        w += text_advance_q6(text[i], px);
    return (UINTN)w;
}

/* Split text into visual lines.  Prefer spaces, but fall back to a character
 * break for long command-line tokens.  Newlines are honoured as well so the
 * same widget can be reused for multi-line values later. */
static UINTN edit_wrap(const CHAR16 *text, UINTN len, UINTN maxw, UINTN px,
                       edit_line_t *lines, UINTN cap) {
    if (!cap) return 0;
    if (!len) {
        lines[0].start = lines[0].end = 0;
        return 1;
    }

    UINT64 maxq = (UINT64)maxw << 6;
    UINTN n = 0, start = 0;

    while (start < len && n < cap) {
        UINTN i = start;
        UINTN last_space = (UINTN)-1;
        UINT64 wq = 0;

        while (i < len) {
            CHAR16 ch = text[i];
            if (ch == L'\r') { i++; continue; }
            if (ch == L'\n') break;

            UINTN adv = text_advance_q6(ch, px);
            if (i > start && wq + (UINT64)adv > maxq) break;
            if (ch == L' ') last_space = i;
            wq += adv;
            i++;
        }

        UINTN end = i, next = i;
        if (i < len && text[i] == L'\n') {
            next = i + 1;
        } else if (i < len && last_space != (UINTN)-1 && last_space > start) {
            /* Break after the last space that still fits. */
            end = last_space + 1;
            next = last_space + 1;
        }
        if (next <= start) next = start + 1;
        if (end < start) end = start;

        lines[n].start = (UINT16)start;
        lines[n].end = (UINT16)end;
        n++;
        start = next;
    }

    /* A trailing newline gets its own empty line so the caret can sit there. */
    if (start == len && len && text[len - 1] == L'\n' && n < cap) {
        lines[n].start = lines[n].end = (UINT16)len;
        n++;
    }

    if (!n) {
        lines[0].start = lines[0].end = 0;
        n = 1;
    }
    return n;
}

static UINTN edit_line_for(const edit_line_t *lines, UINTN n, UINTN cursor) {
    for (UINTN i = 0; i < n; i++) {
        if (cursor < lines[i].start) break;
        if (cursor < lines[i].end) return i;
        if (cursor == lines[i].end) {
            /* At a character-break boundary the caret belongs to the start
             * of the next line, matching what the wrap layout draws. */
            if (i + 1 < n && lines[i + 1].start == cursor) continue;
            return i;
        }
    }
    return n ? n - 1 : 0;
}

static UINTN edit_line_pos_for_q6(const CHAR16 *text, const edit_line_t *line,
                                  UINTN q6, UINTN px) {
    UINTN start = line->start, end = line->end;
    if (end < start) end = start;

    UINTN best = start;
    UINT64 best_dist = ~(UINT64)0;
    UINT64 w = 0;
    for (UINTN i = start; ; i++) {
        UINT64 dist = w > q6 ? w - q6 : q6 - w;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
        if (i >= end) break;
        w += text_advance_q6(text[i], px);
    }
    return best;
}

static void edit_move_vertical(gui_state_t *state, int dir) {
    if (!g_edit_lines_valid || !g_edit_line_count) return;

    UINTN cursor = state->edit_cursor;
    if (cursor > state->edit_len) cursor = state->edit_len;

    UINTN cline = edit_line_for(g_edit_lines, g_edit_line_count, cursor);
    UINTN target;
    if (dir < 0) {
        if (cline == 0) return;
        target = cline - 1;
    } else {
        if (cline + 1 >= g_edit_line_count) return;
        target = cline + 1;
    }

    UINTN caret_to = cursor;
    if (caret_to > g_edit_lines[cline].end) caret_to = g_edit_lines[cline].end;
    UINTN q6 = edit_advance_q6(state->edit_buf, g_edit_lines[cline].start,
                               caret_to, g_edit_line_px);
    state->edit_cursor = edit_line_pos_for_q6(state->edit_buf,
                                              &g_edit_lines[target],
                                              q6, g_edit_line_px);
}

static void copy_text(CHAR16 *dst, UINTN cap, const CHAR16 *src) {
    if (!cap) return;
    UINTN i = 0;
    if (src)
        while (src[i] && i + 1 < cap) {
            dst[i] = src[i];
            i++;
        }
    dst[i] = 0;
}

static void copy_chop(CHAR16 *dst, UINTN cap, const CHAR16 *src,
                      UINTN px, UINTN maxw) {
    copy_text(dst, cap, src);
    if (maxw < 12) maxw = 12;
    chop_to_width(dst, px, maxw);
}

static UINTN hint_line_count(const CHAR16 *text, UINTN px, UINTN maxw,
                             UINTN max_lines) {
    if (!text || !text[0]) return 0;
    UINTN len = 0;
    while (text[len]) len++;
    edit_line_t lines[16];
    UINTN n = edit_wrap(text, len, maxw, px, lines, 16);
    if (n > max_lines) n = max_lines;
    return n;
}

static void draw_wrapped_text(gui_state_t *state, card_t *c, CHAR16 *text,
                              UINTN px, color_t col, INTN alpha,
                              UINTN max_lines) {
    if (!text || !text[0]) return;
    UINTN len = 0;
    while (text[len]) len++;
    INTN maxw = c->w - 2 * c->pad;
    if (maxw < 40) maxw = 40;
    edit_line_t lines[16];
    UINTN n = edit_wrap(text, len, (UINTN)maxw, px, lines, 16);
    if (n > max_lines) n = max_lines;

    for (UINTN i = 0; i < n; i++) {
        UINTN start = lines[i].start, end = lines[i].end;
        if (end > start) {
            CHAR16 saved = text[end];
            text[end] = 0;
            draw_text_px_a(state, text + start, c->x + c->pad,
                           c->cy + (INTN)i * ((INTN)px + 4),
                           col, px, alpha);
            text[end] = saved;
        }
    }
    c->cy += (INTN)n * ((INTN)px + 4) + 4;
}

static INTN editor_width(INTN screen_w, INTN wanted, INTN min_w) {
    INTN max_w = screen_w - 96;
    if (max_w < min_w) max_w = screen_w - 48;
    if (max_w < 120) max_w = screen_w - 16;
    if (wanted > max_w) wanted = max_w;
    if (wanted < min_w) wanted = max_w;
    if (wanted < 64) wanted = 64;
    if (wanted > screen_w - 16) wanted = screen_w - 16;
    return wanted;
}

static INTN editor_height(INTN screen_h, INTN wanted, INTN min_h) {
    INTN max_h = screen_h - 96;
    if (max_h < 160) max_h = screen_h - 48;
    if (max_h < 80) max_h = screen_h - 16;
    if (wanted > max_h) wanted = max_h;
    if (wanted < min_h) wanted = min_h;
    if (wanted > max_h) wanted = max_h;
    if (wanted < 80) wanted = 80;
    if (wanted > screen_h - 16) wanted = screen_h - 16;
    return wanted;
}

static void draw_secret_field(gui_state_t *state, card_t *c, CHAR16 *shown) {
    INTN fx = c->x + c->pad;
    INTN fw = c->w - 2 * c->pad;
    if (fw < 8) fw = 8;
    INTN fh = (INTN)c->body_px + 18;
    INTN fy = c->cy;
    INTN radius = state->box_radius ? (INTN)state->box_radius / 2 : 9;
    if (radius > 12) radius = 12;
    if (radius < 3) radius = 3;

    fill_round_rect(state, fx, fy, fw, fh, radius, COLOR_BLACK, 140);
    if (fw > 2 * radius)
        fill_rect_alpha(state, fx + radius, fy, fw - 2 * radius, 1,
                        COLOR_WHITE, 38);
    fill_rect_alpha(state, fx, fy + fh - 1, fw, 1,
                    state->underline_color, 110);

    UINTN len = state->edit_len;
    UINTN cursor = state->edit_cursor;
    if (cursor > len) cursor = len;

    INTN inner = fw - 26;
    if (inner < 8) inner = 8;
    UINTN maxw = (UINTN)inner;

    UINTN caret_px = (edit_advance_q6(shown, 0, cursor, c->body_px) + 32) / 64;
    UINTN scroll = state->edit_scroll_px;
    if (caret_px < scroll) scroll = caret_px;
    if (caret_px > scroll + maxw) scroll = caret_px - maxw;
    if (scroll > caret_px) scroll = caret_px;
    state->edit_scroll_px = scroll;

    /* Locate the first character that overlaps the visible window. */
    UINTN start = 0;
    UINT64 posq = 0;
    while (start < len) {
        UINTN adv = text_advance_q6(shown[start], c->body_px);
        if (((posq + (UINT64)adv + 32) >> 6) > scroll) break;
        posq += adv;
        start++;
    }
    UINTN begin_px = (UINTN)((posq + 32) >> 6);

    UINTN end = start;
    UINT64 wq = 0;
    UINT64 visq = (UINT64)maxw << 6;
    while (end < len) {
        UINTN adv = text_advance_q6(shown[end], c->body_px);
        if (wq + (UINT64)adv > visq) break;
        wq += adv;
        end++;
    }
    if (end == start && start < len) end = start + 1;

    CHAR16 saved = shown[end];
    shown[end] = 0;
    INTN off = (INTN)(begin_px - scroll);
    if (off < 0) off = 0;
    draw_text_px_a(state, shown + start, fx + 13 - off, fy + 9,
                   COLOR_WHITE, c->body_px, 255);
    shown[end] = saved;

    if (caret_px >= scroll && caret_px <= scroll + maxw) {
        INTN cx = fx + 13 + (INTN)(caret_px - scroll);
        INTN maxx = fx + fw - 13;
        if (cx > maxx) cx = maxx;
        if (cx < fx + 13) cx = fx + 13;
        fill_rect_alpha(state, cx, fy + 8, 2,
                        (INTN)c->body_px + 2, state->underline_color, 255);
    }

    c->cy += fh + 12;
}

static void draw_cmdline_field(gui_state_t *state, card_t *c, CHAR16 *shown) {
    INTN fx = c->x + c->pad;
    INTN fw = c->w - 2 * c->pad;
    if (fw < 8) fw = 8;
    INTN fy = c->cy;
    INTN bottom = c->y + c->h - c->pad / 2 - card_hints_h(c) - 14;
    INTN fh = bottom - fy;
    INTN line_h = (INTN)c->body_px + (INTN)c->body_px / 3 + 4;
    if (line_h < 20) line_h = 20;
    if (fh < 16) fh = 16;

    INTN radius = state->box_radius ? (INTN)state->box_radius / 2 : 9;
    if (radius > 12) radius = 12;
    if (radius < 3) radius = 3;

    fill_round_rect(state, fx, fy, fw, fh, radius, COLOR_BLACK, 140);
    if (fw > 2 * radius)
        fill_rect_alpha(state, fx + radius, fy, fw - 2 * radius, 1,
                        COLOR_WHITE, 38);
    fill_rect_alpha(state, fx, fy + fh - 1, fw, 1,
                    state->underline_color, 110);

    INTN inner = fw - 36;                /* leave room for the scrollbar */
    if (inner < 8) inner = 8;
    UINTN maxw = (UINTN)inner;

    edit_line_t *lines = g_edit_lines;
    UINTN n = edit_wrap(shown, state->edit_len, maxw, c->body_px,
                        lines, EDIT_LINE_MAX);
    g_edit_line_count = n;
    g_edit_line_px = c->body_px;
    g_edit_lines_valid = 1;

    UINTN cursor = state->edit_cursor;
    if (cursor > state->edit_len) cursor = state->edit_len;
    UINTN cline = edit_line_for(lines, n, cursor);

    INTN top_pad = 9;
    INTN visible = (fh - 2 * top_pad) / line_h;
    if (visible < 1) visible = 1;

    if (state->edit_scroll > cline) state->edit_scroll = cline;
    if (cline >= state->edit_scroll + (UINTN)visible)
        state->edit_scroll = cline - (UINTN)visible + 1;

    UINTN max_scroll = n > (UINTN)visible ? n - (UINTN)visible : 0;
    if (state->edit_scroll > max_scroll) state->edit_scroll = max_scroll;

    for (UINTN i = state->edit_scroll;
         i < n && i < state->edit_scroll + (UINTN)visible; i++) {
        UINTN start = lines[i].start, end = lines[i].end;
        if (end <= start) continue;
        CHAR16 saved = shown[end];
        shown[end] = 0;
        draw_text_px_a(state, shown + start, fx + 13,
                       fy + top_pad + (INTN)(i - state->edit_scroll) * line_h,
                       COLOR_WHITE, c->body_px, 255);
        shown[end] = saved;
    }

    UINTN caret_to = cursor;
    if (caret_to > lines[cline].end) caret_to = lines[cline].end;
    UINTN caret_q6 = edit_advance_q6(shown, lines[cline].start, caret_to,
                                     c->body_px);
    INTN caret_x = fx + 13 + (INTN)((caret_q6 + 32) >> 6);
    INTN caret_y = fy + top_pad +
                   (INTN)(cline - state->edit_scroll) * line_h;
    INTN caret_max = fx + fw - 12;
    if (caret_x > caret_max) caret_x = caret_max;
    if (caret_x < fx + 13) caret_x = fx + 13;
    fill_rect_alpha(state, caret_x, caret_y + 1, 2,
                    (INTN)c->body_px + 2, state->underline_color, 255);

    if (n > (UINTN)visible) {
        INTN sb_x = fx + fw - 8;
        INTN sb_y = fy + top_pad;
        INTN sb_h = fh - 2 * top_pad;
        if (sb_h > 6) {
            INTN thumb_h = (INTN)((UINT64)sb_h * (UINTN)visible / n);
            if (thumb_h < 24) thumb_h = 24;
            if (thumb_h > sb_h) thumb_h = sb_h;
            INTN max_s = (INTN)(n - (UINTN)visible);
            INTN thumb_y = sb_y;
            if (max_s > 0)
                thumb_y += (sb_h - thumb_h) * (INTN)state->edit_scroll / max_s;
            fill_round_rect(state, sb_x, sb_y, 4, sb_h, 2,
                            COLOR_WHITE, 30);
            fill_round_rect(state, sb_x, thumb_y, 4, thumb_h, 2,
                            state->underline_color, 200);
        }
    }

    c->cy = fy + fh + 12;
}

void draw_editor_overlay(gui_state_t *state) {
    INTN W = (INTN)state->screen_width, H = (INTN)state->screen_height;
    fill_rect_alpha(state, 0, 0, W, H, COLOR_BLACK, 155);

    int mask = state->edit_secret && !state->edit_reveal;
    CHAR16 secret_buf[512];
    CHAR16 *shown = state->edit_buf;
    if (mask) {
        UINTN n = state->edit_len < 511 ? state->edit_len : 511;
        for (UINTN i = 0; i < n; i++) secret_buf[i] = L'*';
        secret_buf[n] = 0;
        shown = secret_buf;
    }

    card_t c;
    card_metrics(state, &c);
    UINTN title_px = c.title_px < 24 ? 24 : c.title_px;
    UINTN body_px = c.body_px < 18 ? 18 : c.body_px;
    UINTN small_px = c.small_px < 14 ? 14 : c.small_px;
    c.title_px = title_px;
    c.body_px = body_px;
    c.small_px = small_px;

    const CHAR16 *title = state->edit_title ? state->edit_title
                        : state->edit_secret ? L"Password"
                                             : L"Boot options";

    const key_hint_t secret_hints[] = {
        { L"Enter", L"unlock" },
        { L"F2", state->edit_reveal ? L"hide" : L"reveal" },
        { L"Esc", L"cancel" }
    };
    const key_hint_t opts_hints[] = {
        { L"Enter", L"boot" }, { L"Esc", L"cancel" }
    };
    const key_hint_t *hints = state->edit_secret ? secret_hints : opts_hints;
    UINTN nhints = state->edit_secret ? 3 : 2;

    INTN bw, bh;
    if (state->edit_secret) {
        bw = editor_width(W, W * 58 / 100, 420);
        if (bw > 780) bw = 780;
        INTN field_h = (INTN)body_px + 18;
        INTN hint_w = bw - 2 * c.pad;
        if (hint_w < 40) hint_w = 40;
        UINTN hlines = hint_line_count(state->edit_hint, small_px,
                                       (UINTN)hint_w, 3);
        INTN hint_h = hlines ? (INTN)hlines * ((INTN)small_px + 4) + 4 : 0;
        bh = c.pad + (INTN)title_px + 11 + 13 + field_h + 12 + hint_h
           + card_hints_h(&c) + c.pad / 2 + 10;
        bh = editor_height(H, bh, 170);
    } else {
        bw = editor_width(W, W * 88 / 100, 480);
        if (bw > 1800) bw = 1800;
        INTN line_h = (INTN)body_px + (INTN)body_px / 3 + 4;
        INTN min_bh = c.pad + (INTN)title_px + 11
                    + (INTN)small_px + 5 + 13
                    + 3 * line_h + 30 + card_hints_h(&c) + c.pad / 2;
        bh = editor_height(H, H * 62 / 100, min_bh);
    }

    card_open(state, &c, bw, bh);
    c.title_px = title_px;
    c.body_px = body_px;
    c.small_px = small_px;

    CHAR16 titlebuf[128];
    INTN title_avail = c.w - 2 * c.pad - (INTN)(title_px * 9 / 10);
    UINTN title_max = title_avail > 20 ? (UINTN)title_avail : 20;
    copy_chop(titlebuf, 128, title, title_px, title_max);
    card_title(state, &c, titlebuf, state->underline_color, COLOR_WHITE);

    if (!state->edit_secret) {
        boot_entry_t *e = entry_at(state, state->selected);
        if (e && e->name && e->name[0]) {
            CHAR16 sub[160];
            copy_chop(sub, 160, e->name, small_px,
                      (UINTN)(c.w - 2 * c.pad));
            card_line(state, &c, sub, COLOR_GRAY, small_px, 190);
        }
    }

    card_rule(state, &c);

    if (state->edit_secret)
        draw_secret_field(state, &c, shown);
    else
        draw_cmdline_field(state, &c, shown);

    if (state->edit_hint) {
        CHAR16 hintbuf[256];
        copy_text(hintbuf, 256, state->edit_hint);
        draw_wrapped_text(state, &c, hintbuf, small_px, COLOR_GRAY, 190, 3);
    }

    card_hints(state, &c, hints, nhints);
}

void editor_enter(gui_state_t *state) {
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
    state->edit_scroll = 0;
    state->edit_scroll_px = 0;
    g_edit_lines_valid = 0;
    state->editing = 1;
}

void prompt_enter(gui_state_t *state, CHAR16 *title, CHAR16 *hint) {
    state->edit_secret = 1;
    state->edit_reveal = 0;
    state->edit_title = title;
    state->edit_hint = hint;
    state->edit_len = 0;
    state->edit_cursor = 0;
    state->edit_scroll = 0;
    state->edit_scroll_px = 0;
    g_edit_lines_valid = 0;
    state->edit_buf[0] = 0;
    state->editing = 1;
}

static void delete_at_cursor(gui_state_t *state) {
    if (state->edit_cursor >= state->edit_len) return;
    for (UINTN i = state->edit_cursor; i + 1 < state->edit_len; i++)
        state->edit_buf[i] = state->edit_buf[i + 1];
    state->edit_len--;
    state->edit_buf[state->edit_len] = 0;
}

int editor_key(gui_state_t *state, EFI_INPUT_KEY *key) {
    if (key->UnicodeChar == 0x0D) {
        state->edit_buf[state->edit_len] = 0;
        if (!state->edit_secret) {
            if (state->override_cmdline) efi_free_pool(state->override_cmdline);
            state->override_cmdline = efi_strdup(state->edit_buf);
        }
        state->editing = 0;
        return 1;
    }
    if (key->UnicodeChar == 0x1B ||
        (key->UnicodeChar == 0x00 && key->ScanCode == 0x17)) {
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
    if (key->UnicodeChar == 0x7F) {
        delete_at_cursor(state);
        return 0;
    }
    if (key->UnicodeChar == 0x00) {
        switch (key->ScanCode) {
        case 0x01: /* up */
            if (!state->edit_secret) edit_move_vertical(state, -1);
            break;
        case 0x02: /* down */
            if (!state->edit_secret) edit_move_vertical(state, 1);
            break;
        case 0x03: /* right */
            if (state->edit_cursor < state->edit_len) state->edit_cursor++;
            break;
        case 0x04: /* left */
            if (state->edit_cursor > 0) state->edit_cursor--;
            break;
        case 0x05: /* home */
            state->edit_cursor = 0;
            break;
        case 0x06: /* end */
            state->edit_cursor = state->edit_len;
            break;
        case 0x07: /* delete */
            delete_at_cursor(state);
            break;
        case 0x0C: /* F2 */
            if (state->edit_secret) state->edit_reveal = !state->edit_reveal;
            break;
        default:
            break;
        }
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
