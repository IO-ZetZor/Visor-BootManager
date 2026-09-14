/* gui_pointer.c - mouse cursor and pointer polling (feature: pointer) */
#include "gui_internal.h"

#define CUR_W 18

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

void cursor_backing_restore(gui_state_t *state, INTN ox, INTN oy) {
    for (INTN j = 0; j < CUR_H; j++)
        for (INTN i = 0; i < CUR_W; i++) {
            UINT32 *p = get_pixel(state, (UINTN)(ox + i), (UINTN)(oy + j));
            if (p) *p = state->cursor_save[j * CUR_W + i];
        }
}

void cursor_overlay(gui_state_t *state) {
    cursor_backing_save(state, state->cursor_x - 1, state->cursor_y);
    state->cur_prev_x = state->cursor_x;
    state->cur_prev_y = state->cursor_y;
    state->cursor_saved = 1;
    draw_cursor(state);
    gui_present_band(state, state->cursor_y, CUR_H);
}

void cursor_move(gui_state_t *state) {
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

int poll_pointer(gui_state_t *state, int *menu_redraw) {
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

    if (state->browse) {
        if (scroll > 0) fb_move(state->browse, 1);
        else if (scroll < 0) fb_move(state->browse, -1);
        if (scroll && browse_band_set(state)) *menu_redraw = 1;
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
