/* linux_rawboot.c - x86 bzImage handover for non-EFI-stub kernels (feature: rawboot) */
#include "linux_internal.h"

#if defined(__x86_64__)

#define LINUX_MAX_RAW_INIT_SIZE (1024ULL * 1024ULL * 1024ULL)
#define LINUX_MAX_RAW_CMDLINE   (1024ULL * 1024ULL)

static EFI_STATUS linux_alloc_below_4g(EFI_MEMORY_TYPE type, UINTN pages,
                                       EFI_PHYSICAL_ADDRESS *address) {
    if (!pages || !address) return EFI_INVALID_PARAMETER;
    *address = 0xFFFFFFFFULL;
    return BS->AllocatePages(AllocateMaxAddress, type, pages, address);
}

static EFI_STATUS linux_alloc_raw_image(UINTN kernel_size,
                                        const setup_header_t *source,
                                        EFI_PHYSICAL_ADDRESS *allocation,
                                        UINTN *allocation_pages,
                                        EFI_PHYSICAL_ADDRESS *image) {
    UINT64 alignment = source->kernel_alignment;
    UINT64 required = source->init_size;
    UINT64 pref = source->pref_address;
    UINT64 total;

    if (!source->relocatable_kernel && !pref)
        return EFI_INVALID_PARAMETER;
    if (!linux_power_of_two(alignment) || alignment < 0x1000 ||
        alignment > 0x40000000ULL)
        return EFI_INVALID_PARAMETER;
    if (source->min_alignment < 31 &&
        (1ULL << source->min_alignment) > alignment)
        alignment = 1ULL << source->min_alignment;
    if (required < kernel_size) required = kernel_size;
    if (!required || required > LINUX_MAX_RAW_INIT_SIZE ||
        required > (UINT64)(~(UINTN)0))
        return EFI_INVALID_PARAMETER;
    if (required > ~(UINT64)0 - (alignment - 1)) return EFI_INVALID_PARAMETER;
    total = required + alignment - 1;
    if (total > ~(UINT64)0 - 0xFFF) return EFI_INVALID_PARAMETER;

    UINTN pages = EFI_SIZE_TO_PAGES((UINTN)total);
    EFI_PHYSICAL_ADDRESS base = 0;
    EFI_STATUS status;

    if (source->relocatable_kernel && pref &&
        !(pref & (alignment - 1)) &&
        pref <= 0xFFFFFFFFULL &&
        pref + required <= 0x100000000ULL) {
        base = (EFI_PHYSICAL_ADDRESS)pref;
        status = BS->AllocatePages(AllocateAddress, EfiLoaderCode, pages, &base);
        if (!EFI_ERROR(status)) {
            *allocation = base;
            *allocation_pages = pages;
            *image = base;
            return EFI_SUCCESS;
        }
    }

    status = linux_alloc_below_4g(EfiLoaderCode, pages, &base);
    if (EFI_ERROR(status)) return status;

    EFI_PHYSICAL_ADDRESS aligned = (base + alignment - 1) & ~(alignment - 1);
    UINT64 end = (UINT64)aligned + required;
    if (aligned < base || end < aligned || end > (UINT64)base +
        (UINT64)pages * EFI_PAGE_SIZE || aligned < pref || end > 0x100000000ULL) {
        BS->FreePages(base, pages);
        return EFI_OUT_OF_RESOURCES;
    }

    *allocation = base;
    *allocation_pages = pages;
    *image = aligned;
    return EFI_SUCCESS;
}

static EFI_STATUS linux_place_raw_initrd(efi_file_buffer_t *buf,
                                         const setup_header_t *source,
                                         UINT32 *address, UINT32 *size,
                                         EFI_PHYSICAL_ADDRESS *pages_addr,
                                         UINTN *pages) {
    if (!buf || !buf->data || !buf->size || !source || !address || !size ||
        !pages_addr || !pages || buf->size > 0xFFFFFFFFULL)
        return EFI_INVALID_PARAMETER;

    UINTN npages = EFI_SIZE_TO_PAGES(buf->size);
    EFI_PHYSICAL_ADDRESS max = source->initrd_addr_max
        ? source->initrd_addr_max : 0xFFFFFFFFULL;
    EFI_PHYSICAL_ADDRESS addr = max;
    EFI_STATUS status = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                           npages, &addr);
    if (EFI_ERROR(status)) return status;

    UINT64 end = (UINT64)addr + buf->size;
    if (addr > 0xFFFFFFFFULL || end < addr || end > 0x100000000ULL ||
        end - 1 > max) {
        BS->FreePages(addr, npages);
        return EFI_OUT_OF_RESOURCES;
    }
    CopyMem((void *)(UINTN)addr, buf->data, buf->size);
    *address = (UINT32)addr;
    *size = (UINT32)buf->size;
    *pages_addr = addr;
    *pages = npages;
    return EFI_SUCCESS;
}

