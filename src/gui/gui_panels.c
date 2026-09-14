/* gui_panels.c - power, version, snapshot and center-info panels */
#include "gui_internal.h"

static const struct { CHAR16 *label; int action; } POWER_ACTIONS[] = {
    { L"Shutdown", VISOR_ACTION_SHUTDOWN },
    { L"Reboot",   VISOR_ACTION_REBOOT   },
    { L"Firmware", VISOR_ACTION_FIRMWARE },
};

void layout_power(gui_state_t *state) {
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

void draw_power_actions(gui_state_t *state, int focus_idx, int live) {
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

UINTN center_info_block_h(gui_state_t *state, UINTN name_px) {
    UINTN path_px = (name_px * 4) / 5; if (path_px < 10) path_px = 10;
    return state->show_names ? path_px : (name_px + 6 + path_px);
}

void draw_center_info(gui_state_t *state, boot_entry_t *e,
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

void apply_deploy(boot_entry_t *e) {
    if (!e || e->deploy_count == 0) return;
    UINTN s = e->deploy_sel;
    if (s >= e->deploy_count) { s = 0; e->deploy_sel = 0; }
    e->kernel_path = e->deployments[s].kernel;
    e->initrd_path = e->deployments[s].initrd;
    e->cmdline     = e->deployments[s].cmdline;
}

int v_cycle_next(gui_state_t *state) {
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

void v_log_press(gui_state_t *state, boot_entry_t *entry) {
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

void v_cycle_engage(gui_state_t *state, int what) {
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

void draw_version_info(gui_state_t *state, boot_entry_t *e,
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

void snap_metrics(gui_state_t *state, boot_entry_t *e, UINTN name_px,
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

void draw_snap_info(gui_state_t *state, boot_entry_t *e,
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
