
#include "linux_boot.h"
#include "windows_boot.h"
#include "efi_helpers.h"
#include "hash_verify.h"
#include "crypto.h"
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
#define LUKS_DEFAULT_KEY_PATH      L"/crypto_keyfile.bin"
#define CPIO_NEWC_HEADER_SIZE      110u

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

static efi_file_buffer_t* load_entry_file(CHAR16 *path, int encrypted,
                                          CHAR16 *password) {
    efi_file_buffer_t *buf = efi_load_file(path);
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
        if (!slash && c == L'.' && last_slash_check && src[cap + 1] == L'.' &&
            (src[cap + 2] == 0 || src[cap + 2] == L'/' || src[cap + 2] == L'\\'))
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
    for (UINTN i = 0; i < pad4((UINTN)(p - start)); i++) *p++ = 0;
    if (data_size) {
        CopyMem(p, data, data_size);
        p += data_size;
    }
    for (UINTN i = 0; i < pad4((UINTN)(p - start)); i++) *p++ = 0;
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

    *cmdline_out = out;
    *owned_out = 1;
    efi_log(L"luks: appended initramfs keyfile cmdline");
    return EFI_SUCCESS;
}

static EFI_STATUS linux_place_initrd_buffer(efi_file_buffer_t *buf,
                                            UINT32 *addr, UINT32 *size) {
    if (!buf || !buf->data || !buf->size) return EFI_INVALID_PARAMETER;
    if (buf->size > 0xFFFFFFFFULL) return EFI_INVALID_PARAMETER;

    EFI_PHYSICAL_ADDRESS initrd_addr = LINUX_INITRD_BASE;
    EFI_STATUS status = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                          EFI_SIZE_TO_PAGES(buf->size), &initrd_addr);
    if (EFI_ERROR(status)) {
        initrd_addr = 0xFFFFFFFFULL;
        status = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                   EFI_SIZE_TO_PAGES(buf->size), &initrd_addr);
    }
    if (EFI_ERROR(status)) return status;
    if (initrd_addr > 0xFFFFFFFFULL) {
        BS->FreePages(initrd_addr, EFI_SIZE_TO_PAGES(buf->size));
        return EFI_OUT_OF_RESOURCES;
    }

    CopyMem((void*)(UINTN)initrd_addr, buf->data, buf->size);
    *addr = (UINT32)initrd_addr;
    *size = (UINT32)buf->size;
    return EFI_SUCCESS;
}

static EFI_STATUS get_map_and_exit(EFI_MEMORY_DESCRIPTOR **map_out,
                                   UINTN *map_size_out,
                                   UINTN *map_key_out,
                                   UINTN *desc_size_out,
                                   UINT32 *desc_ver_out) {
    if (!map_out || !map_size_out || !map_key_out || !desc_size_out || !desc_ver_out)
        return EFI_INVALID_PARAMETER;

    *map_out = NULL;
    *map_size_out = 0;
    *map_key_out = 0;
    *desc_size_out = 0;
    *desc_ver_out = 0;

    EFI_STATUS last = EFI_ABORTED;

    for (UINTN attempt = 0; attempt < 3; attempt++) {
        UINTN  map_size = 0;
        UINTN  map_key = 0;
        UINTN  desc_size = 0;
        UINT32 desc_ver = 0;

        EFI_STATUS status = BS->GetMemoryMap(&map_size, NULL, &map_key,
                                             &desc_size, &desc_ver);
        if (status != EFI_BUFFER_TOO_SMALL || desc_size == 0) {
            last = EFI_ERROR(status) ? status : EFI_DEVICE_ERROR;
            break;
        }

        map_size += 4 * desc_size;
        EFI_MEMORY_DESCRIPTOR *map = efi_allocate_pool(map_size);
        if (!map) return EFI_OUT_OF_RESOURCES;

        status = BS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(status)) {
            efi_free_pool(map);
            last = status;
            continue;
        }

        status = BS->ExitBootServices(IH, map_key);
        if (!EFI_ERROR(status)) {
            *map_out       = map;
            *map_size_out  = map_size;
            *map_key_out   = map_key;
            *desc_size_out = desc_size;
            *desc_ver_out  = desc_ver;
            return EFI_SUCCESS;
        }

        efi_free_pool(map);
        last = status;
    }

    return last;
}

