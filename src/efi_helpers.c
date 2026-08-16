#include "efi_helpers.h"
#include "arch.h"
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

EFI_HANDLE efi_boot_volume_handle(void) {
    return boot_device_handle();
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

CHAR16* efi_handle_partition_uuid(EFI_HANDLE handle) {
    EFI_DEVICE_PATH *dp = NULL;
    if (EFI_ERROR(BS->HandleProtocol(handle, &gEfiDevicePathProtocolGuid, (void**)&dp)) || !dp)
        return NULL;

    for (EFI_DEVICE_PATH *node = dp; !IsDevicePathEnd(node);
         node = (EFI_DEVICE_PATH*)((UINT8*)node + DevicePathNodeLength(node))) {
        if (DevicePathType(node) != MEDIA_DEVICE_PATH ||
            DevicePathSubType(node) != MEDIA_HARDDRIVE_DP)
            continue;
        HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH*)node;
        if (hd->SignatureType != SIGNATURE_TYPE_GUID) continue;

        EFI_GUID g;
        CopyMem(&g, hd->Signature, sizeof(g));
        UINT8 b[16] = {
            (UINT8)(g.Data1 >> 24), (UINT8)(g.Data1 >> 16),
            (UINT8)(g.Data1 >> 8),  (UINT8)g.Data1,
            (UINT8)(g.Data2 >> 8),  (UINT8)g.Data2,
            (UINT8)(g.Data3 >> 8),  (UINT8)g.Data3,
            g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7],
        };

        CHAR16 *s = efi_allocate_pool(37 * sizeof(CHAR16));
        if (!s) return NULL;
        static const CHAR16 hexd[] = L"0123456789abcdef";
        UINTN o = 0;
        for (UINTN i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) s[o++] = '-';
            s[o++] = hexd[b[i] >> 4];
            s[o++] = hexd[b[i] & 0xF];
        }
        s[o] = 0;
        return s;
    }
    return NULL;
}

#define NORM_PATH_MAX 512
static CHAR16* collapse_backslashes(CHAR16 *path, CHAR16 *buf, UINTN cap) {
    if (!path) return path;
    UINTN o = 0;
    for (UINTN i = 0; path[i]; i++) {
        if (path[i] == '\\' && o > 0 && buf[o - 1] == '\\') continue;
        if (o + 1 >= cap) return path;
        buf[o++] = path[i];
    }
    buf[o] = '\0';
    return buf;
}

