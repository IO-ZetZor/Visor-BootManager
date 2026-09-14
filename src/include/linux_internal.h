/* linux_internal.h - shared internals of the linux_boot.c translation units */
#ifndef LINUX_INTERNAL_H
#define LINUX_INTERNAL_H

#include "linux_boot.h"
#include "windows_boot.h"
#include "efi_helpers.h"
#include "hash_verify.h"
#include "crypto.h"
#include "path_compat.h"
#include "tcg2.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;
extern EFI_HANDLE IH;

#define LUKS_DEFAULT_KEY_PATH      L"/crypto_keyfile.bin"
#define CPIO_NEWC_HEADER_SIZE      110u
EFI_HANDLE initrd_register(void *data, UINTN size);
void initrd_unregister(EFI_HANDLE h);
typedef struct {
    EFI_HANDLE volume;
    EFI_FILE_PROTOCOL *root;
} entry_volume_t;

static inline int linux_power_of_two(UINT64 value) {
    return value && !(value & (value - 1));
}
#if defined(__x86_64__)
EFI_STATUS linux_raw_handover(boot_entry_t *entry, EFI_SYSTEM_TABLE *st,
                                     efi_file_buffer_t *kernel_buf,
                                     CHAR16 *boot_cmdline,
                                     EFI_STATUS *status_out);
#endif
void free_file_buffer_wipe(efi_file_buffer_t *buf);
void free_file_buffer_maybe_wipe(efi_file_buffer_t *buf, int sensitive);
void entry_volume_close(entry_volume_t *keep);
efi_file_buffer_t* load_entry_file(CHAR16 *path, CHAR16 *uuid,
                                          EFI_HANDLE volume,
                                          int encrypted, CHAR16 *password,
                                          CHAR16 **resolved_path,
                                          entry_volume_t *keep);

static inline UINTN strlen8(const CHAR8 *s) {
    UINTN len = 0;
    while (s[len]) len++;
    return len;
}

static inline UINTN pad4(UINTN n) {
    return (4u - (n & 3u)) & 3u;
}
void wipe_bytes(void *buf, UINTN size);
void clear_entry_password(boot_entry_t *entry);

static inline UINTN cpio_entry_size(UINTN name_len, UINTN file_size) {
    UINTN n = CPIO_NEWC_HEADER_SIZE + name_len + 1;
    n += pad4(n) + file_size;
    return n + pad4(n);
}

static inline int add_overflow_uintn(UINTN a, UINTN b, UINTN *out) {
    if (~(UINTN)0 - a < b) return 1;
    *out = a + b;
    return 0;
}
EFI_STATUS luks_build_keyfile_archive(boot_entry_t *entry,
                                             efi_file_buffer_t **out_buf);
EFI_STATUS luks_append_keyfile(boot_entry_t *entry, efi_file_buffer_t **buf_io);
EFI_STATUS luks_effective_cmdline(boot_entry_t *entry,
                                         CHAR16 **cmdline_out,
                                         int *owned_out);

#endif
