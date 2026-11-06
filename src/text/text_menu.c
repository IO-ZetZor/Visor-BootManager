/* text_menu.c - text-mode menu: the no-GOP fallback, always built */
#include "text_menu_internal.h"

static const CHAR16 *PWR_LABEL[3] = { L"Shutdown", L"Reboot", L"Firmware" };

static void tm_line(UINTN row, UINTN cols, UINTN attr, const CHAR16 *text) {
    CHAR16 buf[TEXT_LINE_MAX];
    UINTN rows = 0;
    query_text_geometry(NULL, &rows);
    if (rows > 1 && row >= rows) row = rows - 1;
    UINTN w = text_row_width(cols);
    UINTN tl = text ? StrLen(text) : 0;
    if (tl > w) tl = w;
    UINTN pad = (w > tl) ? (w - tl) / 2 : 0;
    UINTN k = 0;
    for (UINTN i = 0; i < pad; i++) buf[k++] = ' ';
    for (UINTN i = 0; i < tl; i++) buf[k++] = text[i];
    while (k < w) buf[k++] = ' ';
    buf[k] = 0;
    ST->ConOut->SetAttribute(ST->ConOut, attr);
    ST->ConOut->SetCursorPosition(ST->ConOut, 0, row);
    ST->ConOut->OutputString(ST->ConOut, buf);
}

void left_line(UINTN row, UINTN cols, UINTN attr, const CHAR16 *text) {
    CHAR16 buf[TEXT_LINE_MAX];
    UINTN rows = 0;
    query_text_geometry(NULL, &rows);
    if (rows > 1 && row >= rows) row = rows - 1;
    UINTN w = text_row_width(cols);
    UINTN k = 0;
    while (text && text[k] && k < w) { buf[k] = text[k]; k++; }
    while (k < w) buf[k++] = ' ';
    buf[k] = 0;
    ST->ConOut->SetAttribute(ST->ConOut, attr);
    ST->ConOut->SetCursorPosition(ST->ConOut, 0, row);
    ST->ConOut->OutputString(ST->ConOut, buf);
}

static void entry_line(boot_entry_t *e, UINTN i, CHAR16 *out, UINTN out_bytes) {
    const CHAR16 *suffix = (e->type == 1) ? L"  (Windows)" : L"";
    if (i < 9)
        SPrint(out, out_bytes, L"%d.  %s%s", (int)(i + 1), e->name, suffix);
    else
        SPrint(out, out_bytes, L"%s%s", e->name, suffix);
}

static void pick_text_mode(UINTN need_w, UINTN need_h) {
    SIMPLE_TEXT_OUTPUT_INTERFACE *o = ST->ConOut;
    INT32 maxmode = o->Mode->MaxMode;
    INT32 best = o->Mode->Mode;
    UINTN best_area = ~0ULL;
    for (INT32 m = 0; m < maxmode; m++) {
        UINTN c = 0, r = 0;
        if (EFI_ERROR(o->QueryMode(o, (UINTN)m, &c, &r))) continue;
        if (c < need_w || r < need_h) continue;
        UINTN area = c * r;
        if (area < best_area) { best_area = area; best = m; }
    }
    if (best != o->Mode->Mode) o->SetMode(o, (UINTN)best);
}

void query_text_geometry(UINTN *cols, UINTN *rows) {
    UINTN c = 80, r = 25;
    ST->ConOut->QueryMode(ST->ConOut, ST->ConOut->Mode->Mode, &c, &r);
    if (c == 0) c = 80;
    if (r == 0) r = 25;
    if (cols) *cols = c;
    if (rows) *rows = r;
}

void ensure_text_mode(UINTN need_w, UINTN need_h, int keep_current) {
    if (keep_current) {
        UINTN cols = 0, rows = 0;
        query_text_geometry(&cols, &rows);
        if (cols >= need_w && rows >= need_h) return;
    }
    pick_text_mode(need_w, need_h);
}

static void tm_draw(gui_state_t *state, UINTN cols, UINTN rows, UINTN cursor, INTN remaining) {
    UINTN n = state->entry_count;

    UINTN block = 2 + (n ? (2 * n - 1) : 1) + 1 + 3 + 1 + 1;
    UINTN top = (rows > block + 2) ? (rows - block) / 2 : 1;
    UINTN r = top;

    CHAR16 *title = (state->title && state->title[0]) ? state->title : L"Visor";
    tm_line(r, cols, VT_TITLE, title);
    r += 2;

    if (n == 0) {
        tm_line(r, cols, VT_DIM, L"(no boot entries found)");
        r += 1;
    } else {
        boot_entry_t *e = state->entries;
        for (UINTN i = 0; i < n && e; i++, e = e->next) {
            CHAR16 buf[160];
            entry_line(e, i, buf, sizeof(buf));
            tm_line(r, cols, (cursor == i) ? VT_SEL : VT_NORMAL, buf);
            r += 2;
        }
        r -= 1;
    }
    r += 1;

    for (UINTN p = 0; p < 3; p++) {
        tm_line(r + p, cols, (cursor == n + p) ? VT_SEL : VT_DIM, PWR_LABEL[p]);
    }
    r += 3 + 1;

    CHAR16 path[200];
    path[0] = 0;
    if (cursor < n) {
        boot_entry_t *e = entry_at(state, cursor);
        if (e && e->kernel_path) {
            UINTN pl = StrLen(e->kernel_path);
            UINTN maxp = (cols > 8) ? cols - 8 : pl;
            if (pl <= maxp) {
                SPrint(path, sizeof(path), L"%s", e->kernel_path);
            } else {
                UINTN tail = maxp > 3 ? maxp - 3 : 0;
                SPrint(path, sizeof(path), L"...%s", e->kernel_path + (pl - tail));
            }
        }
    }
    tm_line(r, cols, VT_PATH, path);

    CHAR16 foot[120];
    if (state->timeout_active && state->timeout > 0 && remaining > 0)
        SPrint(foot, sizeof(foot),
               L"Up/Down move    Enter boot    S/R/F power    booting in %ds",
               (int)remaining);
    else
        SPrint(foot, sizeof(foot),
               L"Up/Down move    Enter boot    S/R/F power");
    if (rows > 1) tm_line(rows - 1, cols, VT_NORMAL, foot);

    ST->ConOut->SetAttribute(ST->ConOut, VT_NORMAL);
}

