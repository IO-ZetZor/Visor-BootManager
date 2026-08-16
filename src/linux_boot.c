#include "linux_boot.h"
#include "windows_boot.h"
#include "efi_helpers.h"
#include "hash_verify.h"
#include "crypto.h"
#include "path_compat.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;
extern EFI_HANDLE IH;

#define LUKS_DEFAULT_KEY_PATH      L"/crypto_keyfile.bin"
#define CPIO_NEWC_HEADER_SIZE      110u

static UINTN strlen16(CHAR16 *s) {
    UINTN len = 0;
    while (s[len]) len++;
    return len;
}

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

#define LINUX_EFI_INITRD_MEDIA_GUID \
    { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } }

typedef struct visor_lf2_protocol visor_lf2_protocol_t;
typedef EFI_STATUS (EFIAPI *visor_lf2_load_t)(visor_lf2_protocol_t *This,
                                              EFI_DEVICE_PATH_PROTOCOL *FilePath,
                                              BOOLEAN BootPolicy,
                                              UINTN *BufferSize, VOID *Buffer);
struct visor_lf2_protocol { visor_lf2_load_t LoadFile; };

static EFI_GUID visor_load_file2_guid =
    { 0x4006c0c1, 0xfcb3, 0x403e, { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d } };

typedef struct {
    VENDOR_DEVICE_PATH vendor;
    EFI_DEVICE_PATH    end;
} initrd_dev_path_t;

static void  *g_initrd_data = NULL;
static UINTN  g_initrd_size = 0;

static EFI_STATUS EFIAPI initrd_load_file(visor_lf2_protocol_t *This,
                                          EFI_DEVICE_PATH_PROTOCOL *FilePath,
                                          BOOLEAN BootPolicy, UINTN *BufferSize, VOID *Buffer) {
    (void)This; (void)FilePath;
    if (BootPolicy) return EFI_UNSUPPORTED;
    if (!BufferSize) return EFI_INVALID_PARAMETER;
    if (!g_initrd_data || g_initrd_size == 0) return EFI_NOT_FOUND;
    if (Buffer == NULL || *BufferSize < g_initrd_size) {
        *BufferSize = g_initrd_size;
        return EFI_BUFFER_TOO_SMALL;
    }
    CopyMem(Buffer, g_initrd_data, g_initrd_size);
    *BufferSize = g_initrd_size;
    return EFI_SUCCESS;
}

static visor_lf2_protocol_t initrd_lf2 = { initrd_load_file };
static initrd_dev_path_t initrd_dp = {
    .vendor = {
        .Header = { MEDIA_DEVICE_PATH, MEDIA_VENDOR_DP,
                    { (UINT8)(sizeof(VENDOR_DEVICE_PATH) & 0xFF),
                      (UINT8)((sizeof(VENDOR_DEVICE_PATH) >> 8) & 0xFF) } },
        .Guid = LINUX_EFI_INITRD_MEDIA_GUID
    },
    .end = { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
             { (UINT8)sizeof(EFI_DEVICE_PATH), 0 } }
};

static EFI_HANDLE initrd_register(void *data, UINTN size) {
    g_initrd_data = data;
    g_initrd_size = size;
    EFI_HANDLE h = NULL;
    EFI_STATUS s = BS->InstallMultipleProtocolInterfaces(&h,
        &gEfiDevicePathProtocolGuid, &initrd_dp,
        &visor_load_file2_guid, &initrd_lf2,
        NULL);
    if (EFI_ERROR(s)) { g_initrd_data = NULL; g_initrd_size = 0; return NULL; }
    return h;
}

static void initrd_unregister(EFI_HANDLE h) {
    if (h)
        BS->UninstallMultipleProtocolInterfaces(h,
            &gEfiDevicePathProtocolGuid, &initrd_dp,
            &visor_load_file2_guid, &initrd_lf2,
            NULL);
    g_initrd_data = NULL;
    g_initrd_size = 0;
}

#if defined(__x86_64__)

static void free_file_buffer_maybe_wipe(efi_file_buffer_t *buf, int sensitive);
static efi_file_buffer_t* load_entry_file(CHAR16 *path, CHAR16 *uuid,
                                           EFI_HANDLE volume,
                                           int encrypted, CHAR16 *password,
                                           CHAR16 **resolved_path);
static void clear_entry_password(boot_entry_t *entry);
static EFI_STATUS luks_append_keyfile(boot_entry_t *entry,
                                      efi_file_buffer_t **buf_io);

