
#include <efi.h>
#include <efilib.h>
#include "text_menu.h"
#include "efi_helpers.h"

extern EFI_SYSTEM_TABLE *ST;
extern EFI_BOOT_SERVICES *BS;

#define VT_ATTR(fg, bg) ((fg) | ((bg) << 4))
#define VT_NORMAL  VT_ATTR(0x0F, 0x00)
#define VT_TITLE   VT_ATTR(0x0F, 0x00)
#define VT_SEL     VT_ATTR(0x00, 0x07)
#define VT_DIM     VT_ATTR(0x08, 0x00)
#define VT_PATH    VT_ATTR(0x07, 0x00)

#define TEXT_LINE_MAX 512

static const CHAR16 *PWR_LABEL[3] = { L"Shutdown", L"Reboot", L"Firmware" };

static UINTN text_row_width(UINTN cols) {
    UINTN w = (cols > 0) ? cols - 1 : 0;
    if (w > TEXT_LINE_MAX - 2) w = TEXT_LINE_MAX - 2;
    return w;
}

static void line(UINTN row, UINTN cols, UINTN attr, const CHAR16 *text) {
    CHAR16 buf[TEXT_LINE_MAX];
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

static void left_line(UINTN row, UINTN cols, UINTN attr, const CHAR16 *text) {
    CHAR16 buf[TEXT_LINE_MAX];
    UINTN w = text_row_width(cols);
    UINTN k = 0;
    while (text && text[k] && k < w) { buf[k] = text[k]; k++; }
    while (k < w) buf[k++] = ' ';
    buf[k] = 0;
    ST->ConOut->SetAttribute(ST->ConOut, attr);
    ST->ConOut->SetCursorPosition(ST->ConOut, 0, row);
    ST->ConOut->OutputString(ST->ConOut, buf);
}

static boot_entry_t* entry_at(gui_state_t *state, UINTN idx) {
    boot_entry_t *e = state->entries;
    for (UINTN i = 0; i < idx && e; i++) e = e->next;
    return e;
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

static void query_text_geometry(UINTN *cols, UINTN *rows) {
    UINTN c = 80, r = 25;
    ST->ConOut->QueryMode(ST->ConOut, ST->ConOut->Mode->Mode, &c, &r);
    if (c == 0) c = 80;
    if (r == 0) r = 25;
    if (cols) *cols = c;
    if (rows) *rows = r;
}

static void ensure_text_mode(UINTN need_w, UINTN need_h, int keep_current) {
    if (keep_current) {
        UINTN cols = 0, rows = 0;
        query_text_geometry(&cols, &rows);
        if (cols >= need_w && rows >= need_h) return;
    }
    pick_text_mode(need_w, need_h);
}

static void draw(gui_state_t *state, UINTN cols, UINTN rows, UINTN cursor, INTN remaining) {
    UINTN n = state->entry_count;

    UINTN block = 2 + (n ? (2 * n - 1) : 1) + 1 + 3 + 1 + 1;
    UINTN top = (rows > block + 2) ? (rows - block) / 2 : 1;
    UINTN r = top;

    CHAR16 *title = (state->title && state->title[0]) ? state->title : L"Visor";
    line(r, cols, VT_TITLE, title);
    r += 2;

    if (n == 0) {
        line(r, cols, VT_DIM, L"(no boot entries found)");
        r += 1;
    } else {
        boot_entry_t *e = state->entries;
        for (UINTN i = 0; i < n && e; i++, e = e->next) {
            CHAR16 buf[160];
            entry_line(e, i, buf, sizeof(buf));
            line(r, cols, (cursor == i) ? VT_SEL : VT_NORMAL, buf);
            r += 2;
        }
        r -= 1;
    }
    r += 1;

    for (UINTN p = 0; p < 3; p++) {
        line(r + p, cols, (cursor == n + p) ? VT_SEL : VT_DIM, PWR_LABEL[p]);
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
    line(r, cols, VT_PATH, path);

    CHAR16 foot[120];
    if (state->timeout_active && state->timeout > 0 && remaining > 0)
        SPrint(foot, sizeof(foot),
               L"Up/Down move    Enter boot    S/R/F power    booting in %ds",
               (int)remaining);
    else
        SPrint(foot, sizeof(foot),
               L"Up/Down move    Enter boot    S/R/F power");
    if (rows > 1) line(rows - 1, cols, VT_NORMAL, foot);

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

        if (need_redraw) { draw(state, cols, rows, cursor, remaining); need_redraw = 0; }

        status = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_ERROR(status)) { efi_sleep(30); continue; }

        if (state->timeout_active) { state->timeout_active = 0; need_redraw = 1; }

        CHAR16 uc = key.UnicodeChar;
        if (uc >= 'a' && uc <= 'z') uc -= 32;

        if (uc == 'S') { state->action = VISOR_ACTION_SHUTDOWN; state->running = 0; }
        else if (uc == 'R') { state->action = VISOR_ACTION_REBOOT; state->running = 0; }
        else if (uc == 'F') { state->action = VISOR_ACTION_FIRMWARE; state->running = 0; }
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

static UINTN log_lines_from_buffer(efi_file_buffer_t *buf, UINTN *starts, UINTN max_lines) {
    if (!buf || !buf->data || !buf->size || !starts || !max_lines) return 0;
    UINT8 *d = (UINT8*)buf->data;
    UINTN n = 0;
    UINTN slot = 0;
    int wrapped = 0;
    starts[n++] = 0;
    for (UINTN i = 0; i + 1 < buf->size; i++) {
        if (d[i] != '\n') continue;
        UINTN start = i + 1;
        if (n < max_lines) {
            starts[n++] = start;
        } else {
            starts[slot++] = start;
            wrapped = 1;
            if (slot == max_lines) slot = 0;
        }
    }
    if (wrapped && slot) {
        UINTN *ordered = efi_allocate_pool(max_lines * sizeof(UINTN));
        if (ordered) {
            for (UINTN i = 0; i < max_lines; i++)
                ordered[i] = starts[(slot + i) % max_lines];
            for (UINTN i = 0; i < max_lines; i++)
                starts[i] = ordered[i];
            efi_free_pool(ordered);
        }
    }
    return n;
}

static void copy_log_line(efi_file_buffer_t *buf, UINTN start, CHAR16 *out, UINTN cap) {
    if (!out || cap == 0) return;
    UINTN k = 0;
    if (buf && buf->data && start < buf->size) {
        UINT8 *d = (UINT8*)buf->data;
        for (UINTN i = start; i < buf->size && k < cap - 1; i++) {
            if (d[i] == '\r' || d[i] == '\n') break;
            UINT8 c = d[i];
            out[k++] = (c >= 0x20 && c < 0x7F) ? (CHAR16)c : L' ';
        }
    }
    out[k] = 0;
}

static void draw_recovery(gui_state_t *state, boot_entry_t *failed, EFI_STATUS status,
                          efi_file_buffer_t *log, UINTN *starts, UINTN line_count,
                          UINTN top, UINTN cols, UINTN rows, int quiet_boot) {
    (void)state;
    ST->ConOut->SetAttribute(ST->ConOut, VT_NORMAL);
    ST->ConOut->ClearScreen(ST->ConOut);

    CHAR16 title[180];
    SPrint(title, sizeof(title), L"Visor Recovery - boot failed: %s",
           failed && failed->name ? failed->name : L"unknown entry");
    left_line(0, cols, VT_TITLE, title);

    CHAR16 code[96];
    SPrint(code, sizeof(code), L"Status: 0x%x    Log: \\EFI\\visor\\boot.log", (unsigned int)status);
    left_line(1, cols, VT_DIM, code);

    left_line(2, cols, VT_NORMAL,
              L"Enter retry    M menu    R reboot    S shutdown    F firmware    Up/Down scroll");
    if (quiet_boot)
        left_line(3, cols, VT_DIM, L"quiet=1 hid boot logs during handoff; the captured log is shown below.");

    UINTN body_top = 4;
    UINTN body_rows = (rows > body_top + 1) ? rows - body_top - 1 : 0;
    if (!log || !line_count) {
        left_line(body_top, cols, VT_DIM, L"No boot log could be loaded.");
    } else {
        for (UINTN r = 0; r < body_rows; r++) {
            UINTN idx = top + r;
            CHAR16 linebuf[TEXT_LINE_MAX];
            if (idx < line_count) copy_log_line(log, starts[idx], linebuf, TEXT_LINE_MAX);
            else linebuf[0] = 0;
            left_line(body_top + r, cols, VT_NORMAL, linebuf);
        }
    }

    CHAR16 foot[120];
    SPrint(foot, sizeof(foot), L"Showing %d/%d", (int)(line_count ? top + 1 : 0), (int)line_count);
    left_line(rows - 1, cols, VT_DIM, foot);
}

static UINTN text_len(CHAR16 *s) {
    UINTN n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int text_eq_ci(CHAR16 *a, const CHAR16 *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        CHAR16 ca = *a++;
        CHAR16 cb = *b++;
        if (ca >= L'A' && ca <= L'Z') ca = (CHAR16)(ca + 32);
        if (cb >= L'A' && cb <= L'Z') cb = (CHAR16)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

static int text_prefix_ci(CHAR16 *s, const CHAR16 *prefix) {
    if (!s || !prefix) return 0;
    while (*prefix) {
        CHAR16 ca = *s++;
        CHAR16 cb = *prefix++;
        if (ca >= L'A' && ca <= L'Z') ca = (CHAR16)(ca + 32);
        if (cb >= L'A' && cb <= L'Z') cb = (CHAR16)(cb + 32);
        if (ca != cb) return 0;
    }
    return 1;
}

static void trim_in_place(CHAR16 *s) {
    if (!s) return;
    UINTN start = 0;
    while (s[start] == L' ' || s[start] == L'\t') start++;
    if (start) {
        UINTN i = 0;
        while (s[start]) s[i++] = s[start++];
        s[i] = 0;
    }
    UINTN n = text_len(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t')) s[--n] = 0;
}

static void unquote_in_place(CHAR16 *s) {
    if (!s || (s[0] != L'"' && s[0] != L'\'')) return;
    CHAR16 quote = s[0];
    UINTN n = text_len(s);
    if (n < 2 || s[n - 1] != quote) return;
    for (UINTN i = 1; i + 1 < n; i++) s[i - 1] = s[i];
    s[n - 2] = 0;
}

static CHAR16* next_arg(CHAR16 **cursor) {
    if (!cursor || !*cursor) return NULL;
    CHAR16 *p = *cursor;
    while (*p == L' ' || *p == L'\t') p++;
    if (!*p) { *cursor = p; return NULL; }

    CHAR16 quote = 0;
    if (*p == L'"' || *p == L'\'') quote = *p++;
    CHAR16 *start = p;
    CHAR16 *out = p;
    CHAR16 *after = NULL;
    if (quote) {
        while (*p && *p != quote) *out++ = *p++;
        after = (*p == quote) ? p + 1 : p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') *out++ = *p++;
        after = *p ? p + 1 : p;
    }
    *out = 0;
    while (*after == L' ' || *after == L'\t') after++;
    *cursor = after;
    return start;
}

static void out_line(UINTN *row, UINTN rows, UINTN cols, UINTN attr, CHAR16 *text) {
    if (!row || *row >= rows - 1) return;
    left_line((*row)++, cols, attr, text ? text : L"");
}

static void shell_status(UINTN rows, UINTN cols, CHAR16 *text) {
    if (rows < 2) return;
    left_line(rows - 2, cols, VT_DIM, text ? text : L"");
}

static void shell_prompt(UINTN rows, UINTN cols, CHAR16 *cmd, UINTN cursor) {
    if (rows < 1) return;
    CHAR16 linebuf[TEXT_LINE_MAX];
    UINTN w = text_row_width(cols);
    UINTN k = 0;
    if (k < w) linebuf[k++] = L'v';
    if (k < w) linebuf[k++] = L'i';
    if (k < w) linebuf[k++] = L's';
    if (k < w) linebuf[k++] = L'o';
    if (k < w) linebuf[k++] = L'r';
    if (k < w) linebuf[k++] = L'>';
    if (k < w) linebuf[k++] = L' ';
    UINTN prefix = k;
    for (UINTN i = 0; cmd && cmd[i] && k < w; i++) linebuf[k++] = cmd[i];
    while (k < w) linebuf[k++] = L' ';
    linebuf[k] = 0;
    ST->ConOut->SetAttribute(ST->ConOut, VT_NORMAL);
    ST->ConOut->SetCursorPosition(ST->ConOut, 0, rows - 1);
    ST->ConOut->OutputString(ST->ConOut, linebuf);
    UINTN cx = prefix + cursor;
    if (cx >= w) cx = w ? w - 1 : 0;
    ST->ConOut->SetCursorPosition(ST->ConOut, cx, rows - 1);
}

static void shell_clear(UINTN rows, UINTN cols, boot_entry_t *selected,
                        boot_entry_t *failed, EFI_STATUS status, int quiet_boot) {
    ST->ConOut->SetAttribute(ST->ConOut, VT_NORMAL);
    ST->ConOut->ClearScreen(ST->ConOut);

    CHAR16 title[180];
    SPrint(title, sizeof(title), L"Visor rescue shell - failed: %s",
           failed && failed->name ? failed->name : L"unknown entry");
    left_line(0, cols, VT_TITLE, title);

    CHAR16 stat[180];
    SPrint(stat, sizeof(stat), L"Status 0x%x    selected: %s",
           (unsigned int)status,
           selected && selected->name ? selected->name : L"(none)");
    left_line(1, cols, VT_DIM, stat);

    left_line(2, cols, VT_NORMAL,
              L"Type 'help'. Useful: entries, use N, show, set options ..., append ..., boot, log");
    if (quiet_boot)
        left_line(3, cols, VT_DIM, L"quiet boot was active; use 'log' to view captured boot logs.");
    for (UINTN r = 4; r < rows; r++) left_line(r, cols, VT_NORMAL, L"");
}

static void print_help(UINTN *row, UINTN rows, UINTN cols) {
    out_line(row, rows, cols, VT_NORMAL, L"Commands:");
    out_line(row, rows, cols, VT_NORMAL, L"  boot | retry                 retry the selected entry");
    out_line(row, rows, cols, VT_NORMAL, L"  menu                         return to the boot menu");
    out_line(row, rows, cols, VT_NORMAL, L"  entries                      list boot entries");
    out_line(row, rows, cols, VT_NORMAL, L"  use N                        select boot entry N");
    out_line(row, rows, cols, VT_NORMAL, L"  show [N]                     show entry paths and options");
    out_line(row, rows, cols, VT_NORMAL, L"  set options TEXT             replace kernel options for next boot");
    out_line(row, rows, cols, VT_NORMAL, L"  append TEXT                  append options for next boot");
    out_line(row, rows, cols, VT_NORMAL, L"  set kernel PATH              override kernel/image path for next boot");
    out_line(row, rows, cols, VT_NORMAL, L"  set initrd PATH | unset initrd");
    out_line(row, rows, cols, VT_NORMAL, L"  rescue                       append systemd rescue + nomodeset");
    out_line(row, rows, cols, VT_NORMAL, L"  single                       append systemd single-user mode");
    out_line(row, rows, cols, VT_NORMAL, L"  unset options|kernel|initrd  clear one-shot override");
    out_line(row, rows, cols, VT_NORMAL, L"  log                          open boot log pager");
    out_line(row, rows, cols, VT_NORMAL, L"  ls [PATH]                    list a directory on readable EFI filesystems");
    out_line(row, rows, cols, VT_NORMAL, L"  cat PATH                     print a small text file");
    out_line(row, rows, cols, VT_NORMAL, L"  reboot | shutdown | firmware");
    out_line(row, rows, cols, VT_DIM,    L"Tip: quote paths or option strings containing spaces.");
}

static void print_entry_summary(UINTN *row, UINTN rows, UINTN cols,
                                boot_entry_t *e, UINTN idx, int selected) {
    CHAR16 buf[220];
    SPrint(buf, sizeof(buf), L"%c %d  %s%s",
           selected ? L'*' : L' ',
           (int)(idx + 1),
           e && e->name ? e->name : L"(unnamed)",
           (e && e->type == 1) ? L"  [windows]" : L"");
    out_line(row, rows, cols, selected ? VT_SEL : VT_NORMAL, buf);
}

static void print_entries(UINTN *row, UINTN rows, UINTN cols, gui_state_t *state) {
    if (!state || state->entry_count == 0) {
        out_line(row, rows, cols, VT_DIM, L"No boot entries found.");
        return;
    }
    boot_entry_t *e = state->entries;
    for (UINTN i = 0; e && i < state->entry_count; i++, e = e->next)
        print_entry_summary(row, rows, cols, e, i, i == state->selected);
}

static void print_wrapped(UINTN *row, UINTN rows, UINTN cols,
                          const CHAR16 *label, CHAR16 *value) {
    CHAR16 buf[TEXT_LINE_MAX];
    UINTN w = (cols > 4) ? cols - 2 : 78;
    if (w > TEXT_LINE_MAX - 2) w = TEXT_LINE_MAX - 2;
    UINTN k = 0;
    if (label) {
        for (UINTN i = 0; label[i] && k < w; i++) buf[k++] = label[i];
    }
    UINTN prefix = k;
    CHAR16 *v = value ? value : L"";
    if (!v[0]) {
        if (k < w) buf[k++] = L'-';
        buf[k] = 0;
        out_line(row, rows, cols, VT_NORMAL, buf);
        return;
    }
    for (UINTN i = 0; v[i]; i++) {
        if (k >= w) {
            buf[k] = 0;
            out_line(row, rows, cols, VT_NORMAL, buf);
            k = 0;
            for (UINTN p = 0; p < prefix && k < w; p++) buf[k++] = L' ';
        }
        buf[k++] = v[i];
    }
    buf[k] = 0;
    out_line(row, rows, cols, VT_NORMAL, buf);
}

static void print_entry_detail(UINTN *row, UINTN rows, UINTN cols,
                               boot_entry_t *e, UINTN idx, int selected) {
    if (!e) {
        out_line(row, rows, cols, VT_DIM, L"No such entry.");
        return;
    }
    CHAR16 head[220];
    SPrint(head, sizeof(head), L"%sentry %d: %s%s",
           selected ? L"selected " : L"",
           (int)(idx + 1),
           e->name ? e->name : L"(unnamed)",
           e->type == 1 ? L"  [windows]" : L"");
    out_line(row, rows, cols, VT_TITLE, head);
    print_wrapped(row, rows, cols, L"  kernel:  ", e->kernel_path);
    print_wrapped(row, rows, cols, L"  initrd:  ", e->initrd_path);
    print_wrapped(row, rows, cols, L"  options: ", e->cmdline);
    print_wrapped(row, rows, cols, L"  uuid:    ", e->uuid);
}

static int parse_uint(CHAR16 *s, UINTN *out) {
    if (!s || !*s || !out) return 0;
    UINTN v = 0;
    for (UINTN i = 0; s[i]; i++) {
        if (s[i] < L'0' || s[i] > L'9') return 0;
        v = v * 10 + (UINTN)(s[i] - L'0');
    }
    *out = v;
    return 1;
}

static CHAR16* effective_cmdline(boot_entry_t *e, gui_state_t *state) {
    if (state && state->override_cmdline) return state->override_cmdline;
    return e ? e->cmdline : NULL;
}

static CHAR16* join_options(CHAR16 *base, CHAR16 *extra) {
    if (!extra) return NULL;
    trim_in_place(extra);
    UINTN bl = text_len(base);
    UINTN el = text_len(extra);
    UINTN sep = (bl && el) ? 1 : 0;
    CHAR16 *out = efi_allocate_pool((bl + sep + el + 1) * sizeof(CHAR16));
    if (!out) return NULL;
    UINTN k = 0;
    for (UINTN i = 0; i < bl; i++) out[k++] = base[i];
    if (sep) out[k++] = L' ';
    for (UINTN i = 0; i < el; i++) out[k++] = extra[i];
    out[k] = 0;
    return out;
}

static int set_override_cmdline(gui_state_t *state, CHAR16 *value) {
    if (!state) return 0;
    if (state->override_cmdline) {
        efi_free_pool(state->override_cmdline);
        state->override_cmdline = NULL;
    }
    state->override_cmdline = efi_strdup(value ? value : L"");
    return state->override_cmdline != NULL;
}

static int append_override_cmdline(gui_state_t *state, boot_entry_t *e, CHAR16 *value) {
    if (!state) return 0;
    CHAR16 *base = effective_cmdline(e, state);
    CHAR16 *joined = join_options(base, value);
    if (!joined) return 0;
    if (state->override_cmdline) efi_free_pool(state->override_cmdline);
    state->override_cmdline = joined;
    return 1;
}

static int set_override_path(CHAR16 **slot, CHAR16 *value) {
    if (!slot) return 0;
    if (*slot) {
        efi_free_pool(*slot);
        *slot = NULL;
    }
    if (!value || !value[0]) return 1;
    *slot = efi_strdup(value);
    return *slot != NULL;
}

static void free_log_buffer(efi_file_buffer_t *log) {
    if (!log) return;
    if (log->data) efi_free_pool(log->data);
    efi_free_pool(log);
}

static efi_file_buffer_t* load_log_lines(UINTN **starts_out, UINTN *line_count_out) {
    if (starts_out) *starts_out = NULL;
    if (line_count_out) *line_count_out = 0;
    int file_log = efi_log_file_enabled();
    efi_log_set_file(0);
    efi_file_buffer_t *log = efi_load_file(L"\\EFI\\visor\\boot.log");
    efi_log_set_file(file_log);
    if (!log || !log->data || !log->size) return log;

    UINTN cap = log->size / 2 + 2;
    if (cap < 64) cap = 64;
    if (cap > 4096) cap = 4096;
    UINTN *starts = efi_allocate_pool(cap * sizeof(UINTN));
    if (!starts) return log;
    UINTN line_count = log_lines_from_buffer(log, starts, cap);
    if (starts_out) *starts_out = starts;
    else efi_free_pool(starts);
    if (line_count_out) *line_count_out = line_count;
    return log;
}

static void log_pager(gui_state_t *state, boot_entry_t *failed,
                      EFI_STATUS status, int quiet_boot,
                      UINTN cols, UINTN rows) {
    UINTN *starts = NULL;
    UINTN line_count = 0;
    efi_file_buffer_t *log = load_log_lines(&starts, &line_count);
    UINTN body_rows = (rows > 5) ? rows - 5 : 1;
    UINTN top = (line_count > body_rows) ? line_count - body_rows : 0;
    int redraw = 1;
    EFI_INPUT_KEY key;

    for (;;) {
        if (redraw) {
            draw_recovery(state, failed, status, log, starts, line_count,
                          top, cols, rows, quiet_boot);
            left_line(rows - 1, cols, VT_DIM,
                      L"Up/Down scroll  PgUp/PgDn page  Home/End jump  Q/Esc back");
            redraw = 0;
        }
        EFI_STATUS ks = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_ERROR(ks)) { efi_sleep(30); continue; }
        CHAR16 uc = key.UnicodeChar;
        if (uc >= L'a' && uc <= L'z') uc = (CHAR16)(uc - 32);
        if (uc == L'Q' || uc == 0x1B || key.ScanCode == 0x17) break;
        if (key.UnicodeChar == 0x00) {
            if (key.ScanCode == 0x01) {
                if (top > 0) top--;
                redraw = 1;
            } else if (key.ScanCode == 0x02) {
                if (top + body_rows < line_count) top++;
                redraw = 1;
            } else if (key.ScanCode == 0x09) {
                top = (top > body_rows) ? top - body_rows : 0;
                redraw = 1;
            } else if (key.ScanCode == 0x0A) {
                if (top + body_rows < line_count) top += body_rows;
                if (top + body_rows > line_count && line_count > body_rows)
                    top = line_count - body_rows;
                redraw = 1;
            } else if (key.ScanCode == 0x05) {
                top = 0;
                redraw = 1;
            } else if (key.ScanCode == 0x06) {
                top = (line_count > body_rows) ? line_count - body_rows : 0;
                redraw = 1;
            }
        }
    }
    if (starts) efi_free_pool(starts);
    free_log_buffer(log);
}

static void print_file_text(UINTN *row, UINTN rows, UINTN cols, CHAR16 *path) {
    if (!path || !path[0]) {
        out_line(row, rows, cols, VT_DIM, L"usage: cat PATH");
        return;
    }
    efi_file_buffer_t *buf = efi_load_file(path);
    if (!buf || !buf->data || !buf->size) {
        free_log_buffer(buf);
        out_line(row, rows, cols, VT_DIM, L"file not found or unreadable");
        return;
    }
    if (buf->size > 65536) {
        free_log_buffer(buf);
        out_line(row, rows, cols, VT_DIM, L"file is too large for recovery cat");
        return;
    }

    UINT8 *d = (UINT8*)buf->data;
    UINTN w = (cols > 4) ? cols - 2 : 78;
    if (w > TEXT_LINE_MAX - 2) w = TEXT_LINE_MAX - 2;
    CHAR16 linebuf[TEXT_LINE_MAX];
    UINTN k = 0;
    for (UINTN i = 0; i < buf->size && *row < rows - 1; i++) {
        UINT8 c = d[i];
        if (c == '\r') continue;
        if (c == '\n' || k >= w) {
            linebuf[k] = 0;
            out_line(row, rows, cols, VT_NORMAL, linebuf);
            k = 0;
            if (c == '\n') continue;
        }
        linebuf[k++] = (c >= 0x20 && c < 0x7F) ? (CHAR16)c : L'.';
    }
    if (k && *row < rows - 1) {
        linebuf[k] = 0;
        out_line(row, rows, cols, VT_NORMAL, linebuf);
    }
    free_log_buffer(buf);
}

static void list_dir(UINTN *row, UINTN rows, UINTN cols, CHAR16 *path) {
    CHAR16 *p = (path && path[0]) ? path : L"\\";
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) {
        out_line(row, rows, cols, VT_DIM, L"boot volume is not readable");
        return;
    }
    EFI_FILE_PROTOCOL *dir = efi_open_dir(root, p);
    if (!dir) {
        root->Close(root);
        out_line(row, rows, cols, VT_DIM, L"directory not found or unreadable");
        return;
    }
    CHAR16 name[128];
    int is_dir = 0;
    int any = 0;
    while (*row < rows - 1 && efi_read_dirent(dir, name, 128, &is_dir)) {
        CHAR16 buf[180];
        SPrint(buf, sizeof(buf), L"%s%s", name, is_dir ? L"\\" : L"");
        out_line(row, rows, cols, is_dir ? VT_PATH : VT_NORMAL, buf);
        any = 1;
    }
    if (!any) out_line(row, rows, cols, VT_DIM, L"(empty)");
    dir->Close(dir);
    root->Close(root);
}

static int execute_recovery_command(gui_state_t *state, boot_entry_t **selected_io,
                                    CHAR16 *cmdline, UINTN *row, UINTN rows,
                                    UINTN cols, EFI_STATUS status,
                                    boot_entry_t *failed, int quiet_boot) {
    (void)status;
    boot_entry_t *selected = selected_io ? *selected_io : NULL;
    CHAR16 raw[512];
    SPrint(raw, sizeof(raw), L"visor> %s", cmdline ? cmdline : L"");
    out_line(row, rows, cols, VT_DIM, raw);

    CHAR16 *cursor = cmdline;
    CHAR16 *cmd = next_arg(&cursor);
    if (!cmd) return 0;
    {
        CHAR16 logbuf[128];
        SPrint(logbuf, sizeof(logbuf), L"recovery: command '%s'", cmd);
        efi_log(logbuf);
    }
    if (text_eq_ci(cmd, L"help") || text_eq_ci(cmd, L"?")) {
        print_help(row, rows, cols);
    } else if (text_eq_ci(cmd, L"clear") || text_eq_ci(cmd, L"cls")) {
        shell_clear(rows, cols, selected, failed, status, quiet_boot);
        *row = quiet_boot ? 4 : 3;
    } else if (text_eq_ci(cmd, L"entries") || text_eq_ci(cmd, L"lsentries")) {
        print_entries(row, rows, cols, state);
    } else if (text_eq_ci(cmd, L"use") || text_eq_ci(cmd, L"select")) {
        CHAR16 *arg = next_arg(&cursor);
        UINTN n = 0;
        if (!parse_uint(arg, &n) || n == 0 || n > state->entry_count) {
            out_line(row, rows, cols, VT_DIM, L"usage: use N");
        } else {
            state->selected = n - 1;
            *selected_io = entry_at(state, state->selected);
            selected = *selected_io;
            CHAR16 msg[180];
            SPrint(msg, sizeof(msg), L"selected entry %d: %s",
                   (int)n, selected && selected->name ? selected->name : L"(unnamed)");
            out_line(row, rows, cols, VT_NORMAL, msg);
        }
    } else if (text_eq_ci(cmd, L"show")) {
        CHAR16 *arg = next_arg(&cursor);
        UINTN idx = state->selected;
        if (arg && arg[0]) {
            UINTN n = 0;
            if (!parse_uint(arg, &n) || n == 0 || n > state->entry_count) {
                out_line(row, rows, cols, VT_DIM, L"usage: show [N]");
                return 0;
            }
            idx = n - 1;
        }
        print_entry_detail(row, rows, cols, entry_at(state, idx), idx, idx == state->selected);
        if (idx == state->selected) {
            if (state->override_kernel_path)
                print_wrapped(row, rows, cols, L"  next kernel:  ", state->override_kernel_path);
            if (state->override_initrd_set)
                print_wrapped(row, rows, cols, L"  next initrd:  ", state->override_initrd_path);
            if (state->override_cmdline)
                print_wrapped(row, rows, cols, L"  next options: ", state->override_cmdline);
        }
    } else if (text_eq_ci(cmd, L"set")) {
        CHAR16 *field = next_arg(&cursor);
        trim_in_place(cursor);
        unquote_in_place(cursor);
        if (!field) {
            out_line(row, rows, cols, VT_DIM, L"usage: set options|kernel|initrd VALUE");
        } else if (text_eq_ci(field, L"options") || text_eq_ci(field, L"cmdline")) {
            if (set_override_cmdline(state, cursor))
                out_line(row, rows, cols, VT_NORMAL, L"next boot options replaced");
            else
                out_line(row, rows, cols, VT_DIM, L"out of memory setting options");
        } else if (text_eq_ci(field, L"kernel")) {
            if (!cursor || !cursor[0]) {
                out_line(row, rows, cols, VT_DIM, L"usage: set kernel PATH");
            } else if (set_override_path(&state->override_kernel_path, cursor)) {
                out_line(row, rows, cols, VT_NORMAL, L"next boot kernel path overridden");
            } else {
                out_line(row, rows, cols, VT_DIM, L"out of memory setting kernel path");
            }
        } else if (text_eq_ci(field, L"initrd")) {
            if (set_override_path(&state->override_initrd_path, cursor)) {
                state->override_initrd_set = 1;
                out_line(row, rows, cols, VT_NORMAL, cursor && cursor[0]
                         ? L"next boot initrd path overridden"
                         : L"next boot initrd cleared");
            } else {
                out_line(row, rows, cols, VT_DIM, L"out of memory setting initrd path");
            }
        } else {
            out_line(row, rows, cols, VT_DIM, L"unknown field; use options, kernel, or initrd");
        }
    } else if (text_eq_ci(cmd, L"append") || text_eq_ci(cmd, L"addoptions")) {
        trim_in_place(cursor);
        unquote_in_place(cursor);
        if (!cursor || !cursor[0]) out_line(row, rows, cols, VT_DIM, L"usage: append TEXT");
        else if (append_override_cmdline(state, selected, cursor))
            out_line(row, rows, cols, VT_NORMAL, L"options appended for next boot");
        else
            out_line(row, rows, cols, VT_DIM, L"out of memory appending options");
    } else if (text_eq_ci(cmd, L"rescue")) {
        CHAR16 tmp[] = L"systemd.unit=rescue.target nomodeset";
        if (append_override_cmdline(state, selected, tmp))
            out_line(row, rows, cols, VT_NORMAL, L"rescue options appended for next boot");
        else
            out_line(row, rows, cols, VT_DIM, L"out of memory appending rescue options");
    } else if (text_eq_ci(cmd, L"single")) {
        CHAR16 tmp[] = L"single";
        if (append_override_cmdline(state, selected, tmp))
            out_line(row, rows, cols, VT_NORMAL, L"single-user option appended for next boot");
        else
            out_line(row, rows, cols, VT_DIM, L"out of memory appending single-user option");
    } else if (text_eq_ci(cmd, L"unset")) {
        CHAR16 *field = next_arg(&cursor);
        if (field && (text_eq_ci(field, L"options") || text_eq_ci(field, L"cmdline"))) {
            if (state->override_cmdline) {
                efi_free_pool(state->override_cmdline);
                state->override_cmdline = NULL;
            }
            out_line(row, rows, cols, VT_NORMAL, L"next boot options override cleared");
        } else if (field && text_eq_ci(field, L"kernel")) {
            set_override_path(&state->override_kernel_path, NULL);
            out_line(row, rows, cols, VT_NORMAL, L"next boot kernel override cleared");
        } else if (field && text_eq_ci(field, L"initrd")) {
            set_override_path(&state->override_initrd_path, NULL);
            state->override_initrd_set = 0;
            out_line(row, rows, cols, VT_NORMAL, L"next boot initrd override cleared");
        } else {
            out_line(row, rows, cols, VT_DIM, L"usage: unset options|kernel|initrd");
        }
    } else if (text_eq_ci(cmd, L"log") || text_eq_ci(cmd, L"logs")) {
        log_pager(state, failed, status, quiet_boot, cols, rows);
        shell_clear(rows, cols, selected, failed, status, quiet_boot);
        *row = quiet_boot ? 4 : 3;
    } else if (text_eq_ci(cmd, L"ls") || text_eq_ci(cmd, L"dir")) {
        CHAR16 *path = next_arg(&cursor);
        list_dir(row, rows, cols, path);
    } else if (text_eq_ci(cmd, L"cat") || text_eq_ci(cmd, L"type")) {
        CHAR16 *path = next_arg(&cursor);
        print_file_text(row, rows, cols, path);
    } else if (text_eq_ci(cmd, L"boot") || text_eq_ci(cmd, L"retry")) {
        state->action = VISOR_ACTION_RETRY;
        return 1;
    } else if (text_eq_ci(cmd, L"menu") || text_eq_ci(cmd, L"exit")) {
        state->action = VISOR_ACTION_MENU;
        return 1;
    } else if (text_eq_ci(cmd, L"reboot") || text_eq_ci(cmd, L"reset")) {
        state->action = VISOR_ACTION_REBOOT;
        return 1;
    } else if (text_eq_ci(cmd, L"shutdown") || text_eq_ci(cmd, L"poweroff")) {
        state->action = VISOR_ACTION_SHUTDOWN;
        return 1;
    } else if (text_eq_ci(cmd, L"firmware") || text_eq_ci(cmd, L"fwsetup")) {
        state->action = VISOR_ACTION_FIRMWARE;
        return 1;
    } else if (text_prefix_ci(cmd, L"linux") || text_prefix_ci(cmd, L"chainloader") ||
               text_eq_ci(cmd, L"initrd") || text_eq_ci(cmd, L"options")) {
        out_line(row, rows, cols, VT_DIM,
                 L"Use 'set kernel PATH', 'set initrd PATH', or 'set options TEXT', then 'boot'.");
    } else {
        out_line(row, rows, cols, VT_DIM, L"unknown command; type 'help'");
    }
    return 0;
}

int text_recovery_run(gui_state_t *state, boot_entry_t *failed,
                      EFI_STATUS status, int quiet_boot) {
    EFI_INPUT_KEY key;

    efi_log(L"recovery: entering failure recovery console");
    efi_log_close();
    visor_quiet = 0;
    efi_log_set_console(0);

    UINTN need_w = 96;
    UINTN need_h = 28;
    ensure_text_mode(need_w, need_h, 1);
    ST->ConOut->EnableCursor(ST->ConOut, FALSE);

    UINTN cols = 80, rows = 25;
    query_text_geometry(&cols, &rows);

    boot_entry_t *selected = failed;
    if (state && state->entry_count) {
        UINTN idx = 0;
        for (boot_entry_t *e = state->entries; e; e = e->next, idx++) {
            if (e == failed) { state->selected = idx; selected = e; break; }
        }
        if (!selected) selected = entry_at(state, state->selected);
    }

    shell_clear(rows, cols, selected, failed, status, quiet_boot);
    UINTN row = quiet_boot ? 4 : 3;
    print_help(&row, rows, cols);

    CHAR16 cmd[512];
    UINTN len = 0;
    UINTN cursor = 0;
    int prompt_dirty = 1;
    cmd[0] = 0;
    ST->ConOut->EnableCursor(ST->ConOut, TRUE);

    for (;;) {
        if (row >= rows - 2) {
            shell_clear(rows, cols, selected, failed, status, quiet_boot);
            row = quiet_boot ? 4 : 3;
            prompt_dirty = 1;
        }
        if (prompt_dirty) {
            shell_status(rows, cols, L"Enter runs command. Esc/menu exits to menu. Pg/log opens boot log.");
            shell_prompt(rows, cols, cmd, cursor);
            prompt_dirty = 0;
        }
        EFI_STATUS ks = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_ERROR(ks)) { efi_sleep(30); continue; }

        CHAR16 uc = key.UnicodeChar;
        CHAR16 upper = uc;
        if (upper >= L'a' && upper <= L'z') upper = (CHAR16)(upper - 32);

        if (uc == 0x0D) {
            cmd[len] = 0;
            if (execute_recovery_command(state, &selected, cmd, &row,
                                         rows, cols, status, failed, quiet_boot))
                break;
            len = cursor = 0;
            cmd[0] = 0;
            prompt_dirty = 1;
            continue;
        }
        if (uc == 0x1B || (uc == 0 && key.ScanCode == 0x17)) {
            state->action = VISOR_ACTION_MENU;
            break;
        }
        if (uc == 0 && key.ScanCode == 0x09) {
            log_pager(state, failed, status, quiet_boot, cols, rows);
            shell_clear(rows, cols, selected, failed, status, quiet_boot);
            row = quiet_boot ? 4 : 3;
            prompt_dirty = 1;
            continue;
        }
        if (uc == 0x08) {
            if (cursor > 0) {
                for (UINTN i = cursor - 1; i + 1 < len; i++) cmd[i] = cmd[i + 1];
                len--;
                cursor--;
                cmd[len] = 0;
                prompt_dirty = 1;
            }
            continue;
        }
        if (key.UnicodeChar == 0x00) {
            if (key.ScanCode == 0x04 && cursor > 0) { cursor--; prompt_dirty = 1; }
            else if (key.ScanCode == 0x03 && cursor < len) { cursor++; prompt_dirty = 1; }
            else if (key.ScanCode == 0x05) { cursor = 0; prompt_dirty = 1; }
            else if (key.ScanCode == 0x06) { cursor = len; prompt_dirty = 1; }
            continue;
        }
        if (uc >= 0x20 && len < 511) {
            for (UINTN i = len; i > cursor; i--) cmd[i] = cmd[i - 1];
            cmd[cursor++] = uc;
            len++;
            cmd[len] = 0;
            prompt_dirty = 1;
        }
    }

    ST->ConOut->SetAttribute(ST->ConOut, VT_NORMAL);
    ST->ConOut->EnableCursor(ST->ConOut, FALSE);
    ST->ConOut->ClearScreen(ST->ConOut);
    return state->action;
}
