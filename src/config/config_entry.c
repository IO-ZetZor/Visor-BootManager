/* config_entry.c - boot entry construction and teardown */
#include "config_internal.h"

void free_deployments(deployment_t *deps, UINTN count) {
    if (!deps) return;
    for (UINTN i = 0; i < count; i++) {
        if (deps[i].version)  efi_free_pool(deps[i].version);
        if (deps[i].kernel)   efi_free_pool(deps[i].kernel);
        if (deps[i].initrd)   efi_free_pool(deps[i].initrd);
        if (deps[i].cmdline)  efi_free_pool(deps[i].cmdline);
        if (deps[i].bls_path) efi_free_pool(deps[i].bls_path);
    }
    efi_free_pool(deps);
}

static void free_snapshots(snapshot_t *snaps, UINTN count);

void free_entry_contents(boot_entry_t *e) {
    if (!e) return;
    if (e->name)      efi_free_pool(e->name);
    if (e->icon_path) efi_free_pool(e->icon_path);

    if (e->deployments) {
        int cmdline_aliased = 0;
        for (UINTN i = 0; i < e->deploy_count; i++)
            if (e->cmdline == e->deployments[i].cmdline) cmdline_aliased = 1;
        if (!cmdline_aliased && e->cmdline) efi_free_pool(e->cmdline);
        free_deployments(e->deployments, e->deploy_count);
    } else {
        if (e->kernel_path) efi_free_pool(e->kernel_path);
        if (e->initrd_path) efi_free_pool(e->initrd_path);
        if (e->cmdline)     efi_free_pool(e->cmdline);
    }

    if (e->uuid)          efi_free_pool(e->uuid);
    if (e->snapshots)     free_snapshots(e->snapshots, e->snap_count);
    if (e->luks_key_path) efi_free_pool(e->luks_key_path);
    if (e->luks_cmdline)  efi_free_pool(e->luks_cmdline);
    if (e->luks_preset)   efi_free_pool(e->luks_preset);
    if (e->decrypt_password) {
        wipe16(e->decrypt_password);
        efi_free_pool(e->decrypt_password);
        e->decrypt_password = NULL;
    }
    if (e->icon) {
        if (e->icon->scaled) efi_free_pool(e->icon->scaled);
        if (e->icon->pixels) efi_free_pool(e->icon->pixels);
        efi_free_pool(e->icon);
        e->icon = NULL;
    }
}

static void free_snapshots(snapshot_t *snaps, UINTN count) {
    if (!snaps) return;
    for (UINTN i = 0; i < count; i++) {
        if (snaps[i].id)      efi_free_pool(snaps[i].id);
        if (snaps[i].date)    efi_free_pool(snaps[i].date);
        if (snaps[i].desc)    efi_free_pool(snaps[i].desc);
        if (snaps[i].kernel)  efi_free_pool(snaps[i].kernel);
        if (snaps[i].initrd)  efi_free_pool(snaps[i].initrd);
        if (snaps[i].cmdline) efi_free_pool(snaps[i].cmdline);
    }
    efi_free_pool(snaps);
}

