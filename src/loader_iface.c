#include <efi.h>
#include <efilib.h>
#include "loader_iface.h"
#include "efi_helpers.h"
#include "tcg2.h"

extern EFI_SYSTEM_TABLE  *ST;
extern EFI_BOOT_SERVICES *BS;
extern EFI_HANDLE IH;

static UINT64 g_init_usec = 0;

static UINT64 loader_now_usec(void) {
    EFI_TIME t;
    if (EFI_ERROR(RT->GetTime(&t, NULL))) return 0;
    if (t.Year < 1970) return 0;

    static const UINT16 cum[12] =
        { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

    UINT64 years = (UINT64)t.Year - 1970;
    UINT64 days  = years * 365 + (years + 1) / 4;
    if (t.Month >= 1 && t.Month <= 12) days += cum[t.Month - 1];

    if (t.Month > 2 && ((t.Year % 4 == 0 && t.Year % 100 != 0) || t.Year % 400 == 0))
        days += 1;
    if (t.Day >= 1) days += (UINT64)t.Day - 1;

    UINT64 secs = days * 86400ULL + (UINT64)t.Hour * 3600ULL +
                  (UINT64)t.Minute * 60ULL + (UINT64)t.Second;
    return secs * 1000000ULL + (UINT64)(t.Nanosecond / 1000);
}

void loader_mark_init(void) {
    if (!g_init_usec) g_init_usec = loader_now_usec();
}

void loader_mark_menu(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    efi_set_loader_var_usec(L"LoaderTimeMenuUSec", loader_now_usec());
}

static CHAR16* id_from_bls_path(CHAR16 *path) {
    if (!path || !path[0]) return NULL;
    UINTN n = efi_strlen16(path);

    UINTN start = 0;
    for (UINTN i = 0; i < n; i++)
        if (path[i] == L'\\' || path[i] == L'/') start = i + 1;

    UINTN end = n;
    if (end >= start + 5) {
        CHAR16 *e = path + end - 5;
        if ((e[0] == L'.') &&
            (e[1] == L'c' || e[1] == L'C') && (e[2] == L'o' || e[2] == L'O') &&
            (e[3] == L'n' || e[3] == L'N') && (e[4] == L'f' || e[4] == L'F'))
            end -= 5;
    }
    if (end <= start) return NULL;

    UINTN len = end - start;
    CHAR16 *out = efi_allocate_pool((len + 1) * sizeof(CHAR16));
    if (!out) return NULL;
    for (UINTN i = 0; i < len; i++) out[i] = path[start + i];
    out[len] = 0;
    return out;
}

static CHAR16* id_from_name(CHAR16 *name) {
    if (!name || !name[0]) return NULL;
    UINTN n = efi_strlen16(name);
    CHAR16 *out = efi_allocate_pool((n + 1) * sizeof(CHAR16));
    if (!out) return NULL;

    UINTN k = 0;
    for (UINTN i = 0; i < n; i++) {
        CHAR16 c = name[i];
        if (c >= L'A' && c <= L'Z') c = (CHAR16)(c - L'A' + L'a');
        int alnum = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9');
        if (alnum) {
            out[k++] = c;
        } else if (k > 0 && out[k - 1] != L'-') {
            out[k++] = L'-';
        }
    }
    while (k > 0 && out[k - 1] == L'-') k--;
    out[k] = 0;

    if (!k) { efi_free_pool(out); return NULL; }
    return out;
}

CHAR16* loader_entry_id(boot_entry_t *entry) {
    if (!entry) return NULL;

    if (entry->deployments && entry->deploy_count) {
        UINTN sel = entry->deploy_sel < entry->deploy_count ? entry->deploy_sel : 0;
        CHAR16 *id = id_from_bls_path(entry->deployments[sel].bls_path);
        if (id) return id;
    }

    CHAR16 *id = id_from_name(entry->name);
    if (id) return id;

    CHAR16 buf[32];
    SPrint(buf, sizeof(buf), L"visor-entry-%d", (int)entry->index);
    return efi_strdup(buf);
}

static boot_entry_t* entry_by_id(boot_entry_t *entries, CHAR16 *want, UINTN *idx_out) {
    UINTN i = 0;
    for (boot_entry_t *e = entries; e; e = e->next, i++) {
        CHAR16 *id = loader_entry_id(e);
        if (!id) continue;
        int hit = (efi_strcmp(id, want) == 0);
        efi_free_pool(id);
        if (hit) {
            if (idx_out) *idx_out = i;
            return e;
        }
    }
    return NULL;
}

static UINT64 loader_feature_bits(config_t *config) {
    UINT64 f = LOADER_FEATURE_CONFIG_TIMEOUT
             | LOADER_FEATURE_CONFIG_TIMEOUT_ONESHOT
             | LOADER_FEATURE_ENTRY_DEFAULT
             | LOADER_FEATURE_ENTRY_ONESHOT
             | LOADER_FEATURE_BOOT_COUNTING
             | LOADER_FEATURE_XBOOTLDR
             | LOADER_FEATURE_LOAD_DRIVER
             | LOADER_FEATURE_MENU_DISABLED;

    (void)config;
    if (tpm_present()) f |= LOADER_FEATURE_TPM2_ACTIVE_PCR_BANKS;
    return f;
}

void loader_export_common(config_t *config) {
    efi_set_loader_var(L"LoaderInfo", VISOR_LOADER_INFO, 0);

    if (g_init_usec)
        efi_set_loader_var_usec(L"LoaderTimeInitUSec", g_init_usec);

    if (!efi_loader_var_exists(L"LoaderFirmwareInfo")) {
        CHAR16 buf[128];
        SPrint(buf, sizeof(buf), L"%s %d.%02d",
               ST->FirmwareVendor ? ST->FirmwareVendor : L"unknown",
               (int)(ST->FirmwareRevision >> 16),
               (int)(ST->FirmwareRevision & 0xFFFF));
        efi_set_loader_var(L"LoaderFirmwareInfo", buf, 0);
    }
    if (!efi_loader_var_exists(L"LoaderFirmwareType")) {
        CHAR16 buf[64];
        SPrint(buf, sizeof(buf), L"UEFI %d.%02d",
               (int)(ST->Hdr.Revision >> 16),
               (int)(ST->Hdr.Revision & 0xFFFF));
        efi_set_loader_var(L"LoaderFirmwareType", buf, 0);
    }

    EFI_LOADED_IMAGE *li = NULL;
    if (!EFI_ERROR(BS->HandleProtocol(IH, &gEfiLoadedImageProtocolGuid, (void**)&li)) && li) {
        if (li->DeviceHandle && !efi_loader_var_exists(L"LoaderDevicePartUUID")) {
            CHAR16 *uuid = efi_handle_partition_uuid(li->DeviceHandle);
            if (uuid) {
                efi_set_loader_var(L"LoaderDevicePartUUID", uuid, 0);
                efi_free_pool(uuid);
            }
        }
        if (li->FilePath && !efi_loader_var_exists(L"LoaderImageIdentifier")) {
            CHAR16 *s = DevicePathToStr(li->FilePath);
            if (s) {
                efi_set_loader_var(L"LoaderImageIdentifier", s, 0);
                FreePool(s);
            }
        }
    }

    tpm_publish_banks();

    UINT64 features = loader_feature_bits(config);
    efi_set_loader_var_u64(L"LoaderFeatures", features);

    efi_log(L"loader: Boot Loader Interface variables exported");
}

void loader_export_entries(boot_entry_t *entries) {
    UINTN total = 0;
    for (boot_entry_t *e = entries; e; e = e->next) {
        CHAR16 *id = loader_entry_id(e);
        if (!id) continue;
        total += (efi_strlen16(id) + 1) * sizeof(CHAR16);
        efi_free_pool(id);
    }
    if (!total) {
        efi_unset_loader_var(L"LoaderEntries", 0);
        return;
    }

    UINT8 *buf = efi_allocate_pool(total);
    if (!buf) return;

    UINTN off = 0;
    for (boot_entry_t *e = entries; e; e = e->next) {
        CHAR16 *id = loader_entry_id(e);
        if (!id) continue;
        UINTN bytes = (efi_strlen16(id) + 1) * sizeof(CHAR16);
        if (off + bytes <= total) {
            CopyMem(buf + off, id, bytes);
            off += bytes;
        }
        efi_free_pool(id);
    }

    efi_set_loader_var_raw(L"LoaderEntries", buf, off, 0);
    efi_free_pool(buf);
}

UINTN loader_apply_overrides(config_t *config, boot_entry_t *entries,
                             UINTN entry_count, INTN *timeout_io) {
    UINTN pick = (UINTN)-1;
    (void)entry_count;

    /* LoaderConfigTimeoutOneShot is a deliberate, transient request
     * (systemctl reboot --boot-loader-menu=N), so it always wins.
     * LoaderConfigTimeout is persistent NVRAM state that another boot loader
     * may have left behind, so an explicit timeout= in boot.conf beats it -
     * editing the config file has to be what actually decides the countdown. */
    CHAR16 *t1 = efi_get_loader_var(L"LoaderConfigTimeoutOneShot");
    if (t1) {
        INTN v;
        if (timeout_io && efi_parse_loader_timeout(t1, &v)) {
            *timeout_io = v;
            efi_log(L"loader: LoaderConfigTimeoutOneShot applied");
        } else {
            efi_log(L"WARN: LoaderConfigTimeoutOneShot is not a valid timeout - ignored");
        }
        efi_free_pool(t1);
        efi_unset_loader_var(L"LoaderConfigTimeoutOneShot", 1);
    } else {
        CHAR16 *t0 = efi_get_loader_var(L"LoaderConfigTimeout");
        if (t0) {
            INTN v;
            if (!timeout_io || !efi_parse_loader_timeout(t0, &v)) {
                efi_log(L"WARN: LoaderConfigTimeout is not a valid timeout - ignored");
            } else if (config && config->timeout_set) {
                CHAR16 m[112];
                SPrint(m, sizeof(m),
                       L"loader: LoaderConfigTimeout=%d in NVRAM ignored - "
                       L"boot.conf sets timeout=%d",
                       (int)v, (int)config->timeout);
                efi_log(m);
            } else {
                *timeout_io = v;
                efi_log(L"loader: LoaderConfigTimeout applied");
            }
            efi_free_pool(t0);
        }
    }

    CHAR16 *def = efi_get_loader_var(L"LoaderEntryDefault");
    if (def) {
        UINTN idx;
        if (entry_by_id(entries, def, &idx)) {
            pick = idx;
            efi_log(L"loader: LoaderEntryDefault preselected an entry");
        } else {
            efi_log(L"WARN: LoaderEntryDefault names an unknown entry - ignored");
        }
        efi_free_pool(def);
    }

    CHAR16 *one = efi_get_loader_var(L"LoaderEntryOneShot");
    if (one) {
        UINTN idx;
        if (entry_by_id(entries, one, &idx)) {
            pick = idx;

            if (config) config->autoboot = 1;
            efi_log(L"loader: LoaderEntryOneShot selected an entry for this boot");
        } else {
            efi_log(L"WARN: LoaderEntryOneShot names an unknown entry - ignored");
        }
        efi_free_pool(one);

        efi_unset_loader_var(L"LoaderEntryOneShot", 1);
    }

    return pick;
}

void loader_mark_selected(boot_entry_t *entry) {
    if (!entry) return;
    CHAR16 *id = loader_entry_id(entry);
    if (id) {
        efi_set_loader_var(L"LoaderEntrySelected", id, 0);
        efi_free_pool(id);
    }
    efi_set_loader_var_usec(L"LoaderTimeExecUSec", loader_now_usec());
}
