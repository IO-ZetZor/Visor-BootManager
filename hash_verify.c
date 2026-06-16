#include "hash_verify.h"
#include "sha256.h"
#include "efi_helpers.h"
#include <efi.h>
#include <efilib.h>

static void to_hex(const UINT8 *h, CHAR16 *out) {
    static const CHAR16 d[] = L"0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = d[(h[i] >> 4) & 0xF];
        out[i * 2 + 1] = d[h[i] & 0xF];
    }
    out[64] = 0;
}

int visor_hash_ok(boot_entry_t *entry, const void *data, UINTN size) {
    if (!entry->has_sha256) return 1;

    UINT8 got[32];
    sha256((const UINT8 *)data, size, got);

    int mismatch = 0;
    for (int i = 0; i < 32; i++) mismatch |= got[i] ^ entry->sha256[i];

    if (!mismatch) {
        efi_log(L"hash: sha256 pin verified OK");
        return 1;
    }

    CHAR16 hx[65];
    efi_log(L"ERROR: sha256 pin mismatch - refusing to boot");
    to_hex(got, hx);            efi_log(L"hash: computed:"); efi_log(hx);
    to_hex(entry->sha256, hx);  efi_log(L"hash: expected:"); efi_log(hx);
    efi_print(L"SECURITY: kernel hash mismatch, refusing to boot\r\n");
    return 0;
}

int visor_hash_ok_path(boot_entry_t *entry, CHAR16 *path) {
    if (!entry->has_sha256) return 1;
    if (!path) {
        efi_log(L"ERROR: sha256 pin set but no path to verify - refusing to boot");
        return 0;
    }

    efi_file_buffer_t *buf = efi_load_file(path);
    if (!buf) {
        efi_log(L"ERROR: sha256 pin set but file unreadable - refusing to boot");
        return 0;
    }

    int ok = visor_hash_ok(entry, buf->data, buf->size);
    efi_free_pool(buf->data);
    efi_free_pool(buf);
    return ok;
}
