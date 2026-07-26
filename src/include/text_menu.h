#ifndef TEXT_MENU_H
#define TEXT_MENU_H

#include "gui.h"

boot_entry_t* text_menu_run(gui_state_t *state);

int text_recovery_run(gui_state_t *state, boot_entry_t *failed,
                      EFI_STATUS status, int quiet_boot);

#endif