#define LINUX_MAX_RAW_INIT_SIZE (1024ULL * 1024ULL * 1024ULL)
#define LINUX_MAX_RAW_CMDLINE   (1024ULL * 1024ULL)

static int linux_power_of_two(UINT64 value) {
    return value && !(value & (value - 1));
}

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

    UINTN len = strlen16(cmdline);
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

static EFI_STATUS linux_raw_handover(boot_entry_t *entry, EFI_SYSTEM_TABLE *st,
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
                                     entry->decrypt_password, NULL);
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

static void free_file_buffer(efi_file_buffer_t *buf) {
    if (!buf) return;
    if (buf->data) efi_free_pool(buf->data);
    efi_free_pool(buf);
}

static void free_file_buffer_wipe(efi_file_buffer_t *buf) {
    if (!buf) return;
    if (buf->data) {
        volatile UINT8 *p = (volatile UINT8*)buf->data;
        UINTN n = buf->size;
        while (n--) *p++ = 0;
        efi_free_pool(buf->data);
    }
    efi_free_pool(buf);
}

static void free_file_buffer_maybe_wipe(efi_file_buffer_t *buf, int sensitive) {
    if (sensitive) free_file_buffer_wipe(buf);
    else free_file_buffer(buf);
}

static int entry_file_path_exists(CHAR16 *path, CHAR16 *uuid) {
    efi_file_t *file = efi_fopen_uuid(path, uuid);
    if (!file) return 0;
    efi_fclose(file);
    return 1;
}

static efi_file_buffer_t* load_entry_file(CHAR16 *path, CHAR16 *uuid,
                                          EFI_HANDLE volume,
                                          int encrypted, CHAR16 *password,
                                          CHAR16 **resolved_path) {

    if (resolved_path) *resolved_path = path;
    CHAR16 *root_path = visor_path_without_boot_mount(path);

    efi_file_buffer_t *buf = NULL;
    if (volume) {
        buf = efi_load_file_on_handle(volume, path);
        if (!buf && root_path) {
            buf = efi_load_file_on_handle(volume, root_path);
            if (buf) {
                if (resolved_path) *resolved_path = root_path;
                efi_log(L"boot: resolved path after removing host /boot mount prefix");
                efi_log(root_path);
            }
        }
        if (buf) {
            efi_log(L"boot: file loaded from the volume this entry was detected on");
            goto have_buffer;
        }
        efi_log(L"WARN: file not found on the entry's own volume - searching by path");
    }

    buf = efi_load_file_uuid(path, uuid);

    if (!buf && root_path && !entry_file_path_exists(path, uuid)) {
        buf = efi_load_file_uuid(root_path, uuid);
        if (buf) {
            if (resolved_path) *resolved_path = root_path;
            efi_log(L"boot: resolved path after removing host /boot mount prefix");
            efi_log(root_path);
        }
    }

    if (!buf && uuid && uuid[0]) {
        efi_log(L"WARN: file not found on partition uuid= - searching all volumes");
        buf = efi_load_file(path);
        if (!buf && root_path && !entry_file_path_exists(path, NULL)) {
            buf = efi_load_file(root_path);
            if (buf) {
                if (resolved_path) *resolved_path = root_path;
                efi_log(L"boot: resolved path after removing host /boot mount prefix");
                efi_log(root_path);
            }
        }
    }

have_buffer:
    if (!buf) return NULL;
    if (!encrypted) return buf;

    void *plain = NULL;
    UINTN plain_size = 0;
    EFI_STATUS s = visor_decrypt_buffer(buf->data, buf->size, password,
                                        &plain, &plain_size);
    free_file_buffer(buf);
    if (EFI_ERROR(s)) {
        if (s == EFI_SECURITY_VIOLATION) {
            efi_log(L"ERROR: encrypted file password/integrity check failed");
            efi_print(L"Encrypted file could not be decrypted\r\n");
        } else {
            efi_log(L"ERROR: encrypted file container is invalid or unsupported");
            efi_print(L"Encrypted file is invalid\r\n");
        }
        return NULL;
    }

    efi_file_buffer_t *out = efi_allocate_pool(sizeof(efi_file_buffer_t));
    if (!out) {
        volatile UINT8 *p = (volatile UINT8*)plain;
        UINTN n = plain_size;
        while (n--) *p++ = 0;
        efi_free_pool(plain);
        return NULL;
    }
    out->data = plain;
    out->size = plain_size;
    efi_log(L"crypto: encrypted file decrypted");
    return out;
}