EFI_STATUS parse_entry(config_t *config, CHAR16 **lines, UINTN *idx,
                              UINTN count, int win_hint) {
    CHAR16 *name = NULL;
    CHAR16 *icon_path = NULL;
    CHAR16 *kernel_path = NULL;
    CHAR16 *initrd_path = NULL;
    CHAR16 *cmdline = NULL;
    CHAR16 *uuid = NULL;
    int type = win_hint ? 1 : 0;
    color_t color; int has_color = 0; int color_role = -1;
    UINT8 sha256_buf[32]; int has_sha256 = 0;
    UINTN entry_icon_size = 0;
    int encrypted = 0, kernel_encrypted_set = 0, initrd_encrypted_set = 0;
    int kernel_encrypted = 0, initrd_encrypted = 0;
    int luks = 0;
    int luks_confirm = 0;
    int luks_verbose = 0;
    CHAR16 *luks_key_path = NULL;
    CHAR16 *luks_cmdline = NULL;
    CHAR16 *luks_preset = NULL;

    int braced = 0;
    for (CHAR16 *h = lines[*idx]; *h; h++)
        if (*h == '{') { braced = 1; break; }

    while (*idx < count) {
        CHAR16 *line = trim(lines[*idx]);

        if (line[0] == '}') break;
        if (line[0] == '\0') {
            if (!braced) break;
            (*idx)++;
            continue;
        }
        if (line[0] == '#') {
            (*idx)++;
            continue;
        }

        CHAR16 *eq = efi_strchr(line, '=');
        if (eq) {
            *eq = '\0';
            CHAR16 *key = trim(line);
            strip_inline_comment(eq + 1);
            CHAR16 *value = trim(eq + 1);

            if (efi_strcmp(key, L"name") == 0) {
                set_char16(&name, efi_strdup(value));
            } else if (efi_strcmp(key, L"icon") == 0) {
                set_char16(&icon_path, dup_path(value));
            } else if (efi_strcmp(key, L"kernel") == 0) {
                set_char16(&kernel_path, dup_path(value));
            } else if (efi_strcmp(key, L"initrd") == 0) {
                set_char16(&initrd_path, dup_path(value));
            } else if (efi_strcmp(key, L"cmdline") == 0 || efi_strcmp(key, L"options") == 0) {
                set_char16(&cmdline, efi_strdup(value));
            } else if (efi_strcmp(key, L"uuid") == 0) {
                set_char16(&uuid, efi_strdup(value));
            } else if (efi_strcmp(key, L"color") == 0) {
                int role = accent_role_from_str(value);
                if (role >= 0) {
                    color_role = role;
                    has_color = 0;
                } else {
                    has_color = parse_color(value, &color);
                    if (has_color) color_role = -1;
                    else efi_log(L"WARN: invalid entry color= (an accent role or #RRGGBB)");
                }
            } else if (efi_strcmp(key, L"sha256") == 0) {
                has_sha256 = parse_sha256(value, sha256_buf);
                if (!has_sha256) efi_log(L"WARN: invalid sha256= (expect 64 hex chars)");
            } else if (efi_strcmp(key, L"icon_size") == 0) {
                entry_icon_size = parse_uint(value);
            } else if (efi_strcmp(key, L"encrypted") == 0) {
                encrypted = (*value == '1' || *value == 't' || *value == 'y');
            } else if (efi_strcmp(key, L"kernel_encrypted") == 0) {
                kernel_encrypted = (*value == '1' || *value == 't' || *value == 'y');
                kernel_encrypted_set = 1;
            } else if (efi_strcmp(key, L"initrd_encrypted") == 0) {
                initrd_encrypted = (*value == '1' || *value == 't' || *value == 'y');
                initrd_encrypted_set = 1;
            } else if (efi_strcmp(key, L"luks") == 0 ||
                       efi_strcmp(key, L"luks_password") == 0) {
                luks = (*value == '1' || *value == 't' || *value == 'y');
            } else if (efi_strcmp(key, L"luks_confirm") == 0 ||
                       efi_strcmp(key, L"luks_verify") == 0) {
                luks_confirm = (*value == '1' || *value == 't' || *value == 'y');
            } else if (efi_strcmp(key, L"luks_verbose") == 0 ||
                       efi_strcmp(key, L"luks_show_prompt") == 0) {
                luks_verbose = (*value == '1' || *value == 't' || *value == 'y');
            } else if (efi_strcmp(key, L"luks_key_path") == 0) {
                set_char16(&luks_key_path, efi_strdup(value));
            } else if (efi_strcmp(key, L"luks_cmdline") == 0 ||
                       efi_strcmp(key, L"luks_options") == 0 ||
                       efi_strcmp(key, L"luks_options_append") == 0) {
                set_char16(&luks_cmdline, efi_strdup(value));
            } else if (efi_strcmp(key, L"luks_preset") == 0 ||
                       efi_strcmp(key, L"luks_initramfs") == 0) {
                set_char16(&luks_preset, efi_strdup(value));
            }
        }
        (*idx)++;
    }

    if (!type && looks_windows(kernel_path)) type = 1;
    if (!kernel_encrypted_set) kernel_encrypted = encrypted;
    if (!initrd_encrypted_set) initrd_encrypted = encrypted;
    if (luks && !luks_cmdline && luks_preset)
        luks_cmdline = luks_cmdline_from_preset(luks_preset, luks_key_path);

    int entry_added = 0;
    if (name && kernel_path) {
        boot_entry_t *e = config_add_entry(config, name, icon_path, kernel_path,
                                           initrd_path, cmdline, uuid, type,
                                           kernel_encrypted, initrd_encrypted);
        if (e) {
            entry_added = 1;
            e->luks = luks;
            e->luks_confirm = luks_confirm;
            e->luks_verbose = luks_verbose;
            e->luks_key_path = luks_key_path;
            e->luks_cmdline = luks_cmdline;
            e->luks_preset = luks_preset;
            luks_key_path = NULL;
            luks_cmdline = NULL;
            luks_preset = NULL;
        }
        if (e && has_color) { e->color = color; e->has_color = 1; }
        if (e && color_role >= 0) e->color_role = color_role;
        if (e) e->icon_size = entry_icon_size;
        if (e && has_sha256) {
            for (int i = 0; i < 32; i++) e->sha256[i] = sha256_buf[i];
            e->has_sha256 = 1;
        }
    }
    if (!entry_added) {
        free_char16(&name);
        free_char16(&icon_path);
        free_char16(&kernel_path);
        free_char16(&initrd_path);
        free_char16(&cmdline);
        free_char16(&uuid);
    }
    if (luks_key_path) efi_free_pool(luks_key_path);
    if (luks_cmdline) efi_free_pool(luks_cmdline);
    if (luks_preset) efi_free_pool(luks_preset);

    return EFI_SUCCESS;
}