boot_entry_t* text_menu_run(gui_state_t *state) {
    EFI_STATUS status;
    EFI_INPUT_KEY key;

    efi_log(L"text: entering text-mode menu");

    UINTN n = state->entry_count;
    UINTN total = n + 3;

    UINTN need_w = StrLen(L"Up/Down move    Enter boot    S/R/F power    booting in 999s");
    for (UINTN i = 0; i < n; i++) {
        boot_entry_t *e = entry_at(state, i);
        if (!e) break;
        CHAR16 buf[160];
        entry_line(e, i, buf, sizeof(buf));
        UINTN l = StrLen(buf);
        if (l > need_w) need_w = l;
    }
    need_w += 4;
    UINTN need_h = (n ? (2 * n - 1) : 1) + 9;

    pick_text_mode(need_w, need_h);

    ST->ConOut->EnableCursor(ST->ConOut, FALSE);
    ST->ConOut->SetAttribute(ST->ConOut, VT_NORMAL);
    ST->ConOut->ClearScreen(ST->ConOut);

    UINTN cols = 80, rows = 25;
    query_text_geometry(&cols, &rows);

    state->action  = VISOR_ACTION_BOOT;
    state->running = 1;

    UINTN cursor = (n && state->selected < n) ? state->selected : 0;

    BS->SetWatchdogTimer(0, 0, 0, NULL);

    state->timeout_start = efi_get_tick();
    if (state->timeout == 0) state->running = 0;

    INTN last_remaining = -2;
    int  need_redraw = 1;

    while (state->running) {
        INTN remaining = -1;
        if (state->timeout_active && state->timeout > 0) {
            UINT64 elapsed = efi_get_tick() - state->timeout_start;
            remaining = state->timeout - (INTN)(elapsed / 1000);
            if (remaining <= 0) { state->running = 0; break; }
            if (remaining != last_remaining) { last_remaining = remaining; need_redraw = 1; }
        }

        if (need_redraw) { tm_draw(state, cols, rows, cursor, remaining); need_redraw = 0; }

        status = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_ERROR(status)) {

            if (state->hotplug_poll) {
                UINT64 now = efi_get_tick();
                if (!state->hp_last_ms) state->hp_last_ms = now;
                if (now - state->hp_last_ms >= 1200) {
                    state->hp_last_ms = now;
                    boot_entry_t *head = state->entries;
                    UINTN cnt = state->entry_count;
                    UINTN first = state->entry_count;
                    if (state->hotplug_poll(state->hotplug_ctx, &head, &cnt, &first)) {
                        state->entries = head;
                        state->entry_count = cnt;
                        n = cnt;
                        if (state->selected >= n && n) state->selected = n - 1;
                        ST->ConOut->ClearScreen(ST->ConOut);
                        need_redraw = 1;
                    }
                }
            }
            efi_sleep(30);
            continue;
        }

        if (state->timeout_active) { state->timeout_active = 0; need_redraw = 1; }

        CHAR16 uc = key.UnicodeChar;
        if (uc >= 'a' && uc <= 'z') uc -= 32;

        if (uc == 'V') {
            boot_entry_t *e = entry_at(state, cursor);
            CHAR16 line[160];
            SPrint(line, sizeof(line),
                   L"input: V pressed in text menu selected=%d deployments=%d snapshots=%d",
                   (int)cursor, e ? (int)e->deploy_count : 0,
                   e ? (int)e->snap_count : 0);
            efi_log(line);
            if (e && e->name) efi_log(e->name);
            efi_log(L"input: V panels are unavailable in the text menu");
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
            if (cursor >= n) state->action = VISOR_ACTION_SHUTDOWN + (int)(cursor - n);
            else state->selected = cursor;
            state->running = 0;
        }
        else if (key.UnicodeChar >= '1' && key.UnicodeChar <= '9') {
            UINTN idx = key.UnicodeChar - '1';
            if (idx < n) { state->selected = idx; state->running = 0; }
        }
        else if (key.UnicodeChar == 0x00) {
            switch (key.ScanCode) {
                case 0x01:
                    cursor = (cursor > 0) ? cursor - 1 : total - 1;
                    need_redraw = 1;
                    break;
                case 0x02:
                    cursor = (cursor + 1 < total) ? cursor + 1 : 0;
                    need_redraw = 1;
                    break;
                case 0x17:
                    efi_log(L"input: Esc pressed at the menu - opening options/rescue console");
                    state->action = VISOR_ACTION_RESCUE;
                    state->running = 0;
                    break;
            }
        }
    }

    ST->ConOut->SetAttribute(ST->ConOut, VT_NORMAL);
    ST->ConOut->ClearScreen(ST->ConOut);

    if (state->action != VISOR_ACTION_BOOT) return NULL;
    return entry_at(state, state->selected);
}