static UINTN strlen8(const CHAR8 *s) {
    UINTN len = 0;
    while (s[len]) len++;
    return len;
}

static UINTN pad4(UINTN n) {
    return (4u - (n & 3u)) & 3u;
}

static void wipe_bytes(void *buf, UINTN size) {
    if (!buf) return;
    volatile UINT8 *p = (volatile UINT8*)buf;
    while (size--) *p++ = 0;
}

static void clear_entry_password(boot_entry_t *entry) {
    if (!entry || !entry->decrypt_password) return;
    volatile CHAR16 *p = (volatile CHAR16*)entry->decrypt_password;
    while (*p) *p++ = 0;
    efi_free_pool(entry->decrypt_password);
    entry->decrypt_password = NULL;
}

static UINTN utf8_size16(CHAR16 *s) {
    UINTN n = 0;
    if (!s) return 0;
    for (UINTN i = 0; s[i]; i++) {
        UINT16 c = s[i];
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else n += 3;
    }
    return n;
}

static CHAR8* utf8_from16(CHAR16 *s, UINTN *out_len) {
    UINTN len = utf8_size16(s);
    CHAR8 *out = efi_allocate_pool(len ? len : 1);
    if (!out) return NULL;

    UINTN k = 0;
    if (s) {
        for (UINTN i = 0; s[i]; i++) {
            UINT16 c = s[i];
            if (c < 0x80) {
                out[k++] = (CHAR8)c;
            } else if (c < 0x800) {
                out[k++] = (CHAR8)(0xC0 | (c >> 6));
                out[k++] = (CHAR8)(0x80 | (c & 0x3F));
            } else {
                out[k++] = (CHAR8)(0xE0 | (c >> 12));
                out[k++] = (CHAR8)(0x80 | ((c >> 6) & 0x3F));
                out[k++] = (CHAR8)(0x80 | (c & 0x3F));
            }
        }
    }
    if (out_len) *out_len = k;
    return out;
}

static CHAR8* cpio_name_from_path(CHAR16 *path) {
    CHAR16 *src = (path && path[0]) ? path : LUKS_DEFAULT_KEY_PATH;
    while (*src == L'/' || *src == L'\\') src++;
    if (!*src) src = L"crypto_keyfile.bin";

    int last_slash_check = 1;
    UINTN cap = 0;
    while (src[cap]) {
        CHAR16 c = src[cap];
        int slash = (c == L'/' || c == L'\\');
        if (c > 0x7F || c < 0x20) return NULL;
        if (!slash && (c == L':' || c == L'*' || c == L'?' ||
                       c == L'"' || c == L'<' || c == L'>' || c == L'|'))
            return NULL;
        if (!slash && c == L'.' &&
            (last_slash_check || src[cap + 1] == 0 ||
             src[cap + 1] == L'/' || src[cap + 1] == L'\\'))
            return NULL;
        last_slash_check = slash;
        cap++;
    }

    CHAR8 *out = efi_allocate_pool(cap + 1);
    if (!out) return NULL;

    UINTN k = 0;
    int last_slash = 0;
    for (UINTN i = 0; src[i]; i++) {
        CHAR16 c = src[i];
        int slash = (c == L'/' || c == L'\\');
        if (slash) {
            if (last_slash) continue;
            out[k++] = '/';
            last_slash = 1;
        } else {
            out[k++] = (CHAR8)c;
            last_slash = 0;
        }
    }
    while (k && out[k - 1] == '/') k--;
    if (!k) {
        efi_free_pool(out);
        return NULL;
    }
    out[k] = '\0';
    return out;
}

static void cpio_hex8(UINT8 *p, UINT32 v) {
    static const CHAR8 hex[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        p[7 - i] = (UINT8)hex[(v >> (i * 4)) & 0xFu];
    }
}

static UINT8* cpio_header(UINT8 *p, UINT32 ino, UINT32 mode, UINT32 nlink,
                          UINT32 filesize, UINT32 namesize) {
    CopyMem(p, "070701", 6);
    p += 6;
    cpio_hex8(p, ino);       p += 8;
    cpio_hex8(p, mode);      p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, nlink);     p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, filesize);  p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, namesize);  p += 8;
    cpio_hex8(p, 0);         p += 8;
    return p;
}

static UINTN cpio_entry_size(UINTN name_len, UINTN file_size) {
    UINTN n = CPIO_NEWC_HEADER_SIZE + name_len + 1;
    n += pad4(n) + file_size;
    return n + pad4(n);
}

