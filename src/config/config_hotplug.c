/* config_hotplug.c - incremental rescan of newly attached volumes (feature: hotplug) */
#include "config_internal.h"

struct hp_kdir {
    EFI_FILE_PROTOCOL *root;
    EFI_HANDLE volume;
    CHAR16 *dir;
    int global_cmdline;
    EFI_FILE_PROTOCOL *d;
    CHAR16 *auto_cmd;
    int auto_tried;
};

static void hp_kdir_begin(EFI_FILE_PROTOCOL *root, CHAR16 *dir,
                          EFI_HANDLE volume, int global_cmdline,
                          struct hp_kdir *w) {
    w->root = root;
    w->volume = volume;
    w->dir = dir;
    w->global_cmdline = global_cmdline;
    w->d = efi_open_dir(root, dir);
    w->auto_cmd = NULL;
    w->auto_tried = 0;
}

#define HP_DIRENT_SLICE_US 20000ULL

static int hp_kdir_next(config_t *config, struct hp_kdir *w, CHAR16 **out) {
    *out = NULL;
    if (!w->d) return 0;
    CHAR16 name[128];
    int is_dir;
    UINT64 t0 = arch_now_us();
    for (;;) {
        if (efi_key_pending() || arch_now_us() - t0 >= HP_DIRENT_SLICE_US)
            return 1;
        if (!efi_read_dirent(w->d, name, 128, &is_dir)) break;
        if (is_dir) continue;
        if (!is_kernel_name(name)) continue;
        if (add_kernel_entry(config, w->root, w->volume, w->dir, name,
                             w->global_cmdline, &w->auto_cmd, &w->auto_tried))
            *out = efi_strdup(name);
        return 1;
    }
    w->d->Close(w->d);
    w->d = NULL;
    if (w->auto_cmd) efi_free_pool(w->auto_cmd);
    w->auto_cmd = NULL;
    return 0;
}

static EFI_HANDLE *hp_fs_known;  static UINTN hp_fs_n;
static int hp_pending_probe;
static UINTN hp_probe_cursor;
static int hp_probe_round;
static EFI_HANDLE *hp_blk_known; static UINTN hp_blk_n;

#define HP_TRIED_MAX 64
static EFI_HANDLE hp_blk_tried[HP_TRIED_MAX];
static UINTN      hp_blk_tried_n;

static int hp_in_set(EFI_HANDLE *set, UINTN n, EFI_HANDLE h) {
    for (UINTN i = 0; i < n; i++)
        if (set[i] == h) return 1;
    return 0;
}

static int hp_volume_hosts_entry(config_t *config, EFI_HANDLE vol) {
    EFI_FILE_PROTOCOL *root = root_from_handle(vol);
    if (!root) return 0;

    int hosts = 0;
    for (boot_entry_t *e = config->entries; e && !hosts; e = e->next) {
        if (!e->kernel_path || !e->kernel_path[0]) continue;
        if (efi_file_exists_root(root, e->kernel_path)) {
            hosts = 1;
            break;
        }
        CHAR16 *alt = visor_path_without_boot_mount(e->kernel_path);
        if (alt && efi_file_exists_root(root, alt)) hosts = 1;
    }
    root->Close(root);
    return hosts;
}

