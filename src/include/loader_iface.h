#ifndef LOADER_IFACE_H
#define LOADER_IFACE_H

#include <efi.h>
#include "gui.h"
#include "config.h"
#include "version.h"

#define VISOR_LOADER_INFO L"Visor " VISOR_VERSION

#define LOADER_FEATURE_CONFIG_TIMEOUT          (1ULL << 0)
#define LOADER_FEATURE_CONFIG_TIMEOUT_ONESHOT  (1ULL << 1)
#define LOADER_FEATURE_ENTRY_DEFAULT           (1ULL << 2)
#define LOADER_FEATURE_ENTRY_ONESHOT           (1ULL << 3)
#define LOADER_FEATURE_BOOT_COUNTING           (1ULL << 4)
#define LOADER_FEATURE_XBOOTLDR                (1ULL << 5)
#define LOADER_FEATURE_LOAD_DRIVER             (1ULL << 7)
#define LOADER_FEATURE_MENU_DISABLED           (1ULL << 13)
#define LOADER_FEATURE_TPM2_ACTIVE_PCR_BANKS   (1ULL << 18)

void loader_mark_init(void);

void loader_export_common(config_t *config);

void loader_export_entries(boot_entry_t *entries);

CHAR16* loader_entry_id(boot_entry_t *entry);

UINTN loader_apply_overrides(config_t *config, boot_entry_t *entries,
                             UINTN entry_count, INTN *timeout_io);

void loader_mark_selected(boot_entry_t *entry);

void loader_mark_menu(void);

#endif
