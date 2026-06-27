#include "efi_helpers.h"
#include <efi.h>
#include <efilib.h>
#include <stdarg.h>

extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;
extern EFI_HANDLE IH;

static EFI_HANDLE boot_device_handle(void) {
    static EFI_HANDLE cached = NULL;
    static int resolved = 0;
    if (resolved) return cached;
    resolved = 1;
    EFI_LOADED_IMAGE *li = NULL;
    if (!EFI_ERROR(BS->HandleProtocol(IH, &gEfiLoadedImageProtocolGuid, (void**)&li)) && li)
        cached = li->DeviceHandle;
    return cached;
}

static EFI_FILE_PROTOCOL *open_root_on_handle(EFI_HANDLE h) {
    if (!h) return NULL;
    EFI_FILE_IO_INTERFACE *io = NULL;
    if (EFI_ERROR(BS->HandleProtocol(h, &gEfiSimpleFileSystemProtocolGuid, (void**)&io)) || !io)
        return NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (EFI_ERROR(io->OpenVolume(io, &root))) return NULL;
    return root;
}

EFI_FILE_PROTOCOL* efi_boot_volume_root(void) {
    return open_root_on_handle(boot_device_handle());
}

void* efi_allocate_pool(UINTN size) {
    void *ptr = NULL;
    BS->AllocatePool(EfiLoaderData, size, &ptr);
    return ptr;
}

void efi_free_pool(void *ptr) {
    if (ptr) {
        BS->FreePool(ptr);
    }
}

CHAR16* efi_strdup(CHAR16 *src) {
    if (!src) return NULL;
    UINTN len = 0;
    while (src[len]) len++;
    CHAR16 *dst = efi_allocate_pool((len + 1) * sizeof(CHAR16));
    if (!dst) return NULL;
    for (UINTN i = 0; i <= len; i++) {
        dst[i] = src[i];
    }
    return dst;
}

