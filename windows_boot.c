
#include "windows_boot.h"
#include "efi_helpers.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;

EFI_STATUS windows_find_bootmgr(CHAR16 *partition_uuid, EFI_DEVICE_PATH **bootmgr_path) {
    CHAR16 *paths[] = {
        L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
        L"\\EFI\\BOOT\\bootmgfw.efi",
        L"\\EFI\\BOOT\\BOOTX64.EFI",
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        *bootmgr_path = efi_file_device_path(paths[i], partition_uuid);
        if (*bootmgr_path) return EFI_SUCCESS;
    }

    return EFI_NOT_FOUND;
}