void config_hotplug_arm(config_t *config) {
    if (hp_fs_known)  efi_free_pool(hp_fs_known);
    if (hp_blk_known) efi_free_pool(hp_blk_known);
    hp_fs_n = hp_blk_n = 0;

    hp_blk_known = efi_locate_handle_buffer(
        &gEfiBlockIoProtocolGuid, &hp_blk_n);
    if (!hp_blk_known) hp_blk_n = 0;

    UINTN nf = 0;
    EFI_HANDLE *fs = efi_locate_handle_buffer(
        &gEfiSimpleFileSystemProtocolGuid, &nf);

    hp_fs_known = efi_allocate_pool((nf ? nf : 1) * sizeof(EFI_HANDLE));
    if (hp_fs_known) {
        EFI_HANDLE boot_volume = efi_boot_volume_handle();
        for (UINTN i = 0; i < nf; i++) {
            int seen = (fs[i] == boot_volume);
            for (UINTN k = 0; !seen && k < dc_scanned_n; k++)
                if (dc_scanned_vols[k] == fs[i]) seen = 1;
            if (seen) hp_fs_known[hp_fs_n++] = fs[i];
        }
    }

    int sweep;
    if (config->scan_existing >= 0)
        sweep = config->scan_existing;
    else if (config->scan_mode == SCAN_MODE_QUICK)
        sweep = 0;
    else
        sweep = !config->entries_from_config;
    if (!sweep && fs && hp_fs_known) {
        efi_free_pool(hp_fs_known);
        hp_fs_known = fs;
        hp_fs_n = nf;
        fs = NULL;
    }
    if (fs) efi_free_pool(fs);

    hp_pending_probe = sweep && !efi_fs_probe_exhausted();
    hp_probe_cursor = 0;
    hp_probe_round = 0;

    { CHAR16 d[128];
      SPrint(d, sizeof(d),
             L"hotplug: armed, %d of %d volume(s) known, existing-media sweep %s",
             (int)hp_fs_n, (int)nf, sweep ? L"on" : L"off");
      efi_log(d);
      if (sweep && !hp_pending_probe)
          efi_log(L"hotplug: block devices already probed at startup - "
                  L"skipping the re-probe"); }
}

