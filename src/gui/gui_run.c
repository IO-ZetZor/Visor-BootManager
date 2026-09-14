/* gui_run.c - the menu event loop, password prompt and teardown */
#include "gui_internal.h"

static void wipe16(CHAR16 *s, UINTN n) {
    volatile CHAR16 *p = (volatile CHAR16*)s;
    while (n--) *p++ = 0;
}

boot_entry_t* gui_run(gui_state_t *state) {
    EFI_STATUS status;
    EFI_INPUT_KEY key;

    state->timeout_start = efi_get_tick();
    state->action = VISOR_ACTION_BOOT;

    BS->SetWatchdogTimer(0, 0, 0, NULL);

    if (!state->gptw_suppressed)
        gpt_warn_run(state);

    if (state->timeout == 0) {
        state->running = 0;
    }

    INTN last_remaining = -2;
    int  need_redraw = 1;
    int  full_redraw = 1;
    int  intro_fade = 1;

    state->ss_last_input_ms = efi_get_tick();

    while (state->running) {

        if (state->sound_poll) state->sound_poll();

        if (state->screensaver && state->ss_level != SS_LEVEL_AWAKE) {
            if (ss_input_pending(state)) {
                int was_blank = (state->ss_level == SS_LEVEL_BLANK);
                ss_wake(state);
                ss_transition_to_menu(state, was_blank);
                need_redraw = 0;
                full_redraw = 0;
                intro_fade = 0;

                state->timeout_start = efi_get_tick();
                last_remaining = -2;
                continue;
            }

            if (state->ss_level == SS_LEVEL_DIM) {
                int rd = anim_tick(state);
                if (state->ss_keep_clock && clock_needs_tick(state)) rd = 1;
                if (rd) { ss_draw_frame(state); gui_present(state); }
            }

            if (ss_tick(state) && state->ss_level == SS_LEVEL_BLANK) {
                gui_fade_out(state);

                if (state->bg_anim) state->bg_anim->next_ms = 0;
            }

            efi_sleep(state->ss_level == SS_LEVEL_BLANK ? 120 : 60);
            continue;
        }

        if (cap_overlay_live(state)) state->ss_last_input_ms = efi_get_tick();

        if (ss_tick(state) && state->ss_level == SS_LEVEL_DIM) {
            ss_transition_to_saver(state);
            continue;
        }

        if (state->cap_mode == 3 && state->cap_gif) {
            UINT64 el = efi_get_tick() - state->cap_start_ms;
            if (el >= state->cap_next_due_ms) {
                need_redraw = 1;
                full_redraw = 1;
                if (state->bg_anim && state->bg_anim->frame_count > 1)
                    state->scene_valid = 0;
            }
        }

        if (need_redraw) {
            INTN ghost_y = -1;
            if (state->cursor_saved) {
                cursor_backing_restore(state, state->cur_prev_x - 1, state->cur_prev_y);
                ghost_y = state->cur_prev_y;
                state->cursor_saved = 0;
            }
            if (cap_overlay_live(state)) full_redraw = 1;
            gui_draw_menu(state, !full_redraw);
            if (state->editing) draw_editor_overlay(state);

            if (state->cap_mode == 3 && state->cap_gif) {
                if (!cap_grab_due_frames(state)) {
                    if (cap_gif_count(state->cap_gif)) {
                        state->cap_mode = 4;
                    } else {
                        cap_gif_free(state->cap_gif);
                        state->cap_gif = NULL;
                        state->cap_mode = 0;
                        cap_set_toast(state, L"Recording failed",
                                      L"Not enough memory to encode a frame", 1);
                    }
                }
            }

            if (cap_overlay_live(state)) cap_draw_overlay(state);

            if (state->cap_mode == 4) {
                gui_present(state);
                cap_finish_record(state);
                gui_draw_menu(state, 0);
                cap_draw_overlay(state);
            }

            if (state->sound_start) {
                void (*start)(void) = state->sound_start;
                state->sound_start = NULL;
                start();
            }

            if (intro_fade && full_redraw && !state->editing) {
                gui_fade_in_current(state);
                intro_fade = 0;
            } else if (full_redraw || state->editing) {
                gui_present(state);
            } else {
                for (int b = 0; b < state->band_n; b++)
                    gui_present_band(state, state->band_y[b], state->band_h[b]);
                if (ghost_y >= 0) gui_present_band(state, ghost_y, CUR_H);

                if (state->clock_dirty)
                    gui_present_band(state, state->clock_y, state->clock_h);
            }
            if (state->editing) intro_fade = 0;
            need_redraw = state->anim_active || state->page_anim || state->hp_anim;
            full_redraw = 0;
            if (state->cursor_active && !state->editing)
                cursor_overlay(state);
        }

        if (!state->editing) {
            int menu_rd = 0;
            int pr = poll_pointer(state, &menu_rd);
            if (pr || menu_rd) state->ss_last_input_ms = efi_get_tick();
            if (pr == 1) state->running = 0;
            else if (menu_rd) need_redraw = 1;
            else if (pr == 2 && state->cursor_active && !need_redraw)
                cursor_move(state);
        }

        status = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (!EFI_ERROR(status)) {
            state->ss_last_input_ms = efi_get_tick();

            if (state->timeout_active) { state->timeout_active = 0; need_redraw = 1; full_redraw = 1; }

            if (key.UnicodeChar == 0x00 &&
                (key.ScanCode == 0x10 || key.ScanCode == 0x14)) {
                if (key.ScanCode == 0x10) {
                    if (state->cap_mode == 0) cap_do_screenshot(state);
                } else {
                    if (state->cap_mode == 0) cap_start_record(state);
                    else cap_cancel_record(state);
                }
                need_redraw = 1; full_redraw = 1;
                continue;
            }

            if (state->editing) {
                editor_key(state, &key);
                need_redraw = 1; full_redraw = 1;
                continue;
            }

            if (state->browse) {
                fb_t *bs = state->browse;
                CHAR16 bu = key.UnicodeChar;
                if (bu >= 'a' && bu <= 'z') bu -= 32;
                if (key.UnicodeChar == 0x1B ||
                    (key.UnicodeChar == 0 && key.ScanCode == 0x17) || bu == 'Q') {
                    fb_free(bs); state->browse = NULL;
                    need_redraw = 1; full_redraw = 1;
                } else if (key.UnicodeChar == 0x0D) {
                    fb_entry_t *e = fb_cursor(bs);
                    if (e && e->is_dir) {
                        if (fb_enter(bs)) { need_redraw = 1; full_redraw = 1; }
                    } else if (e) {
                        int r = fb_boot_apply(bs, state);
                        if (r == 1) {
                            fb_free(bs); state->browse = NULL;
                            state->running = 0;
                        } else if (r == 2) {
                            fb_free(bs); state->browse = NULL;
                            need_redraw = 1; full_redraw = 1;
                        } else {
                            need_redraw = 1; full_redraw = 1;
                        }
                    }
                } else if (key.UnicodeChar == 0 &&
                           (key.ScanCode == 0x01 || key.ScanCode == 0x02 ||
                            key.ScanCode == 0x05 || key.ScanCode == 0x06 ||
                            key.ScanCode == 0x09 || key.ScanCode == 0x0A)) {
                    UINTN before = bs->cursor;
                    UINTN rows = browse_rows(state,
                        state->name_size ? state->name_size : default_name_px(state));
                    switch (key.ScanCode) {
                        case 0x01: fb_move(bs, -1); break;
                        case 0x02: fb_move(bs, 1); break;
                        case 0x05: fb_move(bs, -(INTN)bs->entry_count - 1); break;
                        case 0x06: fb_move(bs, (INTN)bs->entry_count + 1); break;
                        case 0x09: fb_move(bs, -(INTN)rows); break;
                        case 0x0A: fb_move(bs, (INTN)rows); break;
                    }
                    if (bs->cursor != before) {
                        need_redraw = 1;
                        full_redraw = browse_band_set(state) ? 0 : 1;
                    }
                } else if (key.UnicodeChar == 0 && key.ScanCode == 0x04) {
                    if (fb_up(bs)) { need_redraw = 1; full_redraw = 1; }
                } else if (key.UnicodeChar == 0 && key.ScanCode == 0x03) {
                    fb_switch_volume(bs, 1);
                    need_redraw = 1; full_redraw = 1;
                } else if (key.UnicodeChar == 0x08 || key.UnicodeChar == 0x09) {
                    if (key.UnicodeChar == 0x09) {
                        fb_switch_volume(bs, 1);
                        need_redraw = 1; full_redraw = 1;
                    } else if (fb_up(bs)) {
                        need_redraw = 1; full_redraw = 1;
                    }
                } else if (bu >= 'A' && bu <= 'Z') {
                    UINTN before = bs->cursor;
                    for (UINTN i = 0; i < bs->entry_count; i++) {
                        CHAR16 c0 = bs->entries[i].name[0];
                        if (c0 >= 'a' && c0 <= 'z') c0 -= 32;
                        if (c0 == bu) { bs->cursor = i; break; }
                    }
                    if (bs->cursor != before) {
                        need_redraw = 1;
                        full_redraw = browse_band_set(state) ? 0 : 1;
                    }
                }
                continue;
            }

            if (state->version_mode || state->snap_mode) {
                boot_entry_t *se = entry_at(state, state->selected);
                int snap = state->snap_mode;
                CHAR16 vu = key.UnicodeChar;
                if (vu >= 'a' && vu <= 'z') vu -= 32;
                if (vu == 'V') {
                    v_log_press(state, se);
                    v_cycle_engage(state, v_cycle_next(state));
                    need_redraw = 1; full_redraw = 1;
                } else if (key.UnicodeChar == 0x0D) {
                    if (snap && se && se->snap_count > 0) {
                        snapshot_t *s = &se->snapshots[se->snap_sel];
                        if (state->override_cmdline) { efi_free_pool(state->override_cmdline); state->override_cmdline = NULL; }
                        if (state->override_kernel_path) { efi_free_pool(state->override_kernel_path); state->override_kernel_path = NULL; }
                        if (state->override_initrd_path) { efi_free_pool(state->override_initrd_path); state->override_initrd_path = NULL; }
                        state->override_initrd_set = 0;
                        state->override_cmdline = s->cmdline ? efi_strdup(s->cmdline) : NULL;
                        if (s->kernel) { state->override_kernel_path = efi_strdup(s->kernel); }
                        if (s->initrd) { state->override_initrd_path = efi_strdup(s->initrd);
                                         state->override_initrd_set = 1; }
                    }
                    state->running = 0;
                } else if (key.UnicodeChar == 0x1B ||
                           (key.UnicodeChar == 0x00 && key.ScanCode == 0x17)) {
                    if (!snap && se) { se->deploy_sel = se->deploy_default; apply_deploy(se); }
                    if (state->center_info || !gui_animation_on(state)) {
                        state->version_mode = 0; state->snap_mode = 0;
                        state->ver_fading = 0; state->ver_next = 0;
                    } else {
                        state->ver_what = snap ? 2 : 1;
                        state->ver_dir = -1; state->ver_frame = 0; state->ver_fading = 1;
                        state->ver_next = 0;
                    }
                    need_redraw = 1; full_redraw = 1;
                } else if (key.UnicodeChar == 0x00 &&
                           (key.ScanCode == 0x04 || (snap && key.ScanCode == 0x01))) {
                    if (snap) {
                        if (se && se->snap_sel > 0) { se->snap_sel--; need_redraw = 1; full_redraw = 1; }
                    } else if (se && se->deploy_sel > 0) {
                        se->deploy_sel--; apply_deploy(se); need_redraw = 1; full_redraw = 1;
                    }
                } else if (key.UnicodeChar == 0x00 &&
                           (key.ScanCode == 0x03 || (snap && key.ScanCode == 0x02))) {
                    if (snap) {
                        if (se && se->snap_sel + 1 < se->snap_count) { se->snap_sel++; need_redraw = 1; full_redraw = 1; }
                    } else if (se && se->deploy_sel + 1 < se->deploy_count) {
                        se->deploy_sel++; apply_deploy(se); need_redraw = 1; full_redraw = 1;
                    }
                } else if (vu == 'S') { state->action = VISOR_ACTION_SHUTDOWN; state->running = 0; }
                else if (vu == 'R') { state->action = VISOR_ACTION_REBOOT; state->running = 0; }
                else if (vu == 'F') { state->action = VISOR_ACTION_FIRMWARE; state->running = 0; }
                continue;
            }

            CHAR16 uc = key.UnicodeChar;
            if (uc >= 'a' && uc <= 'z') uc -= 32;

            if (uc == 'V') {
                boot_entry_t *se = entry_at(state, state->selected);
                v_log_press(state, se);
                if (state->focus == FOCUS_ENTRIES && se &&
                    (se->deploy_count > 1 || se->snap_count > 0)) {
                    v_cycle_engage(state, v_cycle_next(state));
                    need_redraw = 1; full_redraw = 1;
                } else if (state->focus != FOCUS_ENTRIES)
                    efi_log(L"input: V ignored because power actions have focus");
                else if (!se)
                    efi_log(L"input: V ignored because no boot entry is selected");
                else
                    efi_log(L"input: V ignored because the entry has no alternate deployments or snapshots");
            }
            else if (uc == 'E') {
                if (state->focus == FOCUS_ENTRIES && state->editor_enabled && state->entry_count > 0) {
                    editor_enter(state);
                    need_redraw = 1; full_redraw = 1;
                }
            }
            else if (uc == 'B') {
                if (state->browse) {
                    fb_free(state->browse);
                    state->browse = NULL;
                    need_redraw = 1; full_redraw = 1;
                } else {
                    fb_t *bs = efi_allocate_pool(sizeof(fb_t));
                    if (bs && fb_init(bs)) {
                        state->browse = bs;
                        fb_list(bs);
                        need_redraw = 1; full_redraw = 1;
                    } else {
                        if (bs) efi_free_pool(bs);
                        efi_log(L"input: browse unavailable - no readable filesystems");
                    }
                }
            }
            else if (uc == 'S') { state->action = VISOR_ACTION_SHUTDOWN; state->running = 0; }
            else if (uc == 'R') { state->action = VISOR_ACTION_REBOOT; state->running = 0; }
            else if (uc == 'F') { state->action = VISOR_ACTION_FIRMWARE; state->running = 0; }
            else if (key.UnicodeChar == 0x1B) {
                efi_log(L"input: Esc pressed at the menu - opening options/rescue console");
                state->action = VISOR_ACTION_RESCUE;
                state->running = 0;
            }
            else if (key.UnicodeChar == 0x0D) {

                if (state->focus == FOCUS_POWER)
                    state->action = VISOR_ACTION_SHUTDOWN + (int)state->power_sel;
                state->running = 0;
            }
            else if (key.UnicodeChar == 0x00) {
                int power_top = (state->power_position == POWER_POS_TOPLEFT ||
                                 state->power_position == POWER_POS_TOPRIGHT);
                switch (key.ScanCode) {
                    case 0x04:
                        state->focus = FOCUS_ENTRIES;
                        if (state->entry_count) {
                            if (state->selected > 0) state->selected--;
                            else state->selected = state->entry_count - 1;
                        }
                        need_redraw = 1;
                        break;
                    case 0x03:
                        state->focus = FOCUS_ENTRIES;
                        if (state->entry_count) {
                            if (state->selected + 1 < state->entry_count) state->selected++;
                            else state->selected = 0;
                        }
                        need_redraw = 1;
                        break;
                    case 0x02:
                        if (power_top) {
                            if (state->focus == FOCUS_POWER) {
                                if (state->power_sel < POWER_ACTION_COUNT - 1)
                                    state->power_sel++;
                                else
                                    state->focus = FOCUS_ENTRIES;
                            }
                        } else {
                            if (state->focus == FOCUS_ENTRIES) {
                                state->focus = FOCUS_POWER;
                                state->power_sel = 0;
                            } else if (state->power_sel < POWER_ACTION_COUNT - 1) {
                                state->power_sel++;
                            }
                        }
                        need_redraw = 1;
                        break;
                    case 0x01:
                        if (power_top) {
                            if (state->focus == FOCUS_ENTRIES) {
                                state->focus = FOCUS_POWER;
                                state->power_sel = POWER_ACTION_COUNT - 1;
                            } else if (state->power_sel > 0) {
                                state->power_sel--;
                            }
                        } else {
                            if (state->focus == FOCUS_POWER) {
                                if (state->power_sel > 0) state->power_sel--;
                                else state->focus = FOCUS_ENTRIES;
                            }
                        }
                        need_redraw = 1;
                        break;
                    case 0x17:
                        efi_log(L"input: Esc pressed at the menu - opening options/rescue console");
                        state->action = VISOR_ACTION_RESCUE;
                        state->running = 0;
                        break;
                }
            }
            else if (key.UnicodeChar >= '1' && key.UnicodeChar <= '9') {
                UINTN idx = key.UnicodeChar - '1';
                if (idx < state->entry_count) {
                    state->focus = FOCUS_ENTRIES;
                    state->selected = idx;
                    state->running = 0;
                }
            }
        }

        if (state->ver_fading) {
            int N = state->anim_frames; if (N < 2) N = 2;
            state->ver_frame++;
            need_redraw = 1; full_redraw = 1;
            if (state->ver_frame >= N) {
                state->ver_fading = 0;
                state->ver_frame = 0;
                if (state->ver_dir < 0) {
                    state->version_mode = 0;
                    state->snap_mode = 0;
                    if (state->ver_next) {
                        int nx = state->ver_next;
                        state->ver_next = 0;
                        state->version_mode = (nx == 1);
                        state->snap_mode    = (nx == 2);
                        state->ver_what = nx;
                        state->ver_dir = 1; state->ver_fading = 1;
                    }
                }
            }
        }

        if (state->hp_anim) {
            int N = state->anim_frames; if (N < 2) N = 2;
            state->hp_frame++;
            need_redraw = 1;
            if (state->hp_frame >= N) {
                state->hp_anim = 0;
                state->hp_frame = 0;
                state->hp_removal = 0;
            }
        }

        if (state->hotplug_poll && !state->editing &&
            !state->page_anim && !state->hp_anim && !state->anim_active &&
            !cap_overlay_live(state)) {
            UINT64 now = efi_get_tick();
            if (!state->hp_last_ms) state->hp_last_ms = now;
            if (now - state->hp_last_ms >= 1200) {
                state->hp_last_ms = now;

                UINTN pre_w = visible_row_width(state);
                boot_entry_t *head = state->entries;
                UINTN cnt = state->entry_count;
                UINTN first = state->entry_count;

                state->hp_scanning = 1;
                gui_draw_menu(state, 0);
                gui_present(state);

                int mask = state->hotplug_poll(state->hotplug_ctx,
                                               &head, &cnt, &first);
                state->hp_scanning = 0;
                if (mask == 1 && cnt > state->entry_count) {
                    gui_entries_added(state, head, cnt, first);
                } else if (mask == 2) {
                    gui_entries_removed(state, head, cnt, first, pre_w);
                } else if (mask) {

                    state->entries = head;
                    state->entry_count = cnt;
                    state->hp_anim = 0;
                    state->hp_removal = 0;
                    if (state->selected >= cnt && cnt)
                        state->selected = cnt - 1;
                }
                need_redraw = 1;
                full_redraw = 1;
            }
        }

        if (!state->editing && !state->browse && anim_tick(state)) {
            need_redraw = 1;
            full_redraw = 1;
        }

        if (state->timeout_active && state->timeout > 0) {
            UINT64 elapsed = efi_get_tick() - state->timeout_start;
            INTN remaining = state->timeout - (INTN)(elapsed / 1000);
            if (remaining <= 0) {
                state->running = 0;
            } else if (remaining != last_remaining) {
                last_remaining = remaining;
                need_redraw = 1;
                full_redraw = 1;
            }
        }

        if (!need_redraw && !state->editing && clock_needs_tick(state)) {
            need_redraw = 1;
            if (!state->clock_drawn) full_redraw = 1;
        }

        if (cap_overlay_live(state)) {
            if (cap_tick(state) == 2) { need_redraw = 1; full_redraw = 1; }
        }

        efi_sleep((state->anim_active || state->page_anim ||
                   state->ver_fading || state->hp_anim) ? 6
                  : (state->cap_mode == 2 || state->cap_mode == 3 ? 6
                  : (state->cap_status_ms > 0 &&
                     state->cap_status_ms < CAP_TOAST_FADE_MS ? 8
                  : (state->bg_anim ? 8
                  : (state->cursor_active ? 12 : 30)))));
    }

    if (state->action != VISOR_ACTION_BOOT) return NULL;

    boot_entry_t *selected = state->entries;
    for (UINTN i = 0; i < state->selected && selected; i++) {
        selected = selected->next;
    }
    return selected;
}

