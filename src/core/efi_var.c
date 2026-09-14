/* efi_var.c - EFI variables, Secure Boot state and the loader interface */
#include "efi_helpers_internal.h"

static EFI_GUID visor_var_guid = { 0xb9d4f5a2, 0x7c3e, 0x4f1a,
    { 0x9a, 0x6b, 0x2d, 0x8e, 0x1f, 0x44, 0x77, 0x10 } };

#define VISOR_VAR_ATTRS (EFI_VARIABLE_NON_VOLATILE | \
                         EFI_VARIABLE_BOOTSERVICE_ACCESS)

int efi_secure_boot_enabled(void) {
    UINT8 sb = 0;
    UINTN sz = sizeof(sb);
    UINT32 attr;
    EFI_STATUS s = RT->GetVariable(L"SecureBoot", &gEfiGlobalVariableGuid,
                                   &attr, &sz, &sb);
    return (!EFI_ERROR(s) && sb == 1) ? 1 : 0;
}

typedef struct {
    EFI_STATUS (EFIAPI *Verify)(void *buffer, UINT32 size);
    void *Hash;
    void *Context;
} shim_lock_protocol_t;

int efi_shim_verify(void *buf, UINTN size) {
    static EFI_GUID shim_guid = { 0x605dab50, 0xe046, 0x4300,
        { 0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23 } };
    shim_lock_protocol_t *shim = NULL;
    EFI_STATUS s = BS->LocateProtocol(&shim_guid, NULL, (void**)&shim);
    if (EFI_ERROR(s) || !shim || !shim->Verify) return -1;
    return EFI_ERROR(shim->Verify(buf, (UINT32)size)) ? 0 : 1;
}

CHAR16* efi_get_var_str(CHAR16 *name) {
    UINTN sz = 0;
    UINT32 attr;
    EFI_STATUS s = RT->GetVariable(name, &visor_var_guid, &attr, &sz, NULL);
    if (s != EFI_BUFFER_TOO_SMALL || sz == 0) return NULL;
    CHAR16 *buf = efi_allocate_pool(sz + sizeof(CHAR16));
    if (!buf) return NULL;
    s = RT->GetVariable(name, &visor_var_guid, &attr, &sz, buf);
    if (EFI_ERROR(s)) { efi_free_pool(buf); return NULL; }
    buf[sz / sizeof(CHAR16)] = 0;
    return buf;
}

void efi_set_var_str(CHAR16 *name, CHAR16 *val) {
    if (!val) return;
    UINTN len = 0;
    while (val[len]) len++;
    RT->SetVariable(name, &visor_var_guid, VISOR_VAR_ATTRS,
                    (len + 1) * sizeof(CHAR16), val);
}

int efi_get_var_u32(CHAR16 *name, UINT32 *out) {
    UINT32 v = 0;
    UINTN sz = sizeof(v);
    UINT32 attr;
    EFI_STATUS s = RT->GetVariable(name, &visor_var_guid, &attr, &sz, &v);
    if (EFI_ERROR(s) || sz != sizeof(v)) return 0;
    *out = v;
    return 1;
}

void efi_set_var_u32(CHAR16 *name, UINT32 val) {
    RT->SetVariable(name, &visor_var_guid, VISOR_VAR_ATTRS, sizeof(val), &val);
}

static EFI_GUID loader_var_guid = { 0x4a67b082, 0x0a4c, 0x41cf,
    { 0xb6, 0xc7, 0x44, 0x0b, 0x29, 0xbb, 0x8c, 0x4f } };

#define LOADER_VAR_VOLATILE (EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                             EFI_VARIABLE_RUNTIME_ACCESS)
#define LOADER_VAR_PERSIST  (EFI_VARIABLE_NON_VOLATILE | LOADER_VAR_VOLATILE)

CHAR16* efi_get_loader_var(CHAR16 *name) {
    UINTN sz = 0;
    UINT32 attr;
    EFI_STATUS s = RT->GetVariable(name, &loader_var_guid, &attr, &sz, NULL);
    if (s != EFI_BUFFER_TOO_SMALL || sz == 0) return NULL;
    CHAR16 *buf = efi_allocate_pool(sz + sizeof(CHAR16));
    if (!buf) return NULL;
    s = RT->GetVariable(name, &loader_var_guid, &attr, &sz, buf);
    if (EFI_ERROR(s)) { efi_free_pool(buf); return NULL; }
    buf[sz / sizeof(CHAR16)] = 0;
    return buf;
}

void efi_set_loader_var(CHAR16 *name, CHAR16 *val, int persist) {
    if (!val) return;
    UINTN len = 0;
    while (val[len]) len++;
    RT->SetVariable(name, &loader_var_guid,
                    persist ? LOADER_VAR_PERSIST : LOADER_VAR_VOLATILE,
                    (len + 1) * sizeof(CHAR16), val);
}

void efi_set_loader_var_raw(CHAR16 *name, void *data, UINTN size, int persist) {
    if (!data || !size) return;
    RT->SetVariable(name, &loader_var_guid,
                    persist ? LOADER_VAR_PERSIST : LOADER_VAR_VOLATILE,
                    size, data);
}

void efi_unset_loader_var(CHAR16 *name, int persist) {
    RT->SetVariable(name, &loader_var_guid,
                    persist ? LOADER_VAR_PERSIST : LOADER_VAR_VOLATILE,
                    0, NULL);
}

int efi_loader_var_exists(CHAR16 *name) {
    UINTN sz = 0;
    UINT32 attr;
    EFI_STATUS s = RT->GetVariable(name, &loader_var_guid, &attr, &sz, NULL);
    return (s == EFI_BUFFER_TOO_SMALL || s == EFI_SUCCESS) ? 1 : 0;
}

void efi_set_loader_var_u64(CHAR16 *name, UINT64 val) {
    UINT8 le[8];
    for (UINTN i = 0; i < 8; i++) le[i] = (UINT8)(val >> (i * 8));
    efi_set_loader_var_raw(name, le, sizeof(le), 0);
}

void efi_set_loader_var_usec(CHAR16 *name, UINT64 usec) {
    if (!usec) return;
    CHAR16 rev[24];
    UINTN r = 0;
    while (usec && r < 20) { rev[r++] = (CHAR16)('0' + (usec % 10)); usec /= 10; }
    CHAR16 buf[24];
    UINTN n = 0;
    while (r) buf[n++] = rev[--r];
    buf[n] = 0;
    efi_set_loader_var(name, buf, 0);
}

int efi_parse_loader_timeout(CHAR16 *s, INTN *out) {
    if (!s || !s[0]) return 0;
    if (efi_strcmp(s, L"menu-force") == 0)    { *out = -1; return 1; }
    if (efi_strcmp(s, L"menu-hidden") == 0)   { *out =  0; return 1; }
    if (efi_strcmp(s, L"menu-disabled") == 0) { *out =  0; return 1; }

    UINT64 v = 0;
    for (UINTN i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (UINT64)(s[i] - '0');
        if (v > 0xFFFF) { v = 0xFFFF; break; }
    }
    *out = (INTN)v;
    return 1;
}