static EFI_STATUS linux_setup_boot_params(void *kernel_data, UINTN kernel_size,
                                           UINT32 initrd_addr, UINT32 initrd_size,
                                           CHAR16 *cmdline,
                                           EFI_PHYSICAL_ADDRESS *cmdline_addr_out,
                                           UINTN *cmdline_pages_out) {
    if (cmdline_addr_out) *cmdline_addr_out = 0;
    if (cmdline_pages_out) *cmdline_pages_out = 0;
    if (kernel_size < LINUX_SETUP_HEADER_OFFSET + sizeof(setup_header_t))
        return EFI_INVALID_PARAMETER;

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
        UINTN  cmdbytes = cmdlen + 1;
        if (cmdbytes == 0 || cmdbytes > 0x10000)
            return EFI_INVALID_PARAMETER;

        EFI_PHYSICAL_ADDRESS cmd_addr = 0xFFFFFFFFULL;
        UINTN cmd_pages = EFI_SIZE_TO_PAGES(cmdbytes);
        EFI_STATUS status = BS->AllocatePages(AllocateMaxAddress, EfiLoaderData,
                                              cmd_pages, &cmd_addr);
        if (EFI_ERROR(status)) return status;

        CHAR8 *cmd = (CHAR8*)(UINTN)cmd_addr;
        for (UINTN j = 0; j < cmdlen; j++) cmd[j] = (CHAR8)cmdline[j];
        cmd[cmdlen] = '\0';
        hdr->cmd_line_ptr = (UINT32)cmd_addr;
        if (cmdline_addr_out) *cmdline_addr_out = cmd_addr;
        if (cmdline_pages_out) *cmdline_pages_out = cmd_pages;
    }

    return EFI_SUCCESS;
}

