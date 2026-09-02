/* Boot-time self-heal for Windows-style boot-takeover.
 *
 * 1. NVRAM: ensure a Boot#### entry exists and (per policy) keep it first.
 * 2. Fallback: re-copy our binary to \EFI\BOOT\BOOTX64.EFI.
 *
 * Safety: never write NVRAM from removable media; never invent BootOrder;
 * only append on fallback boots; compare-and-write only.
 * Nothing here is allowed to fail the boot.
 */

#include "efi_selfheal.h"

#ifdef NVSH_HOST
#include <string.h>
#include <stdlib.h>
#define NVSH_COPY(d, s, n)  memcpy((d), (s), (n))
#define NVSH_ZERO(d, n)     memset((d), 0, (n))
#define NVSH_CMP(a, b, n)   memcmp((a), (b), (n))
#else
#include <efi.h>
#include <efilib.h>
#include "efi_helpers.h"

extern EFI_SYSTEM_TABLE *ST;
extern EFI_BOOT_SERVICES *BS;
extern EFI_RUNTIME_SERVICES *RT;
extern EFI_HANDLE IH;

#define NVSH_COPY(d, s, n)  CopyMem((d), (VOID*)(UINTN)(s), (n))
#define NVSH_ZERO(d, n)     SetMem((d), (n), 0)
#define NVSH_CMP(a, b, n)   CompareMem((a), (b), (n))
#endif

/* Device path node constants (kept local so host tests share the parser). */
#define NVSH_DP_TYPE_MEDIA        0x04
#define NVSH_DP_SUBTYPE_FILEPATH  0x04
#define NVSH_DP_TYPE_END          0x7f

#define NVSH_SLOTS           0x400
#define NVSH_MAX_OPTION      1024

/* Pure data helpers */

static UINTN nvsh_str16_len(const CHAR16 *s) {
    if (!s) return 0;
    UINTN n = 0;
    while (s[n]) n++;
    return n;
}

int nvsh_boot_index(const CHAR16 *name, int len) {
    if (len != 8) return -1;
    if (name[0] != L'B' || name[1] != L'o' || name[2] != L'o' ||
        name[3] != L't') return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        CHAR16 c = name[4 + i];
        int d;
        if (c >= L'0' && c <= L'9') d = (int)(c - L'0');
        else if (c >= L'A' && c <= L'F') d = (int)(c - L'A') + 10;
        else if (c >= L'a' && c <= L'f') d = (int)(c - L'a') + 10;
        else return -1;
        v = (v << 4) | d;
    }
    return v;
}