static int add_overflow_uintn(UINTN a, UINTN b, UINTN *out) {
    if (~(UINTN)0 - a < b) return 1;
    *out = a + b;
    return 0;
}

static UINT8* cpio_write_entry(UINT8 *p, CHAR8 *name, UINT32 mode,
                               UINT32 nlink, UINT32 ino, UINT8 *data,
                               UINTN data_size) {
    UINT8 *start = p;
    UINTN name_len = strlen8(name);

    p = cpio_header(p, ino, mode, nlink, (UINT32)data_size, (UINT32)(name_len + 1));
    CopyMem(p, name, name_len);
    p[name_len] = 0;
    p += name_len + 1;

    UINTN pad = pad4((UINTN)(p - start));
    for (UINTN i = 0; i < pad; i++) *p++ = 0;

    if (data_size) {
        CopyMem(p, data, data_size);
        p += data_size;
    }
    pad = pad4((UINTN)(p - start));
    for (UINTN i = 0; i < pad; i++) *p++ = 0;
    return p;
}

static UINTN cpio_parent_dirs_size(CHAR8 *name) {
    UINTN total = 0;
    for (UINTN i = 0; name[i]; i++) {
        if (name[i] == '/' && i > 0) total += cpio_entry_size(i, 0);
    }
    return total;
}

static UINT8* cpio_write_parent_dirs(UINT8 *p, CHAR8 *name, UINT32 *ino) {
    for (UINTN i = 0; name[i]; i++) {
        if (name[i] != '/' || i == 0) continue;
        name[i] = '\0';
        p = cpio_write_entry(p, name, 0x000041EDu, 2, (*ino)++, NULL, 0);
        name[i] = '/';
    }
    return p;
}

static EFI_STATUS luks_build_keyfile_archive(boot_entry_t *entry,
                                             efi_file_buffer_t **out_buf) {
    if (!entry || !out_buf) return EFI_INVALID_PARAMETER;
    *out_buf = NULL;
    if (!entry->decrypt_password) {
        efi_log(L"ERROR: luks=1 but no password was captured");
        efi_print(L"LUKS password missing\r\n");
        return EFI_SECURITY_VIOLATION;
    }

    CHAR8 *name = cpio_name_from_path(entry->luks_key_path);
    if (!name) {
        efi_log(L"ERROR: invalid luks_key_path (must be a safe ASCII relative path)");
        efi_print(L"Invalid LUKS key path\r\n");
        return EFI_INVALID_PARAMETER;
    }

    UINTN pass_len = 0;
    CHAR8 *pass = utf8_from16(entry->decrypt_password, &pass_len);
    if (!pass) {
        efi_free_pool(name);
        return EFI_OUT_OF_RESOURCES;
    }

    static CHAR8 trailer[] = "TRAILER!!!";
    UINTN name_len = strlen8(name);
    UINTN archive_size = 0;
    UINTN n1 = cpio_parent_dirs_size(name);
    UINTN n2 = cpio_entry_size(name_len, pass_len);
    UINTN n3 = cpio_entry_size(strlen8(trailer), 0);
    if (add_overflow_uintn(n1, n2, &archive_size) ||
        add_overflow_uintn(archive_size, n3, &archive_size)) {
        wipe_bytes(pass, pass_len);
        efi_free_pool(pass);
        efi_free_pool(name);
        return EFI_INVALID_PARAMETER;
    }

    efi_file_buffer_t *out = efi_allocate_pool(sizeof(efi_file_buffer_t));
    if (!out) {
        wipe_bytes(pass, pass_len);
        efi_free_pool(pass);
        efi_free_pool(name);
        return EFI_OUT_OF_RESOURCES;
    }
    out->data = efi_allocate_pool(archive_size);
    if (!out->data) {
        efi_free_pool(out);
        wipe_bytes(pass, pass_len);
        efi_free_pool(pass);
        efi_free_pool(name);
        return EFI_OUT_OF_RESOURCES;
    }
    out->size = archive_size;

    UINT8 *dst = (UINT8*)out->data;
    UINT32 ino = 1;
    dst = cpio_write_parent_dirs(dst, name, &ino);
    dst = cpio_write_entry(dst, name, 0x00008180u, 1, ino++, (UINT8*)pass, pass_len);
    dst = cpio_write_entry(dst, trailer, 0, 1, ino++, NULL, 0);

    wipe_bytes(pass, pass_len);
    efi_free_pool(pass);
    efi_free_pool(name);

    *out_buf = out;
    return EFI_SUCCESS;
}