void bls_decrement(boot_entry_t *e) {
    if (!e || e->deploy_count == 0) return;
    UINTN s = e->deploy_sel; if (s >= e->deploy_count) s = 0;
    deployment_t *d = &e->deployments[s];
    if (d->tries_left <= 0 || !d->bls_path) return;

    UINTN n = 0; while (d->bls_path[n]) n++;
    INTN plus = -1;
    for (UINTN i = 0; i < n; i++) if (d->bls_path[i] == '+') plus = (INTN)i;
    if (plus < 0) return;

    if ((UINTN)plus + 32 >= MAX_PATH) {
        efi_log(L"WARN: bls path too long to update boot-counter");
        return;
    }
    CHAR16 base[MAX_PATH];
    UINTN k = 0;
    for (INTN i = 0; i < plus && k < MAX_PATH - 1; i++) base[k++] = d->bls_path[i];
    base[k] = 0;

    CHAR16 newp[MAX_PATH];
    SPrint(newp, sizeof(newp), L"%s+%d-%d.conf", base, d->tries_left - 1, d->tries_done + 1);

    if (efi_rename_file(d->bls_path, newp))
        efi_log(L"bls: decremented boot-counter for selected deployment");
    else
        efi_log(L"WARN: could not update boot-counter (read-only /boot?)");
}

boot_entry_t* config_add_entry(config_t *config,
                                      CHAR16 *name,
                                      CHAR16 *icon_path,
                                      CHAR16 *kernel_path,
                                      CHAR16 *initrd_path,
                                      CHAR16 *cmdline,
                                      CHAR16 *uuid,
                                      int type,
                                      int encrypted,
                                      int initrd_encrypted) {
    boot_entry_t *entry = efi_allocate_pool(sizeof(boot_entry_t));
    if (!entry) { efi_log(L"ERROR: out of memory adding boot entry"); return NULL; }

    entry->name = name ? name : efi_strdup(L"Unknown");
    if (!entry->name) {
        efi_free_pool(entry);
        efi_log(L"ERROR: out of memory naming boot entry");
        return NULL;
    }
    entry->icon_path = icon_path ? icon_path
                     : (type == 1 || looks_windows(kernel_path)
                        ? icon_path_for(L"windows.png")
                        : distro_icon(name));
    entry->kernel_path = kernel_path;
    entry->initrd_path = initrd_path;
    entry->cmdline = cmdline ? cmdline
                   : (!dc_foreign_volume
                      && entry_takes_default_cmdline(kernel_path, type)
                      && config->def_cmdline
                      ? efi_strdup(config->def_cmdline) : NULL);
    entry->uuid = uuid;
    entry->type = type;
    entry->index = config->entry_count;
    entry->icon = NULL;
    entry->icon_size = 0;
    entry->color = config->name_color;
    entry->has_color = 0;
    entry->color_role = -1;
    entry->has_sha256 = 0;
    entry->encrypted = encrypted;
    entry->initrd_encrypted = initrd_encrypted;
    entry->luks = 0;
    entry->luks_confirm = 0;
    entry->luks_verbose = 0;
    entry->luks_key_path = NULL;
    entry->luks_cmdline = NULL;
    entry->luks_preset = NULL;
    entry->decrypt_password = NULL;
    entry->hp_volume = NULL;
    entry->next = NULL;
    entry->deployments = NULL;
    entry->deploy_count = 0;
    entry->deploy_default = 0;
    entry->deploy_sel = 0;
    entry->snapshots = NULL;
    entry->snap_count = 0;
    entry->snap_sel = 0;

    efi_log(L"config: adding entry");
    efi_log(entry->name);

    if (entry->icon_path) {
        entry->icon = gui_load_icon(entry->icon_path);
        if (!entry->icon) efi_log(L"WARN: entry icon failed to load");
    }

    if (!config->entries) {
        config->entries = entry;
    } else {
        config->tail->next = entry;
    }
    config->tail = entry;

    config->entry_count++;
    return entry;
}

void config_free(config_t *config) {
    boot_entry_t *entry = config->entries;
    while (entry) {
        boot_entry_t *next = entry->next;
        free_entry_contents(entry);
        efi_free_pool(entry);
        entry = next;
    }
    config->entries = NULL;
    config->tail = NULL;
    config->entry_count = 0;

    if (config->background)    efi_free_pool(config->background);
    if (config->theme)         efi_free_pool(config->theme);
    if (config->title)         efi_free_pool(config->title);
    if (config->logo)          efi_free_pool(config->logo);
    if (config->font)          efi_free_pool(config->font);
    if (config->def_cmdline)   efi_free_pool(config->def_cmdline);
    if (config->shutdown_icon) efi_free_pool(config->shutdown_icon);
    if (config->reboot_icon)   efi_free_pool(config->reboot_icon);
    if (config->firmware_icon) efi_free_pool(config->firmware_icon);
    if (config->menu_sound)    efi_free_pool(config->menu_sound);
    config->background = NULL;
    config->theme = NULL;
    config->title = NULL;
    config->logo = NULL;
    config->font = NULL;
    config->def_cmdline = NULL;
    config->shutdown_icon = NULL;
    config->reboot_icon = NULL;
    config->firmware_icon = NULL;
    config->menu_sound = NULL;
}
