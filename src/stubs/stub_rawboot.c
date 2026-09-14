/* stub_rawboot.c - stub for the bzImage handover path when compiled out */

#include "linux_internal.h"

#if defined(__x86_64__)
EFI_STATUS linux_raw_handover(boot_entry_t *entry, EFI_SYSTEM_TABLE *st,
                              efi_file_buffer_t *kernel_buf,
                              CHAR16 *boot_cmdline,
                              EFI_STATUS *status_out) {
    (void)entry; (void)st; (void)kernel_buf; (void)boot_cmdline;
    if (status_out) *status_out = EFI_UNSUPPORTED;
    efi_log(L"boot: raw kernel handover not compiled in");
    return EFI_UNSUPPORTED;
}
#endif
