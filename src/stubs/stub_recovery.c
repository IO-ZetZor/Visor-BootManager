/* stub_recovery.c - stub when the rescue console is compiled out */

#include "text_menu.h"
#include "config_internal.h"

void add_recovery_entries(config_t *config) {
    (void)config;
}

int text_recovery_run(gui_state_t *state, boot_entry_t *failed, EFI_STATUS status, int quiet_boot) {
    (void)state; (void)failed; (void)status; (void)quiet_boot;
    return 0;
}
