/* linux_boot.c - the boot handover: verify, register initrd, StartImage */
#include "linux_internal.h"

static int linux_is_pe_image(const UINT8 *data, UINTN size) {
    if (!data || size < 0x40 || data[0] != 'M' || data[1] != 'Z') return 0;
    UINT32 pe_offset = (UINT32)data[0x3C] |
                       ((UINT32)data[0x3D] << 8) |
                       ((UINT32)data[0x3E] << 16) |
                       ((UINT32)data[0x3F] << 24);
    return pe_offset <= size - 4 && data[pe_offset] == 'P' &&
           data[pe_offset + 1] == 'E' && data[pe_offset + 2] == 0 &&
           data[pe_offset + 3] == 0;
}

int visor_windows_bootnext = 0;

static CHAR16 lower16(CHAR16 c) {
    return (c >= L'A' && c <= L'Z') ? (CHAR16)(c - L'A' + L'a') : c;
}

/* Case-insensitive "ends with" over a counted string (no NUL needed). */
static int ends_with_ci(const CHAR16 *s, UINTN len, const CHAR16 *suffix) {
    UINTN n = 0;
    while (suffix[n]) n++;
    while (len > 0 && s[len - 1] == 0) len--;
    if (len < n) return 0;
    for (UINTN i = 0; i < n; i++)
        if (lower16(s[len - n + i]) != lower16(suffix[i])) return 0;
    return len == n || s[len - n - 1] == L'\\' || s[len - n - 1] == L'/';
}

static int entry_is_windows(boot_entry_t *entry) {
    if (!entry->kernel_path) return 1;
    return ends_with_ci(entry->kernel_path, efi_strlen16(entry->kernel_path),
                        L"bootmgfw.efi");
}

static void *get_global_var(CHAR16 *name, UINTN *size) {
    UINTN sz = 0;
    UINT32 attr;
    EFI_STATUS s = RT->GetVariable(name, &gEfiGlobalVariableGuid, &attr, &sz, NULL);
    if (s != EFI_BUFFER_TOO_SMALL || sz == 0) return NULL;
    void *buf = efi_allocate_pool(sz);
    if (!buf) return NULL;
    s = RT->GetVariable(name, &gEfiGlobalVariableGuid, &attr, &sz, buf);
    if (EFI_ERROR(s)) { efi_free_pool(buf); return NULL; }
    *size = sz;
    return buf;
}

/* EFI_LOAD_OPTION: UINT32 Attributes, UINT16 FilePathListLength,
 * CHAR16 Description[] (NUL-terminated), then the device path list. */
static int load_option_is_bootmgfw(const UINT8 *opt, UINTN size) {
    if (size < 6) return 0;
    UINT32 attrs = (UINT32)opt[0] | ((UINT32)opt[1] << 8) |
                   ((UINT32)opt[2] << 16) | ((UINT32)opt[3] << 24);
    UINT16 fpl = (UINT16)(opt[4] | (opt[5] << 8));
    if (!(attrs & LOAD_OPTION_ACTIVE)) return 0;
    UINTN off = 6;
    while (off + 2 <= size && (opt[off] | opt[off + 1]) != 0) off += 2;
    off += 2;
    if (off > size || fpl > size - off) return 0;
    const UINT8 *dp = opt + off, *end = dp + fpl;
    while (dp + 4 <= end) {
        UINT8 type = dp[0], sub = dp[1];
        UINT16 len = (UINT16)(dp[2] | (dp[3] << 8));
        if (len < 4 || dp + len > end) return 0;
        if (type == 0x7F) break;                       /* end of device path */
        if (type == MEDIA_DEVICE_PATH && sub == MEDIA_FILEPATH_DP) {
            /* Device path nodes are byte-packed: copy to an aligned buffer. */
            CHAR16 path[260];
            UINTN n = (UINTN)(len - 4) / sizeof(CHAR16);
            if (n > 260) n = 260;
            CopyMem(path, (VOID *)(dp + 4), n * sizeof(CHAR16));
            if (ends_with_ci(path, n, L"bootmgfw.efi")) return 1;
        }
        dp += len;
    }
    return 0;
}

/* Returns only if it could not hand Windows to the firmware; the caller then
 * chainloads as usual. Booting straight from the firmware entry keeps shim and
 * Visor out of Windows' measured boot (PCR 4/7), so BitLocker does not ask for
 * its recovery key after a Visor or shim update. */
