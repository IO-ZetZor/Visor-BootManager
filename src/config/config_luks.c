/* config_luks.c - LUKS keyfile presets (feature: luks) */
#include "config_internal.h"

static CHAR16* luks_default_key_path(CHAR16 *key_path) {
    return (key_path && key_path[0]) ? key_path : L"/crypto_keyfile.bin";
}

static CHAR16* luks_make_key_cmdline(const CHAR16 *prefix, CHAR16 *key_path) {
    CHAR16 *path = luks_default_key_path(key_path);
    UINTN plen = 0, klen = 0;
    while (prefix[plen]) plen++;
    while (path[klen]) klen++;

    CHAR16 *out = efi_allocate_pool((plen + klen + 1) * sizeof(CHAR16));
    if (!out) return NULL;

    UINTN k = 0;
    for (UINTN i = 0; i < plen; i++) out[k++] = prefix[i];
    for (UINTN i = 0; i < klen; i++) out[k++] = path[i];
    out[k] = 0;
    return out;
}

CHAR16* luks_cmdline_from_preset(CHAR16 *preset, CHAR16 *key_path) {
    if (!preset || !preset[0]) return NULL;
    if (str_eq_ci(preset, L"mkinitcpio") || str_eq_ci(preset, L"arch"))
        return luks_make_key_cmdline(L"cryptkey=rootfs:", key_path);
    if (str_eq_ci(preset, L"dracut") || str_eq_ci(preset, L"systemd"))
        return luks_make_key_cmdline(L"rd.luks.key=", key_path);
    efi_log(L"WARN: unknown luks_preset; set luks_cmdline manually");
    return NULL;
}