EFI_STATUS gui_prompt_password(gui_state_t *state, CHAR16 *title, CHAR16 *hint,
                               CHAR16 **out) {
    EFI_STATUS status;
    EFI_INPUT_KEY key;
    int running = 1;
    int accepted = 0;

    if (!state || !out) return EFI_INVALID_PARAMETER;
    *out = NULL;

    prompt_enter(state, title ? title : L"Password",
                 hint ? hint : L"F2 reveals what you typed - use it to check your layout");

    while (running) {
        gui_draw_menu(state, 0);
        draw_editor_overlay(state);
        gui_present(state);

        status = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_ERROR(status)) {
            efi_sleep(30);
            continue;
        }

        int r = editor_key(state, &key);
        if (r == 1) { accepted = 1; running = 0; }
        else if (r < 0) { running = 0; }
    }

    state->edit_secret = 0;
    state->edit_reveal = 0;
    state->edit_title = NULL;
    state->edit_hint = NULL;
    state->editing = 0;

    if (!accepted) {
        wipe16(state->edit_buf, 512);
        state->edit_len = state->edit_cursor = 0;
        return EFI_ABORTED;
    }
    *out = efi_strdup(state->edit_buf);
    wipe16(state->edit_buf, 512);
    state->edit_len = state->edit_cursor = 0;
    return *out ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}

