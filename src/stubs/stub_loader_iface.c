/* stub_loader_iface.c - stub when the LoaderXxx EFI variable interface is compiled out */

#include "loader_iface.h"

UINTN loader_apply_overrides(config_t *config, boot_entry_t *entries, UINTN entry_count, INTN *timeout_io) {
    (void)config; (void)entries; (void)entry_count; (void)timeout_io;
    return 0;
}

void loader_export_common(config_t *config) {
    (void)config;
}

void loader_export_entries(boot_entry_t *entries) {
    (void)entries;
}

void loader_mark_init(void) { }

void loader_mark_menu(void) { }

void loader_mark_selected(boot_entry_t *entry) {
    (void)entry;
}