static EFI_STATUS linux_make_cmdline(CHAR16 *cmdline, UINT32 max_size,
                                     UINT32 *address, EFI_PHYSICAL_ADDRESS *pages_addr,
                                     UINTN *pages) {
    if (!address || !pages_addr || !pages) return EFI_INVALID_PARAMETER;
    *address = 0;
    *pages_addr = 0;
    *pages = 0;
    if (!cmdline || !cmdline[0]) return EFI_SUCCESS;

    UINTN len = efi_strlen16(cmdline);
    if (len > LINUX_MAX_RAW_CMDLINE || (max_size && len > max_size))
        return EFI_INVALID_PARAMETER;
    if (len == ~(UINTN)0) return EFI_INVALID_PARAMETER;

    UINTN npages = EFI_SIZE_TO_PAGES(len + 1);
    EFI_PHYSICAL_ADDRESS addr;
    EFI_STATUS status = linux_alloc_below_4g(EfiLoaderData, npages, &addr);
    if (EFI_ERROR(status)) return status;
    CHAR8 *out = (CHAR8 *)(UINTN)addr;
    for (UINTN i = 0; i < len; i++) {
        if (cmdline[i] > 0x7F) {
            BS->FreePages(addr, npages);
            return EFI_INVALID_PARAMETER;
        }
        out[i] = (CHAR8)cmdline[i];
    }
    out[len] = 0;
    *address = (UINT32)addr;
    *pages_addr = addr;
    *pages = npages;
    return EFI_SUCCESS;
}

typedef EFI_STATUS (*linux_handover_t)(EFI_HANDLE image_handle,
                                       EFI_SYSTEM_TABLE *system_table,
                                       VOID *boot_params);

