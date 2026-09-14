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

EFI_STATUS visor_boot(boot_entry_t *entry, EFI_SYSTEM_TABLE *st) {
    EFI_STATUS status;
    UINTN kernel_size = 0;

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