static EFI_STATUS luks_append_keyfile(boot_entry_t *entry, efi_file_buffer_t **buf_io) {
    if (!entry || !entry->luks) return EFI_SUCCESS;
    if (!buf_io || !*buf_io || !(*buf_io)->data || !(*buf_io)->size)
        return EFI_INVALID_PARAMETER;

    efi_file_buffer_t *archive = NULL;
    EFI_STATUS status = luks_build_keyfile_archive(entry, &archive);
    if (EFI_ERROR(status)) return status;

    UINTN prefix_pad = pad4((*buf_io)->size);
    UINTN new_size = 0;
    if (add_overflow_uintn((*buf_io)->size, prefix_pad, &new_size) ||
        add_overflow_uintn(new_size, archive->size, &new_size)) {
        free_file_buffer_wipe(archive);
        return EFI_INVALID_PARAMETER;
    }

    efi_file_buffer_t *out = efi_allocate_pool(sizeof(efi_file_buffer_t));
    if (!out) {
        free_file_buffer_wipe(archive);
        return EFI_OUT_OF_RESOURCES;
    }
    out->data = efi_allocate_pool(new_size);
    if (!out->data) {
        efi_free_pool(out);
        free_file_buffer_wipe(archive);
        return EFI_OUT_OF_RESOURCES;
    }
    out->size = new_size;

    UINT8 *dst = (UINT8*)out->data;
    CopyMem(dst, (*buf_io)->data, (*buf_io)->size);
    dst += (*buf_io)->size;
    for (UINTN i = 0; i < prefix_pad; i++) *dst++ = 0;
    CopyMem(dst, archive->data, archive->size);

    free_file_buffer_wipe(archive);
    free_file_buffer_maybe_wipe(*buf_io, entry->initrd_encrypted);
    *buf_io = out;
    efi_log(L"luks: passphrase keyfile appended to initrd");
    return EFI_SUCCESS;
}

int visor_cmdline_has_word(CHAR16 *cmdline, CHAR16 *word) {
    if (!cmdline || !word) return 0;
    UINTN wl = strlen16(word);
    if (!wl) return 0;

    UINTN i = 0;
    while (cmdline[i]) {
        while (cmdline[i] == L' ') i++;
        UINTN start = i;
        while (cmdline[i] && cmdline[i] != L' ') i++;
        if (i - start == wl) {
            UINTN k = 0;
            while (k < wl && cmdline[start + k] == word[k]) k++;
            if (k == wl) return 1;
        }
    }
    return 0;
}


static void luks_strip_quiet(CHAR16 *cmdline) {
    if (!cmdline) return;
    static const CHAR16 *hide[] = { L"quiet", L"splash", NULL };

    UINTN i = 0;
    while (cmdline[i]) {
        while (cmdline[i] == L' ') i++;
        UINTN start = i;
        while (cmdline[i] && cmdline[i] != L' ') i++;
        UINTN len = i - start;
        for (int h = 0; hide[h]; h++) {
            UINTN hl = strlen16((CHAR16*)hide[h]);
            if (hl != len) continue;
            UINTN k = 0;
            while (k < len && cmdline[start + k] == hide[h][k]) k++;
            if (k == len) {
                UINTN dst = start, src = start + len;
                while (cmdline[src] == L' ') src++;
                if (dst > 0 && cmdline[dst - 1] == L' ' && cmdline[src] == 0) dst--;
                while (cmdline[src]) cmdline[dst++] = cmdline[src++];
                cmdline[dst] = 0;
                i = start;
                break;
            }
        }
    }
}

