/* text_browse.c - file browser in the recovery shell (feature: filebrowse) */
#include "text_menu_internal.h"

void list_dir(UINTN *row, UINTN rows, UINTN cols, CHAR16 *path) {
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

static void browse_clamp_scroll(fb_t *s, UINTN win) {
    if (s->cursor < s->scroll) s->scroll = s->cursor;
    if (win > 0 && s->cursor >= s->scroll + win)
        s->scroll = s->cursor - win + 1;
}

int browse_run(gui_state_t *state, UINTN rows, UINTN cols, CHAR16 *initial) {
    fb_t s;
    if (!fb_init(&s)) {
        out_line(NULL, rows, cols, VT_DIM, L"no readable EFI filesystems");
        return -1;
    }
    if (initial && initial[0]) fb_set_path(&s, initial);
    fb_list(&s);

    CHAR16 status[96];
    status[0] = 0;
    int dirty = 1;
    EFI_INPUT_KEY key;
    ST->ConOut->EnableCursor(ST->ConOut, FALSE);

    for (;;) {
        if (dirty) {
            ST->ConOut->ClearScreen(ST->ConOut);
            UINTN r = 0;
            CHAR16 head[FB_PATH_MAX + 32];
            SPrint(head, sizeof(head), L"browse   [%s]   %s",
                   s.vol_count > 0 ? s.vols[s.vol_cur].label : L"?", s.path);
            out_line(&r, rows, cols, VT_TITLE, head);
            out_line(&r, rows, cols, VT_DIM,
                     L"Enter open/boot   Backspace/Left up   Tab/Right volume   Esc quit");
            if (status[0]) out_line(&r, rows, cols, VT_NORMAL, status);

            UINTN win = (r + 1 < rows) ? rows - r - 1 : 1;
            browse_clamp_scroll(&s, win);
            UINTN shown = 0;
            for (UINTN i = 0; i < win && s.scroll + i < s.entry_count; i++) {
                fb_entry_t *e = &s.entries[s.scroll + i];
                int sel = (s.scroll + i == s.cursor);
                CHAR16 buf[FB_NAME_MAX + 32];
                if (e->is_dir) {
                    SPrint(buf, sizeof(buf), L"%s%s\\", sel ? L"> " : L"  ", e->name);
                } else {
                    CHAR16 sz[24];
                    fb_format_size(e->size, sz, sizeof(sz));
                    SPrint(buf, sizeof(buf), L"%s%s  %s", sel ? L"> " : L"  ", e->name, sz);
                }
                out_line(&r, rows, cols,
                         sel ? VT_SEL : (e->is_dir ? VT_PATH : VT_NORMAL), buf);
                shown++;
            }
            if (shown == 0)
                out_line(&r, rows, cols, VT_DIM, L"(empty)");
            dirty = 0;
        }

        EFI_STATUS ks = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_ERROR(ks)) { efi_sleep(30); continue; }

        if (key.UnicodeChar == 0x0D) {
            fb_entry_t *e = fb_cursor(&s);
            if (e && e->is_dir) {
                fb_enter(&s);
                status[0] = 0;
                dirty = 1;
            } else if (e) {
                int r = fb_boot_apply(&s, state);
                if (r == 1) { fb_free(&s); return VISOR_ACTION_RETRY; }
                if (r == 2) {
                    SPrint(status, sizeof(status),
                           L"initrd override set - exit and type 'boot' to use it");
                } else {
                    SPrint(status, sizeof(status), L"cannot boot this file");
                }
                dirty = 1;
            }
        } else if (key.UnicodeChar == 0x1B ||
                   (key.UnicodeChar == 0 && key.ScanCode == 0x17)) {
            break;
        } else if (key.UnicodeChar == 0x08 ||
                   (key.UnicodeChar == 0 && key.ScanCode == 0x04)) {
            if (fb_up(&s)) { status[0] = 0; dirty = 1; }
        } else if (key.UnicodeChar == 0x09 ||
                   (key.UnicodeChar == 0 && key.ScanCode == 0x03)) {
            fb_switch_volume(&s, 1);
            status[0] = 0;
            dirty = 1;
        } else if (key.UnicodeChar == 0 && key.ScanCode == 0x01) {
            fb_move(&s, -1); dirty = 1;
        } else if (key.UnicodeChar == 0 && key.ScanCode == 0x02) {
            fb_move(&s, 1); dirty = 1;
        } else if (key.UnicodeChar == 0 && key.ScanCode == 0x0B) {
            fb_move(&s, -6); dirty = 1;
        } else if (key.UnicodeChar == 0 && key.ScanCode == 0x0C) {
            fb_move(&s, 6); dirty = 1;
        } else if (key.UnicodeChar == 0x08) {
            if (fb_up(&s)) { status[0] = 0; dirty = 1; }
        } else {
            CHAR16 u = key.UnicodeChar;
            if (u >= 'a' && u <= 'z') u -= 32;
            if (u == 'Q') break;
        }
    }

    fb_free(&s);
    ST->ConOut->EnableCursor(ST->ConOut, TRUE);
    return -1;
}
