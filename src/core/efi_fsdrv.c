/* efi_fsdrv.c - deferred filesystem driver loading and block probing */
#include "efi_helpers_internal.h"

#define MAX_DEFERRED_DRIVERS 8
static EFI_HANDLE g_deferred_drivers[MAX_DEFERRED_DRIVERS];
int g_deferred_count;
int g_deferred_started;

int efi_fs_drivers_deferred(void) {
    return g_deferred_count > 0 && !g_deferred_started;
}

int efi_fs_drivers_pending(void) {
    return efi_fs_drivers_deferred() || !efi_fs_probe_exhausted();
}

int g_deferred_lazy = 1;

void efi_fs_drivers_set_lazy(int enabled) {
    g_deferred_lazy = enabled ? 1 : 0;
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

int fs_probe_in_progress(void) {
    return g_probe_ready && g_probe_i < g_probe_n;
}

int efi_connect_next_block(CHAR16 *prefer_uuid) {
    probe_init(prefer_uuid);
    while (g_probe_i < g_probe_n) {
        EFI_HANDLE h = g_probe_blk[g_probe_i++];
        if (efi_handle_has_filesystem(h)) continue;
        UINT64 t0 = arch_now_us();
        BS->ConnectController(h, NULL, NULL, FALSE);
        efi_log_slow(L"ConnectController (driver binding to a block device)", t0);
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
