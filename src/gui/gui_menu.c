/* gui_menu.c - entry row layout and the main menu paint */
#include "gui_internal.h"

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

UINTN visible_row_width(gui_state_t *state) {
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

void gui_entries_added(gui_state_t *state, boot_entry_t *head,
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

void gui_entries_removed(gui_state_t *state, boot_entry_t *head,
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

int header_box(gui_state_t *state, INTN *out_x, INTN *out_y,
                      INTN *out_w, INTN *out_h) {
    int mode = state->logo ? state->logo_mode : LOGO_MODE_OFF;
    int with_logo = (mode != LOGO_MODE_OFF);
    int with_text = state->show_title && mode != LOGO_MODE_ONLY;
    if (!with_logo && !with_text) return 0;

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

    *out_x = (bw < state->screen_width) ? (INTN)(state->screen_width - bw) / 2 : 0;
    *out_y = (INTN)(state->screen_height / 14);
    *out_w = (INTN)bw;
    *out_h = (INTN)bh;
    return 1;
}

static void draw_header(gui_state_t *state) {
    int mode = state->logo ? state->logo_mode : LOGO_MODE_OFF;
    int with_logo = (mode != LOGO_MODE_OFF);
    int with_text = state->show_title && mode != LOGO_MODE_ONLY;

    INTN bx, by, bwi, bhi;
    if (!header_box(state, &bx, &by, &bwi, &bhi)) return;
    UINTN bw = (UINTN)bwi, bh = (UINTN)bhi;

    CHAR16 *title = (state->title && state->title[0]) ? state->title : L"Visor";
    UINTN title_px = state->title_size ? state->title_size : default_title_px(state);
    UINTN tw = with_text ? text_width_px(title, title_px) : 0;

    UINTN lsz = 0, gap = 0;
    if (with_logo) {
        gap = state->logo_gap ? state->logo_gap : title_px / 2;
        if (mode == LOGO_MODE_ABOVE && with_text) lsz = bh - gap - title_px;
        else if (with_text)                       lsz = bw - gap - tw;
        else                                      lsz = bh;
    }

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

static void draw_hp_scan_overlay(gui_state_t *state) {
    UINTN W = state->screen_width, H = state->screen_height;
    fill_rect_alpha(state, 0, 0, (INTN)W, (INTN)H, COLOR_BLACK, 150);

    INTN bw = (INTN)(W * 7 / 10), bx = (INTN)((W - (UINTN)bw) / 2);
    INTN bh = (INTN)(state->name_size ? state->name_size : 26) * 3 + 30;
    INTN by = (INTN)((H - (UINTN)bh) / 2);
    fill_round_rect(state, bx, by, bw, bh,
                    state->box_radius ? (INTN)state->box_radius : 14,
                    COLOR_BLACK, 215);

    CHAR16 msg[] = L"Scanning USB drive...";
    UINTN th = state->name_size ? state->name_size : 26;
    if (th < 22) th = 22;
    draw_text_centered_px(state, msg, bx, (UINTN)bw, by + 14, COLOR_WHITE, th);
}

void gui_draw_menu(gui_state_t *state, int partial) {

    layout_power(state);

    UINTN px = state->screen_width * state->screen_height;
    int building = (!state->scene_cache) || (!state->scene_valid);
    if (building) {
        if (!gui_draw_background(state))
            fill_rect_alpha(state, 0, 0, state->screen_width, state->screen_height,
                            COLOR_BLACK, 60);

        if (state->blur || state->blur_title ||
            (state->show_clock && state->clock_blur))
            build_blur_cache(state);

        draw_header(state);

        draw_power_actions(state, -1, 0);

        if (state->hp_scanning)
            draw_hp_scan_overlay(state);

        if (state->scene_cache) {
            CopyMem(state->scene_cache, state->backbuffer, px * sizeof(UINT32));
            state->scene_valid = 1;
        }
    } else if (partial) {
        for (int b = 0; b < state->band_n; b++)
            scene_restore_band(state, state->band_y[b], state->band_h[b]);

        if (state->clock_drawn)
            scene_restore_band(state, state->clock_y, state->clock_h);
    } else {
        CopyMem(state->backbuffer, state->scene_cache, px * sizeof(UINT32));
    }

    if (draw_clock(state)) {
        state->clock_dirty = 1;
    } else {

        state->clock_dirty = state->clock_drawn;
        state->clock_drawn = 0;
    }

    if (state->entry_count == 0) {
        CHAR16 msg[] = L"No boot entries found";
        UINTN msg_px = default_aux_text_px(state);
        draw_text_centered_px(state, msg, 0, state->screen_width,
                              (INTN)state->screen_height / 2, state->fg_color, msg_px);
        if (!building && state->scene_valid)
            scene_restore_band(state, state->pwr_y0 - 6,
                               state->pwr_y1 - state->pwr_y0 + 12);
        if (state->browse)
            draw_browse_panel(state, state->name_size ? state->name_size : default_name_px(state));
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
    int ci_active = !state->browse && (state->center_info || ci_version || ci_snap);

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

        if (!state->browse) {
            state->band_n = 1;
            state->band_y[0] = (INTN)row_top - pad - 2;
            state->band_h[0] = (INTN)(name_y + name_px + pad) + 2 - state->band_y[0];
            if (state->center_info) {
                state->band_y[1] = ci_band_lo;
                state->band_h[1] = ci_band_hi - ci_band_lo;
                state->band_n = 2;
            }
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

        if (!state->browse && state->center_info && state->entry_count > 0)
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
    if (!state->browse) {
        state->band_n = nb;
        for (int a = 0; a < nb; a++) { state->band_y[a] = bl[a]; state->band_h[a] = bh[a] - bl[a]; }
    }

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

    if (state->browse)
        draw_browse_panel(state, name_px);

    state->prev_focus = state->focus;
    state->prev_page = page;
    state->prev_selected = state->selected;

    if (partial) return;

    if (state->timeout_active && state->timeout > 0 && !state->browse) {
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
