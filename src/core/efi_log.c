/* efi_log.c - console and ESP logging with rotation */
#include "efi_helpers_internal.h"

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

void efi_log_slow(CHAR16 *what, UINT64 t0_us) {
    UINT64 now = arch_now_us();
    if (now <= t0_us) return;
    UINT64 ms = (now - t0_us) / 1000;
#if SLOW_STEP_MIN_MS > 0
    if (ms < (UINT64)SLOW_STEP_MIN_MS) return;
#endif
    CHAR16 m[144];
    SPrint(m, sizeof(m), L"slow: %s took %d ms", what, (int)ms);
    efi_log(m);
}

#define LOG_PATH      L"\\EFI\\visor\\boot.log"
#define LOG_MARKER_W  L"=================== visor boot ==================="
#define LOG_MARKER_A   "=================== visor boot ==================="
#define LOG_KEEP      3
#define LOG_MAX_BYTES (128 * 1024)

static UINTN log_elapsed_cs(void) {
    static UINT64 base_us = 0;
    static int based = 0;
    UINT64 now = arch_now_us();
    if (!based) {
        based = 1;
        base_us = now;
        return 0;
    }
    if (now < base_us) return 0;
    return (UINTN)((now - base_us) / 10000);
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
            g_volume_opens++;
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

    static int in_log = 0;
    if (in_log) return;
    in_log = 1;

    UINTN cs = log_elapsed_cs();
    CHAR16 pfx[20];
    SPrint(pfx, sizeof(pfx), L"[%4d.%d%d] ",
           (int)(cs / 100), (int)((cs / 10) % 10), (int)(cs % 10));

    if (visor_log_to_console && !visor_quiet && ST && ST->ConOut) {
        ST->ConOut->OutputString(ST->ConOut, pfx);
        ST->ConOut->OutputString(ST->ConOut, msg);
        ST->ConOut->OutputString(ST->ConOut, L"\r\n");
    }

    if (!visor_log_to_file) { in_log = 0; return; }

    EFI_FILE_PROTOCOL *f = log_file_open();
    if (!f) { in_log = 0; return; }

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
    in_log = 0;
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