EFI_DEVICE_PATH* efi_make_file_path(EFI_HANDLE handle, CHAR16 *filename) {
    CHAR16 nbuf[NORM_PATH_MAX];
    filename = collapse_backslashes(filename, nbuf, NORM_PATH_MAX);
    EFI_DEVICE_PATH *dp = NULL;
    BS->HandleProtocol(handle, &gEfiDevicePathProtocolGuid, (void**)&dp);
    if (!dp) return NULL;

    UINTN dp_total = DevicePathSize(dp);
    if (dp_total < sizeof(EFI_DEVICE_PATH_PROTOCOL)) return NULL;
    UINTN dp_len    = dp_total - sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN fname_len = StrLen(filename) * sizeof(CHAR16);
    UINTN fp_size   = sizeof(FILEPATH_DEVICE_PATH) + fname_len;
    UINTN end_size  = sizeof(EFI_DEVICE_PATH_PROTOCOL);
    UINTN total_len = dp_len + fp_size + end_size;

    if (fp_size > 0xFFFF) {
        efi_log(L"ERROR: file path too long for a device-path node");
        return NULL;
    }

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

EFI_DEVICE_PATH* efi_file_device_path_on_handle(EFI_HANDLE volume, CHAR16 *path) {
    if (!volume) return NULL;
    EFI_FILE_PROTOCOL *root = open_root_on_handle(volume);
    if (!root) return NULL;
    int found = efi_file_exists_root(root, path);
    root->Close(root);
    return found ? efi_make_file_path(volume, path) : NULL;
}

static int dp_node_is_harddrive(EFI_DEVICE_PATH *n) {
    return DevicePathType(n) == MEDIA_DEVICE_PATH &&
           DevicePathSubType(n) == MEDIA_HARDDRIVE_DP;
}


int efi_handles_same_disk(EFI_HANDLE a, EFI_HANDLE b) {
    if (!a || !b) return 0;
    if (a == b) return 1;

    EFI_DEVICE_PATH *pa = NULL, *pb = NULL;
    if (EFI_ERROR(BS->HandleProtocol(a, &gEfiDevicePathProtocolGuid, (void**)&pa)) || !pa)
        return 0;
    if (EFI_ERROR(BS->HandleProtocol(b, &gEfiDevicePathProtocolGuid, (void**)&pb)) || !pb)
        return 0;

    for (;;) {
        int end_a = IsDevicePathEnd(pa) || dp_node_is_harddrive(pa);
        int end_b = IsDevicePathEnd(pb) || dp_node_is_harddrive(pb);
        if (end_a || end_b) return end_a && end_b;

        UINTN la = DevicePathNodeLength(pa);
        UINTN lb = DevicePathNodeLength(pb);
        if (la != lb || la < 4 || CompareMem(pa, pb, la) != 0) return 0;

        pa = (EFI_DEVICE_PATH*)((UINT8*)pa + la);
        pb = (EFI_DEVICE_PATH*)((UINT8*)pb + lb);
    }
}

static efi_file_t* efi_fopen_inner(CHAR16 *path, CHAR16 *uuid) {
    efi_file_t *file = efi_allocate_pool(sizeof(efi_file_t));
    if (!file) return NULL;
    file->root = NULL;
    file->handle = NULL;

    EFI_HANDLE boot_handle = boot_device_handle();
    if (efi_handle_matches_partition_uuid(boot_handle, uuid)) {
        EFI_FILE_PROTOCOL *root = open_root_on_handle(boot_handle);
        if (root) {
            if (!EFI_ERROR(root->Open(root, &file->handle, path, EFI_FILE_MODE_READ, 0))) {
                file->root = root;
                return file;
            }
            root->Close(root);
        }
    }

    UINTN count = 0;
    EFI_HANDLE *handles = efi_locate_handle_buffer(
        &gEfiSimpleFileSystemProtocolGuid, &count);
    if (handles) {
        for (UINTN i = 0; i < count; i++) {
            if (!efi_handle_matches_partition_uuid(handles[i], uuid))
                continue;
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

#define MAX_DEFERRED_DRIVERS 8
static EFI_HANDLE g_deferred_drivers[MAX_DEFERRED_DRIVERS];
static int g_deferred_count;
static int g_deferred_started;

int efi_fs_drivers_deferred(void) {
    return g_deferred_count > 0 && !g_deferred_started;
}


int efi_fs_drivers_pending(void) {
    return efi_fs_drivers_deferred() || !efi_fs_probe_exhausted();
}


static int g_deferred_lazy = 1;

void efi_fs_drivers_set_lazy(int enabled) {
    g_deferred_lazy = enabled ? 1 : 0;
}

efi_file_t* efi_fopen_uuid(CHAR16 *path, CHAR16 *uuid) {
    CHAR16 nbuf[NORM_PATH_MAX];
    path = collapse_backslashes(path, nbuf, NORM_PATH_MAX);

    efi_file_t *file = efi_fopen_inner(path, uuid);
    if (file) return file;

    if (g_deferred_lazy && !g_deferred_started && g_deferred_count > 0) {
        efi_start_deferred_images();
        file = efi_fopen_inner(path, uuid);
        if (file) return file;

        UINTN probed = 0;
        while (efi_connect_next_block(uuid)) {
            probed++;
            file = efi_fopen_inner(path, uuid);
            if (!file) continue;
            CHAR16 m[96];
            SPrint(m, sizeof(m),
                   L"drivers: file found after probing %d block device(s)",
                   (int)probed);
            efi_log(m);
            return file;
        }
        return NULL;
    }

    return NULL;
}

efi_file_t* efi_fopen(CHAR16 *path) {
    return efi_fopen_uuid(path, NULL);
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
    UINTN requested = size;
    EFI_STATUS status = file->handle->Read(file->handle, &size, buf);
    if (EFI_ERROR(status)) return 0;
    return size > requested ? requested : size;
}

int efi_file_exists_root(EFI_FILE_PROTOCOL *root, CHAR16 *path) {
    if (!root) return 0;
    CHAR16 nbuf[NORM_PATH_MAX];
    path = collapse_backslashes(path, nbuf, NORM_PATH_MAX);
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

static int read_dirent_core(EFI_FILE_PROTOCOL *dir, CHAR16 *name_out,
                            UINTN name_cap, int *is_dir) {
    UINT8 sbuf[1024] __attribute__((aligned(8)));
    for (;;) {
        UINT8 *buf = sbuf;
        UINT8 *heap = NULL;
        UINTN size = sizeof(sbuf);
        EFI_STATUS s = dir->Read(dir, &size, buf);
        if (s == EFI_BUFFER_TOO_SMALL && size > sizeof(sbuf)) {
            heap = efi_allocate_pool(size);
            if (!heap) return 0;
            buf = heap;
            s = dir->Read(dir, &size, buf);
        }
        if (EFI_ERROR(s) || size == 0) {
            if (heap) efi_free_pool(heap);
            return 0;
        }

        if (size < SIZE_OF_EFI_FILE_INFO + sizeof(CHAR16)) {
            if (heap) efi_free_pool(heap);
            continue;
        }
        EFI_FILE_INFO *info = (EFI_FILE_INFO *)buf;
        CHAR16 *fn = info->FileName;
        UINTN fn_max = (size - SIZE_OF_EFI_FILE_INFO) / sizeof(CHAR16);

        if (fn_max >= 2 && fn[0] == '.' &&
            (fn[1] == '\0' || (fn_max >= 3 && fn[1] == '.' && fn[2] == '\0'))) {
            if (heap) efi_free_pool(heap);
            continue;
        }
        UINTN i = 0;
        while (i < fn_max && fn[i] && i < name_cap - 1) {
            name_out[i] = fn[i];
            i++;
        }
        name_out[i] = '\0';
        if (is_dir) *is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
        if (heap) efi_free_pool(heap);
        return 1;
    }
}

int efi_read_dirent(EFI_FILE_PROTOCOL *dir, CHAR16 *name_out, UINTN name_cap, int *is_dir) {
    if (!dir || !name_out || name_cap == 0) return 0;
    return read_dirent_core(dir, name_out, name_cap, is_dir);
}

int efi_readdir(efi_file_t *dir, CHAR16 *name_out, UINTN name_cap, int *is_dir) {
    if (!dir || !dir->handle || !name_out || name_cap == 0) return 0;
    return read_dirent_core(dir->handle, name_out, name_cap, is_dir);
}

UINT64 efi_file_size(EFI_FILE_PROTOCOL *fh) {
    if (!fh) return 0;

    UINTN info_size = 0;
    EFI_STATUS s = fh->GetInfo(fh, &gEfiFileInfoGuid, &info_size, NULL);
    if (s == EFI_BUFFER_TOO_SMALL && info_size > 0) {
        EFI_FILE_INFO *info = efi_allocate_pool(info_size);
        if (info) {
            s = fh->GetInfo(fh, &gEfiFileInfoGuid, &info_size, info);

            if (!EFI_ERROR(s) && info->FileSize > 0) {
                UINT64 size = info->FileSize;
                efi_free_pool(info);
                return size;
            }
            efi_free_pool(info);
        }
    }

    UINT64 size = 0;
    fh->SetPosition(fh, ~0ULL);
    if (EFI_ERROR(fh->GetPosition(fh, &size))) size = 0;
    fh->SetPosition(fh, 0);
    return size;
}

static efi_file_buffer_t* efi_read_open_file(efi_file_t *file) {
    if (!file) return NULL;

    UINT64 size = efi_file_size(file->handle);

    if (size == 0 || size > 256ULL * 1024 * 1024) {
        CHAR16 d[96];
        SPrint(d, sizeof(d), L"WARN: file size %ld is zero or implausibly large - skipping", size);
        efi_log(d);
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

    UINTN total = 0;
    while (total < (UINTN)size) {
        UINTN n = efi_fread(file, (UINT8*)buf->data + total, (UINTN)size - total);
        if (n == 0) break;
        total += n;
    }
    efi_fclose(file);

    if (total != (UINTN)size) {
        efi_log(L"WARN: short file read - skipping incomplete data");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    buf->size = total;
    return buf;
}

efi_file_buffer_t* efi_load_file_uuid(CHAR16 *path, CHAR16 *uuid) {
    return efi_read_open_file(efi_fopen_uuid(path, uuid));
}

efi_file_buffer_t* efi_load_file(CHAR16 *path) {
    return efi_load_file_uuid(path, NULL);
}


efi_file_buffer_t* efi_load_file_on_handle(EFI_HANDLE volume, CHAR16 *path) {
    if (!volume) return NULL;
    CHAR16 nbuf[NORM_PATH_MAX];
    path = collapse_backslashes(path, nbuf, NORM_PATH_MAX);

    EFI_FILE_PROTOCOL *root = open_root_on_handle(volume);
    if (!root) return NULL;

    efi_file_t *file = efi_allocate_pool(sizeof(efi_file_t));
    if (!file) { root->Close(root); return NULL; }
    file->root = root;
    file->handle = NULL;
    if (EFI_ERROR(root->Open(root, &file->handle, path, EFI_FILE_MODE_READ, 0))) {
        root->Close(root);
        efi_free_pool(file);
        return NULL;
    }
    return efi_read_open_file(file);
}

int efi_rename_file(CHAR16 *oldp, CHAR16 *newp) {
    UINTN n = 0;
    EFI_HANDLE *h = efi_locate_handle_buffer(&gEfiSimpleFileSystemProtocolGuid, &n);
    if (!h) return 0;
    int ok = 0;
    for (UINTN i = 0; i < n && !ok; i++) {
        EFI_FILE_IO_INTERFACE *io = NULL;
        if (EFI_ERROR(BS->HandleProtocol(h[i], &gEfiSimpleFileSystemProtocolGuid, (void**)&io)) || !io) continue;
        EFI_FILE_PROTOCOL *root = NULL;
        if (EFI_ERROR(io->OpenVolume(io, &root)) || !root) continue;
        EFI_FILE_PROTOCOL *fh = NULL;
        if (!EFI_ERROR(root->Open(root, &fh, oldp, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && fh) {
            UINT64 sz = efi_file_size(fh);
            if (sz > 0 && sz < 1024 * 1024) {
                UINT8 *buf = efi_allocate_pool((UINTN)sz);
                if (buf) {
                    UINTN rd = (UINTN)sz;
                    if (!EFI_ERROR(fh->Read(fh, &rd, buf)) && rd == (UINTN)sz) {
                        EFI_FILE_PROTOCOL *nf = NULL;
                        if (!EFI_ERROR(root->Open(root, &nf, newp,
                                EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && nf) {
                            UINTN w = rd;
                            EFI_STATUS ws = nf->Write(nf, &w, buf);
                            EFI_STATUS fs = nf->Flush(nf);
                            nf->Close(nf);
                            if (!EFI_ERROR(ws) && !EFI_ERROR(fs) && w == rd) {
                                EFI_STATUS ds = fh->Delete(fh);
                                fh = NULL;
                                ok = !EFI_ERROR(ds);
                            }
                        }
                    }
                    efi_free_pool(buf);
                }
            }
            if (fh) fh->Close(fh);
        }
        root->Close(root);
    }
    efi_free_pool(h);
    return ok;
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

int efi_handle_has_filesystem(EFI_HANDLE handle) {
    void *io = NULL;
    return handle &&
           !EFI_ERROR(BS->HandleProtocol(handle,
                          &gEfiSimpleFileSystemProtocolGuid, &io)) && io;
}


void efi_start_deferred_images(void) {
    if (g_deferred_started) return;
    if (g_deferred_count == 0) { g_deferred_started = 1; return; }

    efi_log(L"drivers: starting deferred filesystem drivers");
    int started = 0;
    for (int i = 0; i < g_deferred_count; i++) {
        if (!EFI_ERROR(BS->StartImage(g_deferred_drivers[i], NULL, NULL)))
            started++;
        else
            BS->UnloadImage(g_deferred_drivers[i]);
        g_deferred_drivers[i] = NULL;
    }
    g_deferred_count = 0;
    g_deferred_started = 1;

    CHAR16 msg[64];
    SPrint(msg, sizeof(msg), L"drivers: started %d filesystem driver(s)", started);
    efi_log(msg);
}


static EFI_HANDLE *g_probe_blk;
static UINTN       g_probe_n;
static UINTN       g_probe_i;
static int         g_probe_ready;

static void probe_init(CHAR16 *prefer_uuid) {
    if (g_probe_ready) return;
    g_probe_ready = 1;
    g_probe_n = 0;
    g_probe_i = 0;
    g_probe_blk = efi_locate_handle_buffer(&gEfiBlockIoProtocolGuid, &g_probe_n);
    if (!g_probe_blk) { g_probe_n = 0; return; }

    UINTN out = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (UINTN i = out; i < g_probe_n; i++) {
            EFI_BLOCK_IO *bio = NULL;
            BS->HandleProtocol(g_probe_blk[i], &gEfiBlockIoProtocolGuid, (void**)&bio);
            int logical = bio && bio->Media && bio->Media->LogicalPartition;
            int match = (pass == 0) && prefer_uuid && prefer_uuid[0] &&
                        efi_handle_matches_partition_uuid(g_probe_blk[i], prefer_uuid);
            int want = (pass == 0) ? match : (pass == 1 ? logical : 1);
            if (!want) continue;
            EFI_HANDLE t = g_probe_blk[out];
            g_probe_blk[out] = g_probe_blk[i];
            g_probe_blk[i] = t;
            out++;
        }
    }
}

int efi_fs_probe_exhausted(void) {
    return g_probe_ready && g_probe_i >= g_probe_n;
}

int efi_connect_next_block(CHAR16 *prefer_uuid) {
    probe_init(prefer_uuid);
    while (g_probe_i < g_probe_n) {
        EFI_HANDLE h = g_probe_blk[g_probe_i++];
        if (efi_handle_has_filesystem(h)) continue;
        BS->ConnectController(h, NULL, NULL, FALSE);
        return 1;
    }
    return 0;
}

void efi_start_deferred_drivers(void) {
    if (g_deferred_started && efi_fs_probe_exhausted()) return;
    efi_start_deferred_images();

    UINTN connected = 0;
    while (efi_connect_next_block(NULL)) connected++;

    { CHAR16 m[112]; SPrint(m, sizeof(m),
        L"drivers: deferred start complete (probed %d of %d block device(s))",
        (int)connected, (int)g_probe_n);
      efi_log(m); }
}

void efi_load_fs_drivers(void) {
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return;

    EFI_FILE_PROTOCOL *dir = efi_open_dir(root, L"\\EFI\\visor\\drivers");
    if (!dir) { root->Close(root); return; }

    int sb = efi_secure_boot_enabled();
    CHAR16 name[128];
    int is_dir;
    while (efi_read_dirent(dir, name, 128, &is_dir)) {
        if (is_dir || !has_efi_suffix(name)) continue;

        CHAR16 path[256];
        SPrint(path, sizeof(path), L"\\EFI\\visor\\drivers\\%s", name);

        efi_file_buffer_t *buf = efi_load_file(path);
        if (!buf) continue;
        if (buf->data && buf->size) {
            if (sb && efi_shim_verify(buf->data, buf->size) == 0) {
                efi_log(L"WARN: driver rejected by SHIM_LOCK - skipping");
            } else {
                EFI_HANDLE drv = NULL;
                if (!EFI_ERROR(BS->LoadImage(FALSE, IH, NULL, buf->data, buf->size, &drv)) && drv) {
                    if (g_deferred_count < MAX_DEFERRED_DRIVERS)
                        g_deferred_drivers[g_deferred_count++] = drv;
                    else
                        BS->UnloadImage(drv);
                }
            }
            efi_free_pool(buf->data);
        }
        efi_free_pool(buf);
    }
    dir->Close(dir);
    root->Close(root);

    if (g_deferred_count == 0) { efi_log(L"drivers: none loaded"); return; }

    CHAR16 msg[64];
    SPrint(msg, sizeof(msg), L"drivers: loaded %d (deferred start)", g_deferred_count);
    efi_log(msg);
}

int visor_quiet = 0;
int visor_log_to_console = 0;
int visor_boot_services_active = 1;
static int visor_log_to_file = 1;

void efi_log_set_console(int enabled) {
    visor_log_to_console = enabled ? 1 : 0;
}

void efi_log_set_file(int enabled) {
    visor_log_to_file = enabled ? 1 : 0;
}

int efi_log_file_enabled(void) {
    return visor_log_to_file;
}

void efi_print(CHAR16 *msg, ...) {
    if (!visor_boot_services_active) return;
    if (visor_quiet) return;
    if (msg)
        ST->ConOut->OutputString(ST->ConOut, msg);
}

#define LOG_PATH      L"\\EFI\\visor\\boot.log"
#define LOG_MARKER_W  L"=================== visor boot ==================="
#define LOG_MARKER_A   "=================== visor boot ==================="
#define LOG_KEEP      3
#define LOG_MAX_BYTES (128 * 1024)

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
    EFI_STATUS status = g_log_root->Open(
        g_log_root, &f, LOG_PATH, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(status) || !f)
        status = g_log_root->Open(
            g_log_root, &f, LOG_PATH,
            EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(status) || !f) {
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
    if (!visor_boot_services_active) return;

    UINTN cs = log_elapsed_cs();
    CHAR16 pfx[20];
    SPrint(pfx, sizeof(pfx), L"[%4d.%d%d] ",
           (int)(cs / 100), (int)((cs / 10) % 10), (int)(cs % 10));

    if (visor_log_to_console && !visor_quiet && ST && ST->ConOut) {
        ST->ConOut->OutputString(ST->ConOut, pfx);
        ST->ConOut->OutputString(ST->ConOut, msg);
        ST->ConOut->OutputString(ST->ConOut, L"\r\n");
    }

    if (!visor_log_to_file) return;

    EFI_FILE_PROTOCOL *f = log_file_open();
    if (!f) return;

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

void efi_log_close(void) {
    if (g_log_file) { g_log_file->Close(g_log_file); g_log_file = NULL; }
    if (g_log_root) { g_log_root->Close(g_log_root); g_log_root = NULL; }
}


void efi_log_begin(void) {
    efi_log(LOG_MARKER_W);
}


void efi_log_rotate(void) {
    if (!visor_log_to_file) return;

    visor_log_to_file = 0;
    efi_file_buffer_t *buf = efi_load_file(LOG_PATH);
    visor_log_to_file = 1;
    if (!buf) return;
    if (!buf->data || buf->size <= LOG_MAX_BYTES) {
        if (buf->data) efi_free_pool(buf->data);
        efi_free_pool(buf);
        return;
    }

    UINT8 *d  = (UINT8*)buf->data;
    UINTN  sz = buf->size;
    const char *m = LOG_MARKER_A;
    UINTN mlen = 0; while (m[mlen]) mlen++;

    UINT8 *keep = NULL;
    UINTN  keep_len = 0;

    UINTN ring[LOG_KEEP];
    UINTN nofs = 0;

    for (UINTN i = 0; i + mlen <= sz; i++) {
        UINTN k = 0;
        while (k < mlen && d[i + k] == (UINT8)m[k]) k++;
        if (k == mlen) {
            ring[nofs % LOG_KEEP] = i;
            nofs++;
            i += mlen - 1;
        }
    }

    if (nofs) {
        UINTN start = (nofs >= LOG_KEEP) ? ring[nofs % LOG_KEEP] : ring[0];
        keep = d + start;
        keep_len = sz - start;
    }

    efi_log_close();

    EFI_FILE_PROTOCOL *root = log_open_root();
    if (root) {
        EFI_FILE_PROTOCOL *f = NULL;
        if (!EFI_ERROR(root->Open(root, &f, LOG_PATH,
                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && f)
            f->Delete(f);
        f = NULL;
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

    efi_free_pool(buf->data);
    efi_free_pool(buf);
    efi_log(L"log: rotated boot.log (size cap reached)");
}

void efi_sleep(UINTN milliseconds) {
    if (!visor_boot_services_active) return;
    BS->Stall(milliseconds * 1000);
}

int efi_key_pending(void) {
    if (!visor_boot_services_active || !ST || !ST->ConIn) return 0;
    if (!ST->ConIn->WaitForKey) return 0;
    return BS->CheckEvent(ST->ConIn->WaitForKey) == EFI_SUCCESS;
}

UINT64 efi_get_tick(void) {
    return arch_now_us() / 1000;
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

