/* text_menu_internal.h - shared internals of the src/text/text_menu.c translation units. */
#ifndef TEXT_MENU_INTERNAL_H
#define TEXT_MENU_INTERNAL_H

#include <efi.h>
#include <efilib.h>
#include "text_menu.h"
#include "efi_helpers.h"
#include "gpt.h"
#include "gpt_disk.h"
#include "efi_selfheal.h"

extern EFI_SYSTEM_TABLE *ST;
extern EFI_BOOT_SERVICES *BS;

#define VT_ATTR(fg, bg) ((fg) | ((bg) << 4))
#define VT_NORMAL  VT_ATTR(0x0F, 0x00)
#define VT_TITLE   VT_ATTR(0x0F, 0x00)
#define VT_SEL     VT_ATTR(0x00, 0x07)
#define VT_DIM     VT_ATTR(0x08, 0x00)
#define VT_PATH    VT_ATTR(0x07, 0x00)
#define TEXT_LINE_MAX 512

static inline UINTN text_row_width(UINTN cols) {
    UINTN w = (cols > 0) ? cols - 1 : 0;
    if (w > TEXT_LINE_MAX - 2) w = TEXT_LINE_MAX - 2;
    return w;
}
void left_line(UINTN row, UINTN cols, UINTN attr, const CHAR16 *text);

#ifndef GUI_INTERNAL_H
static inline boot_entry_t* entry_at(gui_state_t *state, UINTN idx) {
    boot_entry_t *e = state->entries;
    for (UINTN i = 0; i < idx && e; i++) e = e->next;
    return e;
}
#endif
void query_text_geometry(UINTN *cols, UINTN *rows);
void ensure_text_mode(UINTN need_w, UINTN need_h, int keep_current);

static inline UINTN text_len(CHAR16 *s) {
    UINTN n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static inline void out_line(UINTN *row, UINTN rows, UINTN cols, UINTN attr, CHAR16 *text) {
    if (!row || *row >= rows - 1) return;
    left_line((*row)++, cols, attr, text ? text : L"");
}
void gpt_cmd_scan(UINTN *row, UINTN rows, UINTN cols);
void gpt_cmd_detail(UINTN *row, UINTN rows, UINTN cols, UINT32 media_id);
int gpt_cmd_repair(UINTN *row, UINTN rows, UINTN cols, UINT32 media_id);
void list_dir(UINTN *row, UINTN rows, UINTN cols, CHAR16 *path);
int browse_run(gui_state_t *state, UINTN rows, UINTN cols, CHAR16 *initial);

#endif
