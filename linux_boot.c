
#include "linux_boot.h"
#include "windows_boot.h"
#include "efi_helpers.h"
#include "hash_verify.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;
extern EFI_HANDLE IH;

#define LINUX_SETUP_HEADER_OFFSET  0x1F1u
#define LINUX_BOOT_FLAG_MAGIC      0xAA55u
#define LINUX_SETUP_SECTOR_SIZE    0x200u
#define LINUX_INITRD_BASE          0x10000000u
#define LINUX_LOADFLAG_KEEP_SEGS   0x80u

typedef struct {
    EFI_HANDLE            image_handle;
    EFI_SYSTEM_TABLE     *system_table;
    EFI_MEMORY_DESCRIPTOR*memory_map;
    UINTN                 memory_map_size;
    UINTN                 memory_map_key;
    UINTN                 desc_size;
    UINT32                desc_version;
} linux_efi_handover_t;

static UINTN strlen16(CHAR16 *s) {
    UINTN len = 0;
    while (s[len]) len++;
    return len;
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

static void* load_kernel_file(CHAR16 *path, UINTN *size_out) {
    efi_file_buffer_t *buf = efi_load_file(path);
    if (!buf) return NULL;
    *size_out = buf->size;
    return buf->data;
}

static EFI_STATUS linux_load_initrd_impl(CHAR16 *path, UINT32 *addr, UINT32 *size);

EFI_STATUS linux_load_initrd(boot_entry_t *entry, UINT32 *addr, UINT32 *size) {
    if (!entry || !entry->initrd_path) return EFI_INVALID_PARAMETER;
    return linux_load_initrd_impl(entry->initrd_path, addr, size);
}

static EFI_STATUS linux_load_initrd_impl(CHAR16 *path, UINT32 *addr, UINT32 *size) {
    efi_file_buffer_t *buf = efi_load_file(path);
    if (!buf) return EFI_NOT_FOUND;

    EFI_PHYSICAL_ADDRESS initrd_addr = LINUX_INITRD_BASE;
    EFI_STATUS status = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                          EFI_SIZE_TO_PAGES(buf->size), &initrd_addr);
    if (EFI_ERROR(status)) {
        status = BS->AllocatePages(AllocateAnyPages, EfiLoaderData,
                                   EFI_SIZE_TO_PAGES(buf->size), &initrd_addr);
    }
    if (EFI_ERROR(status)) {
        efi_free_pool(buf);
        return status;
    }

    CopyMem((void*)(UINTN)initrd_addr, buf->data, buf->size);
    *addr = (UINT32)initrd_addr;
    *size = (UINT32)buf->size;
    efi_free_pool(buf);
    return EFI_SUCCESS;
}

static EFI_STATUS get_map_and_exit(EFI_MEMORY_DESCRIPTOR **map_out,
                                   UINTN *map_size_out,
                                   UINTN *map_key_out,
                                   UINTN *desc_size_out,
                                   UINT32 *desc_ver_out) {
    UINTN  map_size = 0;
    UINTN  map_key, desc_size;
    UINT32 desc_ver;
    EFI_STATUS status;

    BS->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_ver);
    map_size += 2 * desc_size;

    EFI_MEMORY_DESCRIPTOR *map = efi_allocate_pool(map_size);
    do {
        status = BS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(status)) {
            map_size += desc_size;
            efi_free_pool(map);
            map = efi_allocate_pool(map_size);
        }
    } while (EFI_ERROR(status));

    *map_out       = map;
    *map_size_out  = map_size;
    *map_key_out   = map_key;
    *desc_size_out = desc_size;
    *desc_ver_out  = desc_ver;

    return BS->ExitBootServices(IH, map_key);
}

