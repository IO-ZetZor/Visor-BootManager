/* linux_file.c - kernel/initrd file loading and sensitive-buffer wiping */
#include "linux_internal.h"

static void free_file_buffer(efi_file_buffer_t *buf) {
    if (!buf) return;
    if (buf->data) efi_free_pool(buf->data);
    efi_free_pool(buf);
}

void free_file_buffer_wipe(efi_file_buffer_t *buf) {
    if (!buf) return;
    if (buf->data) {
        volatile UINT8 *p = (volatile UINT8*)buf->data;
        UINTN n = buf->size;
        while (n--) *p++ = 0;
        efi_free_pool(buf->data);
    }
    efi_free_pool(buf);
}

void free_file_buffer_maybe_wipe(efi_file_buffer_t *buf, int sensitive) {
    if (sensitive) free_file_buffer_wipe(buf);
    else free_file_buffer(buf);
}

void entry_volume_close(entry_volume_t *keep) {
    if (!keep || !keep->root) return;
    keep->root->Close(keep->root);
    keep->root = NULL;
    keep->volume = NULL;
}

static void entry_volume_keep(entry_volume_t *keep, EFI_HANDLE volume,
                              EFI_FILE_PROTOCOL *root) {
    if (!root) return;
    if (!keep || keep->root) {
        root->Close(root);
        return;
    }
    keep->volume = volume;
    keep->root = root;
}

static efi_file_buffer_t* load_from_open_root(EFI_FILE_PROTOCOL *root, CHAR16 *path,
                                              CHAR16 *root_path,
                                              CHAR16 **resolved_path) {
    int opened = 0;
    efi_file_buffer_t *buf = efi_load_file_from_root(root, path, &opened);
    if (buf || opened || !root_path) return buf;

    buf = efi_load_file_from_root(root, root_path, NULL);
    if (buf) {
        if (resolved_path) *resolved_path = root_path;
        efi_log(L"boot: resolved path after removing host /boot mount prefix");
        efi_log(root_path);
    }
    return buf;
}

efi_file_buffer_t* load_entry_file(CHAR16 *path, CHAR16 *uuid,
                                          EFI_HANDLE volume,
                                          int encrypted, CHAR16 *password,
                                          CHAR16 **resolved_path,
                                          entry_volume_t *keep) {

    if (resolved_path) *resolved_path = path;
    CHAR16 *root_path = visor_path_without_boot_mount(path);

    efi_file_buffer_t *buf = NULL;

    if (keep && keep->root) {
        buf = load_from_open_root(keep->root, path, root_path, resolved_path);
        if (buf) {
            efi_log(L"boot: file read from the volume already open for this entry");
            goto have_buffer;
        }
        efi_log(L"WARN: file not on the volume the kernel came from - searching by path");
    }

    if (volume) {
        EFI_FILE_PROTOCOL *root = efi_open_volume_root(volume);
        if (root) {
            buf = load_from_open_root(root, path, root_path, resolved_path);
            if (buf) {
                efi_log(L"boot: file loaded from the volume this entry was detected on");
                entry_volume_keep(keep, volume, root);
                goto have_buffer;
            }
            root->Close(root);
        }
        efi_log(L"WARN: file not found on the entry's own volume - searching by path");
    }

    for (int pass = 0; pass < 2 && !buf; pass++) {
        CHAR16 *want = (pass == 0) ? uuid : NULL;
        if (pass == 1) {
            if (!uuid || !uuid[0]) break;
            efi_log(L"WARN: file not found on partition uuid= - searching all volumes");
        }

        EFI_HANDLE found_vol = NULL;
        EFI_FILE_PROTOCOL *found_root = NULL;
        int opened = 0;
        buf = efi_load_file_keep_volume(path, want, &found_vol, &found_root, &opened);
        if (!buf && !opened && root_path) {
            buf = efi_load_file_keep_volume(root_path, want, &found_vol, &found_root, NULL);
            if (buf) {
                if (resolved_path) *resolved_path = root_path;
                efi_log(L"boot: resolved path after removing host /boot mount prefix");
                efi_log(root_path);
            }
        }
        if (buf) entry_volume_keep(keep, found_vol, found_root);
        else if (found_root) found_root->Close(found_root);
    }

have_buffer:
    if (!buf) return NULL;
    if (!encrypted) return buf;

    void *plain = NULL;
    UINTN plain_size = 0;
    EFI_STATUS s = visor_decrypt_buffer(buf->data, buf->size, password,
                                        &plain, &plain_size);
    free_file_buffer(buf);
    if (EFI_ERROR(s)) {
        if (s == EFI_SECURITY_VIOLATION) {
            efi_log(L"ERROR: encrypted file password/integrity check failed");
            efi_print(L"Encrypted file could not be decrypted\r\n");
        } else {
            efi_log(L"ERROR: encrypted file container is invalid or unsupported");
            efi_print(L"Encrypted file is invalid\r\n");
        }
        return NULL;
    }

    efi_file_buffer_t *out = efi_allocate_pool(sizeof(efi_file_buffer_t));
    if (!out) {
        volatile UINT8 *p = (volatile UINT8*)plain;
        UINTN n = plain_size;
        while (n--) *p++ = 0;
        efi_free_pool(plain);
        return NULL;
    }
    out->data = plain;
    out->size = plain_size;
    efi_log(L"crypto: encrypted file decrypted");
    return out;
}

void wipe_bytes(void *buf, UINTN size) {
    if (!buf) return;
    volatile UINT8 *p = (volatile UINT8*)buf;
    while (size--) *p++ = 0;
}

void clear_entry_password(boot_entry_t *entry) {
    if (!entry || !entry->decrypt_password) return;
    volatile CHAR16 *p = (volatile CHAR16*)entry->decrypt_password;
    while (*p) *p++ = 0;
    efi_free_pool(entry->decrypt_password);
    entry->decrypt_password = NULL;
}