static void free_icon(icon_t *ic) {
    if (!ic) return;
    if (ic->scaled) efi_free_pool(ic->scaled);
    if (ic->pixels) efi_free_pool(ic->pixels);
    efi_free_pool(ic);
}

void gui_shutdown(gui_state_t *state) {

    boot_entry_t *entry = state->entries;
    while (entry) {
        if (entry->icon) { free_icon(entry->icon); entry->icon = NULL; }
        entry = entry->next;
    }

    free_icon(state->background);
    state->background = NULL;
    anim_free(state->bg_anim);
    state->bg_anim = NULL;
    fb_free(state->browse);
    state->browse = NULL;
    free_icon(state->logo);
    state->logo = NULL;
    free_icon(state->shutdown_icon);
    state->shutdown_icon = NULL;
    free_icon(state->reboot_icon);
    state->reboot_icon = NULL;
    free_icon(state->firmware_icon);
    state->firmware_icon = NULL;

    if (state->background_path) {
        efi_free_pool(state->background_path);
    }

    gui_fill_rect(state, 0, 0, state->screen_width, state->screen_height, COLOR_BLACK);
    gui_present(state);

    if (state->backbuffer) {
        efi_free_pool(state->backbuffer);
        state->backbuffer = NULL;
    }
    if (state->scene_cache) {
        efi_free_pool(state->scene_cache);
        state->scene_cache = NULL;
    }
    if (state->blur_cache) {
        efi_free_pool(state->blur_cache);
        state->blur_cache = NULL;
    }
    if (state->blur_line) {
        efi_free_pool(state->blur_line);
        state->blur_line = NULL;
    }
    glyph_cache_flush();
}
