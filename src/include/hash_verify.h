#ifndef HASH_VERIFY_H
#define HASH_VERIFY_H

#include <efi.h>
#include "gui.h"

int visor_hash_ok(boot_entry_t *entry, const void *data, UINTN size);

#endif
