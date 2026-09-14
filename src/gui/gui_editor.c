/* gui_editor.c - in-menu cmdline editor overlay (feature: editor) */
#include "gui_internal.h"

void draw_editor_overlay(gui_state_t *state) {
    UINTN W = state->screen_width, H = state->screen_height;
    fill_rect_alpha(state, 0, 0, (INTN)W, (INTN)H, COLOR_BLACK, 155);

    int mask = state->edit_secret && !state->edit_reveal;
    CHAR16 secret_buf[512];
    CHAR16 *shown = state->edit_buf;
    if (mask) {
        UINTN n = state->edit_len < 511 ? state->edit_len : 511;
        for (UINTN i = 0; i < n; i++) secret_buf[i] = L'*';
        secret_buf[n] = 0;
        shown = secret_buf;
    }

    card_t c;
    card_metrics(state, &c);

    const CHAR16 *title = state->edit_title ? state->edit_title
                        : state->edit_secret ? L"Password"
                                             : L"Boot options";
    const key_hint_t secret_hints[] = {
        { L"Enter", L"unlock" }, { L"F2", L"reveal" }, { L"Esc", L"cancel" }
    };
    const key_hint_t opts_hints[] = {
        { L"Enter", L"boot" }, { L"Esc", L"cancel" }
    };
    const key_hint_t *hints = state->edit_secret ? secret_hints : opts_hints;
    UINTN nhints = state->edit_secret ? 3 : 2;

    INTN bw = (INTN)(W * 8 / 10);
    INTN bh = c.pad + (INTN)c.title_px + 11
            + (INTN)c.title_px + 18 + 12
            + (state->edit_hint ? (INTN)c.small_px + 8 : 0)
            + card_hints_h(&c) + c.pad / 2;

    card_open(state, &c, bw, bh);
    card_title(state, &c, title, state->underline_color, COLOR_WHITE);

    CHAR16 upto[512];
    UINTN k = 0;
    for (; k < state->edit_cursor && k < 511; k++)
        upto[k] = mask ? L'*' : state->edit_buf[k];
    upto[k] = 0;
    card_field(state, &c, shown, text_width_px(upto, c.title_px), 1);

    if (state->edit_hint)
        card_line(state, &c, state->edit_hint, COLOR_GRAY, c.small_px, 190);

    card_hints(state, &c, hints, nhints);
}

void editor_enter(gui_state_t *state) {
    boot_entry_t *e = entry_at(state, state->selected);
    state->edit_secret = 0;
    state->edit_reveal = 0;
    state->edit_title = NULL;
    state->edit_hint = NULL;
    state->edit_len = 0;
    if (e && e->cmdline)
        while (e->cmdline[state->edit_len] && state->edit_len < 511) {
            state->edit_buf[state->edit_len] = e->cmdline[state->edit_len];
            state->edit_len++;
        }
    state->edit_buf[state->edit_len] = 0;
    state->edit_cursor = state->edit_len;
    state->editing = 1;
}

void prompt_enter(gui_state_t *state, CHAR16 *title, CHAR16 *hint) {
    state->edit_secret = 1;
    state->edit_reveal = 0;
    state->edit_title = title;
    state->edit_hint = hint;
    state->edit_len = 0;
    state->edit_cursor = 0;
    state->edit_buf[0] = 0;
    state->editing = 1;
}

int editor_key(gui_state_t *state, EFI_INPUT_KEY *key) {
    if (key->UnicodeChar == 0x0D) {
        state->edit_buf[state->edit_len] = 0;
        if (!state->edit_secret) {
            if (state->override_cmdline) efi_free_pool(state->override_cmdline);
            state->override_cmdline = efi_strdup(state->edit_buf);
        }
        state->editing = 0;
        return 1;
    }
    if (key->UnicodeChar == 0x1B || (key->UnicodeChar == 0x00 && key->ScanCode == 0x17)) {
        state->editing = 0;
        return -1;
    }
    if (key->UnicodeChar == 0x08) {
        if (state->edit_cursor > 0) {
            for (UINTN i = state->edit_cursor - 1; i + 1 < state->edit_len; i++)
                state->edit_buf[i] = state->edit_buf[i + 1];
            state->edit_len--;
            state->edit_cursor--;
            state->edit_buf[state->edit_len] = 0;
        }
        return 0;
    }
    if (key->UnicodeChar == 0x00) {
        if (key->ScanCode == 0x04 && state->edit_cursor > 0) state->edit_cursor--;
        else if (key->ScanCode == 0x03 && state->edit_cursor < state->edit_len) state->edit_cursor++;
        else if (key->ScanCode == 0x0C && state->edit_secret)
            state->edit_reveal = !state->edit_reveal;
        return 0;
    }
    CHAR16 c = key->UnicodeChar;
    if (c >= 0x20 && state->edit_len < 510) {
        for (UINTN i = state->edit_len; i > state->edit_cursor; i--)
            state->edit_buf[i] = state->edit_buf[i - 1];
        state->edit_buf[state->edit_cursor] = c;
        state->edit_len++;
        state->edit_cursor++;
        state->edit_buf[state->edit_len] = 0;
    }
    return 0;
}
