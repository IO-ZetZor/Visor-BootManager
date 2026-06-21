
#include "linux_boot.h"
#include "efi_helpers.h"
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

static int contains_token16(CHAR16 *s, const CHAR16 *needle) {
    if (!s || !needle) return 0;
    for (UINTN i = 0; s[i]; i++) {
        UINTN j = 0;
        while (needle[j] && s[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static CHAR16* efi_stub_options(boot_entry_t *entry) {
    if (!entry->initrd_path || contains_token16(entry->cmdline, L"initrd="))
        return entry->cmdline;

    UINTN initrd_key_len = strlen16(L"initrd=");
    UINTN initrd_len = strlen16(entry->initrd_path);
    UINTN cmd_len = entry->cmdline ? strlen16(entry->cmdline) : 0;
    UINTN total = initrd_key_len + initrd_len + (cmd_len ? 1 + cmd_len : 0);
    CHAR16 *opts = efi_allocate_pool((total + 1) * sizeof(CHAR16));
    if (!opts) return entry->cmdline;

    UINTN k = 0;
    CHAR16 *key = L"initrd=";
    for (UINTN i = 0; key[i]; i++) opts[k++] = key[i];
    for (UINTN i = 0; entry->initrd_path[i]; i++) opts[k++] = entry->initrd_path[i];
    if (cmd_len) {
        opts[k++] = ' ';
        for (UINTN i = 0; entry->cmdline[i]; i++) opts[k++] = entry->cmdline[i];
    }
    opts[k] = '\0';
    efi_log(L"linux: synthesized EFI-stub initrd= option");
    return opts;
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

EFI_STATUS linux_boot(boot_entry_t *entry, EFI_SYSTEM_TABLE *st) {
    EFI_STATUS status;
    UINTN kernel_size = 0;

    efi_print(L"Booting Linux: ");
    efi_print(entry->name);
    efi_print(L"\r\n");

    efi_log(L"linux: loading kernel file");
    efi_log(entry->kernel_path);
    void *kernel_data = load_kernel_file(entry->kernel_path, &kernel_size);
    if (!kernel_data) {
        efi_log(L"ERROR: kernel file not found or unreadable");
        efi_print(L"Failed to load kernel\r\n");
        return EFI_NOT_FOUND;
    }

    UINT8 *kernel = (UINT8*)kernel_data;

    if (kernel[0] == 'M' && kernel[1] == 'Z') {
        EFI_HANDLE kernel_handle;

        efi_log(L"linux: PE/UKI image, LoadImage()");
        EFI_DEVICE_PATH *kernel_dp = efi_file_device_path(entry->kernel_path, NULL);
        if (kernel_dp) {
            status = BS->LoadImage(FALSE, IH, kernel_dp, NULL, 0, &kernel_handle);
            efi_free_pool(kernel_dp);
        } else {
            efi_log(L"WARN: could not build kernel device path - falling back to source buffer");
            status = BS->LoadImage(FALSE, IH, NULL, kernel_data, kernel_size, &kernel_handle);
        }
        if (EFI_ERROR(status)) {
            efi_log(L"ERROR: LoadImage failed (not a valid EFI image?)");
            efi_print(L"LoadImage failed\r\n");
            return status;
        }

        CHAR16 *load_options = efi_stub_options(entry);
        if (load_options) {
            EFI_LOADED_IMAGE *loaded;
            status = BS->HandleProtocol(kernel_handle, &gEfiLoadedImageProtocolGuid, (void**)&loaded);
            if (!EFI_ERROR(status)) {
                loaded->LoadOptions     = load_options;
                loaded->LoadOptionsSize = (UINT32)((strlen16(load_options) + 1) * sizeof(CHAR16));
            }
        }

        efi_log(L"linux: StartImage() - handing control to kernel stub");
        status = BS->StartImage(kernel_handle, NULL, NULL);

        efi_log(L"ERROR: kernel StartImage returned - boot failed");
        return status;
    }

    UINT32 initrd_addr = 0, initrd_size = 0;
    if (entry->initrd_path) {
        efi_log(L"linux: loading initrd");
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
