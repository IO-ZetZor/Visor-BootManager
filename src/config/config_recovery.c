/* config_recovery.c - generated recovery entries (feature: recovery) */
#include "config_internal.h"

#define MAX_RECOVERY_ORIG 32

void add_recovery_entries(config_t *config) {
    static const CHAR16 *suffix = L"systemd.unit=rescue.target nomodeset";

    boot_entry_t *orig[MAX_RECOVERY_ORIG];
    int n = 0;
    for (boot_entry_t *e = config->entries; e && n < MAX_RECOVERY_ORIG; e = e->next)
        orig[n++] = e;

    for (int i = 0; i < n; i++) {
        boot_entry_t *o = orig[i];
        if (o->type == 1 || !o->kernel_path) continue;

        UINTN nlen = 0; while (o->name[nlen]) nlen++;
        CHAR16 *rname = efi_allocate_pool((nlen + 16) * sizeof(CHAR16));
        if (!rname) continue;
        SPrint(rname, (nlen + 16) * sizeof(CHAR16), L"%s (recovery)", o->name);

        UINTN olen = 0; if (o->cmdline) while (o->cmdline[olen]) olen++;
        UINTN slen = 0; while (suffix[slen]) slen++;
        CHAR16 *rcmd = efi_allocate_pool((olen + slen + 2) * sizeof(CHAR16));
        if (!rcmd) { efi_free_pool(rname); continue; }
        if (olen) SPrint(rcmd, (olen + slen + 2) * sizeof(CHAR16), L"%s %s", o->cmdline, suffix);
        else      SPrint(rcmd, (olen + slen + 2) * sizeof(CHAR16), L"%s", suffix);

        CHAR16 *ricon = o->icon_path ? efi_strdup(o->icon_path) : NULL;
        CHAR16 *rkernel = efi_strdup(o->kernel_path);
        CHAR16 *rinitrd = o->initrd_path ? efi_strdup(o->initrd_path) : NULL;
        CHAR16 *ruuid = o->uuid ? efi_strdup(o->uuid) : NULL;
        if (!rkernel || (o->icon_path && !ricon) ||
            (o->initrd_path && !rinitrd) || (o->uuid && !ruuid)) {
            free_char16(&rname);
            free_char16(&ricon);
            free_char16(&rkernel);
            free_char16(&rinitrd);
            free_char16(&rcmd);
            free_char16(&ruuid);
            continue;
        }

        boot_entry_t *r = config_add_entry(config, rname, ricon, rkernel,
            rinitrd, rcmd, ruuid, o->type, o->encrypted, o->initrd_encrypted);
        if (r) {
            r->luks = o->luks;
            r->luks_key_path = o->luks_key_path ? efi_strdup(o->luks_key_path) : NULL;
            r->luks_cmdline = o->luks_cmdline ? efi_strdup(o->luks_cmdline) : NULL;
            r->luks_preset = o->luks_preset ? efi_strdup(o->luks_preset) : NULL;
            r->color = o->color;
            r->has_color = o->has_color;
            r->icon_size = o->icon_size;
            if (o->has_sha256) {
                for (int k = 0; k < 32; k++) r->sha256[k] = o->sha256[k];
                r->has_sha256 = 1;
            }
        } else {
            free_char16(&rname);
            free_char16(&ricon);
            free_char16(&rkernel);
            free_char16(&rinitrd);
            free_char16(&rcmd);
            free_char16(&ruuid);
        }
    }
}
