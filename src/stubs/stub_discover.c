/* stub_discover.c - stub when automatic kernel discovery is compiled out */

#include "config_internal.h"

EFI_STATUS detect_entries(config_t *config) {
    (void)config;
    return EFI_NOT_FOUND;
}

int entry_takes_default_cmdline(CHAR16 *kernel_path, int type) {
    (void)kernel_path; (void)type;
    return 0;
}

EFI_FILE_PROTOCOL * root_from_handle(EFI_HANDLE h) {
    (void)h;
    return NULL;
}