static void linux_free_cmdline_pages(EFI_PHYSICAL_ADDRESS addr, UINTN pages) {
    if (addr && pages) BS->FreePages(addr, pages);
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
        efi_log_close();
        return BS->StartImage(bh, NULL, NULL);
    }

    efi_log(L"boot: loading kernel/image file");
    efi_log(entry->kernel_path);
    efi_file_buffer_t *kernel_buf = load_entry_file(entry->kernel_path,
                                                    entry->encrypted,
                                                    entry->decrypt_password);
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

    if (kernel[0] == 'M' && kernel[1] == 'Z') {
        EFI_HANDLE kernel_handle;

        efi_log(L"boot: PE image (Windows/UKI/EFI-stub), LoadImage()");
        EFI_DEVICE_PATH *kernel_dp = entry->encrypted ? NULL
            : efi_file_device_path(entry->kernel_path, entry->uuid);
        if (kernel_dp) {
            status = BS->LoadImage(FALSE, IH, kernel_dp, NULL, 0, &kernel_handle);
            efi_free_pool(kernel_dp);
        } else {
            efi_log(L"WARN: could not build device path - loading from source buffer");
            status = BS->LoadImage(FALSE, IH, NULL, kernel_data, kernel_size, &kernel_handle);
        }
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        if (EFI_ERROR(status)) {
            efi_log(L"ERROR: LoadImage failed (not a valid EFI image?)");
            efi_print(L"LoadImage failed\r\n");
            if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
            clear_entry_password(entry);
            return status;
        }

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
            initrd_buf = load_entry_file(entry->initrd_path,
                                         entry->initrd_encrypted,
                                         entry->decrypt_password);
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
                    SPrint(m, sizeof(m), L"linux: initrd ready, %d bytes via LoadFile2 media path",
                           (int)initrd_buf->size);
                    efi_log(m);
                } else {
                    efi_log(L"WARN: could not install initrd LoadFile2 protocol");
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

        efi_log(L"linux: StartImage() - handing control to kernel stub");
        efi_log_close();
        clear_entry_password(entry);
        status = BS->StartImage(kernel_handle, NULL, NULL);

        efi_log(L"ERROR: kernel StartImage returned - boot failed");
        if (initrd_handle) initrd_unregister(initrd_handle);
        free_file_buffer_maybe_wipe(initrd_buf, entry->initrd_encrypted || entry->luks);
        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
        return status;
    }

    if (sb && shim != 1) {
        efi_log(L"ERROR: Secure Boot on but raw kernel is unverifiable (no SHIM_LOCK) - refusing");
        efi_print(L"Secure Boot: refusing unverified kernel\r\n");
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
        clear_entry_password(entry);
        return EFI_SECURITY_VIOLATION;
    }

    if (entry->luks && !entry->initrd_path) {
        efi_log(L"ERROR: raw kernel luks=1 requires a separate initrd=");
        efi_print(L"LUKS boot requires an initrd\r\n");
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
        clear_entry_password(entry);
        return EFI_INVALID_PARAMETER;
    }

    UINT32 initrd_addr = 0, initrd_size = 0;
    if (entry->initrd_path) {
        efi_log(L"linux: loading initrd (boot-params ramdisk)");
        efi_file_buffer_t *ib = load_entry_file(entry->initrd_path,
                                                entry->initrd_encrypted,
                                                entry->decrypt_password);
        if (ib) {
            status = luks_append_keyfile(entry, &ib);
            if (!EFI_ERROR(status))
                status = linux_place_initrd_buffer(ib, &initrd_addr, &initrd_size);
            free_file_buffer_maybe_wipe(ib, entry->initrd_encrypted || entry->luks);
        } else {
            status = entry->initrd_encrypted ? EFI_SECURITY_VIOLATION : EFI_NOT_FOUND;
        }
        if (EFI_ERROR(status)) {
            if (entry->initrd_encrypted || entry->luks) {
                efi_log(L"ERROR: required initrd failed - refusing to boot");
                free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
                if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
                clear_entry_password(entry);
                return EFI_SECURITY_VIOLATION;
            }
            efi_log(L"WARN: initrd load failed - continuing without it");
            efi_print(L"Warning: Could not load initrd\r\n");
        }
    }

    EFI_PHYSICAL_ADDRESS raw_cmdline_addr = 0;
    UINTN raw_cmdline_pages = 0;
    status = linux_setup_boot_params(kernel_data, kernel_size,
                                     initrd_addr, initrd_size, boot_cmdline,
                                     &raw_cmdline_addr, &raw_cmdline_pages);
    if (EFI_ERROR(status)) {
        efi_print(L"Setup failed\r\n");
        linux_free_cmdline_pages(raw_cmdline_addr, raw_cmdline_pages);
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
        clear_entry_password(entry);
        return status;
    }

    setup_header_t *hdr = (setup_header_t*)(kernel + LINUX_SETUP_HEADER_OFFSET);
    UINT32 handover_offset = hdr->handover_offset;

    if ((UINTN)LINUX_SETUP_SECTOR_SIZE + handover_offset >= kernel_size) {
        efi_log(L"ERROR: kernel handover offset out of range - refusing to jump");
        efi_print(L"Bad kernel (handover offset)\r\n");
        linux_free_cmdline_pages(raw_cmdline_addr, raw_cmdline_pages);
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
        clear_entry_password(entry);
        return EFI_INVALID_PARAMETER;
    }

    linux_efi_handover_t handover_info;
    handover_info.image_handle  = IH;
    handover_info.system_table  = st;

    EFI_MEMORY_DESCRIPTOR *map = NULL;
    UINTN map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;

    efi_log(L"linux: closing boot log and exiting boot services");
    efi_log_close();
    clear_entry_password(entry);
    status = get_map_and_exit(&map, &map_size, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(status)) {
        efi_log(L"ERROR: ExitBootServices failed before kernel handoff");
        if (map) efi_free_pool(map);
        linux_free_cmdline_pages(raw_cmdline_addr, raw_cmdline_pages);
        free_file_buffer_maybe_wipe(kernel_buf, entry->encrypted);
        if (boot_cmdline_owned) efi_free_pool(boot_cmdline);
        return status;
    }
    visor_boot_services_active = 0;

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
