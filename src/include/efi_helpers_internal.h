/* efi_helpers_internal.h - shared internals of the efi_helpers.c units */
#ifndef EFI_HELPERS_INTERNAL_H
#define EFI_HELPERS_INTERNAL_H

#include "efi_helpers.h"
#include "arch.h"
#include <efi.h>
#include <efilib.h>
#include <stdarg.h>

extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;
extern EFI_HANDLE IH;

EFI_HANDLE boot_device_handle(void);
extern UINTN g_volume_opens;
EFI_FILE_PROTOCOL *open_root_on_handle(EFI_HANDLE h);

static inline int hex_digit(CHAR16 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline int parse_hex_byte(CHAR16 *s, UINT8 *out) {
    int hi = hex_digit(s[0]);
    int lo = hex_digit(s[1]);
    if (hi < 0 || lo < 0) return 0;
    *out = (UINT8)((hi << 4) | lo);
    return 1;
}
#define NORM_PATH_MAX 512
CHAR16* collapse_backslashes(CHAR16 *path, CHAR16 *buf, UINTN cap);

static inline int dp_node_is_harddrive(EFI_DEVICE_PATH *n) {
    return DevicePathType(n) == MEDIA_DEVICE_PATH &&
           DevicePathSubType(n) == MEDIA_HARDDRIVE_DP;
}
extern int g_deferred_count;
extern int g_deferred_started;
extern int g_deferred_lazy;

static inline int has_efi_suffix(CHAR16 *name) {
    UINTN n = 0;
    while (name[n]) n++;
    if (n < 4) return 0;
    CHAR16 c[4];
    for (int i = 0; i < 4; i++) {
        CHAR16 ch = name[n - 4 + i];
        c[i] = (ch >= 'A' && ch <= 'Z') ? (CHAR16)(ch + 32) : ch;
    }
    return c[0] == '.' && c[1] == 'e' && c[2] == 'f' && c[3] == 'i';
}
int fs_probe_in_progress(void);

#ifndef SLOW_STEP_MIN_MS
#define SLOW_STEP_MIN_MS 250
#endif
void efi_log_slow(CHAR16 *what, UINT64 t0_us);

#endif