int efi_strcmp(CHAR16 *s1, CHAR16 *s2) {
    if (!s1 || !s2) return 1;
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

CHAR16* efi_strchr(CHAR16 *s, CHAR16 c) {
    while (*s && *s != c) s++;
    return (*s == c) ? s : NULL;
}

static int hex_digit(CHAR16 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex_byte(CHAR16 *s, UINT8 *out) {
    int hi = hex_digit(s[0]);
    int lo = hex_digit(s[1]);
    if (hi < 0 || lo < 0) return 0;
    *out = (UINT8)((hi << 4) | lo);
    return 1;
}

static int parse_partition_uuid(CHAR16 *s, EFI_GUID *out) {
    if (!s || !out) return 0;
    UINT8 raw[16];
    int pos = 0;

    for (UINTN i = 0; s[i];) {
        if (s[i] == '-') {
            i++;
            continue;
        }
        if (!s[i + 1] || pos >= 16 || !parse_hex_byte(s + i, &raw[pos++]))
            return 0;
        i += 2;
    }
    if (pos != 16) return 0;

    out->Data1 = ((UINT32)raw[0] << 24) | ((UINT32)raw[1] << 16) |
                 ((UINT32)raw[2] << 8)  | raw[3];
    out->Data2 = ((UINT16)raw[4] << 8) | raw[5];
    out->Data3 = ((UINT16)raw[6] << 8) | raw[7];
    for (int i = 0; i < 8; i++) out->Data4[i] = raw[8 + i];
    return 1;
}

int efi_handle_matches_partition_uuid(EFI_HANDLE handle, CHAR16 *partition_uuid) {
    if (!partition_uuid || partition_uuid[0] == '\0') return 1;

    EFI_GUID want;
    if (!parse_partition_uuid(partition_uuid, &want)) return 0;

    EFI_DEVICE_PATH *dp = NULL;
    if (EFI_ERROR(BS->HandleProtocol(handle, &gEfiDevicePathProtocolGuid, (void**)&dp)) || !dp)
        return 0;

    EFI_DEVICE_PATH *node = dp;
    while (!IsDevicePathEnd(node)) {
        if (DevicePathType(node) == MEDIA_DEVICE_PATH &&
            DevicePathSubType(node) == MEDIA_HARDDRIVE_DP) {
            HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH*)node;
            if (hd->SignatureType == SIGNATURE_TYPE_GUID &&
                CompareMem(hd->Signature, &want, sizeof(want)) == 0)
                return 1;
        }
        node = (EFI_DEVICE_PATH*)((UINT8*)node + DevicePathNodeLength(node));
    }

    return 0;
}

EFI_DEVICE_PATH* efi_make_file_path(EFI_HANDLE handle, CHAR16 *filename) {
    EFI_DEVICE_PATH *dp = NULL;
    BS->HandleProtocol(handle, &gEfiDevicePathProtocolGuid, (void**)&dp);
    if (!dp) return NULL;

    UINTN dp_len    = DevicePathSize(dp) - sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN fname_len = StrLen(filename) * sizeof(CHAR16);
    UINTN fp_size   = sizeof(FILEPATH_DEVICE_PATH) + fname_len;
    UINTN end_size  = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN total_len = dp_len + fp_size + end_size;

    UINT8 *new_dp = efi_allocate_pool(total_len);
    if (!new_dp) return NULL;

    CopyMem(new_dp, dp, dp_len);

    FILEPATH_DEVICE_PATH *fp = (FILEPATH_DEVICE_PATH*)(new_dp + dp_len);
    fp->Header.Type    = MEDIA_DEVICE_PATH;
    fp->Header.SubType = MEDIA_FILEPATH_DP;
    SetDevicePathNodeLength(&fp->Header, (UINT16)fp_size);
    StrCpy(fp->PathName, filename);

    EFI_DEVICE_PATH_PROTOCOL *end = (EFI_DEVICE_PATH_PROTOCOL*)(new_dp + dp_len + fp_size);
    end->Type    = END_DEVICE_PATH_TYPE;
    end->SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
    SetDevicePathNodeLength(end, (UINT16)end_size);

    return (EFI_DEVICE_PATH*)new_dp;
}

EFI_DEVICE_PATH* efi_file_device_path(CHAR16 *path, CHAR16 *partition_uuid) {
    EFI_HANDLE boot_handle = boot_device_handle();
    EFI_FILE_PROTOCOL *root = open_root_on_handle(boot_handle);
    if (root) {
        if (efi_handle_matches_partition_uuid(boot_handle, partition_uuid) &&
            efi_file_exists_root(root, path)) {
            root->Close(root);
            return efi_make_file_path(boot_handle, path);
        }
        root->Close(root);
    }

    UINTN count = 0;
    EFI_HANDLE *handles = efi_locate_handle_buffer(
        &gEfiSimpleFileSystemProtocolGuid, &count);
    if (!handles) return NULL;

    EFI_DEVICE_PATH *dp = NULL;
    for (UINTN i = 0; i < count; i++) {
        if (!efi_handle_matches_partition_uuid(handles[i], partition_uuid))
            continue;

        EFI_FILE_IO_INTERFACE *io = NULL;
        if (EFI_ERROR(BS->HandleProtocol(handles[i],
                &gEfiSimpleFileSystemProtocolGuid, (void**)&io)) || !io)
            continue;

        EFI_FILE_PROTOCOL *r = NULL;
        if (EFI_ERROR(io->OpenVolume(io, &r)) || !r)
            continue;

        if (efi_file_exists_root(r, path))
            dp = efi_make_file_path(handles[i], path);
        r->Close(r);
        if (dp) break;
    }

    efi_free_pool(handles);
    return dp;
}

efi_file_t* efi_fopen(CHAR16 *path) {
    efi_file_t *file = efi_allocate_pool(sizeof(efi_file_t));
    if (!file) return NULL;
    file->root = NULL;
    file->handle = NULL;

    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (root) {
        if (!EFI_ERROR(root->Open(root, &file->handle, path, EFI_FILE_MODE_READ, 0))) {
            file->root = root;
            return file;
        }
        root->Close(root);
    }

    UINTN count = 0;
    EFI_HANDLE *handles = efi_locate_handle_buffer(
        &gEfiSimpleFileSystemProtocolGuid, &count);
    if (handles) {
        for (UINTN i = 0; i < count; i++) {
            EFI_FILE_IO_INTERFACE *io = NULL;
            if (EFI_ERROR(BS->HandleProtocol(handles[i],
                    &gEfiSimpleFileSystemProtocolGuid, (void**)&io)) || !io)
                continue;
            EFI_FILE_PROTOCOL *r = NULL;
            if (EFI_ERROR(io->OpenVolume(io, &r)) || !r)
                continue;
            if (!EFI_ERROR(r->Open(r, &file->handle, path, EFI_FILE_MODE_READ, 0))) {
                file->root = r;
                efi_free_pool(handles);
                return file;
            }
            r->Close(r);
        }
        efi_free_pool(handles);
    }

    efi_free_pool(file);
    return NULL;
}

void efi_fclose(efi_file_t *file) {
    if (!file) return;
    if (file->handle) {
        file->handle->Close(file->handle);
    }
    if (file->root) {
        file->root->Close(file->root);
    }
    efi_free_pool(file);
}

UINTN efi_fread(efi_file_t *file, void *buf, UINTN size) {
    if (!file || !file->handle || !buf) return 0;
    file->handle->Read(file->handle, &size, buf);
    return size;
}

int efi_file_exists(CHAR16 *path) {
    efi_file_t *f = efi_fopen(path);
    if (f) {
        efi_fclose(f);
        return 1;
    }
    return 0;
}

UINTN efi_volume_count(void) {
    UINTN count = 0;
    EFI_HANDLE *handles = efi_locate_handle_buffer(
        &gEfiSimpleFileSystemProtocolGuid, &count);
    if (handles) efi_free_pool(handles);
    return count;
}

EFI_FILE_PROTOCOL* efi_open_volume(UINTN index) {
    UINTN count = 0;
    EFI_HANDLE *handles = efi_locate_handle_buffer(
        &gEfiSimpleFileSystemProtocolGuid, &count);
    if (!handles) return NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (index < count) {
        EFI_FILE_IO_INTERFACE *io = NULL;
        if (!EFI_ERROR(BS->HandleProtocol(handles[index],
                &gEfiSimpleFileSystemProtocolGuid, (void**)&io))) {
            if (EFI_ERROR(io->OpenVolume(io, &root))) root = NULL;
        }
    }
    efi_free_pool(handles);
    return root;
}

int efi_file_exists_root(EFI_FILE_PROTOCOL *root, CHAR16 *path) {
    if (!root) return 0;
    EFI_FILE_PROTOCOL *f = NULL;
    if (EFI_ERROR(root->Open(root, &f, path, EFI_FILE_MODE_READ, 0)) || !f)
        return 0;
    f->Close(f);
    return 1;
}

EFI_FILE_PROTOCOL* efi_open_dir(EFI_FILE_PROTOCOL *root, CHAR16 *path) {
    if (!root) return NULL;
    EFI_FILE_PROTOCOL *d = NULL;
    if (EFI_ERROR(root->Open(root, &d, path, EFI_FILE_MODE_READ, 0)))
        return NULL;
    return d;
}

int efi_read_dirent(EFI_FILE_PROTOCOL *dir, CHAR16 *name_out, UINTN name_cap, int *is_dir) {
    if (!dir || !name_out || name_cap == 0) return 0;
    UINT8 buf[1024];
    for (;;) {
        UINTN size = sizeof(buf);
        EFI_STATUS s = dir->Read(dir, &size, buf);
        if (EFI_ERROR(s) || size == 0) return 0;
        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        CHAR16 *fn = info->FileName;
        if (fn[0] == '.' && (fn[1] == '\0' || (fn[1] == '.' && fn[2] == '\0')))
            continue;
        UINTN i = 0;
        while (fn[i] && i < name_cap - 1) { name_out[i] = fn[i]; i++; }
        name_out[i] = '\0';
        if (is_dir) *is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
        return 1;
    }
}

int efi_readdir(efi_file_t *dir, CHAR16 *name_out, UINTN name_cap, int *is_dir) {
    if (!dir || !dir->handle || !name_out || name_cap == 0) return 0;

    UINT8 buf[1024];
    for (;;) {
        UINTN size = sizeof(buf);
        EFI_STATUS s = dir->handle->Read(dir->handle, &size, buf);
        if (EFI_ERROR(s) || size == 0) return 0;

        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        CHAR16 *fn = info->FileName;

        if (fn[0] == '.' && (fn[1] == '\0' || (fn[1] == '.' && fn[2] == '\0')))
            continue;

        UINTN i = 0;
        while (fn[i] && i < name_cap - 1) { name_out[i] = fn[i]; i++; }
        name_out[i] = '\0';

        if (is_dir)
            *is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
        return 1;
    }
}

efi_file_buffer_t* efi_load_file(CHAR16 *path) {
    efi_file_t *file = efi_fopen(path);
    if (!file) return NULL;

    UINT64 size = 0;
    file->handle->SetPosition(file->handle, ~0ULL);
    file->handle->GetPosition(file->handle, &size);
    file->handle->SetPosition(file->handle, 0);

    if (size == 0 || size > 256ULL * 1024 * 1024) {
        efi_log(L"WARN: file size is zero or implausibly large - skipping");
        efi_fclose(file);
        return NULL;
    }

    efi_file_buffer_t *buf = efi_allocate_pool(sizeof(efi_file_buffer_t));
    if (!buf) {
        efi_fclose(file);
        return NULL;
    }

    buf->data = efi_allocate_pool((UINTN)size);
    if (!buf->data) {
        efi_free_pool(buf);
        efi_fclose(file);
        return NULL;
    }

    buf->size = efi_fread(file, buf->data, (UINTN)size);
    efi_fclose(file);

    return buf;
}

static int has_efi_suffix(CHAR16 *name) {
    UINTN n = 0;
    while (name[n]) n++;
    if (n < 4) return 0;
    CHAR16 c[4];
    for (int i = 0; i < 4; i++) {
        CHAR16 ch = name[n - 4 + i];
        c[i] = (ch >= 'A' && ch <= 'Z') ? (CHAR16)(ch + 32) : ch;
    }
    return c[0] == '.' && c[1] == 'e' && c[2] == 'f' && c[3] == 'i';
}

void efi_load_fs_drivers(void) {
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return;

    EFI_FILE_PROTOCOL *dir = efi_open_dir(root, L"\\EFI\\visor\\drivers");
    if (!dir) { root->Close(root); return; }

    int sb = efi_secure_boot_enabled();
    int started = 0;
    CHAR16 name[128];
    int is_dir;
    while (efi_read_dirent(dir, name, 128, &is_dir)) {
        if (is_dir || !has_efi_suffix(name)) continue;

        CHAR16 path[256];
        SPrint(path, sizeof(path), L"\\EFI\\visor\\drivers\\%s", name);

        efi_file_buffer_t *buf = efi_load_file(path);
        if (!buf) continue;
        if (buf->data && buf->size) {
            if (sb && efi_shim_verify(buf->data, buf->size) != 1) {
                efi_log(L"WARN: driver failed Secure Boot verification - skipping");
            } else {
                EFI_HANDLE drv = NULL;
                if (!EFI_ERROR(BS->LoadImage(FALSE, IH, NULL, buf->data, buf->size, &drv)) && drv) {
                    if (!EFI_ERROR(BS->StartImage(drv, NULL, NULL))) started++;
                }
            }
            efi_free_pool(buf->data);
        }
        efi_free_pool(buf);
    }
    dir->Close(dir);
    root->Close(root);

    if (!started) { efi_log(L"drivers: none started"); return; }

    CHAR16 msg[64];
    SPrint(msg, sizeof(msg), L"drivers: started %d, connecting controllers", started);
    efi_log(msg);

    UINTN nh = 0;
    EFI_HANDLE *handles = NULL;
    if (!EFI_ERROR(BS->LocateHandleBuffer(AllHandles, NULL, NULL, &nh, &handles)) && handles) {
        for (UINTN i = 0; i < nh; i++)
            BS->ConnectController(handles[i], NULL, NULL, TRUE);
        efi_free_pool(handles);
    }
}

int visor_quiet = 0;

void efi_print(CHAR16 *msg, ...) {
    if (visor_quiet) return;
    if (msg)
        ST->ConOut->OutputString(ST->ConOut, msg);
}

#define LOG_PATH     L"\\EFI\\visor\\boot.log"
#define LOG_MARKER    "===== visor boot ====="
#define LOG_MARKER_W L"=================== visor boot ==================="
#define LOG_KEEP   3

static UINTN log_elapsed_cs(void) {
    static EFI_EVENT lt = NULL;
    static UINTN cs = 0;
    if (!lt) {
        if (EFI_ERROR(BS->CreateEvent(EVT_TIMER, TPL_APPLICATION, NULL, NULL, &lt))) {
            lt = NULL;
            return 0;
        }
        BS->SetTimer(lt, TimerPeriodic, 100000ULL);
        cs = 0;
        return 0;
    }
    while (BS->CheckEvent(lt) == EFI_SUCCESS) cs++;
    return cs;
}

static EFI_FILE_PROTOCOL *log_open_root(void) {
    EFI_FILE_PROTOCOL *boot_root = efi_boot_volume_root();
    if (boot_root) return boot_root;

    UINTN count = 0;
    EFI_HANDLE *handles = efi_locate_handle_buffer(
        &gEfiSimpleFileSystemProtocolGuid, &count);
    if (!handles) return NULL;

    EFI_FILE_IO_INTERFACE *io = NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    for (UINTN i = 0; i < count; i++) {
        if (!EFI_ERROR(BS->HandleProtocol(handles[i],
                &gEfiSimpleFileSystemProtocolGuid, (void**)&io))) {
            if (!EFI_ERROR(io->OpenVolume(io, &root))) break;
        }
        io = NULL; root = NULL;
    }
    efi_free_pool(handles);
    return root;
}

static EFI_FILE_PROTOCOL *g_log_root = NULL;
static EFI_FILE_PROTOCOL *g_log_file = NULL;

static EFI_FILE_PROTOCOL *log_file_open(void) {
    if (g_log_file) return g_log_file;
    g_log_root = log_open_root();
    if (!g_log_root) return NULL;
    EFI_FILE_PROTOCOL *f = NULL;
    if (EFI_ERROR(g_log_root->Open(g_log_root, &f, LOG_PATH,
            EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) || !f) {
        g_log_root->Close(g_log_root);
        g_log_root = NULL;
        return NULL;
    }
    f->SetPosition(f, 0xFFFFFFFFFFFFFFFFULL);
    g_log_file = f;
    return f;
}

void efi_log(CHAR16 *msg) {
    if (!msg) return;

    EFI_FILE_PROTOCOL *f = log_file_open();
    if (!f) return;

    UINTN cs = log_elapsed_cs();
    CHAR16 pfx[20];
    SPrint(pfx, sizeof(pfx), L"[%4d.%d%d] ",
           (int)(cs / 100), (int)((cs / 10) % 10), (int)(cs % 10));

    UINT8 line[320];
    UINTN n = 0;
    for (UINTN i = 0; pfx[i] && n < sizeof(line) - 2; i++)
        line[n++] = (pfx[i] < 0x80) ? (UINT8)pfx[i] : '?';
    for (UINTN i = 0; msg[i] && n < sizeof(line) - 2; i++) {
        CHAR16 c = msg[i];
        line[n++] = (c < 0x80) ? (UINT8)c : '?';
    }
    line[n++] = '\r';
    line[n++] = '\n';

    UINTN wsize = n;
    f->Write(f, &wsize, line);
    f->Flush(f);
}

void efi_log_begin(void) {
    efi_file_buffer_t *buf = efi_load_file(LOG_PATH);
    UINT8  *keep = NULL;
    UINTN   keep_len = 0;

    if (buf && buf->data && buf->size > 0) {
        UINT8 *d = (UINT8*)buf->data;
        UINTN  sz = buf->size;
        const char *m = LOG_MARKER;
        UINTN mlen = 0; while (m[mlen]) mlen++;

        UINTN offs[64]; UINTN nofs = 0;
        for (UINTN i = 0; i + mlen <= sz && nofs < 64; i++) {
            UINTN k = 0;
            while (k < mlen && d[i + k] == (UINT8)m[k]) k++;
            if (k == mlen) { offs[nofs++] = i; i += mlen - 1; }
        }

        if (nofs >= LOG_KEEP) {
            keep = d + offs[nofs - (LOG_KEEP - 1)];
            keep_len = sz - offs[nofs - (LOG_KEEP - 1)];
        } else if (nofs > 0) {
            keep = d + offs[0];
            keep_len = sz - offs[0];
        }
    }

    EFI_FILE_PROTOCOL *root = log_open_root();
    if (root) {

        EFI_FILE_PROTOCOL *f = NULL;
        if (!EFI_ERROR(root->Open(root, &f, LOG_PATH,
                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && f) {
            f->Delete(f);
        }
        if (!EFI_ERROR(root->Open(root, &f, LOG_PATH,
                EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && f) {
            if (keep && keep_len > 0) {
                UINTN w = keep_len;
                f->Write(f, &w, keep);
            }
            f->Flush(f);
            f->Close(f);
        }
        root->Close(root);
    }
    if (buf) { if (buf->data) efi_free_pool(buf->data); efi_free_pool(buf); }

    efi_log(LOG_MARKER_W);
}

void efi_sleep(UINTN milliseconds) {
    BS->Stall(milliseconds * 1000);
}

UINT64 efi_get_tick(void) {
    static EFI_EVENT timer = NULL;
    static UINT64 seconds = 0;

    if (!timer) {
        if (EFI_ERROR(BS->CreateEvent(EVT_TIMER, TPL_APPLICATION,
                                      NULL, NULL, &timer))) {
            timer = NULL;
            return 0;
        }

        BS->SetTimer(timer, TimerPeriodic, 10000000ULL);
        seconds = 0;
        return 0;
    }

    while (BS->CheckEvent(timer) == EFI_SUCCESS) {
        seconds++;
    }
    return seconds * 1000;
}

EFI_HANDLE* efi_locate_handle_buffer(EFI_GUID *proto, UINTN *count) {
    EFI_HANDLE *buffer = NULL;
    EFI_STATUS status = BS->LocateHandleBuffer(ByProtocol, proto, NULL, count, &buffer);
    if (EFI_ERROR(status)) {
        *count = 0;
        return NULL;
    }
    return buffer;
}

EFI_HANDLE efi_get_device_handle(EFI_DEVICE_PATH *dp) {
    if (!dp) return NULL;
    EFI_HANDLE handle = NULL;
    EFI_DEVICE_PATH *remaining = dp;
    EFI_STATUS status = BS->LocateDevicePath(
        &gEfiDevicePathProtocolGuid, &remaining, &handle);
    if (EFI_ERROR(status)) return NULL;
    return handle;
}

void efi_exit_boot_services(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    UINTN map_key = 0;
    UINTN map_size = 0;
    EFI_MEMORY_DESCRIPTOR *map = NULL;
    UINTN desc_size;
    UINT32 desc_version;

    system_table->BootServices->GetMemoryMap(&map_size, NULL, &map_key, &desc_size, &desc_version);
    map_size += 2 * desc_size;
    map = (EFI_MEMORY_DESCRIPTOR*)efi_allocate_pool(map_size);

    EFI_STATUS status;
    do {
        status = system_table->BootServices->GetMemoryMap(
            &map_size, map, &map_key, &desc_size, &desc_version);
        if (EFI_ERROR(status)) {
            map_size += desc_size;
            efi_free_pool(map);
            map = (EFI_MEMORY_DESCRIPTOR*)efi_allocate_pool(map_size);
        }
    } while (EFI_ERROR(status));

    status = system_table->BootServices->ExitBootServices(image_handle, map_key);
    if (!EFI_ERROR(status)) {
        system_table->BootServices = NULL;
    }

    efi_free_pool(map);
}

static EFI_GUID visor_var_guid = { 0xb9d4f5a2, 0x7c3e, 0x4f1a,
    { 0x9a, 0x6b, 0x2d, 0x8e, 0x1f, 0x44, 0x77, 0x10 } };

#define VISOR_VAR_ATTRS (EFI_VARIABLE_NON_VOLATILE | \
                         EFI_VARIABLE_BOOTSERVICE_ACCESS)

int efi_secure_boot_enabled(void) {
    UINT8 sb = 0;
    UINTN sz = sizeof(sb);
    UINT32 attr;
    EFI_STATUS s = RT->GetVariable(L"SecureBoot", &gEfiGlobalVariableGuid,
                                   &attr, &sz, &sb);
    return (!EFI_ERROR(s) && sb == 1) ? 1 : 0;
}

typedef struct {
    EFI_STATUS (EFIAPI *Verify)(void *buffer, UINT32 size);
    void *Hash;
    void *Context;
} shim_lock_protocol_t;

int efi_shim_verify(void *buf, UINTN size) {
    static EFI_GUID shim_guid = { 0x605dab50, 0xe046, 0x4300,
        { 0xab, 0xb6, 0x3d, 0xd8, 0x10, 0xdd, 0x8b, 0x23 } };
    shim_lock_protocol_t *shim = NULL;
    EFI_STATUS s = BS->LocateProtocol(&shim_guid, NULL, (void**)&shim);
    if (EFI_ERROR(s) || !shim || !shim->Verify) return -1;
    return EFI_ERROR(shim->Verify(buf, (UINT32)size)) ? 0 : 1;
}

CHAR16* efi_get_var_str(CHAR16 *name) {
    UINTN sz = 0;
    UINT32 attr;
    EFI_STATUS s = RT->GetVariable(name, &visor_var_guid, &attr, &sz, NULL);
    if (s != EFI_BUFFER_TOO_SMALL || sz == 0) return NULL;
    CHAR16 *buf = efi_allocate_pool(sz + sizeof(CHAR16));
    if (!buf) return NULL;
    s = RT->GetVariable(name, &visor_var_guid, &attr, &sz, buf);
    if (EFI_ERROR(s)) { efi_free_pool(buf); return NULL; }
    buf[sz / sizeof(CHAR16)] = 0;
    return buf;
}

void efi_set_var_str(CHAR16 *name, CHAR16 *val) {
    if (!val) return;
    UINTN len = 0;
    while (val[len]) len++;
    RT->SetVariable(name, &visor_var_guid, VISOR_VAR_ATTRS,
                    (len + 1) * sizeof(CHAR16), val);
}

int efi_get_var_u32(CHAR16 *name, UINT32 *out) {
    UINT32 v = 0;
    UINTN sz = sizeof(v);
    UINT32 attr;
    EFI_STATUS s = RT->GetVariable(name, &visor_var_guid, &attr, &sz, &v);
    if (EFI_ERROR(s) || sz != sizeof(v)) return 0;
    *out = v;
    return 1;
}

void efi_set_var_u32(CHAR16 *name, UINT32 val) {
    RT->SetVariable(name, &visor_var_guid, VISOR_VAR_ATTRS, sizeof(val), &val);
}

UINT32 efi_rand(void) {
    static UINT32 state = 0;
    if (!state) {
        EFI_TIME t;
        if (!EFI_ERROR(RT->GetTime(&t, NULL)))
            state = t.Nanosecond ^ ((UINT32)t.Second << 24) ^
                    ((UINT32)t.Minute << 16) ^ ((UINT32)t.Hour << 8) ^ t.Day;
        state ^= (UINT32)efi_get_tick();
        if (!state) state = 0x2545F491;
    }
    state = state * 1103515245u + 12345u;
    return state;
}