static EFI_STATUS linux_setup_boot_params(void *kernel_data, UINTN kernel_size,
                                           UINT32 initrd_addr, UINT32 initrd_size,
                                           CHAR16 *cmdline) {
    if (kernel_size < LINUX_SETUP_SECTOR_SIZE) return EFI_INVALID_PARAMETER;

    UINT8 *kernel = (UINT8*)kernel_data;

    UINT16 boot_flag = *(UINT16*)(kernel + 0x1FEu);
    if (boot_flag != LINUX_BOOT_FLAG_MAGIC) {
        if (kernel[0] == 'M' && kernel[1] == 'Z') return EFI_SUCCESS;
        return EFI_INVALID_PARAMETER;
    }

    setup_header_t *hdr = (setup_header_t*)(kernel + LINUX_SETUP_HEADER_OFFSET);

    if (!(hdr->loadflags & HANDOVER_MASK)) return EFI_UNSUPPORTED;

    hdr->loadflags    |= LINUX_LOADFLAG_KEEP_SEGS;
    hdr->ramdisk_image = initrd_addr;
    hdr->ramdisk_size  = initrd_size;

    if (cmdline) {
        UINTN  cmdlen = strlen16(cmdline);
        CHAR8 *cmd    = efi_allocate_pool(cmdlen + 1);
        if (cmd) {
            for (UINTN j = 0; j < cmdlen; j++) cmd[j] = (CHAR8)cmdline[j];
            cmd[cmdlen] = '\0';
            hdr->cmd_line_ptr = (UINT32)(UINTN)cmd;
        }
    }

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
            efi_log(L"ERROR: LoadImage failed for bootmgfw.efi");
            efi_print(L"LoadImage failed\r\n");
            return status;
        }
        efi_log(L"boot: StartImage() - handing control to Windows Boot Manager");
        return BS->StartImage(bh, NULL, NULL);
    }

    efi_log(L"boot: loading kernel/image file");
    efi_log(entry->kernel_path);
    void *kernel_data = load_kernel_file(entry->kernel_path, &kernel_size);
    if (!kernel_data) {
        efi_log(L"ERROR: kernel file not found or unreadable");
        efi_print(L"Failed to load kernel\r\n");
        return EFI_NOT_FOUND;
    }

    if (!visor_hash_ok(entry, kernel_data, kernel_size)) {
        efi_free_pool(kernel_data);
        return EFI_SECURITY_VIOLATION;
    }

    int sb = efi_secure_boot_enabled();
    int shim = efi_shim_verify(kernel_data, kernel_size);
    if (shim == 0) {
        efi_log(L"ERROR: SHIM_LOCK verification failed - refusing to boot image");
        efi_print(L"Secure Boot: image verification failed\r\n");
        efi_free_pool(kernel_data);
        return EFI_SECURITY_VIOLATION;
    }
    if (shim == 1) efi_log(L"secure: image verified via SHIM_LOCK");

    UINT8 *kernel = (UINT8*)kernel_data;

    if (kernel[0] == 'M' && kernel[1] == 'Z') {
        EFI_HANDLE kernel_handle;

        efi_log(L"boot: PE image (Windows/UKI/EFI-stub), LoadImage()");
        EFI_DEVICE_PATH *kernel_dp = efi_file_device_path(entry->kernel_path, entry->uuid);
        if (kernel_dp) {
            status = BS->LoadImage(FALSE, IH, kernel_dp, NULL, 0, &kernel_handle);
            efi_free_pool(kernel_dp);
        } else {
            efi_log(L"WARN: could not build device path - loading from source buffer");
            status = BS->LoadImage(FALSE, IH, NULL, kernel_data, kernel_size, &kernel_handle);
        }
        efi_free_pool(kernel_data);
        if (EFI_ERROR(status)) {
            efi_log(L"ERROR: LoadImage failed (not a valid EFI image?)");
            efi_print(L"LoadImage failed\r\n");
            return status;
        }

        if (entry->cmdline) {
            EFI_LOADED_IMAGE *loaded;
            status = BS->HandleProtocol(kernel_handle, &gEfiLoadedImageProtocolGuid, (void**)&loaded);
            if (!EFI_ERROR(status)) {
                loaded->LoadOptions     = entry->cmdline;
                loaded->LoadOptionsSize = (UINT32)((strlen16(entry->cmdline) + 1) * sizeof(CHAR16));
                efi_log(L"linux: cmdline set via LoadOptions");
            }
        }

        efi_file_buffer_t *initrd_buf = NULL;
        EFI_HANDLE initrd_handle = NULL;
        if (entry->initrd_path) {
            efi_log(L"linux: loading initrd for stub (LINUX_EFI_INITRD_MEDIA)");
            efi_log(entry->initrd_path);
            initrd_buf = efi_load_file(entry->initrd_path);
            if (initrd_buf && initrd_buf->data && initrd_buf->size) {
                initrd_handle = initrd_register(initrd_buf->data, initrd_buf->size);
                if (initrd_handle) {
                    CHAR16 m[72];
                    SPrint(m, sizeof(m), L"linux: initrd ready, %d bytes via LoadFile2 media path",
                           (int)initrd_buf->size);
                    efi_log(m);
                } else {
                    efi_log(L"WARN: could not install initrd LoadFile2 protocol");
                }
            } else {
                efi_log(L"WARN: initrd load failed - continuing without it");
                efi_print(L"Warning: Could not load initrd\r\n");
            }
        }

        efi_log(L"linux: StartImage() - handing control to kernel stub");
        status = BS->StartImage(kernel_handle, NULL, NULL);

        efi_log(L"ERROR: kernel StartImage returned - boot failed");
        if (initrd_handle) initrd_unregister(initrd_handle);
        if (initrd_buf) {
            if (initrd_buf->data) efi_free_pool(initrd_buf->data);
            efi_free_pool(initrd_buf);
        }
        return status;
    }

    if (sb && shim != 1) {
        efi_log(L"ERROR: Secure Boot on but raw kernel is unverifiable (no SHIM_LOCK) - refusing");
        efi_print(L"Secure Boot: refusing unverified kernel\r\n");
        efi_free_pool(kernel_data);
        return EFI_SECURITY_VIOLATION;
    }

    UINT32 initrd_addr = 0, initrd_size = 0;
    if (entry->initrd_path) {
        efi_log(L"linux: loading initrd (boot-params ramdisk)");
        status = linux_load_initrd_impl(entry->initrd_path, &initrd_addr, &initrd_size);
        if (EFI_ERROR(status)) {
            efi_log(L"WARN: initrd load failed - continuing without it");
            efi_print(L"Warning: Could not load initrd\r\n");
        }
    }

    status = linux_setup_boot_params(kernel_data, kernel_size,
                                     initrd_addr, initrd_size, entry->cmdline);
    if (EFI_ERROR(status)) {
        efi_print(L"Setup failed\r\n");
        return status;
    }

    setup_header_t *hdr = (setup_header_t*)(kernel + LINUX_SETUP_HEADER_OFFSET);
    UINT32 handover_offset = hdr->handover_offset;

    linux_efi_handover_t handover_info;
    handover_info.image_handle  = IH;
    handover_info.system_table  = st;

    EFI_MEMORY_DESCRIPTOR *map;
    UINTN map_size, map_key, desc_size;
    UINT32 desc_ver;

    status = get_map_and_exit(&map, &map_size, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) return status;

    handover_info.memory_map      = map;
    handover_info.memory_map_size = map_size;
    handover_info.memory_map_key  = map_key;
    handover_info.desc_size       = desc_size;
    handover_info.desc_version    = desc_ver;

    typedef EFI_STATUS (EFIAPI *kernel_entry_t)(EFI_HANDLE, EFI_SYSTEM_TABLE*, linux_efi_handover_t*);
    kernel_entry_t kernel_entry = (kernel_entry_t)(kernel + LINUX_SETUP_SECTOR_SIZE + handover_offset);

    status = kernel_entry(IH, st, &handover_info);

    return status;
}