static EFI_GUID hp_fs_info_guid = { 0x09576e93, 0x6d3f, 0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

static CHAR16* hp_volume_label(EFI_FILE_PROTOCOL *root) {
    UINT8 buf[SIZE_OF_EFI_FILE_SYSTEM_INFO + 64 * sizeof(CHAR16)]
        __attribute__((aligned(8)));
    UINTN size = sizeof(buf);
    EFI_FILE_SYSTEM_INFO *info = (EFI_FILE_SYSTEM_INFO*)buf;
    if (EFI_ERROR(root->GetInfo(root, &hp_fs_info_guid, &size, info)))
        return NULL;
    if (size < SIZE_OF_EFI_FILE_SYSTEM_INFO + sizeof(CHAR16) ||
        !info->VolumeLabel[0])
        return NULL;
    return efi_strdup(info->VolumeLabel);
}

enum {
    HP_STAGE_WINDOWS = 0,
    HP_STAGE_UKI,
    HP_STAGE_BOOT,
    HP_STAGE_BTRFS_BOOT,
    HP_STAGE_ROOT,
    HP_STAGE_VENDOR,
    HP_STAGE_FALLBACK,
    HP_STAGE_DONE
};

static EFI_HANDLE hp_cur_vol;
static EFI_FILE_PROTOCOL *hp_cur_root;
static int   hp_cur_stage;
static UINTN hp_cur_before;
static int   hp_cur_uki;
static int   hp_cur_raw;
static struct hp_kdir hp_cur_walk;
static int   hp_cur_walking;

static void hp_scan_finish(config_t *config) {
    if (hp_cur_walk.d) { hp_cur_walk.d->Close(hp_cur_walk.d); hp_cur_walk.d = NULL; }
    if (hp_cur_walk.auto_cmd) {
        efi_free_pool(hp_cur_walk.auto_cmd);
        hp_cur_walk.auto_cmd = NULL;
    }
    if (hp_cur_root) { hp_cur_root->Close(hp_cur_root); hp_cur_root = NULL; }
    dc_foreign_volume = 0;

    if (hp_cur_vol) {
        int total = (int)(config->entry_count - hp_cur_before);
        if (total > 0) {
            CHAR16 d[64];
            SPrint(d, sizeof(d), L"hotplug: added %d boot entr%s", total,
                   total == 1 ? L"y" : L"ies");
            efi_log(d);
        }
    }
    hp_cur_vol = NULL;
    hp_cur_walking = 0;
    hp_cur_stage = HP_STAGE_DONE;
}

static CHAR16* hp_stage_dir(int stage) {
    switch (stage) {
    case HP_STAGE_BOOT:       return L"\\boot";
    case HP_STAGE_BTRFS_BOOT: return L"\\@\\boot";
    default:                  return L"\\";
    }
}

static int hp_scan_step(config_t *config, int *done) {
    *done = 0;
    EFI_HANDLE vol = hp_cur_vol;
    EFI_FILE_PROTOCOL *root = hp_cur_root;
    if (!root) { hp_scan_finish(config); *done = 1; return 0; }

    UINTN before = config->entry_count;
    dc_foreign_volume = 1;

    switch (hp_cur_stage) {
    case HP_STAGE_WINDOWS: {
        static CHAR16 *win_paths[] = {
            L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
            L"\\EFI\\BOOT\\bootmgfw.efi",
            NULL
        };
        for (int i = 0; win_paths[i]; i++) {
            if (!efi_file_exists_root(root, win_paths[i])) continue;
            CHAR16 *entry_name = efi_strdup(L"Windows Boot Manager");
            CHAR16 *entry_icon = icon_path_for(L"windows.png");
            CHAR16 *entry_path = efi_strdup(win_paths[i]);
            if (!entry_name || !entry_path ||
                !config_add_entry(config, entry_name, entry_icon,
                                  entry_path, NULL, NULL, NULL, 1, 0, 0)) {
                free_char16(&entry_name);
                free_char16(&entry_icon);
                free_char16(&entry_path);
            }
            break;
        }
        hp_cur_stage++;
        break;
    }
    case HP_STAGE_UKI:
        hp_cur_uki += scan_uki_dir(config, root, L"\\EFI\\Linux");
        hp_cur_stage++;
        break;
    case HP_STAGE_BOOT:
    case HP_STAGE_BTRFS_BOOT:
    case HP_STAGE_ROOT: {
        if (!hp_cur_walking) {
            hp_kdir_begin(root, hp_stage_dir(hp_cur_stage), vol, 0, &hp_cur_walk);
            hp_cur_walking = 1;
        }
        CHAR16 *added_name = NULL;
        if (!hp_kdir_next(config, &hp_cur_walk, &added_name)) {
            hp_cur_walking = 0;
            hp_cur_stage++;
        }
        if (added_name) { hp_cur_raw++; efi_free_pool(added_name); }
        break;
    }
    case HP_STAGE_VENDOR:
        if (!hp_cur_uki && !hp_cur_raw && config->entry_count == hp_cur_before)
            scan_vendor_loaders(config, root);
        hp_cur_stage++;
        break;
    case HP_STAGE_FALLBACK:
        if (config->entry_count == hp_cur_before) {
#if defined(__aarch64__)
            CHAR16 *fallback = L"\\EFI\\BOOT\\BOOTAA64.EFI";
#else
            CHAR16 *fallback = L"\\EFI\\BOOT\\BOOTX64.EFI";
#endif
            if (efi_file_exists_root(root, fallback)) {
                CHAR16 *label = hp_volume_label(root);
                CHAR16 *entry_name = label ? label : efi_strdup(L"USB Device");
                CHAR16 *entry_icon = icon_path_for(L"unknown.png");
                CHAR16 *entry_path = efi_strdup(fallback);
                if (!entry_name || !entry_path ||
                    !config_add_entry(config, entry_name, entry_icon,
                                      entry_path, NULL, NULL, NULL, 1, 0, 0)) {
                    free_char16(&entry_name);
                    free_char16(&entry_icon);
                    free_char16(&entry_path);
                }
            }
        }
        hp_cur_stage++;
        break;
    default:
        hp_cur_stage = HP_STAGE_DONE;
        break;
    }

    dc_foreign_volume = 0;

    int added = (int)(config->entry_count - before);
    if (added > 0) tag_entries_since(config, before, vol);

    if (hp_cur_stage >= HP_STAGE_DONE) {
        hp_scan_finish(config);
        *done = 1;
    }
    return added;
}

static int hp_scan_begin(config_t *config, EFI_HANDLE vol) {
    for (boot_entry_t *e = config->entries; e; e = e->next)
        if (e->hp_volume == vol ||
            (e->uuid && e->uuid[0] &&
             efi_handle_matches_partition_uuid(vol, e->uuid)))
            return 0;

    EFI_FILE_PROTOCOL *root = root_from_handle(vol);
    if (!root) return 0;

    hp_cur_vol = vol;
    hp_cur_root = root;
    hp_cur_stage = HP_STAGE_WINDOWS;
    hp_cur_before = config->entry_count;
    hp_cur_uki = 0;
    hp_cur_raw = 0;
    hp_cur_walking = 0;
    hp_cur_walk.d = NULL;
    hp_cur_walk.auto_cmd = NULL;
    return 1;
}

static void hp_free_entry(boot_entry_t *e) {
    free_entry_contents(e);
    efi_free_pool(e);
}

#define HP_MAX_PROBES_PER_POLL 1

#define HP_SLICE_BUDGET_US 40000ULL

static int hp_vol_present(EFI_HANDLE *fs, UINTN nf, EFI_HANDLE h) {
    if (!hp_in_set(fs, nf, h)) return 0;
    EFI_BLOCK_IO *bio = NULL;
    if (!EFI_ERROR(BS->HandleProtocol(h, &gEfiBlockIoProtocolGuid,
                                      (void**)&bio)) &&
        bio && bio->Media && !bio->Media->MediaPresent)
        return 0;
    return 1;
}

static int hp_vol_exists(EFI_HANDLE *fs, UINTN nf, EFI_HANDLE h) {
    return hp_in_set(fs, nf, h);
}

int config_hotplug_poll(config_t *config, UINTN *first_new) {
    UINT64 t_poll = arch_now_us();
    UINT64 us_probe = 0, us_blk = 0, us_fsloc = 0;
    UINT64 us_removal = 0, us_discover = 0, us_scan = 0;

    int probed_now = 0;
    UINT64 t_phase = arch_now_us();
    if (hp_pending_probe) {
        UINTN probed = 0;
        while (hp_probe_cursor < hp_blk_n && probed < HP_MAX_PROBES_PER_POLL) {
            EFI_HANDLE h = hp_blk_known[hp_probe_cursor++];
            if (efi_handle_has_filesystem(h)) continue;
            UINT64 t0 = arch_now_us();
            BS->ConnectController(h, NULL, NULL, FALSE);
            UINT64 dt = arch_now_us() - t0;
            probed++;
            if (dt > 100000) {
                CHAR16 d[112];
                SPrint(d, sizeof(d),
                       L"hotplug: probing block device %d took %d ms",
                       (int)(hp_probe_cursor - 1), (int)(dt / 1000));
                efi_log(d);
            }
        }
        hp_probe_round += (int)probed;
        if (hp_probe_cursor >= hp_blk_n) {
            hp_pending_probe = 0;
            if (hp_probe_round) {
                CHAR16 d[96];
                SPrint(d, sizeof(d),
                       L"hotplug: probed %d unreadable block device(s)",
                       hp_probe_round);
                efi_log(d);
            }
        }
        probed_now = (probed > 0);
    }
    us_probe = arch_now_us() - t_phase;

    t_phase = arch_now_us();
    if (!probed_now) {
        UINTN nb = 0;
        EFI_HANDLE *blk = efi_locate_handle_buffer(&gEfiBlockIoProtocolGuid, &nb);
        if (blk) {
            int fresh = 0;
            for (UINTN i = 0; i < nb; i++)
                if (!hp_in_set(hp_blk_known, hp_blk_n, blk[i])) {
                    if (hp_in_set(hp_blk_tried, hp_blk_tried_n, blk[i])) continue;
                    if (hp_blk_tried_n < HP_TRIED_MAX)
                        hp_blk_tried[hp_blk_tried_n++] = blk[i];
                    UINT64 t0 = arch_now_us();
                    BS->ConnectController(blk[i], NULL, NULL, FALSE);
                    UINT64 dt = arch_now_us() - t0;
                    if (dt > 100000) {
                        CHAR16 d[112];
                        SPrint(d, sizeof(d),
                               L"hotplug: connecting a new block device took %d ms",
                               (int)(dt / 1000));
                        efi_log(d);
                    }
                    fresh = 1;
                }
            if (fresh) {
                if (hp_blk_known) efi_free_pool(hp_blk_known);
                hp_blk_known = blk;
                hp_blk_n = nb;
                hp_probe_cursor = 0;
            } else {
                efi_free_pool(blk);
            }
        }
    }
    us_blk = arch_now_us() - t_phase;

    t_phase = arch_now_us();
    UINTN nf = 0;
    EFI_HANDLE *fs = efi_locate_handle_buffer(&gEfiSimpleFileSystemProtocolGuid, &nf);
    us_fsloc = arch_now_us() - t_phase;
    if (!fs) return 0;

    int mask = 0;

    t_phase = arch_now_us();
    UINTN removed_at = 0, idx = 0;
    int removed = 0;
    boot_entry_t **link = &config->entries;
    while (*link) {
        boot_entry_t *e = *link;
        if (e->hp_volume && !hp_vol_present(fs, nf, e->hp_volume)) {
            if (!removed) removed_at = idx;
            *link = e->next;
            efi_log(L"hotplug: volume removed - dropping boot entry");
            efi_log(e->name);
            hp_free_entry(e);
            config->entry_count--;
            removed++;
            continue;
        }
        link = &e->next;
        idx++;
    }
    if (removed) {
        boot_entry_t *t = config->entries;
        while (t && t->next) t = t->next;
        config->tail = t;
        UINTN i2 = 0;
        for (boot_entry_t *e = config->entries; e; e = e->next) e->index = i2++;
        mask |= 2;
    }

    if (hp_cur_vol && !hp_vol_present(fs, nf, hp_cur_vol)) {
        efi_log(L"hotplug: volume vanished mid-scan - abandoning it");
        hp_scan_finish(config);
    }
    us_removal = arch_now_us() - t_phase;

    int added = 0;
    UINTN before = config->entry_count;
    UINTN scanned_idx = nf;
    int discovery_ran = 0;
    if (!probed_now) {
        t_phase = arch_now_us();
        if (!hp_cur_vol) {
            discovery_ran = 1;
            for (UINTN i = 0; i < nf; i++) {
                if (hp_in_set(hp_fs_known, hp_fs_n, fs[i])) continue;
                if (hp_volume_hosts_entry(config, fs[i])) {
                    efi_log(L"hotplug: volume already backs a configured entry - not scanning");
                } else {
                    efi_log(L"hotplug: scanning a volume Visor has not seen yet");
                    hp_scan_begin(config, fs[i]);
                }
                scanned_idx = i;
                break;
            }
        }
        us_discover = arch_now_us() - t_phase;

        if (hp_cur_vol) {
            UINT64 t0 = arch_now_us();
            int done = 0;
            while (hp_cur_vol && !done) {
                added += hp_scan_step(config, &done);
                if (efi_key_pending()) {
                    efi_log(L"hotplug: key pressed - pausing the scan");
                    break;
                }
                if (arch_now_us() - t0 >= HP_SLICE_BUDGET_US) break;
            }
            us_scan = arch_now_us() - t0;
        }
    }

    if (scanned_idx < nf) {
        EFI_HANDLE *grown = efi_allocate_pool((hp_fs_n + 1) * sizeof(EFI_HANDLE));
        if (grown) {
            UINTN k = 0;
            for (UINTN i = 0; i < hp_fs_n; i++)
                if (hp_vol_exists(fs, nf, hp_fs_known[i]))
                    grown[k++] = hp_fs_known[i];
            grown[k++] = fs[scanned_idx];
            if (hp_fs_known) efi_free_pool(hp_fs_known);
            hp_fs_known = grown;
            hp_fs_n = k;
        }
        efi_free_pool(fs);
    } else if (discovery_ran) {
        if (hp_fs_known) efi_free_pool(hp_fs_known);
        hp_fs_known = fs;
        hp_fs_n = nf;
    } else {
        efi_free_pool(fs);
    }

    if (added > 0) mask |= 1;
    if (first_new) *first_new = (mask & 1) ? before : removed_at;

    UINT64 us_total = arch_now_us() - t_poll;
    if (us_total > 150000) {
        CHAR16 d[224];
        SPrint(d, sizeof(d),
               L"hotplug: SLOW poll %d ms (probe %d, blk %d, fs-locate %d, "
               L"removal %d, discover %d, scan %d)",
               (int)(us_total / 1000), (int)(us_probe / 1000),
               (int)(us_blk / 1000), (int)(us_fsloc / 1000),
               (int)(us_removal / 1000), (int)(us_discover / 1000),
               (int)(us_scan / 1000));
        efi_log(d);
    }
    return mask;
}
