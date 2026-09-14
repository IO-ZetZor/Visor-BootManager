/* gui_internal.h - shared internals of the src/gui/gui.c translation units. */
#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H

#include "gui.h"
#include "font.h"
#include "efi_helpers.h"
#include "arch.h"
#include "accent.h"
#include "capture.h"
#include "gpt_disk.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;

icon_t* png_load(UINT8 *data, UINTN size);


static inline UINTN isqrt_(UINTN n) {
    if (n == 0) return 0;
    UINTN x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

static inline UINT32 color_to_u32(color_t c) {
    return (0xFF << 24) | (c.r << 16) | (c.g << 8) | c.b;
}

static inline UINTN clamp_uintn(UINTN v, UINTN lo, UINTN hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline UINTN ui_base(gui_state_t *state) {
    UINTN a = state->screen_width < state->screen_height
            ? state->screen_width : state->screen_height;
    return a ? a : 800;
}

static inline UINTN default_icon_size(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 9, 64, 128);
}

static inline UINTN default_icon_spacing(gui_state_t *state, UINTN icon_size) {
    UINTN by_res = ui_base(state) / 12;
    UINTN by_icon = icon_size * 7 / 10;
    UINTN s = by_res > by_icon ? by_res : by_icon;
    return clamp_uintn(s, 48, 104);
}

static inline UINTN default_name_px(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 38, 18, 28);
}

static inline UINTN default_title_px(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 11, 48, 86);
}

static inline UINTN default_power_px(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 32, 20, 28);
}

static inline UINTN default_power_icon_size(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 18, 40, 56);
}

static inline UINTN default_aux_text_px(gui_state_t *state) {
    return clamp_uintn(ui_base(state) / 34, 20, 30);
}

static inline int add_overflow_uintn(UINTN a, UINTN b, UINTN *out) {
    if (~(UINTN)0 - a < b) return 1;
    *out = a + b;
    return 0;
}

static inline int mul_overflow_uintn(UINTN a, UINTN b, UINTN *out) {
    if (a && b > ~(UINTN)0 / a) return 1;
    *out = a * b;
    return 0;
}

