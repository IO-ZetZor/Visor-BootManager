/* stub_luks.c - stub when LUKS support is compiled out */

#include "config_internal.h"
#include "linux_boot.h"

/* Without the luks feature, entries that do not ask for luks=1 must boot
 * exactly as before: mirror the real implementation's early returns and only
 * refuse when a LUKS handoff was actually requested. */
EFI_STATUS luks_append_keyfile(boot_entry_t *entry, efi_file_buffer_t **buf_io) {
    (void)buf_io;
    if (!entry || !entry->luks) return EFI_SUCCESS;
    return EFI_UNSUPPORTED;
}

EFI_STATUS luks_build_keyfile_archive(boot_entry_t *entry, efi_file_buffer_t **out_buf) {
    (void)entry; (void)out_buf;
    return EFI_UNSUPPORTED;
}

CHAR16 * luks_cmdline_from_preset(CHAR16 *preset, CHAR16 *key_path) {
    (void)preset; (void)key_path;
    return NULL;
}

EFI_STATUS luks_effective_cmdline(boot_entry_t *entry, CHAR16 **cmdline_out, int *owned_out) {
    *cmdline_out = entry ? entry->cmdline : NULL;
    *owned_out = 0;
    if (entry && entry->luks) return EFI_UNSUPPORTED;
    return EFI_SUCCESS;
}