EFI_STATUS linux_raw_handover(boot_entry_t *entry, EFI_SYSTEM_TABLE *st,
                                     efi_file_buffer_t *kernel_buf,
                                     CHAR16 *boot_cmdline,
                                     EFI_STATUS *status_out) {
    UINT8 *source_data = (UINT8 *)kernel_buf->data;
    UINTN source_size = kernel_buf->size;
    if (source_size < LINUX_SETUP_HEADER_OFFSET + sizeof(setup_header_t) ||
        source_size < 0x268)
        return EFI_INVALID_PARAMETER;

    setup_header_t source;
    CopyMem(&source, source_data + LINUX_SETUP_HEADER_OFFSET, sizeof(source));
    if (source.boot_flag != LINUX_BOOT_FLAG_MAGIC ||
        source.header != LINUX_SIGNATURE || source.version < 0x020B ||
        !(source.xloadflags & LINUX_XLF_HANDOVER_64) ||
        !source.handover_offset)
        return EFI_UNSUPPORTED;
    UINTN setup_sects = source.setup_sects ? source.setup_sects : 4;
    if (setup_sects > (UINTN)(~(UINTN)0) / 0x200 - 1)
        return EFI_INVALID_PARAMETER;
    UINTN startup_offset = (setup_sects + 1) * 0x200;
    if (startup_offset > source_size ||
        source.handover_offset < startup_offset ||
        source.handover_offset >= source_size)
        return EFI_INVALID_PARAMETER;

    EFI_PHYSICAL_ADDRESS image_alloc = 0, image = 0;
    UINTN image_pages = 0;
    EFI_STATUS status = linux_alloc_raw_image(source_size, &source,
                                              &image_alloc, &image_pages, &image);
    if (EFI_ERROR(status)) return status;
    CopyMem((void *)(UINTN)image, source_data, source_size);

    EFI_PHYSICAL_ADDRESS bp_addr = 0;
    status = linux_alloc_below_4g(EfiLoaderData,
                                  EFI_SIZE_TO_PAGES(LINUX_BOOT_PARAMS_SIZE),
                                  &bp_addr);
    if (EFI_ERROR(status)) {
        BS->FreePages(image_alloc, image_pages);
        return status;
    }
    SetMem((void *)(UINTN)bp_addr, LINUX_BOOT_PARAMS_SIZE, 0);

    UINT32 initrd_addr = 0, initrd_size = 0;
    EFI_PHYSICAL_ADDRESS initrd_pages_addr = 0;
    UINTN initrd_pages = 0;
    efi_file_buffer_t *initrd_buf = NULL;
    if (entry->initrd_path) {
        efi_log(L"linux: loading initrd for raw handover");
        efi_log(entry->initrd_path);
        initrd_buf = load_entry_file(entry->initrd_path, entry->uuid,
                                     entry->hp_volume,
                                     entry->initrd_encrypted,
                                     entry->decrypt_password, NULL, NULL);
        if (!initrd_buf) {
            status = (entry->initrd_encrypted || entry->luks)
                ? EFI_SECURITY_VIOLATION : EFI_NOT_FOUND;
        } else {
            status = luks_append_keyfile(entry, &initrd_buf);
            if (!EFI_ERROR(status))
                status = linux_place_raw_initrd(initrd_buf, &source,
                                                &initrd_addr, &initrd_size,
                                                &initrd_pages_addr, &initrd_pages);
        }
        free_file_buffer_maybe_wipe(initrd_buf,
                                    entry->initrd_encrypted || entry->luks);
        initrd_buf = NULL;
        if (EFI_ERROR(status)) {
            if (initrd_pages_addr) BS->FreePages(initrd_pages_addr, initrd_pages);
            BS->FreePages(bp_addr, EFI_SIZE_TO_PAGES(LINUX_BOOT_PARAMS_SIZE));
            BS->FreePages(image_alloc, image_pages);
            return status;
        }
    } else if (entry->luks) {
        BS->FreePages(bp_addr, EFI_SIZE_TO_PAGES(LINUX_BOOT_PARAMS_SIZE));
        BS->FreePages(image_alloc, image_pages);
        efi_log(L"ERROR: raw kernel luks=1 requires initrd=");
        return EFI_INVALID_PARAMETER;
    }

    UINT32 cmdline_addr = 0;
    EFI_PHYSICAL_ADDRESS cmdline_pages_addr = 0;
    UINTN cmdline_pages = 0;
    status = linux_make_cmdline(boot_cmdline, source.cmdline_size,
                                &cmdline_addr, &cmdline_pages_addr,
                                &cmdline_pages);
    if (EFI_ERROR(status)) {
        if (initrd_pages_addr) BS->FreePages(initrd_pages_addr, initrd_pages);
        BS->FreePages(bp_addr, EFI_SIZE_TO_PAGES(LINUX_BOOT_PARAMS_SIZE));
        BS->FreePages(image_alloc, image_pages);
        return status;
    }

    setup_header_t *bp_hdr = (setup_header_t *)((UINT8 *)(UINTN)bp_addr +
                                                LINUX_SETUP_HEADER_OFFSET);
    bp_hdr->type_of_loader = 0xFF;
    bp_hdr->loadflags |= 0x80;
    bp_hdr->heap_end_ptr = 0xe000 - 0x200;
    bp_hdr->code32_start = (UINT32)((UINTN)image + startup_offset);
    if (cmdline_addr) bp_hdr->cmd_line_ptr = cmdline_addr;
    if (initrd_addr) {
        bp_hdr->ramdisk_image = initrd_addr;
        bp_hdr->ramdisk_size = initrd_size;
    }

    efi_log(L"linux: raw handover prepared; entering kernel EFI stub");
    efi_log_close();
    clear_entry_password(entry);

    UINTN entry_offset = source.handover_offset + 0x200;
    linux_handover_t handover = (linux_handover_t)((UINT8 *)(UINTN)image +
                                                    entry_offset);
    status = handover(IH, st, (VOID *)(UINTN)bp_addr);
    (void)status;

    if (cmdline_pages_addr) BS->FreePages(cmdline_pages_addr, cmdline_pages);
    if (initrd_pages_addr) BS->FreePages(initrd_pages_addr, initrd_pages);
    BS->FreePages(bp_addr, EFI_SIZE_TO_PAGES(LINUX_BOOT_PARAMS_SIZE));
    BS->FreePages(image_alloc, image_pages);
    if (status_out) *status_out = EFI_LOAD_ERROR;
    return EFI_LOAD_ERROR;
}

#endif