static inline UINT16 rd16le(const UINT8 *p) {
    return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static inline UINT32 rd32le(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static inline UINT32* get_pixel(gui_state_t *state, UINTN x, UINTN y) {
    if (x >= state->screen_width || y >= state->screen_height || !state->backbuffer)
        return NULL;
    return &state->backbuffer[y * state->screen_width + x];
}
#define FADE_MIN_FRAME_US         1500
int gui_animation_on(gui_state_t *state);
INTN ease_permille(INTN frame, INTN frames);
UINTN ease_alpha(INTN frame, INTN frames);
void fade_write_black(gui_state_t *state, UINTN px);
void fade_write_scaled(gui_state_t *state, UINT32 *snapshot,
                              UINTN px, UINTN alpha);
void gui_fade_in_current(gui_state_t *state);
#define SS_LEVEL_AWAKE 0
#define SS_LEVEL_DIM   1
#define SS_LEVEL_BLANK 2
void gui_crossfade(gui_state_t *state, UINT32 *from, UINT32 *to);
int ss_wake(gui_state_t *state);
int ss_tick(gui_state_t *state);
int ss_input_pending(gui_state_t *state);
void gui_fill_rect(gui_state_t *state, UINTN x, UINTN y, UINTN w, UINTN h, color_t color);
void fill_rect_alpha(gui_state_t *state, INTN x, INTN y, INTN w, INTN h,
                            color_t color, UINT8 alpha);
void fill_round_rect(gui_state_t *state, INTN x, INTN y, INTN w, INTN h,
                            INTN r, color_t color, UINT8 alpha);
void draw_image_sized_a(gui_state_t *state, icon_t *icon,
                               UINTN x, UINTN y, UINTN size, INTN master);
void draw_image_sized(gui_state_t *state, icon_t *icon,
                             UINTN x, UINTN y, UINTN size);
void draw_image_tinted_a(gui_state_t *state, icon_t *icon,
                                UINTN x, UINTN y, UINTN size,
                                color_t tint, INTN master);

static inline INTN scale_metric(INTN v, UINTN dh, UINTN size) {
    INTN num = v * (INTN)dh;
    INTN half = (INTN)size / 2;
    return num >= 0 ? (num + half) / (INTN)size
                    : -((-num + half) / (INTN)size);
}
void glyph_cache_flush(void);
UINTN text_width_px(CHAR16 *text, UINTN dh);
void draw_text_px_a(gui_state_t *state, CHAR16 *text, INTN x, INTN y,
                           color_t color, UINTN dh, INTN master);
void draw_text_px(gui_state_t *state, CHAR16 *text, INTN x, INTN y,
                         color_t color, UINTN dh);
void draw_text_centered_px(gui_state_t *state, CHAR16 *text, INTN x, UINTN w,
                                  INTN y, color_t color, UINTN dh);
int path_anim_kind(CHAR16 *path);
anim_t* gui_load_anim(CHAR16 *path, icon_t **first_out, UINTN sw, UINTN sh);
int entry_own_color(gui_state_t *state, boot_entry_t *e, color_t *out);
int gui_draw_background(gui_state_t *state);
int anim_tick(gui_state_t *state);
#define POWER_ACTION_COUNT 3
void layout_power(gui_state_t *state);
void draw_power_actions(gui_state_t *state, int focus_idx, int live);
void scene_restore_band(gui_state_t *state, INTN y, INTN h);
#define A_CARDX 0
#define A_CARDA 1
#define A_ULX   2
#define A_ULY   3
#define A_ULW   4
#define A_BOXX  5
#define A_BOXY  6
#define A_BOXW  7
#define A_BOXH  8
void blur_free(gui_state_t *state);
void build_blur_cache(gui_state_t *state);
void draw_frost(gui_state_t *state, INTN x, INTN y, INTN w, INTN h, INTN a);
typedef struct {
    const CHAR16 *key;
    const CHAR16 *label;
} key_hint_t;
typedef struct {
    INTN   x, y, w, h;
    INTN   pad;
    INTN   cy;
    INTN   label_w;
    UINTN  title_px;
    UINTN  body_px;
    UINTN  small_px;
} card_t;
void card_metrics(gui_state_t *state, card_t *c);

static inline INTN card_row_h(UINTN px) { return (INTN)px + 7; }
void card_open(gui_state_t *state, card_t *c, INTN w, INTN h);
void card_dot(gui_state_t *state, INTN x, INTN y, INTN d,
                     color_t col, INTN alpha);
void card_title(gui_state_t *state, card_t *c, const CHAR16 *text,
                       color_t dot, color_t text_col);
void card_rule(gui_state_t *state, card_t *c);
void card_line(gui_state_t *state, card_t *c, const CHAR16 *text,
                      color_t col, UINTN px, INTN alpha);
void card_row(gui_state_t *state, card_t *c, const CHAR16 *label,
                     const CHAR16 *value);
INTN card_label_w(card_t *c, const CHAR16 * const *labels, UINTN n);

static inline void card_space(card_t *c, INTN px) { c->cy += px; }
void card_bar(gui_state_t *state, INTN x, INTN y, INTN w, INTN h,
                     UINTN num, UINTN den, color_t fill);
void card_field(gui_state_t *state, card_t *c, const CHAR16 *text,
                       UINTN caret_px, int caret);
void card_hints(gui_state_t *state, card_t *c,
                       const key_hint_t *hints, UINTN n);
INTN card_hints_h(card_t *c);

static inline boot_entry_t* entry_at(gui_state_t *state, UINTN idx) {
    boot_entry_t *e = state->entries;
    for (UINTN i = 0; i < idx && e; i++) e = e->next;
    return e;
}

static inline UINTN entry_icon_size(boot_entry_t *e, UINTN fallback) {
    return (e && e->icon_size) ? e->icon_size : fallback;
}
UINTN visible_row_width(gui_state_t *state);
void gui_entries_added(gui_state_t *state, boot_entry_t *head,
                              UINTN count, UINTN first);
void gui_entries_removed(gui_state_t *state, boot_entry_t *head,
                                UINTN count, UINTN gap, UINTN old_w);
UINTN center_info_block_h(gui_state_t *state, UINTN name_px);
void draw_center_info(gui_state_t *state, boot_entry_t *e,
                             UINTN top_y, UINTN name_px, INTN master);
void apply_deploy(boot_entry_t *e);
int v_cycle_next(gui_state_t *state);
void v_log_press(gui_state_t *state, boot_entry_t *entry);
void v_cycle_engage(gui_state_t *state, int what);
void draw_version_info(gui_state_t *state, boot_entry_t *e,
                              UINTN top_y, UINTN name_px, INTN master);
void snap_metrics(gui_state_t *state, boot_entry_t *e, UINTN name_px,
                         INTN avail_top, UINTN *head_h, UINTN *row_h,
                         UINTN *rows, INTN *bottom);
void chop_to_width(CHAR16 *s, UINTN px, UINTN maxw);
void draw_snap_info(gui_state_t *state, boot_entry_t *e,
                           UINTN name_px, INTN master, INTN expand_pm,
                           INTN avail_top);
UINTN browse_rows(gui_state_t *state, UINTN name_px);
int browse_band_set(gui_state_t *state);
void draw_browse_panel(gui_state_t *state, UINTN name_px);
int header_box(gui_state_t *state, INTN *out_x, INTN *out_y,
                      INTN *out_w, INTN *out_h);
int draw_clock_ex(gui_state_t *state, int frost);
int draw_clock(gui_state_t *state);
int clock_needs_tick(gui_state_t *state);
void ss_draw_frame(gui_state_t *state);
void ss_transition_to_saver(gui_state_t *state);
void ss_transition_to_menu(gui_state_t *state, int was_blank);
#define CUR_H 24
void cursor_backing_restore(gui_state_t *state, INTN ox, INTN oy);
void cursor_overlay(gui_state_t *state);
void cursor_move(gui_state_t *state);
void draw_editor_overlay(gui_state_t *state);
void editor_enter(gui_state_t *state);
void prompt_enter(gui_state_t *state, CHAR16 *title, CHAR16 *hint);
int editor_key(gui_state_t *state, EFI_INPUT_KEY *key);
int  cap_overlay_live(gui_state_t *state);
void gpt_warn_run(gui_state_t *state);

static inline int point_in(INTN px, INTN py, INTN x, INTN y, INTN w, INTN h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}
int poll_pointer(gui_state_t *state, int *menu_redraw);
#define CAP_TOAST_FADE_MS  500
void cap_set_toast(gui_state_t *state, const CHAR16 *msg,
                          const CHAR16 *detail, int is_err);
void cap_do_screenshot(gui_state_t *state);
void cap_start_record(gui_state_t *state);
void cap_cancel_record(gui_state_t *state);
void cap_finish_record(gui_state_t *state);
int cap_grab_due_frames(gui_state_t *state);
int cap_tick(gui_state_t *state);
void cap_draw_overlay(gui_state_t *state);

#endif
