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
            EFI_HANDLE drv = NULL;
            if (!EFI_ERROR(BS->LoadImage(FALSE, IH, NULL, buf->data, buf->size, &drv)) && drv) {
                if (!EFI_ERROR(BS->StartImage(drv, NULL, NULL))) started++;
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
#define LOG_MARKER_W L"===== visor boot ====="
#define LOG_KEEP   3

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

void efi_log(CHAR16 *msg) {
    if (!msg) return;

    EFI_FILE_PROTOCOL *root = log_open_root();
    if (!root) return;

    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS s = root->Open(root, &f, LOG_PATH,
        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(s) || !f) { root->Close(root); return; }

    f->SetPosition(f, 0xFFFFFFFFFFFFFFFFULL);

    UINT8 line[256];
    UINTN n = 0;
    for (UINTN i = 0; msg[i] && n < sizeof(line) - 2; i++) {
        CHAR16 c = msg[i];
        line[n++] = (c < 0x80) ? (UINT8)c : '?';
    }
    line[n++] = '\r';
    line[n++] = '\n';

    UINTN wsize = n;
    f->Write(f, &wsize, line);
    f->Flush(f);
    f->Close(f);
    root->Close(root);
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
