/* stub_verify.c - stub when SHA-256 kernel pinning is compiled out */

#include "hash_verify.h"

int visor_hash_ok(boot_entry_t *entry, const void *data, UINTN size) {
    (void)data; (void)size;
    return !entry->has_sha256;
}
