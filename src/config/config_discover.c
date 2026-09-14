/* config_discover.c - automatic entry discovery (feature: autodetect) */
#include "config_internal.h"

static int is_loader_efi(CHAR16 *name) {
    static const CHAR16 *skip[] = {
        L"grub", L"refind", L"shim", L"systemd-boot", L"bootx64",
        L"bootia32", L"mmx64", L"fbx64", L"mokmanager", L"bootmgr", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (contains_ci(name, skip[i])) return 1;
    return 0;
}

static CHAR16* dir_prefix(CHAR16 *dir) {
    return (dir[0] == '\\' && dir[1] == '\0') ? L"" : dir;
}

int scan_uki_dir(config_t *config, EFI_FILE_PROTOCOL *root, CHAR16 *dir) {
    EFI_FILE_PROTOCOL *d = efi_open_dir(root, dir);
    if (!d) return 0;

    int added = 0;
    CHAR16 name[128];
    int is_dir;
    while (efi_read_dirent(d, name, 128, &is_dir)) {
        if (is_dir) continue;
        if (!ends_with_ci(name, L".efi")) continue;
        if (is_loader_efi(name)) continue;

        CHAR16 path[MAX_PATH];
        SPrint(path, sizeof(path), L"%s\\%s", dir_prefix(dir), name);

        CHAR16 disp[128];
        UINTN n = 0;
        while (name[n] && n < 127) { disp[n] = name[n]; n++; }
        disp[n] = '\0';
        if (n > 4) disp[n - 4] = '\0';

        CHAR16 *entry_name = efi_strdup(disp);
        CHAR16 *entry_icon = distro_icon(disp);
        CHAR16 *entry_path = efi_strdup(path);
        if (!entry_name || !entry_path) {
            free_char16(&entry_name);
            free_char16(&entry_icon);
            free_char16(&entry_path);
        } else if (config_add_entry(config, entry_name, entry_icon, entry_path,
                             NULL, NULL, NULL, 0, 0, 0)) {
            added++;
        } else {
            free_char16(&entry_name);
            free_char16(&entry_icon);
            free_char16(&entry_path);
        }
    }
    d->Close(d);
    return added;
}

int is_kernel_name(CHAR16 *name) {
    if (ends_with_ci(name, L".img")) return 0;
    if (contains_ci(name, L"initrd") || contains_ci(name, L"initramfs")) return 0;
    if (lc16(name[0]) == 'v' && lc16(name[1]) == 'm' && lc16(name[2]) == 'l') return 1;
    if (contains_ci(name, L"bzimage")) return 1;
    return 0;
}

int entry_takes_default_cmdline(CHAR16 *kernel_path, int type) {
    static const CHAR16 *loaders[] = {
        L"grubx64.efi", L"grubaa64.efi", L"shimx64.efi", L"shimaa64.efi",
        L"bootmgfw.efi", L"refind_x64.efi", L"refind_aa64.efi",
        L"systemd-bootx64.efi", L"systemd-bootaa64.efi", NULL
    };
    if (!kernel_path || type != 0) return 0;
    if (looks_windows(kernel_path)) return 0;
    if (contains_ci(kernel_path, L"\\EFI\\BOOT\\")) return 0;
    for (int i = 0; loaders[i]; i++)
        if (ends_with_ci(kernel_path, loaders[i])) return 0;
    return 1;
}

static CHAR16* find_initrd(EFI_FILE_PROTOCOL *root, CHAR16 *dir, CHAR16 *kernel_name) {
    CHAR16 *suffix = efi_strchr(kernel_name, '-');
    CHAR16 sbuf[96];
    if (suffix) {
        UINTN i = 0;
        while (suffix[i] && i < 95) { sbuf[i] = suffix[i]; i++; }
        sbuf[i] = '\0';
    } else {
        sbuf[0] = '\0';
    }

    const CHAR16 *patterns[] = {
        L"%s\\initramfs%s.img", L"%s\\initrd.img%s", L"%s\\initramfs%s",
        L"%s\\initrd%s.img",    L"%s\\initrd%s",     L"%s\\initramfs.img",
        L"%s\\initrd.img",      NULL
    };
    for (int p = 0; patterns[p]; p++) {
        CHAR16 cand[MAX_PATH];
        SPrint(cand, sizeof(cand), patterns[p], dir_prefix(dir), sbuf);
        if (efi_file_exists_root(root, cand)) return efi_strdup(cand);
    }
    return NULL;
}

int add_kernel_entry(config_t *config, EFI_FILE_PROTOCOL *root,
                            EFI_HANDLE volume, CHAR16 *dir, CHAR16 *name,
                            int global_cmdline, CHAR16 **auto_cmd,
                            int *auto_tried) {
    CHAR16 path[MAX_PATH];
    SPrint(path, sizeof(path), L"%s\\%s", dir_prefix(dir), name);
    CHAR16 *initrd = find_initrd(root, dir, name);

    efi_log(L"config: auto-detected raw kernel");
    efi_log(path);
    if (initrd) efi_log(initrd);

    CHAR16 *entry_cmd = dc_from_loader_entries(root, name);
    if (!entry_cmd) {
        if (!*auto_tried) {
            *auto_tried = 1;
            *auto_cmd = dc_derive_cmdline(root, volume, NULL, global_cmdline);
        }
        entry_cmd = *auto_cmd ? efi_strdup(*auto_cmd) : NULL;
    }

    CHAR16 *entry_name = efi_strdup(name);
    CHAR16 *entry_icon = distro_icon(name);
    CHAR16 *entry_path = efi_strdup(path);
    if (entry_name && entry_path &&
        config_add_entry(config, entry_name, entry_icon,
                         entry_path, initrd, entry_cmd, NULL, 0, 0, 0))
        return 1;

    free_char16(&entry_name);
    free_char16(&entry_icon);
    free_char16(&entry_path);
    free_char16(&initrd);
    free_char16(&entry_cmd);
    return 0;
}

static int scan_kernel_dir(config_t *config, EFI_FILE_PROTOCOL *root,
                           EFI_HANDLE volume, CHAR16 *dir, int global_cmdline) {
    EFI_FILE_PROTOCOL *d = efi_open_dir(root, dir);
    if (!d) return 0;

    int added = 0;
    CHAR16 name[128];
    int is_dir;
    CHAR16 *auto_cmd = NULL;
    int auto_tried = 0;
    while (efi_read_dirent(d, name, 128, &is_dir)) {
        if (is_dir) continue;
        if (!is_kernel_name(name)) continue;
        added += add_kernel_entry(config, root, volume, dir, name,
                                  global_cmdline, &auto_cmd, &auto_tried);
    }
    if (auto_cmd) efi_free_pool(auto_cmd);
    d->Close(d);
    return added;
}

EFI_FILE_PROTOCOL *root_from_handle(EFI_HANDLE h) {
    EFI_FILE_IO_INTERFACE *io = NULL;
    if (EFI_ERROR(BS->HandleProtocol(h, &gEfiSimpleFileSystemProtocolGuid, (void**)&io)) || !io)
        return NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (EFI_ERROR(io->OpenVolume(io, &root))) return NULL;
    return root;
}

int scan_vendor_loaders(config_t *config, EFI_FILE_PROTOCOL *root) {
    static const CHAR16 *skip_dirs[] = {
        L"BOOT", L"Microsoft", L"visor", L"Linux", L"systemd",
        L"refind", L"tools", NULL
    };
    static const CHAR16 *loaders[] = { L"shimx64.efi", L"grubx64.efi", NULL };

    EFI_FILE_PROTOCOL *d = efi_open_dir(root, L"\\EFI");
    if (!d) return 0;

    int added = 0;
    CHAR16 name[128];
    int is_dir;
    while (efi_read_dirent(d, name, 128, &is_dir)) {
        if (!is_dir) continue;
        int skip = 0;
        for (int i = 0; skip_dirs[i]; i++)
            if (equals_ci(name, skip_dirs[i])) { skip = 1; break; }
        if (skip) continue;

        for (int i = 0; loaders[i]; i++) {
            CHAR16 path[MAX_PATH];
            SPrint(path, sizeof(path), L"\\EFI\\%s\\%s", name, loaders[i]);
            if (!efi_file_exists_root(root, path)) continue;

            efi_log(L"config: auto-detected distro loader");
            efi_log(path);

            CHAR16 *entry_name = efi_strdup(name);
            CHAR16 *entry_icon = distro_icon(name);
            CHAR16 *entry_path = efi_strdup(path);
            if (!entry_name || !entry_path) {
                free_char16(&entry_name);
                free_char16(&entry_icon);
                free_char16(&entry_path);
            } else if (config_add_entry(config, entry_name, entry_icon,
                                 entry_path, NULL, NULL, NULL, 0, 0, 0)) {
                added++;
            } else {
                free_char16(&entry_name);
                free_char16(&entry_icon);
                free_char16(&entry_path);
            }
            break;
        }
    }
    d->Close(d);
    return added;
}

void tag_entries_since(config_t *config, UINTN first,
                              EFI_HANDLE volume) {
    if (!config || !volume || first >= config->entry_count) return;

    CHAR16 *uuid = efi_handle_partition_uuid(volume);
    UINTN idx = 0;
    for (boot_entry_t *e = config->entries; e; e = e->next, idx++) {
        if (idx < first) continue;
        e->hp_volume = volume;
        if (uuid && !e->uuid) e->uuid = efi_strdup(uuid);
    }
    if (uuid) efi_free_pool(uuid);
}

static const EFI_GUID scope_type_esp =
    { 0xc12a7328, 0xf81f, 0x11d2, { 0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b } };
static const EFI_GUID scope_type_xbootldr =
    { 0xbc13c2ff, 0x59e6, 0x4262, { 0xa3,0x52,0xb2,0x75,0xfd,0x6f,0x71,0x72 } };

#define SCOPE_MAX_PARTS 64
static UINT8 scope_parts[SCOPE_MAX_PARTS][16];
static UINTN scope_part_n;
static int   scope_gpt_read;

static void scope_read_gpt(void) {
    if (scope_gpt_read) return;
    scope_gpt_read = 1;

    UINTN nh = 0;
    EFI_HANDLE *hs = efi_locate_handle_buffer(&gEfiBlockIoProtocolGuid, &nh);
    if (!hs) return;

    for (UINTN h = 0; h < nh && scope_part_n < SCOPE_MAX_PARTS; h++) {
        EFI_BLOCK_IO *bio = NULL;
        if (EFI_ERROR(BS->HandleProtocol(hs[h], &gEfiBlockIoProtocolGuid,
                                         (void**)&bio)) || !bio || !bio->Media)
            continue;
        if (bio->Media->LogicalPartition || !bio->Media->MediaPresent) continue;
        UINT32 bs = bio->Media->BlockSize;
        if (bs < 512 || bs > 4096) continue;
        UINTN align = bio->Media->IoAlign > 1 ? bio->Media->IoAlign : 1;

        UINT8 *raw = efi_allocate_pool(bs + align);
        if (!raw) continue;
        UINT8 *hdr = raw + ((align - ((UINTN)raw & (align - 1))) & (align - 1));
        if (EFI_ERROR(bio->ReadBlocks(bio, bio->Media->MediaId, 1, bs, hdr)) ||
            CompareMem(hdr, (void*)"EFI PART", 8) != 0) {
            efi_free_pool(raw);
            continue;
        }
        UINT64 elba;
        UINT32 num, esz;
        CopyMem(&elba, hdr + 72, sizeof(elba));
        CopyMem(&num,  hdr + 80, sizeof(num));
        CopyMem(&esz,  hdr + 84, sizeof(esz));
        efi_free_pool(raw);
        if (esz < 128 || esz > 4096 || !num) continue;
        if (num > 128) num = 128;
        UINTN rdsz = (((UINTN)num * esz + bs - 1) / bs) * bs;

        raw = efi_allocate_pool(rdsz + align);
        if (!raw) continue;
        UINT8 *ents = raw + ((align - ((UINTN)raw & (align - 1))) & (align - 1));
        if (EFI_ERROR(bio->ReadBlocks(bio, bio->Media->MediaId, elba, rdsz, ents))) {
            efi_free_pool(raw);
            continue;
        }
        for (UINT32 i = 0; i < num && scope_part_n < SCOPE_MAX_PARTS; i++) {
            UINT8 *e = ents + (UINTN)i * esz;
            if (CompareMem(e, &scope_type_esp, 16) != 0 &&
                CompareMem(e, &scope_type_xbootldr, 16) != 0)
                continue;
            CopyMem(scope_parts[scope_part_n++], e + 16, 16);
        }
        efi_free_pool(raw);
    }
    efi_free_pool(hs);

    { CHAR16 d[64]; SPrint(d, sizeof(d), L"config: %d boot partition(s) in GPT",
                           (int)scope_part_n); efi_log(d); }
}

static int scope_part_guid(EFI_HANDLE h, UINT8 out[16]) {
    EFI_DEVICE_PATH *dp = NULL;
    if (EFI_ERROR(BS->HandleProtocol(h, &gEfiDevicePathProtocolGuid, (void**)&dp)) || !dp)
        return 0;
    for (EFI_DEVICE_PATH *n = dp; !IsDevicePathEnd(n);
         n = (EFI_DEVICE_PATH*)((UINT8*)n + DevicePathNodeLength(n))) {
        if (DevicePathType(n) != MEDIA_DEVICE_PATH ||
            DevicePathSubType(n) != MEDIA_HARDDRIVE_DP)
            continue;
        HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH*)n;
        if (hd->SignatureType != SIGNATURE_TYPE_GUID) continue;
        CopyMem(out, hd->Signature, 16);
        return 1;
    }
    return 0;
}

