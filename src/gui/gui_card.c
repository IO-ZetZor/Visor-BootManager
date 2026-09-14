/* gui_card.c - card/panel widget toolkit shared by the overlays */
#include "gui_internal.h"

void scene_restore_band(gui_state_t *state, INTN y, INTN h) {
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

void card_metrics(gui_state_t *state, card_t *c) {
    UINTN base = state->name_size ? state->name_size : 20;
    if (base < 18) base = 18;
    if (base > 34) base = 34;
    c->title_px = base;
    c->body_px  = base * 4 / 5;
    c->small_px = base * 7 / 10;
    if (c->body_px < 15) c->body_px = 15;
    if (c->small_px < 13) c->small_px = 13;
    c->pad = (INTN)base + 6;
    c->label_w = 0;
}

static void card_panel(gui_state_t *state, INTN x, INTN y, INTN w, INTN h) {
    INTN r = state->box_radius ? (INTN)state->box_radius : 16;
    fill_round_rect(state, x - 1, y + 4, w + 2, h, r, COLOR_BLACK, 40);
    fill_round_rect(state, x + 2, y + 9, w - 4, h, r, COLOR_BLACK, 66);
    if (state->blur)
        draw_frost(state, x, y, w, h, 255);
    else
        fill_round_rect(state, x, y, w, h, r, COLOR_BLACK, 236);
    fill_rect_alpha(state, x + r, y, w - 2 * r, 1, COLOR_WHITE, 38);
}

static void card_open_at(gui_state_t *state, card_t *c,
                         INTN x, INTN y, INTN w, INTN h) {
    card_metrics(state, c);
    c->x = x; c->y = y; c->w = w; c->h = h;
    card_panel(state, x, y, w, h);
    c->cy = y + c->pad;
}

void card_open(gui_state_t *state, card_t *c, INTN w, INTN h) {
    INTN W = (INTN)state->screen_width, H = (INTN)state->screen_height;
    if (w > W - 48) w = W - 48;
    if (h > H - 48) h = H - 48;
    card_open_at(state, c, (W - w) / 2, (H - h) / 2, w, h);
}

void card_dot(gui_state_t *state, INTN x, INTN y, INTN d,
                     color_t col, INTN alpha) {
    if (d < 4) d = 4;
    fill_round_rect(state, x, y, d, d, d / 2, col, (UINT8)alpha);
}

void card_title(gui_state_t *state, card_t *c, const CHAR16 *text,
                       color_t dot, color_t text_col) {
    INTN tx = c->x + c->pad;
    INTN d = (INTN)c->title_px * 2 / 5;
    if (d < 9) d = 9;
    card_dot(state, tx, c->cy + ((INTN)c->title_px - d) / 2 + 1, d, dot, 255);
    draw_text_px_a(state, (CHAR16*)text, tx + d + (INTN)c->title_px / 2, c->cy,
                   text_col, c->title_px, 255);
    c->cy += (INTN)c->title_px + 11;
}

void card_rule(gui_state_t *state, card_t *c) {
    fill_rect_alpha(state, c->x + c->pad, c->cy, c->w - 2 * c->pad, 1,
                    COLOR_WHITE, 28);
    c->cy += 13;
}

void card_line(gui_state_t *state, card_t *c, const CHAR16 *text,
                      color_t col, UINTN px, INTN alpha) {
    draw_text_px_a(state, (CHAR16*)text, c->x + c->pad, c->cy, col, px, alpha);
    c->cy += (INTN)px + 5;
}

void card_row(gui_state_t *state, card_t *c, const CHAR16 *label,
                     const CHAR16 *value) {
    draw_text_px_a(state, (CHAR16*)label, c->x + c->pad, c->cy,
                   COLOR_GRAY, c->body_px, 225);
    draw_text_px_a(state, (CHAR16*)value, c->x + c->pad + c->label_w, c->cy,
                   COLOR_WHITE, c->body_px, 245);
    c->cy += card_row_h(c->body_px);
}

INTN card_label_w(card_t *c, const CHAR16 * const *labels, UINTN n) {
    UINTN w = 0;
    for (UINTN i = 0; i < n; i++) {
        UINTN t = text_width_px((CHAR16*)labels[i], c->body_px);
        if (t > w) w = t;
    }
    return (INTN)w + (INTN)c->body_px;
}

void card_bar(gui_state_t *state, INTN x, INTN y, INTN w, INTN h,
                     UINTN num, UINTN den, color_t fill) {
    if (h < 4) h = 4;
    INTN r = h / 2;
    fill_round_rect(state, x, y, w, h, r, COLOR_WHITE, 36);
    if (!den) return;
    if (num > den) num = den;
    INTN fw = (INTN)((UINT64)w * num / den);
    if (num && fw < h) fw = h;
    if (fw > 0) fill_round_rect(state, x, y, fw, h, r, fill, 238);
}

void card_field(gui_state_t *state, card_t *c, const CHAR16 *text,
                       UINTN caret_px, int caret) {
    INTN fx = c->x + c->pad, fw = c->w - 2 * c->pad;
    INTN fh = (INTN)c->title_px + 18;
    fill_round_rect(state, fx, c->cy, fw, fh, 9, COLOR_BLACK, 132);
    fill_rect_alpha(state, fx, c->cy, fw, 1, COLOR_WHITE, 34);
    draw_text_px_a(state, (CHAR16*)text, fx + 13, c->cy + 9, COLOR_WHITE,
                   c->title_px, 255);
    if (caret)
        fill_rect_alpha(state, fx + 13 + (INTN)caret_px, c->cy + 8, 2,
                        (INTN)c->title_px + 2, state->underline_color, 255);
    c->cy += fh + 12;
}

void card_hints(gui_state_t *state, card_t *c,
                       const key_hint_t *hints, UINTN n) {
    UINTN px = c->small_px;
    INTN chip_h = (INTN)px + 9;
    INTN y = c->y + c->h - c->pad / 2 - chip_h;

    fill_rect_alpha(state, c->x + c->pad, y - 13, c->w - 2 * c->pad, 1,
                    COLOR_WHITE, 24);

    INTN total = 0;
    for (UINTN i = 0; i < n; i++) {
        total += (INTN)text_width_px((CHAR16*)hints[i].key, px) + 18;
        if (hints[i].label)
            total += (INTN)text_width_px((CHAR16*)hints[i].label, px) + 9;
        if (i + 1 < n) total += 20;
    }
    INTN x = c->x + (c->w - total) / 2;
    if (x < c->x + c->pad) x = c->x + c->pad;

    for (UINTN i = 0; i < n; i++) {
        INTN kw = (INTN)text_width_px((CHAR16*)hints[i].key, px) + 18;
        fill_round_rect(state, x, y, kw, chip_h, 5,
                        state->underline_color, 52);
        draw_text_px_a(state, (CHAR16*)hints[i].key, x + 9, y + 4,
                       state->underline_color, px, 255);
        x += kw + 9;
        if (hints[i].label) {
            draw_text_px_a(state, (CHAR16*)hints[i].label, x, y + 4,
                           COLOR_GRAY, px, 225);
            x += (INTN)text_width_px((CHAR16*)hints[i].label, px) + 20;
        } else {
            x += 20;
        }
    }
}

INTN card_hints_h(card_t *c) { return (INTN)c->small_px + 9 + 13; }