static EFI_STATUS luks_effective_cmdline(boot_entry_t *entry,
                                         CHAR16 **cmdline_out,
                                         int *owned_out) {
    *cmdline_out = entry->cmdline;
    *owned_out = 0;

    if (!entry->luks) return EFI_SUCCESS;
    if (!entry->luks_cmdline || !entry->luks_cmdline[0]) {
        efi_log(L"WARN: luks=1 without luks_cmdline; initramfs may still prompt");
        return EFI_SUCCESS;
    }
    if (!entry->luks_verbose && entry->cmdline &&
        (visor_cmdline_has_word(entry->cmdline, L"quiet") ||
         visor_cmdline_has_word(entry->cmdline, L"splash")))
        efi_log(L"WARN: luks=1 with quiet/splash - a rejected passphrase will "
                L"reprompt invisibly; set luks_verbose=1");

    UINTN base_len = entry->cmdline ? strlen16(entry->cmdline) : 0;
    UINTN extra_len = strlen16(entry->luks_cmdline);
    UINTN sep = base_len ? 1 : 0;
    CHAR16 *out = efi_allocate_pool((base_len + sep + extra_len + 1) * sizeof(CHAR16));
    if (!out) return EFI_OUT_OF_RESOURCES;

    UINTN k = 0;
    for (UINTN i = 0; i < base_len; i++) out[k++] = entry->cmdline[i];
    if (sep) out[k++] = L' ';
    for (UINTN i = 0; i < extra_len; i++) out[k++] = entry->luks_cmdline[i];
    out[k] = 0;

    if (entry->luks_verbose) {
        luks_strip_quiet(out);
        efi_log(L"luks: luks_verbose=1 - removed quiet/splash so the initramfs "
                L"can prompt if the passphrase is rejected");
    }

    *cmdline_out = out;
    *owned_out = 1;
    efi_log(L"luks: appended initramfs keyfile cmdline");
    return EFI_SUCCESS;
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
    CHAR16 *kernel_load_path = entry->kernel_path;
    efi_file_buffer_t *kernel_buf = load_entry_file(entry->kernel_path,
                                                    entry->uuid,
                                                    entry->hp_volume,
                                                    entry->encrypted,
                                                    entry->decrypt_password,
                                                    &kernel_load_path);
    if (!kernel_buf) {
        efi_log(L"ERROR: kernel file not found or unreadable");
        efi_print(L"Failed to load kernel\r\n");
        clear_entry_password(entry);
        return EFI_NOT_FOUND;
    }
    void *kernel_data = kernel_buf->data;
    kernel_size = kernel_buf->size;

    if (!visor_hash_ok(entry, kernel_data, kernel_size)) {
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        clear_entry_password(entry);
        return EFI_SECURITY_VIOLATION;
    }

    int sb = efi_secure_boot_enabled();
    int shim = efi_shim_verify(kernel_data, kernel_size);
    if (shim == 0) {
        efi_log(L"ERROR: SHIM_LOCK verification failed - refusing to boot image");
        efi_print(L"Secure Boot: image verification failed\r\n");
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        clear_entry_password(entry);
        return EFI_SECURITY_VIOLATION;
    }
    if (shim == 1) efi_log(L"secure: image verified via SHIM_LOCK");

    CHAR16 *boot_cmdline = NULL;
    int boot_cmdline_owned = 0;
    status = luks_effective_cmdline(entry, &boot_cmdline, &boot_cmdline_owned);
    if (EFI_ERROR(status)) {
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
        if (entry->has_sha256) {
            efi_log(L"secure: sha256 pin set - loading the verified buffer, "
                    L"not re-reading the file by device path");
            if (!entry->initrd_path && !entry->cmdline)
                efi_log(L"WARN: a pinned chainload (e.g. bootmgfw.efi) loses its "
                        L"device path; Windows may not find its BCD");
        } else if (!entry->encrypted) {
            if (entry->hp_volume) {
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
        if (kernel_dp) {
            status = BS->LoadImage(FALSE, IH, kernel_dp, NULL, 0, &kernel_handle);
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
                loaded->LoadOptionsSize = (UINT32)((strlen16(boot_cmdline) + 1) * sizeof(CHAR16));
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
                                         entry->decrypt_password, NULL);
            if (initrd_buf && initrd_buf->data && initrd_buf->size) {
                status = luks_append_keyfile(entry, &initrd_buf);
                if (EFI_ERROR(status)) {
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
                        free_file_buffer_maybe_wipe(initrd_buf, entry->initrd_encrypted || entry->luks);
                        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
                        clear_entry_password(entry);
                        return EFI_SECURITY_VIOLATION;
                    }
                }
            } else {
                if (entry->initrd_encrypted || entry->luks) {
                    efi_log(L"ERROR: required initrd failed - refusing to boot");
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
                if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
                clear_entry_password(entry);
                return status;
            }
            initrd_handle = initrd_register(initrd_buf->data, initrd_buf->size);
            if (!initrd_handle) {
                efi_log(L"ERROR: could not install supplemental LUKS initrd LoadFile2 protocol");
                efi_print(L"LUKS initrd setup failed\r\n");
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
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
        clear_entry_password(entry);
        return EFI_SECURITY_VIOLATION;
    }

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
    free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
    if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
    clear_entry_password(entry);
    return EFI_UNSUPPORTED;
 #endif
}

