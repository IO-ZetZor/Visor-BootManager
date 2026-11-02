/* efi_log.c - console and ESP logging with rotation */
#include "efi_helpers_internal.h"

int visor_quiet = 0;
int visor_log_to_console = 0;
int visor_boot_services_active = 1;
static int visor_log_to_file = 1;
/* Boot-scoped latch: once a write or flush fails, file logging stays off for
 * the rest of the boot.  efi_log_set_file() would otherwise let the config
 * path re-enable it and hand every later line back to the driver that failed. */
static int log_write_failed = 0;

void efi_log_set_console(int enabled) {
    visor_log_to_console = enabled ? 1 : 0;
}

/**
 * efi_log_set_file - turn ESP logging on or off.
 *
 * An enable request is ignored once a write, flush or rotation has failed:
 * the file driver is wedged for this boot and every extra call is another
 * chance to hang.  Disable requests always go through.
 *
 * @param enabled non-zero to enable file logging
 */
void efi_log_set_file(int enabled) {
    if (enabled && log_write_failed) return;
    visor_log_to_file = enabled ? 1 : 0;
}

/**
 * efi_log_write_failed - report whether file logging was latched off.
 *
 * @return non-zero once any write, flush or rotation failure happened
 */
int efi_log_write_failed(void) {
    return log_write_failed;
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

/* Boot-scoped latch: set once, checked by efi_log_set_file() and by
 * efi_log_rotate(), never cleared.  Every path that touches the log file after
 * a failure would be another firmware call to the driver that already failed. */
/**
 * log_latch_off - stop file logging for the rest of the boot.
 *
 * Used by every path that talks to the log file (efi_log and
 * efi_log_rotate), so the latch cannot be bypassed by a caller that writes
 * through the driver directly.
 *
 * @param why short description of the failed step, shown on the console
 */
static void log_latch_off(CHAR16 *why) {
    log_write_failed = 1;
    efi_log_close();
    visor_log_to_file = 0;
    if (ST && ST->ConOut) {
        ST->ConOut->OutputString(ST->ConOut, L"Visor: boot.log ");
        ST->ConOut->OutputString(ST->ConOut, why);
        ST->ConOut->OutputString(ST->ConOut, L" failed - file logging disabled\r\n");
    }
}

/**
 * efi_log - print a line to the console, the boot log, or both.
 *
 * Console output is gated by visor_log_to_console and visor_quiet.  File
 * output re-opens the log lazily, stamps the line with seconds since
 * loader_mark_init(), and checks Write/Flush: a failure latches file logging
 * off instead of retrying a wedged FAT driver on every later line.
 *
 * @param msg NUL-terminated UTF-16 message
 */
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
    if (!f) {
        /* Deliberately not latched: the header is written before
         * efi_load_fs_drivers() runs, so on some boards the boot volume is
         * not reachable yet and a later line can still open the file. */
        in_log = 0; return;
    }

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
    EFI_STATUS ws = f->Write(f, &wsize, line);
    if (EFI_ERROR(ws) || wsize != n) {
        /* A firmware FAT driver that refuses the write will refuse every
         * later one too; stop retrying (each retry is a firmware call that
         * can hang) and say so on the console instead.  The latch keeps
         * later efi_log_set_file(1) calls from re-arming a wedged driver. */
        log_latch_off(L"write");
    } else if (EFI_ERROR(f->Flush(f))) {
        log_latch_off(L"flush");
    }
    in_log = 0;
}

void efi_log_close(void) {
    if (g_log_file) { g_log_file->Close(g_log_file); g_log_file = NULL; }
    if (g_log_root) { g_log_root->Close(g_log_root); g_log_root = NULL; }
}

void efi_log_begin(void) {
    efi_log(LOG_MARKER_W);
}

/**
 * efi_log_rotate - keep boot.log under the size cap.
 *
 * Reads the file, keeps the tail from the last few boot headers, then
 * deletes and rewrites it.  Rotation owns its own error handling because it
 * writes through the driver directly; a failed rewrite latches file logging
 * off so the following efi_log() cannot re-open a wedged driver.
 */
void efi_log_rotate(void) {
    if (!visor_log_to_file) return;

    efi_log_set_file(0);
    efi_file_buffer_t *buf = efi_load_file(LOG_PATH);
    efi_log_set_file(1);
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

    /* Every step below reports into rs; the shutdown path is single, so an
     * open failure cannot fall through to the success message.  The old file
     * is already gone at this point, which is why a failure here has to latch
     * rather than be retried by the next efi_log() line. */
    EFI_STATUS rs = EFI_SUCCESS;
    EFI_FILE_PROTOCOL *root = log_open_root();
    if (!root) {
        rs = EFI_DEVICE_ERROR;
    } else {
        EFI_FILE_PROTOCOL *f = NULL;
        if (!EFI_ERROR(root->Open(root, &f, LOG_PATH,
                EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)) && f) {
            /* Delete closes the handle itself.  A failure here is not fatal:
             * the create below truncates whatever is left. */
            f->Delete(f);
        }
        f = NULL;
        EFI_STATUS os = root->Open(root, &f, LOG_PATH,
            EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
        if (EFI_ERROR(os) || !f) {
            rs = EFI_ERROR(os) ? os : EFI_DEVICE_ERROR;
        } else {
            f->SetPosition(f, 0);
            UINTN w = keep_len;
            rs = f->Write(f, &w, keep);
            if (!EFI_ERROR(rs) && w != keep_len) rs = EFI_DEVICE_ERROR;
            if (!EFI_ERROR(rs)) rs = f->Flush(f);
            f->Close(f);
        }
        root->Close(root);
    }

    efi_free_pool(buf->data);
    efi_free_pool(buf);

    if (EFI_ERROR(rs)) {
        /* Rotation talks to the driver directly, so it has to set the latch
         * itself; otherwise efi_log() would reopen the same wedged file on
         * the next line and report success that never happened. */
        log_latch_off(L"rewrite");
        return;
    }
    efi_log(L"log: rotated boot.log (size cap reached)");
}