int nvsh_paths_equal16(const CHAR16 *a, const CHAR16 *b) {
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

UINTN nvsh_load_option_size(const CHAR16 *desc, UINTN path_len) {
    return 6 + (nvsh_str16_len(desc) + 1) * sizeof(CHAR16) + path_len;
}

int nvsh_build_load_option(UINT8 *out, UINTN cap, UINT32 attributes,
                           const CHAR16 *desc, const UINT8 *path,
                           UINTN path_len, UINTN *out_size) {
    if (!desc) return -1;
    UINTN dlen = nvsh_str16_len(desc) + 1;
    UINTN total = 6 + dlen * sizeof(CHAR16) + path_len;
    if (out_size) *out_size = total;
    if (!out) return 0;
    if (cap < total) return -1;
    out[0] = (UINT8)(attributes);
    out[1] = (UINT8)(attributes >> 8);
    out[2] = (UINT8)(attributes >> 16);
    out[3] = (UINT8)(attributes >> 24);
    out[4] = (UINT8)(path_len);
    out[5] = (UINT8)(path_len >> 8);
    NVSH_COPY(out + 6, desc, dlen * sizeof(CHAR16));
    if (path_len) NVSH_COPY(out + 6 + dlen * sizeof(CHAR16), path, path_len);
    return 0;
}

int nvsh_parse_load_option(const UINT8 *buf, UINTN size, UINT32 *attributes,
                           UINT16 *path_len, const CHAR16 **desc,
                           const UINT8 **path) {
    if (!buf || size < 6) return -1;
    UINT32 attr = (UINT32)buf[0] | ((UINT32)buf[1] << 8) |
                  ((UINT32)buf[2] << 16) | ((UINT32)buf[3] << 24);
    UINTN plen = (UINTN)buf[4] | ((UINTN)buf[5] << 8);
    const CHAR16 *d = (const CHAR16 *)(buf + 6);
    UINTN dmax = (size - 6) / sizeof(CHAR16);
    UINTN dl = 0;
    while (dl < dmax && d[dl]) dl++;
    if (dl >= dmax) return -1;
    if (6 + (dl + 1) * sizeof(CHAR16) + plen > size) return -1;
    if (attributes) *attributes = attr;
    if (path_len) *path_len = (UINT16)plen;
    if (desc) *desc = d;
    if (path) *path = buf + 6 + (dl + 1) * sizeof(CHAR16);
    return 0;
}

int nvsh_parse_bootorder(const UINT8 *buf, UINTN size, UINT16 *list,
                         UINTN cap, UINTN *count) {
    if (!buf || size % 2) return -1;
    UINTN n = size / 2;
    if (count) *count = n;
    if (!list || n > cap) return (n > cap) ? -2 : 0;
    for (UINTN i = 0; i < n; i++)
        list[i] = (UINT16)(buf[2 * i] | (buf[2 * i + 1] << 8));
    return 0;
}

int nvsh_replan_order(UINT16 *list, UINTN n, UINT16 ours, int mode,
                      int normal_boot, UINTN *new_n) {
    if (new_n) *new_n = n;
    if (mode == NVSH_ORDER_OFF) return 0;
    if (mode != NVSH_ORDER_ENSURE && mode != NVSH_ORDER_FIRST) return -1;

    UINTN pos = n;
    for (UINTN i = 0; i < n; i++) {
        if (list[i] == ours) { pos = i; break; }
    }

    if (pos < n) {
        /* FIRST + normal boot may promote us to the head. */
        if (mode == NVSH_ORDER_FIRST && normal_boot && pos != 0) {
            UINT16 keep = ours;
            for (UINTN i = pos; i > 0; i--) list[i] = list[i - 1];
            list[0] = keep;
            return 1;
        }
        return 0;
    }

    /* Absent: re-add, preserving every existing entry. */
    if (new_n) *new_n = n + 1;
    list[n] = ours;
    if (mode == NVSH_ORDER_FIRST && normal_boot) {
        for (UINTN i = n; i > 0; i--) list[i] = list[i - 1];
        list[0] = ours;
    }
    return 1;
}

int nvsh_walk_filepath(const UINT8 *dp, UINTN dp_len, const CHAR16 **path) {
    if (!dp || !path || dp_len < 4) return 0;
    UINTN off = 0;
    while (off + 4 <= dp_len) {
        UINT8 type = dp[off];
        UINT16 node_len = (UINT16)(dp[off + 2] | (dp[off + 3] << 8));
        if (node_len < 4 || off + node_len > dp_len) return 0;
        if (type == NVSH_DP_TYPE_MEDIA && dp[off + 1] == NVSH_DP_SUBTYPE_FILEPATH) {
            *path = (const CHAR16 *)(dp + off + 4);
            return 1;
        }
        if (type == NVSH_DP_TYPE_END && node_len == 4) return 0;
        off += node_len;
    }
    return 0;
}

#ifndef NVSH_HOST

/* EFI glue */

static void nsh_log(const CHAR16 *fmt, ...) {
    CHAR16 buf[220];
    va_list ap;
    va_start(ap, fmt);
    VSPrint(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    efi_log(buf);
}

static EFI_STATUS nvsh_var_get(const CHAR16 *name, EFI_GUID *guid,
                               UINT32 *attrs, UINT8 **data, UINTN *size) {
    UINTN sz = 0;
    UINT32 a = 0;
    EFI_STATUS s = RT->GetVariable((CHAR16 *)name, guid, &a, &sz, NULL);
    if (s == EFI_NOT_FOUND || s == EFI_UNSUPPORTED) return s;
    if (s != EFI_BUFFER_TOO_SMALL) return EFI_DEVICE_ERROR;
    if (sz == 0) return EFI_NOT_FOUND;
    UINT8 *buf = efi_allocate_pool(sz);
    if (!buf) return EFI_OUT_OF_RESOURCES;
    s = RT->GetVariable((CHAR16 *)name, guid, &a, &sz, buf);
    if (EFI_ERROR(s)) { efi_free_pool(buf); return s; }
    *attrs = a;
    *data = buf;
    *size = sz;
    return EFI_SUCCESS;
}

static int nvsh_boot_volume_removable(void) {
    EFI_LOADED_IMAGE *li = NULL;
    if (EFI_ERROR(BS->HandleProtocol(IH, &gEfiLoadedImageProtocolGuid,
                                     (void **)&li)) || !li || !li->DeviceHandle)
        return 0;
    EFI_BLOCK_IO *bio = NULL;
    if (!EFI_ERROR(BS->HandleProtocol(li->DeviceHandle,
                                      &gEfiBlockIoProtocolGuid, (void **)&bio))
        && bio && bio->Media)
        return bio->Media->RemovableMedia ? 1 : 0;
    return 0;
}

static CHAR16* nvsh_running_path(void) {
    EFI_LOADED_IMAGE *li = NULL;
    if (EFI_ERROR(BS->HandleProtocol(IH, &gEfiLoadedImageProtocolGuid,
                                     (void **)&li)) || !li || !li->FilePath)
        return NULL;
    UINTN dplen = DevicePathSize(li->FilePath);
    if (dplen < 4) return NULL;
    const CHAR16 *rel = NULL;
    if (!nvsh_walk_filepath((const UINT8 *)li->FilePath, dplen, &rel))
        return NULL;
    UINTN len = nvsh_str16_len(rel);
    if (!len) return NULL;
    CHAR16 *s = efi_allocate_pool((len + 1) * sizeof(CHAR16));
    if (!s) return NULL;
    NVSH_COPY(s, rel, (len + 1) * sizeof(CHAR16));
    return s;
}

static void nvsh_strcpy_safe(CHAR16 *dst, UINTN cap, const CHAR16 *src) {
    UINTN i = 0;
    if (!dst || !cap) return;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Runs the full NVRAM pass; reports into rep. */
static void nvsh_nvram_pass(const CHAR16 *run, int mode, int dry_run,
                            nvsh_report_t *rep) {
    /* BootCurrent */
    UINT8 *bc = NULL;
    UINTN bc_sz = 0;
    UINT32 bc_at = 0;
    EFI_STATUS bcs = nvsh_var_get(L"BootCurrent", &gEfiGlobalVariableGuid,
                                  &bc_at, &bc, &bc_sz);
    UINT16 bootcurrent = 0xFFFF;
    int have_bc = !EFI_ERROR(bcs) && bc && bc_sz >= 2;
    if (have_bc) bootcurrent = (UINT16)(bc[0] | (bc[1] << 8));
    rep->bootcurrent = bootcurrent;
    if (bc) efi_free_pool(bc);
    if (!have_bc)
        nsh_log(L"selfheal: BootCurrent unavailable - treating as fallback/one-time boot");
    else
        nsh_log(L"selfheal: BootCurrent=Boot%04X", (unsigned)bootcurrent);

    /* Enumerate real variables with GetNextVariableName, not GetVariable:
     * some firmwares answer stale data for absent Boot#### names. */
    UINT8 used[NVSH_SLOTS];
    NVSH_ZERO(used, sizeof(used));
    CHAR16 vn[512];
    vn[0] = 0;
    UINTN vnsz = sizeof(vn);
    EFI_GUID vg;
    UINT16 slot = 0xFFFF;
    int present = 0;
    UINTN boot_vars = 0;
    EFI_STATUS es = RT->GetNextVariableName(&vnsz, vn, &vg);
    while (!EFI_ERROR(es)) {
        int idx = nvsh_boot_index(vn, (int)nvsh_str16_len(vn));
        if (idx >= 0 && (UINT16)idx < NVSH_SLOTS) {
            boot_vars++;
            UINT32 at = 0;
            UINT8 *data = NULL;
            UINTN sz = 0;
            EFI_STATUS s = nvsh_var_get(vn, &vg, &at, &data, &sz);
            if (s == EFI_SUCCESS) {
                UINT16 num = (UINT16)idx;
                UINT16 plen = 0;
                const UINT8 *p = NULL;
                const CHAR16 *desc = NULL;
                int valid = !nvsh_parse_load_option(data, sz, NULL, &plen,
                                                    &desc, &p) &&
                            plen > 0;
                int ours = 0;
                if (valid) {
                    const CHAR16 *ep = NULL;
                    ours = nvsh_walk_filepath(p, plen, &ep) &&
                           nvsh_paths_equal16(ep, run);
                }
                if (valid) used[num] = 1;
                if (ours) { present = 1; slot = num; }
                efi_free_pool(data);
            }
        }
        vnsz = sizeof(vn);
        es = RT->GetNextVariableName(&vnsz, vn, &vg);
    }
    {
        UINTN used_n = 0, first_free = NVSH_SLOTS, first_present = NVSH_SLOTS;
        for (UINTN i = 0; i < NVSH_SLOTS; i++) {
            if (used[i]) { used_n++; if (i < first_present) first_present = i; }
            else if (i < first_free) first_free = i;
        }
        nsh_log(L"selfheal: slot scan used=%d boot_vars=%d first_free=%d",
                (int)used_n, (int)boot_vars, (int)first_free);
    }
    rep->entry_present = present;
    rep->our_entry = present ? slot : 0xFFFF;

    if (!present) {
        UINT16 could = 0xFFFF;
        for (UINT16 i = 0; i < NVSH_SLOTS; i++)
            if (!used[i]) { could = i; break; }
        if (could == 0xFFFF) {
            rep->error = NVSH_ERR_NOSLOT;
            nsh_log(L"selfheal: no free Boot#### slot - cannot create our entry");
            return;
        }
        if (dry_run) {
            nsh_log(L"selfheal: dry run - would create Boot%04X -> %s",
                    (unsigned)could, run);
            slot = could;
        } else {
            EFI_DEVICE_PATH *full = efi_make_file_path(
                efi_boot_volume_handle(), (CHAR16 *)run);
            if (!full) {
                rep->error = NVSH_ERR_BOOTMGR;
                nsh_log(L"selfheal: cannot build a device path for our own entry");
                return;
            }
            UINTN full_len = DevicePathSize(full);
            UINT8 opt[NVSH_MAX_OPTION];
            UINTN osz = 0;
            int ob = (full_len + 32 > sizeof(opt) ||
                      nvsh_build_load_option(opt, sizeof(opt), LOAD_OPTION_ACTIVE,
                                             L"Visor", (const UINT8 *)full,
                                             full_len, &osz));
            if (ob) {
                rep->error = NVSH_ERR_BOOTMGR;
                nsh_log(L"selfheal: load-option for our entry exceeds the working buffer");
            } else {
                SPrint(vn, sizeof(vn), L"Boot%04X", (unsigned)could);
                EFI_STATUS as = RT->SetVariable(
                    vn, &gEfiGlobalVariableGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                        EFI_VARIABLE_RUNTIME_ACCESS,
                    osz, opt);
                if (EFI_ERROR(as)) {
                    rep->error = (as == EFI_ACCESS_DENIED) ? NVSH_ERR_ENTRY_LOCKED
                                                           : NVSH_ERR_BOOTMGR;
                    if (as == EFI_INVALID_PARAMETER)
                        nsh_log(L"selfheal: firmware refuses to create a new "
                                L"Boot%04X (variable-policy/reserved namespace); "
                                L"the fallback copy still boots us",
                                (unsigned)could);
                    else
                        nsh_log(L"selfheal: SetVariable %s failed (%d)", vn,
                                (int)as);
                } else {
                    rep->entry_created = 1;
                    slot = could;
                    rep->our_entry = slot;
                    present = 1;
                    nsh_log(L"selfheal: created Boot%04X -> %s",
                            (unsigned)slot, run);
                }
            }
            efi_free_pool(full);
            if (!present) return;
        }
    } else {
        nsh_log(L"selfheal: found existing entry Boot%04X for %s",
                (unsigned)slot, run);
    }

    /* BootOrder */
    UINT8 *od = NULL;
    UINTN osz = 0;
    UINT32 oat = 0;
    EFI_STATUS os = nvsh_var_get(L"BootOrder", &gEfiGlobalVariableGuid,
                                 &oat, &od, &osz);
    if (os == EFI_NOT_FOUND) {
        rep->order_missing = 1;
        nsh_log(L"selfheal: BootOrder absent - not inventing one; fallback copy"
                L" guards reachability");
        efi_free_pool(od);
        return;
    }
    if (EFI_ERROR(os)) {
        nsh_log(L"selfheal: cannot read BootOrder");
        efi_free_pool(od);
        return;
    }

    UINTN n = osz / 2;
    UINT16 *list = efi_allocate_pool((n + 2) * sizeof(UINT16));
    if (!list) {
        nsh_log(L"selfheal: out of memory parsing BootOrder");
        efi_free_pool(od);
        return;
    }
    UINTN cnt = 0;
    nvsh_parse_bootorder(od, osz, list, n + 2, &cnt);
    n = cnt;
    int in = 0;
    for (UINTN i = 0; i < n; i++) if (list[i] == slot) in = 1;
    rep->entry_in_order = in;
    rep->normal_boot = have_bc && bootcurrent == slot;
    if (rep->normal_boot)
        nsh_log(L"selfheal: normal boot-manager boot of our entry");
    else
        nsh_log(L"selfheal: not a BootCurrent match - conservative append-only");

    UINTN nn = 0;
    int changed = nvsh_replan_order(list, n, slot, mode, rep->normal_boot, &nn);
    if (changed < 0) {
        nsh_log(L"selfheal: invalid boot-order mode (%d)", mode);
    } else if (changed) {
        rep->order_updated = 1;
        nsh_log(L"selfheal: BootOrder needs %s (entry Boot%04X)",
                rep->normal_boot ? L"promotion to head" : L"re-add",
                (unsigned)slot);
        if (!dry_run) {
            UINT8 ob[(NVSH_SLOTS + 2) * 2];
            UINTN k = 0;
            for (UINTN i = 0; i < nn; i++) {
                ob[k] = (UINT8)list[i];
                ob[k + 1] = (UINT8)(list[i] >> 8);
                k += 2;
            }
            EFI_STATUS ws = RT->SetVariable(
                L"BootOrder", &gEfiGlobalVariableGuid,
                EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                    EFI_VARIABLE_RUNTIME_ACCESS,
                k, ob);
            if (EFI_ERROR(ws)) {
                rep->error = (ws == EFI_ACCESS_DENIED) ? NVSH_ERR_ORDER_LOCKED
                                                       : NVSH_ERR_BOOTMGR;
                nsh_log(L"selfheal: SetVariable BootOrder failed (%d)", (int)ws);
            } else {
                nsh_log(L"selfheal: BootOrder rewritten (%d entries)", (int)nn);
            }
        } else {
            nsh_log(L"selfheal: dry run - BootOrder not written");
        }
    } else {
        nsh_log(n ? L"selfheal: BootOrder fine, no change" : L"selfheal: BootOrder empty");
    }
    efi_free_pool(list);
    efi_free_pool(od);
}

static void nvsh_fallback_restore(const CHAR16 *run, int dry_run,
                                  nvsh_report_t *rep) {
#if defined(__aarch64__)
    static const CHAR16 fbpath[] = L"\\EFI\\BOOT\\BOOTAA64.EFI";
#else
    static const CHAR16 fbpath[] = L"\\EFI\\BOOT\\BOOTX64.EFI";
#endif
    if (nvsh_paths_equal16(run, fbpath)) {
        rep->fallback_unneeded = 1;
        nsh_log(L"selfheal: already running from the fallback path");
        return;
    }

    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) {
        nsh_log(L"selfheal: no boot volume - fallback restore skipped");
        return;
    }
    EFI_FILE_PROTOCOL *src = NULL;
    EFI_STATUS s = root->Open(root, &src, (CHAR16 *)run, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s) || !src) {
        if (src) src->Close(src);
        root->Close(root);
        nsh_log(L"selfheal: cannot open our own binary (%d) for copy", (int)s);
        return;
    }
    UINTN sz = (UINTN)efi_file_size(src);

    int need = 1;
    EFI_FILE_PROTOCOL *dst = NULL;
    EFI_STATUS ds = root->Open(root, &dst, (CHAR16 *)fbpath, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR(ds) && dst) {
        UINTN dsz = (UINTN)efi_file_size(dst);
        if (dsz == sz && sz > 0 && sz <= 0x400000) {
            UINT8 *a = efi_allocate_pool(sz);
            UINT8 *b = efi_allocate_pool(sz);
            if (a && b) {
                src->SetPosition(src, 0);
                dst->SetPosition(dst, 0);
                UINTN n1 = sz, n2 = sz;
                EFI_STATUS r1 = src->Read(src, &n1, a);
                EFI_STATUS r2 = dst->Read(dst, &n2, b);
                if (!EFI_ERROR(r1) && !EFI_ERROR(r2) && n1 == sz && n2 == sz &&
                    NVSH_CMP(a, b, sz) == 0)
                    need = 0;
            }
            if (a) efi_free_pool(a);
            if (b) efi_free_pool(b);
        }
        dst->Close(dst);
    }

    if (!need) {
        rep->fallback_unneeded = 1;
        nsh_log(L"selfheal: fallback %s is already our binary", fbpath);
    } else if (dry_run) {
        rep->fallback_restored = 1;
        nsh_log(L"selfheal: dry run - would restore %s", fbpath);
    } else if (sz > 0x400000) {
        nsh_log(L"selfheal: binary too large to buffer for fallback copy");
    } else {
        UINT8 *data = efi_allocate_pool(sz ? sz : 1);
        if (!data) {
            nsh_log(L"selfheal: out of memory buffering our binary");
        } else {
            src->SetPosition(src, 0);
            UINTN rd = sz;
            EFI_STATUS rs = src->Read(src, &rd, data);
            if (EFI_ERROR(rs) || rd != sz) {
                nsh_log(L"selfheal: read of our binary failed (%d)", (int)rs);
            } else {
                EFI_FILE_PROTOCOL *d = NULL;
                EFI_STATUS es = root->Open(root, &d, L"\\EFI\\BOOT",
                                           EFI_FILE_MODE_READ, 0);
                if (EFI_ERROR(es) || !d) {
                    es = root->Open(root, &d, L"\\EFI\\BOOT",
                                    EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ |
                                        EFI_FILE_MODE_WRITE,
                                    EFI_FILE_DIRECTORY);
                }
                if (d) d->Close(d);
                if (EFI_ERROR(es)) {
                    nsh_log(L"selfheal: cannot create \\EFI\\BOOT directory");
                } else {
                    EFI_FILE_PROTOCOL *w = NULL;
                    EFI_STATUS ws = root->Open(
                        root, &w, (CHAR16 *)fbpath,
                        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ |
                            EFI_FILE_MODE_WRITE,
                        0);
                    if (EFI_ERROR(ws) || !w) {
                        if (w) w->Close(w);
                        nsh_log(L"selfheal: cannot open fallback file for writing");
                    } else {
                        UINTN ww = sz;
                        EFI_STATUS ws2 = w->Write(w, &ww, data);
                        if (!EFI_ERROR(ws2)) ws2 = w->Flush(w);
                        if (EFI_ERROR(ws2)) {
                            nsh_log(L"selfheal: writing fallback file failed (%d)",
                                    (int)ws2);
                        } else {
                            rep->fallback_restored = 1;
                            nsh_log(L"selfheal: restored %s", fbpath);
                        }
                        w->Close(w);
                    }
                }
            }
            efi_free_pool(data);
        }
    }
    src->Close(src);
    root->Close(root);
}

nvsh_report_t nvram_self_heal(nvsh_policy_t *policy) {
    nvsh_report_t rep;
    NVSH_ZERO(&rep, sizeof(rep));
    rep.our_entry = 0xFFFF;
    rep.bootcurrent = 0xFFFF;

    nvsh_policy_t defp;
    nvsh_policy_defaults(&defp);
    if (!policy) policy = &defp;

    int order_on = policy->order_mode == NVSH_ORDER_ENSURE ||
                   policy->order_mode == NVSH_ORDER_FIRST;
    if (!order_on && !policy->restore_fallback) {
        rep.error = NVSH_ERR_DISABLED;
        return rep;
    }

    CHAR16 *run = nvsh_running_path();
    if (!run) {
        rep.error = NVSH_ERR_NO_IMAGE;
        nsh_log(L"selfheal: cannot resolve the image path we were loaded from");
        return rep;
    }
    nvsh_strcpy_safe(rep.our_path, sizeof(rep.our_path) / sizeof(CHAR16), run);
    nsh_log(L"selfheal: running image = %s (mode %s, fallback %s)",
            run, nvsh_order_name(policy->order_mode),
            policy->restore_fallback ? L"on" : L"off");

    if (!policy->dry_run && nvsh_boot_volume_removable()) {
        rep.removable = 1;
        rep.error = NVSH_ERR_REMOVABLE;
        nsh_log(L"selfheal: removable media - skipping NVRAM and fallback writes");
        efi_free_pool(run);
        return rep;
    }

    if (order_on)
        nvsh_nvram_pass(run, policy->order_mode, policy->dry_run, &rep);
    else
        nsh_log(L"selfheal: boot-order policy off");

    if (policy->restore_fallback)
        nvsh_fallback_restore(run, policy->dry_run, &rep);

    efi_free_pool(run);
    return rep;
}

void nvsh_policy_defaults(nvsh_policy_t *policy) {
    if (!policy) return;
    policy->order_mode = NVSH_ORDER_FIRST;
    policy->restore_fallback = 1;
    policy->dry_run = 0;
}

const CHAR16* nvsh_order_name(int mode) {
    return mode == NVSH_ORDER_OFF   ? L"off"
         : mode == NVSH_ORDER_ENSURE ? L"ensure"
         : mode == NVSH_ORDER_FIRST  ? L"first"
         : L"?";
}

const CHAR16* nvsh_err_text(int err) {
    switch (err) {
    case NVSH_ERR_OK:           return L"ok";
    case NVSH_ERR_DISABLED:     return L"disabled";
    case NVSH_ERR_REMOVABLE:    return L"removable-media";
    case NVSH_ERR_NO_IMAGE:     return L"no-image-path";
    case NVSH_ERR_BOOTMGR:      return L"bootmgr-failure";
    case NVSH_ERR_ORDER_LOCKED: return L"order-locked";
    case NVSH_ERR_ENTRY_LOCKED: return L"entry-locked";
    case NVSH_ERR_NOSLOT:       return L"no-free-slot";
    default:                    return L"?";
    }
}

#endif /* !NVSH_HOST */