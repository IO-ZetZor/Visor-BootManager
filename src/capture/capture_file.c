/* capture_file.c - ESP file helpers for saving captures */
#include "capture_internal.h"

#ifndef CAP_NO_IO

EFI_STATUS cap_ensure_dir(const CHAR16 *path) {
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return EFI_DEVICE_ERROR;
    EFI_FILE_PROTOCOL *d = NULL;
    EFI_STATUS st = root->Open(root, &d, (CHAR16*)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !d) {
        st = root->Open(root, &d, (CHAR16*)path,
                        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                        EFI_FILE_DIRECTORY);
    }
    if (d) d->Close(d);
    root->Close(root);
    return EFI_ERROR(st) ? st : EFI_SUCCESS;
}

EFI_STATUS cap_save_file(const CHAR16 *path, const UINT8 *data, UINTN size) {
    if (!path || (!data && size)) return EFI_INVALID_PARAMETER;
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return EFI_DEVICE_ERROR;
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = root->Open(root, &f, (CHAR16*)path,
                               EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                               0);
    if (EFI_ERROR(st) || !f) {
        root->Close(root);
        return EFI_DEVICE_ERROR;
    }
    UINTN w = size ? size : 0;
    st = f->Write(f, &w, (void*)data);
    if (!EFI_ERROR(st)) st = f->Flush(f);
    if (EFI_ERROR(st)) st = EFI_DEVICE_ERROR;
    f->Close(f);
    root->Close(root);
    return st;
}

void cap_timestamp_name(CHAR16 *out, UINTN cap, const CHAR16 *base,
                        const CHAR16 *ext) {
    UINTN ho = 0, mi = 0, se = 0;
    EFI_TIME t;
    if (!EFI_ERROR(RT->GetTime(&t, NULL))) {
        ho = t.Hour; mi = t.Minute; se = t.Second;
    }
    if (ho > 99) ho = 0;
    if (mi > 99) mi = 0;
    if (se > 99) se = 0;
    UINTN n = 0;
    for (UINTN i = 0; base && base[i] && n + 1 < cap; i++) out[n++] = base[i];
    if (n + 1 < cap) out[n++] = L'_';
    CHAR16 digs[6];
    digs[0] = (CHAR16)(L'0' + ho / 10); digs[1] = (CHAR16)(L'0' + ho % 10);
    digs[2] = (CHAR16)(L'0' + mi / 10); digs[3] = (CHAR16)(L'0' + mi % 10);
    digs[4] = (CHAR16)(L'0' + se / 10); digs[5] = (CHAR16)(L'0' + se % 10);
    for (UINTN i = 0; i < 6 && n + 1 < cap; i++) out[n++] = digs[i];
    if (ext) for (UINTN i = 0; ext[i] && n + 1 < cap; i++) out[n++] = ext[i];
    out[n < cap ? n : cap - 1] = 0;
}

#endif