static void windows_try_bootnext(void) {
    UINTN order_size = 0;
    UINT16 *order = get_global_var(L"BootOrder", &order_size);
    { CHAR16 m[64]; SPrint(m, sizeof(m), L"bootnext: BootOrder has %d entries",
                           (int)(order ? order_size / sizeof(UINT16) : 0)); efi_log(m); }
    if (!order) {
        efi_log(L"bootnext: no BootOrder - chainloading Windows instead");
        return;
    }
    INTN found = -1;
    for (UINTN i = 0; i < order_size / sizeof(UINT16) && found < 0; i++) {
        CHAR16 name[9];
        efi_boot_var_name(name, order[i]);
        UINTN opt_size = 0;
        UINT8 *opt = get_global_var(name, &opt_size);
        if (!opt) {
            CHAR16 m[64]; SPrint(m, sizeof(m), L"bootnext: %s unreadable", name); efi_log(m);
            continue;
        }
        int match = load_option_is_bootmgfw(opt, opt_size);
        { CHAR16 m[96]; SPrint(m, sizeof(m), L"bootnext: %s size=%d bootmgfw=%d",
                               name, (int)opt_size, match); efi_log(m); }
        if (match) found = (INTN)order[i];
        efi_free_pool(opt);
    }
    efi_free_pool(order);
    if (found < 0) {
        efi_log(L"bootnext: no active Boot#### entry for bootmgfw.efi - chainloading instead");
        return;
    }

    UINT16 next = (UINT16)found;
    EFI_STATUS s = RT->SetVariable(L"BootNext", &gEfiGlobalVariableGuid,
                                   EFI_VARIABLE_NON_VOLATILE |
                                   EFI_VARIABLE_BOOTSERVICE_ACCESS |
                                   EFI_VARIABLE_RUNTIME_ACCESS,
                                   sizeof(next), &next);
    { CHAR16 bn[9]; efi_boot_var_name(bn, next);
      CHAR16 m[96]; SPrint(m, sizeof(m), L"bootnext: BootNext=%s (status=0x%lx)",
                           bn, (long)s); efi_log(m); }
    if (EFI_ERROR(s)) return;
    efi_log(L"bootnext: resetting so the firmware boots Windows directly");
    efi_log_close();
    RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
}

