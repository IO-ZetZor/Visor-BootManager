#ifndef WINDOWS_BOOT_H
#define WINDOWS_BOOT_H

#include <efi.h>
#include "gui.h"

typedef struct {
    EFI_DEVICE_PATH *device_path;
    EFI_DEVICE_PATH *file_path;
    CHAR16 *description;
} windows_boot_entry_t;

EFI_STATUS windows_find_bootmgr(CHAR16 *partition_uuid,
                                EFI_DEVICE_PATH **bootmgr_path);

#endif