int scope_wants_volume(int quick, EFI_HANDLE vol) {
    if (!quick) return 1;
    if (vol == efi_boot_volume_handle()) return 1;

    scope_read_gpt();

    UINT8 g[16];
    if (scope_part_n && scope_part_guid(vol, g)) {
        for (UINTN i = 0; i < scope_part_n; i++)
            if (CompareMem(scope_parts[i], g, 16) == 0) return 1;
        return 0;
    }

    EFI_FILE_PROTOCOL *root = root_from_handle(vol);
    if (!root) return 0;
    int ok = efi_file_exists_root(root, L"\\EFI") ||
             efi_file_exists_root(root, L"\\loader\\entries");
    root->Close(root);
    return ok;
}

EFI_HANDLE dc_scanned_vols[64];
UINTN      dc_scanned_n;

static void dc_mark_scanned(EFI_HANDLE vol) {
    if (!vol) return;
    for (UINTN i = 0; i < dc_scanned_n; i++)
        if (dc_scanned_vols[i] == vol) return;
    if (dc_scanned_n < 64) dc_scanned_vols[dc_scanned_n++] = vol;
}

static int detect_scan_volumes(config_t *config, int bls, int quick) {
    static CHAR16 *windows_paths[] = {
        L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
        L"\\EFI\\BOOT\\bootmgfw.efi",
        NULL
    };

    UINTN start_count = config->entry_count;

    UINTN nvol = 0;
    EFI_HANDLE *vols = efi_locate_handle_buffer(&gEfiSimpleFileSystemProtocolGuid, &nvol);
    if (!vols || nvol == 0) {
        if (vols) efi_free_pool(vols);
        return 0;
    }

    int windows_found = 0;
    int uki_found = 0;

    for (UINTN v = 0; v < nvol; v++) {
        if (!scope_wants_volume(quick, vols[v])) continue;
        EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
        if (!root) continue;

        UINTN volume_start = config->entry_count;
        dc_mark_scanned(vols[v]);
        if (!windows_found) {
            for (int i = 0; windows_paths[i] != NULL; i++) {
                if (efi_file_exists_root(root, windows_paths[i])) {
                    CHAR16 *entry_name = efi_strdup(L"Windows Boot Manager");
                    CHAR16 *entry_icon = icon_path_for(L"windows.png");
                    CHAR16 *entry_path = efi_strdup(windows_paths[i]);
                    if (!entry_name || !entry_path) {
                        free_char16(&entry_name);
                        free_char16(&entry_icon);
                        free_char16(&entry_path);
                    } else if (config_add_entry(config, entry_name, entry_icon,
                                         entry_path, NULL, NULL, NULL, 1, 0, 0)) {
                        windows_found = 1;
                    } else {
                        free_char16(&entry_name);
                        free_char16(&entry_icon);
                        free_char16(&entry_path);
                    }
                    break;
                }
            }
        }

        uki_found += scan_uki_dir(config, root, L"\\EFI\\Linux");
        tag_entries_since(config, volume_start, vols[v]);

        root->Close(root);
    }

    int raw_found = 0;
    if (!bls) {
        for (UINTN v = 0; v < nvol; v++) {
            if (!scope_wants_volume(quick, vols[v])) continue;
            EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
            if (!root) continue;
            UINTN volume_start = config->entry_count;
            raw_found += scan_kernel_dir(config, root, vols[v], L"\\boot", 1);
            raw_found += scan_kernel_dir(config, root, vols[v], L"\\@\\boot", 1);
            raw_found += scan_kernel_dir(config, root, vols[v], L"\\", 1);
            tag_entries_since(config, volume_start, vols[v]);
            root->Close(root);
        }
        { CHAR16 d[64]; SPrint(d, sizeof(d), L"config: raw kernel scan found %d", raw_found); efi_log(d); }
    }
    if (!uki_found && !bls && !raw_found) {
        for (UINTN v = 0; v < nvol; v++) {
            if (!scope_wants_volume(quick, vols[v])) continue;
            EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
            if (!root) continue;
            UINTN volume_start = config->entry_count;
            scan_vendor_loaders(config, root);
            tag_entries_since(config, volume_start, vols[v]);
            root->Close(root);
        }
    }

    efi_free_pool(vols);
    return (int)(config->entry_count - start_count);
}

EFI_STATUS detect_entries(config_t *config) {
    if (!show_names_set)  config->show_names = 0;
    if (!center_info_set) config->center_info = 1;

    if (efi_fs_drivers_pending()) {
        efi_log(L"config: starting FS drivers before auto-detection");
        efi_start_deferred_drivers();
    }

    int quick = (config->scan_mode == SCAN_MODE_QUICK);
    if (quick) efi_log(L"config: quick scan - ESP and /boot partitions only");

    int bls = bls_detect(config, quick);
    int found = detect_scan_volumes(config, bls, quick);

    if (quick && !found && !bls) {
        efi_log(L"config: quick scan found nothing - widening to a deep scan");
        bls = bls_detect(config, 0);
        found = detect_scan_volumes(config, bls, 0);
    }

    if (!found && !bls) return EFI_NOT_FOUND;
    return EFI_SUCCESS;
}
