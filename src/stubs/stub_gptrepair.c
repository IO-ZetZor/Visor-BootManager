/* stub_gptrepair.c - stub when GPT repair is compiled out */

#include "gpt.h"
#include "gui_internal.h"
#include "text_menu_internal.h"

void gpt_cmd_detail(UINTN *row, UINTN rows, UINTN cols, UINT32 media_id) {
    (void)row; (void)rows; (void)cols; (void)media_id;
}

int gpt_cmd_repair(UINTN *row, UINTN rows, UINTN cols, UINT32 media_id) {
    (void)row; (void)rows; (void)cols; (void)media_id;
    return 0;
}

void gpt_cmd_scan(UINTN *row, UINTN rows, UINTN cols) {
    (void)row; (void)rows; (void)cols;
}

void gpt_warn_run(gui_state_t *state) {
    (void)state;
}
