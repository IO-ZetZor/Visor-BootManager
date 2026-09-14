/* efi_file.c - file open/read/rename and whole-file loading */
#include "efi_helpers_internal.h"

static efi_file_t* efi_fopen_inner(CHAR16 *path, CHAR16 *uuid) {
    efi_file_t *file = efi_allocate_pool(sizeof(efi_file_t));
    if (!file) return NULL;
    file->root = NULL;
    file->handle = NULL;
    file->volume = NULL;

    EFI_HANDLE boot_handle = boot_device_handle();
    if (efi_handle_matches_partition_uuid(boot_handle, uuid)) {
        EFI_FILE_PROTOCOL *root = open_root_on_handle(boot_handle);
        if (root) {
            if (!EFI_ERROR(root->Open(root, &file->handle, path, EFI_FILE_MODE_READ, 0))) {
                file->root = root;
                file->volume = boot_handle;
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
            g_volume_opens++;
            UINT64 t_mount = arch_now_us();
            EFI_STATUS ms = io->OpenVolume(io, &r);
            efi_log_slow(L"OpenVolume (mounting the volume)", t_mount);
            if (EFI_ERROR(ms) || !r)
                continue;
            UINT64 t_open = arch_now_us();
            EFI_STATUS os = r->Open(r, &file->handle, path, EFI_FILE_MODE_READ, 0);
            efi_log_slow(L"Open (resolving the path on the volume)", t_open);
            if (!EFI_ERROR(os)) {
                file->root = r;
                file->volume = handles[i];
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

efi_file_t* efi_fopen_uuid(CHAR16 *path, CHAR16 *uuid) {
    CHAR16 nbuf[NORM_PATH_MAX];
    path = collapse_backslashes(path, nbuf, NORM_PATH_MAX);

    efi_file_t *file = efi_fopen_inner(path, uuid);
    if (file) return file;

    int start_pending = !g_deferred_started && g_deferred_count > 0;
    if (g_deferred_lazy && (start_pending || fs_probe_in_progress())) {
        if (start_pending) {
            efi_start_deferred_images();
            file = efi_fopen_inner(path, uuid);
            if (file) return file;
        }

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
    EFI_STATUS status = file->handle->Read(file->handle, &size, buf);
    if (EFI_ERROR(status)) return 0;
    return size;
}

int efi_file_exists_root(EFI_FILE_PROTOCOL *root, CHAR16 *path) {
    if (!root) return 0;
    CHAR16 nbuf[NORM_PATH_MAX];
    path = collapse_backslashes(path, nbuf, NORM_PATH_MAX);
    EFI_FILE_PROTOCOL *f = NULL;
    UINT64 t0 = arch_now_us();
    EFI_STATUS s = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    efi_log_slow(L"Open (existence check while searching for the file)", t0);
    if (EFI_ERROR(s) || !f)
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

    UINT64 t0 = arch_now_us();
    UINTN info_size = 0;
    EFI_STATUS s = fh->GetInfo(fh, &gEfiFileInfoGuid, &info_size, NULL);
    if (s == EFI_BUFFER_TOO_SMALL && info_size > 0) {
        EFI_FILE_INFO *info = efi_allocate_pool(info_size);
        if (info) {
            s = fh->GetInfo(fh, &gEfiFileInfoGuid, &info_size, info);

            if (!EFI_ERROR(s) && info->FileSize > 0) {
                UINT64 size = info->FileSize;
                efi_free_pool(info);
                efi_log_slow(L"GetInfo (asking the driver for the file size)", t0);
                return size;
            }
            efi_free_pool(info);
        }
    }

    efi_log(L"size: GetInfo gave no file size - falling back to a seek to the end");
    UINT64 size = 0;
    fh->SetPosition(fh, ~0ULL);
    if (EFI_ERROR(fh->GetPosition(fh, &size))) size = 0;
    fh->SetPosition(fh, 0);
    efi_log_slow(L"SetPosition/GetPosition (file size by seeking to the end)", t0);
    return size;
}

#define BIG_READ_MIN   (16ULL * 1024 * 1024)
#define BIG_READ_CHUNK (8ULL  * 1024 * 1024)

static int big_read_console(void) {
    return visor_log_to_console && !visor_quiet && ST && ST->ConOut;
}

static void big_read_progress(UINTN done, UINT64 size) {
    if (!big_read_console()) return;
    CHAR16 line[64];
    SPrint(line, sizeof(line), L"\r  reading %d of %d MB ",
           (int)(done / (1024 * 1024)), (int)(size / (1024 * 1024)));
    ST->ConOut->OutputString(ST->ConOut, line);
}

static efi_file_buffer_t* efi_read_open_file(efi_file_t *file) {
    if (!file) return NULL;

    UINT64 size = efi_file_size(file->handle);

    if (size == 0 || size > 512ULL * 1024 * 1024) {
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

    UINT64 t_alloc = arch_now_us();
    buf->data = efi_allocate_pool((UINTN)size);
    if (!buf->data) {
        efi_free_pool(buf);
        efi_fclose(file);
        return NULL;
    }
    efi_log_slow(L"AllocatePool for the whole-file buffer", t_alloc);

    int big = size >= BIG_READ_MIN;
    if (big) {
        CHAR16 d[112];
        SPrint(d, sizeof(d),
               L"read: %d MB to load - a non-FAT filesystem driver makes this slow",
               (int)(size / (1024 * 1024)));
        efi_log(d);
    }

    UINT64 t0 = arch_now_us();
    UINTN total = 0;
    int chunk = 0;
    while (total < (UINTN)size) {
        UINTN want = (UINTN)size - total;
        if (big && want > (UINTN)BIG_READ_CHUNK) want = (UINTN)BIG_READ_CHUNK;
        UINT64 t_chunk = arch_now_us();
        UINTN n = efi_fread(file, (UINT8*)buf->data + total, want);
        if (n == 0) break;
        total += n;
        if (big) {
            UINT64 cms = (arch_now_us() - t_chunk) / 1000;
            if (cms == 0) cms = 1;
            CHAR16 d[96];
            SPrint(d, sizeof(d), L"read: chunk %d - %d MB in %d ms (%d MB/s)",
                   chunk, (int)(n / (1024 * 1024)), (int)cms,
                   (int)((n / 1024) * 1000 / cms / 1024));
            efi_log(d);
            big_read_progress(total, size);
        }
        chunk++;
    }
    efi_fclose(file);

    if (big) {
        if (big_read_console()) ST->ConOut->OutputString(ST->ConOut, L"\r\n");
        UINT64 ms = (arch_now_us() - t0) / 1000;
        if (ms == 0) ms = 1;
        CHAR16 d[96];
        SPrint(d, sizeof(d), L"read: %d MB in %d ms (%d MB/s)",
               (int)(total / (1024 * 1024)), (int)ms,
               (int)((total / 1024) * 1000 / ms / 1024));
        efi_log(d);
    }

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

EFI_FILE_PROTOCOL* efi_open_volume_root(EFI_HANDLE volume) {
    return open_root_on_handle(volume);
}

efi_file_buffer_t* efi_load_file_keep_volume(CHAR16 *path, CHAR16 *uuid,
                                             EFI_HANDLE *volume_out,
                                             EFI_FILE_PROTOCOL **root_out,
                                             int *opened_out) {
    if (volume_out) *volume_out = NULL;
    if (root_out) *root_out = NULL;
    if (opened_out) *opened_out = 0;

    efi_file_t *file = efi_fopen_uuid(path, uuid);
    if (!file) return NULL;
    if (opened_out) *opened_out = 1;

    EFI_HANDLE volume = file->volume;
    EFI_FILE_PROTOCOL *root = file->root;
    file->root = NULL;

    efi_file_buffer_t *buf = efi_read_open_file(file);
    if (!buf) {
        if (root) root->Close(root);
        return NULL;
    }
    if (volume_out) *volume_out = volume;
    if (root_out) *root_out = root;
    else if (root) root->Close(root);
    return buf;
}

efi_file_buffer_t* efi_load_file_from_root(EFI_FILE_PROTOCOL *root, CHAR16 *path,
                                           int *opened_out) {
    if (opened_out) *opened_out = 0;
    if (!root || !path) return NULL;
    CHAR16 nbuf[NORM_PATH_MAX];
    path = collapse_backslashes(path, nbuf, NORM_PATH_MAX);

    efi_file_t *file = efi_allocate_pool(sizeof(efi_file_t));
    if (!file) return NULL;
    file->root = NULL;
    file->handle = NULL;
    file->volume = NULL;
    UINT64 t_open = arch_now_us();
    EFI_STATUS os = root->Open(root, &file->handle, path, EFI_FILE_MODE_READ, 0);
    efi_log_slow(L"Open (path resolve on the already-mounted volume)", t_open);
    if (EFI_ERROR(os) || !file->handle) {
        efi_free_pool(file);
        return NULL;
    }
    if (opened_out) *opened_out = 1;
    return efi_read_open_file(file);
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
    file->volume = volume;
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
        g_volume_opens++;
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
