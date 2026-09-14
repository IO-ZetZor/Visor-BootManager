/* gui_gptwarn.c - GPT damage warning and repair screen (feature: gptrepair) */
#include "gui_internal.h"

static void gptw_close(gui_state_t *state) {
    gpt_result_free(&state->gptw_res);
    ZeroMem(&state->gptw_res, sizeof(state->gptw_res));
    gpt_plan_free(&state->gptw_plan);
    ZeroMem(&state->gptw_plan, sizeof(state->gptw_plan));
    gpt_diag_free(&state->gptw_diag);
    ZeroMem(&state->gptw_diag, sizeof(state->gptw_diag));
    if (state->gptw_have_dev) {
        gpt_disk_close(&state->gptw_dev);
        state->gptw_have_dev = 0;
    }
}

static int gptw_eq_ci(CHAR16 *a, const CHAR16 *b) {
    while (*a && *b) {
        CHAR16 ca = *a++; if (ca >= L'a' && ca <= L'z') ca = (CHAR16)(ca - 32);
        CHAR16 cb = *b++; if (cb >= L'a' && cb <= L'z') cb = (CHAR16)(cb - 32);
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static UINTN gptw_len(const CHAR16 *s) {
    UINTN n = 0;
    while (s[n]) n++;
    return n;
}

static void gptw_fit(CHAR16 *s, UINTN px, INTN avail) {
    if ((INTN)text_width_px(s, px) <= avail) return;
    UINTN n = gptw_len(s);
    while (n > 1) {
        s[n - 1] = 0;
        s[n - 2] = L'.';
        if ((INTN)text_width_px(s, px) <= avail) return;
        n--;
    }
}

static void gptw_draw(gui_state_t *state) {
    UINTN W = state->screen_width, H = state->screen_height;
    fill_rect_alpha(state, 0, 0, (INTN)W, (INTN)H, COLOR_BLACK, 175);

    card_t c;
    card_metrics(state, &c);

    CHAR16 line[160], val[96];
    gpt_diag_t *dg = &state->gptw_diag;
    gpt_plan_t  *pl = &state->gptw_plan;

    INTN bw = (INTN)(W * 4 / 5);
    if (bw < 560) bw = 560;

    if (state->gptw_state == 2) {
        static const CHAR16 * const labels[] = { L"Disk", L"Backup", L"Restores" };
        c.label_w = card_label_w(&c, labels, 3);

        const key_hint_t hints[] = {
            { L"Enter", L"repair" }, { L"D", L"details" }, { L"Esc", L"boot anyway" }
        };
        INTN bh = c.pad + (INTN)c.title_px + 11 + 13
                + 3 * card_row_h(c.body_px) + 14
                + 2 * ((INTN)c.body_px + 5) + 6
                + card_hints_h(&c) + c.pad / 2;
        card_open(state, &c, bw, bh);

        card_title(state, &c, L"Disk corruption detected", COLOR_RED, COLOR_WHITE);
        card_rule(state, &c);

        SPrint(val, sizeof(val), L"%s  -  %lld sectors x %d bytes",
               state->gptw_disk, (long long)dg->total_sectors,
               (int)dg->sector_size);
        gptw_fit(val, c.body_px, c.w - 2 * c.pad - c.label_w);
        card_row(state, &c, L"Disk", val);

        SPrint(val, sizeof(val), L"LBA %lld  -  %d partitions",
               (long long)pl->src_header_lba, (int)pl->part_count);
        card_row(state, &c, L"Backup", val);

        SPrint(val, sizeof(val), L"%d x %d-byte entries to LBA %lld",
               (int)pl->entry_count, (int)pl->entry_size,
               (long long)pl->dst_entries_lba);
        gptw_fit(val, c.body_px, c.w - 2 * c.pad - c.label_w);
        card_row(state, &c, L"Restores", val);

        card_space(&c, 14);
        card_line(state, &c,
            L"The primary partition table failed validation. A verified backup",
            COLOR_WHITE, c.body_px, 225);
        card_line(state, &c,
            L"copy exists; repair rewrites the primary from it and never the backup.",
            COLOR_WHITE, c.body_px, 225);

        card_hints(state, &c, hints, 3);
        return;
    }

    if (state->gptw_state == 3) {
        const key_hint_t hints[] = {
            { L"B", L"back" }, { L"R", L"repair" }, { L"Esc", L"boot anyway" }
        };
        INTN bh = (INTN)(H * 4 / 5);
        card_open(state, &c, bw, bh);
        INTN limit = c.y + c.h - card_hints_h(&c) - c.pad;

        card_title(state, &c, L"Recovery details", COLOR_ORANGE, COLOR_WHITE);
        card_rule(state, &c);

        static const CHAR16 * const labels[] = { L"Overall", L"MBR", L"Usable" };
        c.label_w = card_label_w(&c, labels, 3);

        SPrint(val, sizeof(val), L"%s", gpt_status_text(dg->overall));
        card_row(state, &c, L"Overall", val);
        SPrint(val, sizeof(val), L"%s%s", gpt_status_text(dg->mbr_status),
               dg->mbr_protective ? L" (protective)" : L"");
        card_row(state, &c, L"MBR", val);
        SPrint(val, sizeof(val), L"%lld sectors [%lld..%lld] x %d bytes",
               (long long)dg->total_sectors,
               (long long)pl->first_usable_lba,
               (long long)pl->last_usable_lba, (int)dg->sector_size);
        gptw_fit(val, c.body_px, c.w - 2 * c.pad - c.label_w);
        card_row(state, &c, L"Usable", val);

        card_space(&c, 10);

        gpt_table_t *ts[2] = { &dg->primary, &dg->backup };
        const CHAR16 *nm[2] = { L"Primary", L"Backup" };
        for (int t = 0; t < 2 && c.cy < limit - 3 * (INTN)c.small_px; t++) {
            color_t hc = ts[t]->header_status == GPT_VALID &&
                         ts[t]->entries_status == GPT_VALID
                       ? COLOR_GREEN : COLOR_RED;
            INTN d = (INTN)c.small_px / 2;
            card_dot(state, c.x + c.pad, c.cy + ((INTN)c.small_px - d) / 2, d,
                     hc, 255);
            draw_text_px_a(state, (CHAR16*)nm[t],
                           c.x + c.pad + d + 8, c.cy, COLOR_WHITE,
                           c.small_px, 250);
            c.cy += (INTN)c.small_px + 5;

            SPrint(line, sizeof(line),
                   L"    header %s (%s)   entries %s (%s)   layout %s",
                   gpt_status_text(ts[t]->header_status),
                   gpt_reason_text(ts[t]->header_reason),
                   gpt_status_text(ts[t]->entries_status),
                   gpt_reason_text(ts[t]->entries_reason),
                   gpt_status_text(ts[t]->layout_status));
            gptw_fit(line, c.small_px, c.w - 2 * c.pad);
            card_line(state, &c, line, COLOR_GRAY, c.small_px, 225);

            SPrint(line, sizeof(line),
                   L"    entries @LBA %lld   %d x %d bytes   crc 0x%08x   %d used",
                   (long long)ts[t]->hdr.entry_lba, (int)ts[t]->hdr.entry_count,
                   (int)ts[t]->hdr.entry_size,
                   (unsigned)ts[t]->hdr.entries_crc32,
                   (int)ts[t]->used_count);
            gptw_fit(line, c.small_px, c.w - 2 * c.pad);
            card_line(state, &c, line, COLOR_GRAY, c.small_px, 205);
            card_space(&c, 6);
        }

        for (UINTN i = 0; i < dg->note_count && c.cy < limit; i++) {
            gpt_note_text(&dg->notes[i], line, sizeof(line) / sizeof(CHAR16));
            gptw_fit(line, c.small_px, c.w - 2 * c.pad);
            card_line(state, &c, line, COLOR_GRAY, c.small_px, 190);
        }

        card_hints(state, &c, hints, 3);
        return;
    }

    if (state->gptw_state == 6) {
        static const CHAR16 * const labels[] = { L"Disk", L"Header", L"Entries" };
        c.label_w = card_label_w(&c, labels, 3);

        const key_hint_t hints[] = {
            { L"Enter", L"confirm" }, { L"Esc", L"go back" }
        };
        INTN bh = c.pad + (INTN)c.title_px + 11 + 13
                + 3 * card_row_h(c.body_px) + 14
                + 2 * ((INTN)c.body_px + 5) + 12
                + (INTN)c.title_px + 18 + 12
                + card_hints_h(&c) + c.pad / 2;
        card_open(state, &c, bw, bh);

        card_title(state, &c, L"Confirm repair", COLOR_RED, COLOR_WHITE);
        card_rule(state, &c);

        SPrint(val, sizeof(val), L"%s  (media %d)", state->gptw_disk,
               (int)dg->media_id);
        card_row(state, &c, L"Disk", val);
        SPrint(val, sizeof(val), L"write LBA %lld",
               (long long)pl->dst_header_lba);
        card_row(state, &c, L"Header", val);
        SPrint(val, sizeof(val), L"write %d entries @LBA %lld",
               (int)pl->entry_count, (long long)pl->dst_entries_lba);
        card_row(state, &c, L"Entries", val);

        card_space(&c, 14);
        card_line(state, &c,
            L"This rewrites the primary table from the verified backup.",
            COLOR_WHITE, c.body_px, 230);
        card_line(state, &c,
            L"The backup copy itself is never written. Type YES to proceed.",
            COLOR_WHITE, c.body_px, 230);
        card_space(&c, 12);

        card_field(state, &c, state->gptw_confirm,
                   text_width_px(state->gptw_confirm, c.title_px), 1);
        card_hints(state, &c, hints, 2);
        return;
    }

    if (state->gptw_state == 4) {
        INTN bh = c.pad + (INTN)c.title_px + 11 + 13
                + 2 * ((INTN)c.body_px + 5) + 16 + 10 + c.pad;
        card_open(state, &c, bw, bh);

        card_title(state, &c, L"Repairing", COLOR_ORANGE, COLOR_WHITE);
        card_rule(state, &c);
        SPrint(line, sizeof(line), L"Disk %s  (media %d)", state->gptw_disk,
               (int)dg->media_id);
        card_line(state, &c, line, COLOR_WHITE, c.body_px, 235);
        SPrint(line, sizeof(line),
               L"Writing header @LBA %lld and %d entries @LBA %lld",
               (long long)pl->dst_header_lba, (int)pl->entry_count,
               (long long)pl->dst_entries_lba);
        gptw_fit(line, c.body_px, c.w - 2 * c.pad);
        card_line(state, &c, line, COLOR_GRAY, c.body_px, 225);
        card_space(&c, 6);
        card_bar(state, c.x + c.pad, c.cy, c.w - 2 * c.pad, 6, 1, 1,
                 COLOR_ORANGE);
        return;
    }

    if (state->gptw_state == 5) {
        gpt_result_t *r = &state->gptw_res;
        const key_hint_t hints[] = { { L"Enter", L"continue" } };

        if (r->success) {
            static const CHAR16 * const labels[] =
                { L"Primary", L"Backup", L"Tables", L"Restored" };
            c.label_w = card_label_w(&c, labels, 4);
            INTN bh = c.pad + (INTN)c.title_px + 11 + 13
                    + 4 * card_row_h(c.body_px) + 10
                    + card_hints_h(&c) + c.pad / 2;
            card_open(state, &c, bw, bh);

            card_title(state, &c, L"Repair completed and verified",
                       COLOR_GREEN, COLOR_WHITE);
            card_rule(state, &c);

            SPrint(val, sizeof(val), L"%s",
                   gpt_status_text(gpt_table_status(&r->after.primary)));
            card_row(state, &c, L"Primary", val);
            SPrint(val, sizeof(val), L"%s",
                   gpt_status_text(gpt_table_status(&r->after.backup)));
            card_row(state, &c, L"Backup", val);
            card_row(state, &c, L"Tables",
                     r->after.cmp.kind == GPT_CMP_IDENTICAL ? L"match"
                                                            : L"differ");
            SPrint(val, sizeof(val), L"%d partitions",
                   (int)r->after.primary.used_count);
            card_row(state, &c, L"Restored", val);

            card_hints(state, &c, hints, 1);
        } else {
            INTN bh = c.pad + (INTN)c.title_px + 11 + 13
                    + 2 * ((INTN)c.body_px + 5) + 10
                    + card_hints_h(&c) + c.pad / 2;
            card_open(state, &c, bw, bh);

            card_title(state, &c, L"Repair could not be completed",
                       COLOR_ORANGE, COLOR_WHITE);
            card_rule(state, &c);
            SPrint(line, sizeof(line), L"Reason: %s", gpt_reason_text(r->reason));
            gptw_fit(line, c.body_px, c.w - 2 * c.pad);
            card_line(state, &c, line, COLOR_WHITE, c.body_px, 235);
            card_line(state, &c, L"The disk was not modified.",
                      COLOR_GRAY, c.body_px, 225);
            card_hints(state, &c, hints, 1);
        }
    }
}

static void con_in_drain(gui_state_t *state) {
    EFI_INPUT_KEY k;
    while (!EFI_ERROR(uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2,
                                        ST->ConIn, &k))) { }
    if (state->mouse_enabled && state->has_pointer && state->spp) {
        EFI_SIMPLE_POINTER_PROTOCOL *sp =
            (EFI_SIMPLE_POINTER_PROTOCOL*)state->spp;
        EFI_SIMPLE_POINTER_STATE st;
        while (!EFI_ERROR(sp->GetState(sp, &st))) { }
    }
}

static void gptw_do_repair(gui_state_t *state) {
    CHAR16 lb[128];
    SPrint(lb, sizeof(lb), L"gpt: repairing primary GPT on disk media %d",
           (int)state->gptw_diag.media_id);
    efi_log(lb);
    gpt_result_t res;
    ZeroMem(&res, sizeof(res));
    EFI_STATUS st = gpt_execute_plan(&state->gptw_dev, &state->gptw_plan, &res);
    state->gptw_res = res;
    state->gptw_state = 5;
    if (!EFI_ERROR(st) && res.success)
        efi_log(L"gpt: primary GPT rebuilt and verified from backup");
    else
        efi_log(L"gpt: repair failed");
}
void gpt_warn_run(gui_state_t *state) {
    if (state->gptw_suppressed) return;

    UINTN n = 0;
    EFI_HANDLE *hs = gpt_disk_enum(&n);
    if (!hs) return;
    if (n == 0) {
        efi_free_pool(hs);
        return;
    }

    int found = 0;
    for (UINTN i = 0; i < n && !found; i++) {
        gpt_dev_t dev;
        if (!gpt_disk_from_bio(&dev, hs[i])) continue;

        gpt_diag_t dg;
        ZeroMem(&dg, sizeof(dg));
        EFI_STATUS st = gpt_diagnose(&dev, 0, &dg);
        if (EFI_ERROR(st)) {
            gpt_diag_free(&dg);
            gpt_disk_close(&dev);
            continue;
        }

        if (dg.klass == GPT_CLASS_PRIMARY_CORRUPT_BACKUP_VALID &&
            !dg.read_only) {
            gpt_plan_t plan;
            ZeroMem(&plan, sizeof(plan));
            int ok = gpt_build_primary_plan(&dg, &plan);
            if (ok && plan.safety == GPT_VALID) {
                found = 1;
                SPrint(state->gptw_disk, sizeof(state->gptw_disk),
                       L"media %d", (int)dg.media_id);
                state->gptw_have_dev = 1;
                state->gptw_dev = dev;
                state->gptw_diag = dg;
                state->gptw_plan = plan;
            } else {
                gpt_plan_free(&plan);
                gpt_diag_free(&dg);
                gpt_disk_close(&dev);
            }
        } else {
            gpt_diag_free(&dg);
            gpt_disk_close(&dev);
        }
    }
    efi_free_pool(hs);

    if (!found || !state->gptw_have_dev) return;

    con_in_drain(state);
    state->gptw_state = 2;
    state->gptw_confirm[0] = 0;
    efi_log(L"gpt: primary GPT corruption detected - offering repair");

    int dirty = 1;
    while (state->gptw_state != 0) {
        if (anim_tick(state)) dirty = 1;
        if (cap_overlay_live(state) && cap_tick(state) == 2) dirty = 1;

        if (dirty) {
            gui_draw_menu(state, 0);
            gptw_draw(state);
            if (cap_overlay_live(state)) cap_draw_overlay(state);
            gui_present(state);
            dirty = 0;
        }

        EFI_INPUT_KEY key;
        EFI_STATUS ks = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_ERROR(ks)) {
            efi_sleep(state->bg_anim ? 8
                      : (cap_overlay_live(state) ? 12 : 30));
            continue;
        }
        dirty = 1;
        CHAR16 u = key.UnicodeChar;
        int esc = (u == 0x1B || (u == 0x00 && key.ScanCode == 0x17));

        if (u == 0x00 && key.ScanCode == 0x10) {
            cap_do_screenshot(state);
            continue;
        }

        if (state->gptw_state == 2) {
            if (u == 0x0D) {
                state->gptw_state = 6;
                state->gptw_confirm[0] = 0;
            } else if (u == L'd' || u == L'D') {
                state->gptw_state = 3;
            } else if (esc || u == L'b' || u == L'B') {
                state->gptw_suppressed = 1;
                state->gptw_state = 0;
            }
        } else if (state->gptw_state == 3) {
            if (esc) {
                state->gptw_suppressed = 1;
                state->gptw_state = 0;
            } else if (u == 0x0D || u == L'b' || u == L'B') {
                state->gptw_state = 2;
            } else if (u == L'r' || u == L'R') {
                state->gptw_state = 6;
                state->gptw_confirm[0] = 0;
            }
        } else if (state->gptw_state == 6) {
            if (esc) {
                state->gptw_state = 2;
                state->gptw_confirm[0] = 0;
            } else if (u == 0x0D) {
                if (gptw_eq_ci(state->gptw_confirm, L"YES") ||
                    gptw_eq_ci(state->gptw_confirm, L"Y")) {
                    state->gptw_state = 4;
                    gui_draw_menu(state, 0);
                    gptw_draw(state);
                    gui_present(state);
                    gptw_do_repair(state);
                } else {
                    state->gptw_confirm[0] = 0;
                    state->gptw_state = 2;
                }
            } else if (u == 0x08) {
                UINTN l = gptw_len(state->gptw_confirm);
                if (l) state->gptw_confirm[l - 1] = 0;
            } else if (u >= L'0' && u <= L'z' &&
                       gptw_len(state->gptw_confirm) <
                           sizeof(state->gptw_confirm) / sizeof(CHAR16) - 1) {
                UINTN l = gptw_len(state->gptw_confirm);
                CHAR16 c = u;
                if (c >= L'a' && c <= L'z') c = (CHAR16)(c - 32);
                state->gptw_confirm[l] = c;
                state->gptw_confirm[l + 1] = 0;
            }
        } else if (state->gptw_state == 5) {
            if (u == 0x0D || esc) {
                state->gptw_suppressed = 1;
                state->gptw_state = 0;
            }
        }
    }

    gptw_close(state);
}