EFI_STATUS visor_boot(boot_entry_t *entry, EFI_SYSTEM_TABLE *st) {
    EFI_STATUS status;
    UINTN kernel_size = 0;

    if (visor_windows_bootnext && entry_is_windows(entry))
        windows_try_bootnext();

    efi_print(L"Booting: ");
    efi_print(entry->name);
    efi_print(L"\r\n");

    if (!entry->kernel_path) {
        efi_log(L"boot: no kernel path - searching for Windows Boot Manager");
        EFI_DEVICE_PATH *bootmgr_dp = NULL;
        status = windows_find_bootmgr(entry->uuid, &bootmgr_dp);
        if (EFI_ERROR(status) || !bootmgr_dp) {
            efi_log(L"ERROR: no kernel and no bootmgfw.efi found");
            efi_print(L"Nothing to boot\r\n");
            return EFI_NOT_FOUND;
        }
        EFI_HANDLE bh;
        status = BS->LoadImage(FALSE, IH, bootmgr_dp, NULL, 0, &bh);
        efi_free_pool(bootmgr_dp);
        if (EFI_ERROR(status)) {
            { CHAR16 m[96]; SPrint(m, sizeof(m),
                  L"ERROR: LoadImage failed for bootmgfw.efi (status=0x%lx)",
                  (long)status); efi_log(m); }
            if (status == EFI_SECURITY_VIOLATION || status == EFI_ACCESS_DENIED)
                efi_print(L"Secure Boot rejected bootmgfw.efi\r\n");
            efi_print(L"LoadImage failed\r\n");
            return status;
        }
        efi_log(L"boot: StartImage() - handing control to Windows Boot Manager");
        efi_log_close();
        return BS->StartImage(bh, NULL, NULL);
    }

    efi_log(L"boot: loading kernel/image file");
    efi_log(entry->kernel_path);
    UINTN volume_opens_at_handoff = efi_volume_open_count();
    CHAR16 *kernel_load_path = entry->kernel_path;
    entry_volume_t entry_vol = { NULL, NULL };
    efi_file_buffer_t *kernel_buf = load_entry_file(entry->kernel_path,
                                                    entry->uuid,
                                                    entry->hp_volume,
                                                    entry->encrypted,
                                                    entry->decrypt_password,
                                                    &kernel_load_path,
                                                    &entry_vol);
    if (!kernel_buf) {
        efi_log(L"ERROR: kernel file not found or unreadable");
        efi_print(L"Failed to load kernel\r\n");
        entry_volume_close(&entry_vol);
        clear_entry_password(entry);
        return EFI_NOT_FOUND;
    }
    void *kernel_data = kernel_buf->data;
    kernel_size = kernel_buf->size;

    if (!visor_hash_ok(entry, kernel_data, kernel_size)) {
        entry_volume_close(&entry_vol);
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        clear_entry_password(entry);
        return EFI_SECURITY_VIOLATION;
    }

    int sb = efi_secure_boot_enabled();
    int shim = efi_shim_verify(kernel_data, kernel_size);
    if (shim == 0) {
        efi_log(L"ERROR: SHIM_LOCK verification failed - refusing to boot image");
        efi_print(L"Secure Boot: image verification failed\r\n");
        entry_volume_close(&entry_vol);
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        clear_entry_password(entry);
        return EFI_SECURITY_VIOLATION;
    }
    if (shim == 1) efi_log(L"secure: image verified via SHIM_LOCK");

    CHAR16 *boot_cmdline = NULL;
    int boot_cmdline_owned = 0;
    status = luks_effective_cmdline(entry, &boot_cmdline, &boot_cmdline_owned);
    if (EFI_ERROR(status)) {
        entry_volume_close(&entry_vol);
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        clear_entry_password(entry);
        return status;
    }

    UINT8 *kernel = (UINT8*)kernel_data;

    { CHAR16 d[128]; SPrint(d, sizeof(d), L"boot: is_pe=%d size=%d kernel=%s",
           linux_is_pe_image(kernel, kernel_size), (int)kernel_size,
           kernel_load_path ? kernel_load_path : L"(null)"); efi_log(d); }
    if (boot_cmdline) {
        CHAR16 d[256]; SPrint(d, sizeof(d), L"boot: cmdline=[%s]", boot_cmdline); efi_log(d);
    } else {
        efi_log(L"boot: cmdline=(null)");
    }

    tpm_measure_cmdline(boot_cmdline);
    if (entry->initrd_path) {
        efi_log(L"boot: initrd_path:");
        efi_log(entry->initrd_path);
    } else {
        efi_log(L"boot: initrd_path=(null)");
    }

    int is_pe = linux_is_pe_image(kernel, kernel_size);
    if (is_pe) {
        EFI_HANDLE kernel_handle;

        efi_log(L"boot: PE image (Windows/UKI/EFI-stub), LoadImage()");
        EFI_DEVICE_PATH *kernel_dp = NULL;

        int dp_may_be_reread = !entry->has_sha256 && !entry->encrypted;
        if (!entry->encrypted) {
            if (entry_vol.volume) {
                kernel_dp = efi_make_file_path(entry_vol.volume, kernel_load_path);
                if (kernel_dp)
                    efi_log(L"boot: LoadImage() pinned to the volume the kernel was read from");
            }
            if (!kernel_dp && entry->hp_volume) {
                kernel_dp = efi_file_device_path_on_handle(entry->hp_volume,
                                                           kernel_load_path);
                if (kernel_dp)
                    efi_log(L"boot: LoadImage() pinned to the entry's own volume");
            }
            if (!kernel_dp) {
                kernel_dp = efi_file_device_path(kernel_load_path, entry->uuid);
                if (kernel_dp && entry->hp_volume)
                    efi_log(L"WARN: LoadImage() falling back to a path search - "
                            L"another volume may answer first");
            }
        }
        if (entry->has_sha256)
            efi_log(L"secure: sha256 pin set - the verified buffer is what runs, "
                    L"the file is not read again");
        if (!kernel_dp && !entry->initrd_path && !entry->cmdline)
            efi_log(L"WARN: this chainload (e.g. bootmgfw.efi) has no device path; "
                    L"Windows may not find its BCD");

        if (kernel_dp) {

            efi_log(L"boot: LoadImage() from the copy already in memory");
            status = BS->LoadImage(FALSE, IH, kernel_dp, kernel_data, kernel_size,
                                   &kernel_handle);
            if (EFI_ERROR(status) && status != EFI_SECURITY_VIOLATION &&
                status != EFI_ACCESS_DENIED && dp_may_be_reread) {

                CHAR16 m[112];
                SPrint(m, sizeof(m),
                       L"WARN: LoadImage(devpath+buffer) failed (status=0x%lx) - "
                       L"letting the firmware read the file",
                       (long)status);
                efi_log(m);
                status = BS->LoadImage(FALSE, IH, kernel_dp, NULL, 0, &kernel_handle);
            }
            efi_free_pool(kernel_dp);
            if (EFI_ERROR(status) && status != EFI_SECURITY_VIOLATION &&
                status != EFI_ACCESS_DENIED) {

                CHAR16 m[96];
                SPrint(m, sizeof(m),
                       L"WARN: LoadImage(devpath) failed (status=0x%lx) - retrying from buffer",
                       (long)status);
                efi_log(m);
                status = BS->LoadImage(FALSE, IH, NULL, kernel_data, kernel_size,
                                       &kernel_handle);
            }
        } else {
            efi_log(L"WARN: could not build device path - loading from source buffer");
            status = BS->LoadImage(FALSE, IH, NULL, kernel_data, kernel_size, &kernel_handle);
        }
        if (EFI_ERROR(status)) {
            { CHAR16 m[96]; SPrint(m, sizeof(m),
                  L"ERROR: LoadImage failed (status=0x%lx)", (long)status); efi_log(m); }
            if (status == EFI_SECURITY_VIOLATION || status == EFI_ACCESS_DENIED) {
                efi_log(L"ERROR: Secure Boot rejected the image (unsigned or not enrolled)");
                efi_print(L"Secure Boot rejected this image.\r\n"
                          L"Sign or enroll it (sbctl, MokManager) or disable Secure Boot.\r\n");
            }
#if defined(__x86_64__)
            if (!(sb && shim != 1)) {
                efi_log(L"boot: LoadImage failed - trying raw Linux EFI handover fallback");
                EFI_STATUS rs = linux_raw_handover(entry, st, kernel_buf,
                                                   boot_cmdline, NULL);

                efi_log(L"boot: raw handover fallback did not boot");
                status = rs;
            }
#endif
            efi_print(L"LoadImage failed\r\n");
            entry_volume_close(&entry_vol);
            free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
            if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
            clear_entry_password(entry);
            return status;
        }
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);

        if (boot_cmdline) {
            EFI_LOADED_IMAGE *loaded;
            status = BS->HandleProtocol(kernel_handle, &gEfiLoadedImageProtocolGuid, (void**)&loaded);
            if (!EFI_ERROR(status)) {
                loaded->LoadOptions     = boot_cmdline;
                loaded->LoadOptionsSize = (UINT32)((efi_strlen16(boot_cmdline) + 1) * sizeof(CHAR16));
                efi_log(L"linux: cmdline set via LoadOptions");
            }
        }

        efi_file_buffer_t *initrd_buf = NULL;
        EFI_HANDLE initrd_handle = NULL;
        if (entry->initrd_path) {
            efi_log(L"linux: loading initrd for stub (LINUX_EFI_INITRD_MEDIA)");
            efi_log(entry->initrd_path);
            initrd_buf = load_entry_file(entry->initrd_path, entry->uuid,
                                         entry->hp_volume,
                                         entry->initrd_encrypted,
                                         entry->decrypt_password, NULL,
                                         &entry_vol);
            if (initrd_buf && initrd_buf->data && initrd_buf->size) {
                status = luks_append_keyfile(entry, &initrd_buf);
                if (EFI_ERROR(status)) {
                    entry_volume_close(&entry_vol);
                    free_file_buffer_maybe_wipe(initrd_buf, entry->initrd_encrypted || entry->luks);
                    if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
                    clear_entry_password(entry);
                    return status;
                }
                initrd_handle = initrd_register(initrd_buf->data, initrd_buf->size);
                if (initrd_handle) {
                    CHAR16 m[72];
                    SPrint(m, sizeof(m), L"linux: initrd registered OK, %d bytes",
                           (int)initrd_buf->size);
                    efi_log(m);
                } else {
                    efi_log(L"ERROR: initrd LoadFile2 registration FAILED");
                    if (entry->luks) {
                        efi_log(L"ERROR: luks=1 requires initrd LoadFile2 registration");
                        efi_print(L"LUKS initrd setup failed\r\n");
                        entry_volume_close(&entry_vol);
                        free_file_buffer_maybe_wipe(initrd_buf, entry->initrd_encrypted || entry->luks);
                        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
                        clear_entry_password(entry);
                        return EFI_SECURITY_VIOLATION;
                    }
                }
            } else {
                if (entry->initrd_encrypted || entry->luks) {
                    efi_log(L"ERROR: required initrd failed - refusing to boot");
                    entry_volume_close(&entry_vol);
                    free_file_buffer_maybe_wipe(initrd_buf, entry->initrd_encrypted || entry->luks);
                    if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
                    clear_entry_password(entry);
                    return EFI_SECURITY_VIOLATION;
                }
                efi_log(L"WARN: initrd load failed - continuing without it");
                efi_log(entry->initrd_path ? entry->initrd_path : L"(null path)");
                efi_print(L"Warning: Could not load initrd\r\n");
            }
        } else if (entry->luks) {
            efi_log(L"linux: building supplemental LUKS keyfile initrd for PE/UKI");
            status = luks_build_keyfile_archive(entry, &initrd_buf);
            if (EFI_ERROR(status)) {
                entry_volume_close(&entry_vol);
                if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
                clear_entry_password(entry);
                return status;
            }
            initrd_handle = initrd_register(initrd_buf->data, initrd_buf->size);
            if (!initrd_handle) {
                efi_log(L"ERROR: could not install supplemental LUKS initrd LoadFile2 protocol");
                efi_print(L"LUKS initrd setup failed\r\n");
                entry_volume_close(&entry_vol);
                free_file_buffer_wipe(initrd_buf);
                if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
                clear_entry_password(entry);
                return EFI_SECURITY_VIOLATION;
            }
            efi_log(L"luks: supplemental keyfile initrd ready");
        }

        { CHAR16 m[80]; SPrint(m, sizeof(m),
            L"linux: about to StartImage, initrd_handle=%d, initrd_size=%d",
            initrd_handle ? 1 : 0,
            initrd_buf ? (int)initrd_buf->size : 0);
          efi_log(m); }
        { CHAR16 m[80]; SPrint(m, sizeof(m),
            L"linux: %d volume open(s) for this boot, %d this session",
            (int)(efi_volume_open_count() - volume_opens_at_handoff),
            (int)efi_volume_open_count());
          efi_log(m); }
        entry_volume_close(&entry_vol);
        efi_log(L"linux: StartImage() - handing control to kernel stub");
        efi_log_close();
        clear_entry_password(entry);
        status = BS->StartImage(kernel_handle, NULL, NULL);

        { CHAR16 m[80]; SPrint(m, sizeof(m), L"linux: StartImage returned 0x%lx", (long)status);
          efi_log(m); }
        efi_print(L"ERROR: kernel StartImage returned - boot failed\r\n");
        if (initrd_handle) initrd_unregister(initrd_handle);
        BS->UnloadImage(kernel_handle);
        free_file_buffer_maybe_wipe(initrd_buf, entry->initrd_encrypted || entry->luks);
        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
        return status;
    }

 #if defined(__x86_64__)
    if (sb && shim != 1) {
        efi_log(L"ERROR: Secure Boot on but raw kernel is unverifiable (no SHIM_LOCK) - refusing");
        efi_print(L"Secure Boot: refusing unverified kernel\r\n");
        entry_volume_close(&entry_vol);
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
        clear_entry_password(entry);
        return EFI_SECURITY_VIOLATION;
    }

    entry_volume_close(&entry_vol);
    status = linux_raw_handover(entry, st, kernel_buf, boot_cmdline, NULL);
    if (!visor_boot_services_active) return status;
    efi_print(L"Raw kernel handover failed\r\n");
    free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
    if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
    clear_entry_password(entry);
    return status;
 #else
    (void)sb; (void)st;
    efi_log(L"ERROR: image is not an EFI application; only EFI-stub kernels/UKIs are supported");
    efi_print(L"Not an EFI-stub kernel (use a UKI or stub kernel)\r\n");
    entry_volume_close(&entry_vol);
    free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
    if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
    clear_entry_password(entry);
    return EFI_UNSUPPORTED;
 #endif
}
