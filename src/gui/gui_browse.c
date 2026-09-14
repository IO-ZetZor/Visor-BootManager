/* gui_browse.c - file browser panel (feature: filebrowse) */
#include "gui_internal.h"

UINTN browse_rows(gui_state_t *state, UINTN name_px) {
    UINTN path_px = (name_px * 4) / 5; if (path_px < 10) path_px = 10;
    UINTN head_h = name_px + 14;
    UINTN row_h  = path_px + 12;
    INTN  bottom = (INTN)state->screen_height - 48;
    INTN  avail  = bottom - 48 - (INTN)head_h;
    INTN  fit    = avail > (INTN)row_h ? avail / (INTN)row_h : 1;
    UINTN rows   = (UINTN)fit;
    if (rows > 8) rows = 8;
    if (rows < 1) rows = 1;
    return rows;
}

int browse_band_set(gui_state_t *state) {
    if (!state->scene_cache || !state->scene_valid) return 0;
    UINTN name_px = state->name_size ? state->name_size : default_name_px(state);
    UINTN path_px = (name_px * 4) / 5; if (path_px < 10) path_px = 10;
    UINTN head_h = name_px + 14;
    UINTN row_h  = path_px + 12;
    INTN  bottom = (INTN)state->screen_height - 48;
    INTN  avail  = bottom - 48 - (INTN)head_h;
    INTN  fit    = avail > (INTN)row_h ? avail / (INTN)row_h : 1;
    UINTN rows   = (UINTN)fit;
    if (rows > 8) rows = 8;
    if (rows < 1) rows = 1;
    UINTN block_h = head_h + rows * row_h;
    INTN top = bottom - (INTN)block_h - 30;
    if (top < 8) top = 8;
    state->band_n = 1;
    state->band_y[0] = top - 40;
    state->band_h[0] = (INTN)block_h + (INTN)path_px + 62;
    return 1;
}

void draw_browse_panel(gui_state_t *state, UINTN name_px) {
    fb_t *s = state->browse;
    if (!s) return;

    UINTN path_px = (name_px * 4) / 5; if (path_px < 10) path_px = 10;
    UINTN head_h = name_px + 14;
    UINTN row_h  = path_px + 12;
    INTN  bottom = (INTN)state->screen_height - 48;
    UINTN rows   = browse_rows(state, name_px);

    if (s->cursor < s->scroll) s->scroll = s->cursor;
    if (s->cursor >= s->scroll + rows) s->scroll = s->cursor - rows + 1;
    if (s->entry_count > rows && s->scroll + rows > s->entry_count)
        s->scroll = s->entry_count - rows;

    UINTN block_h = head_h + rows * row_h;

    CHAR16 head[FB_PATH_MAX + 40];
    SPrint(head, sizeof(head), L"%s   %s%s",
           s->vol_count > 0 ? s->vols[s->vol_cur].label : L"?", s->path,
           s->truncated ? L"  [...]" : L"");
    UINTN maxw = state->screen_width * 8 / 10;
    chop_to_width(head, name_px, maxw);
    UINTN block_w = text_width_px(head, name_px);
    for (UINTN i = 0; i < rows && s->scroll + i < s->entry_count; i++) {
        fb_entry_t *e = &s->entries[s->scroll + i];
        CHAR16 line[FB_NAME_MAX + 32];
        if (e->is_dir) {
            SPrint(line, sizeof(line), L"  %s\\", e->name);
        } else {
            CHAR16 sz[24];
            fb_format_size(e->size, sz, sizeof(sz));
            SPrint(line, sizeof(line), L"  %s   %s", e->name, sz);
        }
        chop_to_width(line, path_px, maxw);
        UINTN w = text_width_px(line, path_px);
        if (w > block_w) block_w = w;
    }
    static CHAR16 hint[] = L"Enter open/boot   Backspace up   Tab volume   Esc close";
    UINTN hw = text_width_px(hint, path_px);
    if (hw > block_w) block_w = hw;

    INTN cx = (INTN)state->screen_width / 2;
    INTN top = bottom - (INTN)block_h - 30;
    if (top < 8) top = 8;
    if (state->blur) {
        INTN fpad = 16;
        draw_frost(state, cx - (INTN)block_w / 2 - fpad, top - fpad,
                   (INTN)block_w + 2 * fpad, (INTN)block_h + 2 * fpad, 255);
    }

    color_t name_col = state->name_color;
    color_t dim = { state->name_color.r * 6 / 10,
                    state->name_color.g * 6 / 10,
                    state->name_color.b * 6 / 10 };
    color_t sel_col = state->underline_color;

    UINTN hh = text_width_px(head, name_px);
    draw_text_px_a(state, head, cx - (INTN)hh / 2, top, name_col, name_px, 255);

    INTN lx = cx - (INTN)block_w / 2;
    INTN y  = top + (INTN)head_h;
    if (s->entry_count == 0) {
        static CHAR16 empty[] = L"(empty)";
        draw_text_px_a(state, empty, lx, y, dim, path_px, 255);
        y += (INTN)row_h;
    }
    for (UINTN i = 0; i < rows && s->scroll + i < s->entry_count; i++) {
        if (y + (INTN)row_h > top + (INTN)block_h + 1) break;
        UINTN gi = s->scroll + i;
        fb_entry_t *e = &s->entries[gi];
        int selr = (gi == s->cursor);
        CHAR16 line[FB_NAME_MAX + 32];
        if (e->is_dir) {
            SPrint(line, sizeof(line), L"%s%s\\", selr ? L"> " : L"  ", e->name);
        } else {
            CHAR16 sz[24];
            fb_format_size(e->size, sz, sizeof(sz));
            SPrint(line, sizeof(line), L"%s%s   %s", selr ? L"> " : L"  ", e->name, sz);
        }
        chop_to_width(line, path_px, maxw);
        draw_text_px_a(state, line, lx, y, selr ? sel_col : dim, path_px, 255);
        y += (INTN)row_h;
    }
    if (s->entry_count > rows) {
        INTN bar_x = cx + (INTN)block_w / 2 + 6;
        INTN bar_y = top + (INTN)head_h;
        INTN bar_h = (INTN)block_h - (INTN)head_h;
        INTN thumb_h = bar_h * (INTN)rows / (INTN)s->entry_count;
        if (thumb_h < 4) thumb_h = 4;
        if (thumb_h > bar_h) thumb_h = bar_h;
        UINTN span = s->entry_count - rows;
        UINTN pos = s->scroll < span ? s->scroll : span;
        INTN thumb_y = bar_y + (INTN)((INTN)pos * (bar_h - thumb_h) / (INTN)span);
        fill_rect_alpha(state, bar_x, bar_y, 3, bar_h, (color_t){0, 0, 0}, 110);
        fill_rect_alpha(state, bar_x, thumb_y, 3, thumb_h, sel_col, 220);
    }

    draw_text_px_a(state, hint, cx - (INTN)hw / 2, top + (INTN)block_h + 6,
                   dim, path_px, 255);
}
