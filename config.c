
#include "config.h"
#include "efi_helpers.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;
extern EFI_SYSTEM_TABLE *ST;

static CHAR16* trim(CHAR16 *s) {
    while (*s == ' ' || *s == '\t') s++;
    CHAR16 *end = s;
    while (*end) end++;
    while (end > s && (*(end-1) == ' ' || *(end-1) == '\t' || *(end-1) == '\n' || *(end-1) == '\r')) end--;
    *end = '\0';
    if (end - s >= 2 && *s == '"' && *(end-1) == '"') {
        s++;
        *(end-1) = '\0';
    }
    return s;
}

static int is_space16(CHAR16 c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void strip_inline_comment(CHAR16 *s) {
    int in_quote = 0;
    UINTN first = 0;
    while (s[first] && is_space16(s[first])) first++;

    for (UINTN i = 0; s[i]; i++) {
        if (s[i] == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote && s[i] == '#' && i != first && (i == 0 || is_space16(s[i - 1]))) {
            s[i] = '\0';
            return;
        }
    }
}

static CHAR16* dup_path(CHAR16 *value) {
    if (!value) return NULL;

    UINTN len = 0;
    while (value[len]) len++;

    int absolute = (value[0] == '\\' || value[0] == '/');
    const CHAR16 *prefix = absolute ? L"" : CONFIG_DIR L"\\";

    UINTN plen = 0;
    while (prefix[plen]) plen++;

    CHAR16 *out = efi_allocate_pool((plen + len + 1) * sizeof(CHAR16));
    if (!out) return NULL;

    UINTN k = 0;
    for (UINTN i = 0; i < plen; i++) out[k++] = prefix[i];
    for (UINTN i = 0; i < len; i++) {
        CHAR16 c = value[i];
        out[k++] = (c == '/') ? '\\' : c;
    }
    out[k] = '\0';
    return out;
}

static int hexval(CHAR16 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_sha256(CHAR16 *s, UINT8 out[32]) {
    if (*s == '#') s++;
    for (int i = 0; i < 32; i++) {
        if (!s[i * 2] || !s[i * 2 + 1]) return 0;
        int hi = hexval(s[i * 2]);
        int lo = hexval(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (UINT8)((hi << 4) | lo);
    }
    return s[64] == '\0';
}

static int parse_color(CHAR16 *s, color_t *out) {
    if (*s == '#') s++;
    int v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = hexval(s[i]);
        if (v[i] < 0) return 0;
    }
    if (s[6] != '\0') return 0;
    out->r = (UINT8)(v[0] * 16 + v[1]);
    out->g = (UINT8)(v[2] * 16 + v[3]);
    out->b = (UINT8)(v[4] * 16 + v[5]);
    return 1;
}

static void wipe16(CHAR16 *s) {
    if (!s) return;
    volatile CHAR16 *p = (volatile CHAR16*)s;
    while (*p) *p++ = 0;
}

static void free_char16(CHAR16 **p) {
    if (!p || !*p) return;
    efi_free_pool(*p);
    *p = NULL;
}

static void free_deployments(deployment_t *deps, UINTN count) {
    if (!deps) return;
    for (UINTN i = 0; i < count; i++) {
        if (deps[i].version)  efi_free_pool(deps[i].version);
        if (deps[i].kernel)   efi_free_pool(deps[i].kernel);
        if (deps[i].initrd)   efi_free_pool(deps[i].initrd);
        if (deps[i].cmdline)  efi_free_pool(deps[i].cmdline);
        if (deps[i].bls_path) efi_free_pool(deps[i].bls_path);
    }
    efi_free_pool(deps);
}

/* 'emilia-chan' */
static UINTN parse_uint(CHAR16 *s) {
    UINTN n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}

static int is_header(CHAR16 *line, const CHAR16 *kw) {
    UINTN i = 0;
    while (kw[i] && line[i] == kw[i]) i++;
    if (kw[i] != '\0') return 0;
    CHAR16 *r = line + i;
    while (*r == ' ' || *r == '\t' || *r == '{') r++;
    return (*r == '\0');
}

static int contains_ci(CHAR16 *hay, const CHAR16 *needle);
static boot_entry_t* config_add_entry(config_t *config,
                                      CHAR16 *name,
                                      CHAR16 *icon_path,
                                      CHAR16 *kernel_path,
                                      CHAR16 *initrd_path,
                                      CHAR16 *cmdline,
                                      CHAR16 *uuid,
                                      int type,
                                      int encrypted,
                                      int initrd_encrypted);

static int looks_windows(CHAR16 *kernel_path) {
    if (!kernel_path) return 0;
    return contains_ci(kernel_path, L"bootmgfw") ||
           contains_ci(kernel_path, L"\\Microsoft\\");
}

static int str_eq_ci(CHAR16 *a, const CHAR16 *b) {
    if (!a || !b) return 0;
    UINTN i = 0;
    while (a[i] && b[i]) {
        CHAR16 ca = a[i], cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca += 32;
        if (cb >= L'A' && cb <= L'Z') cb += 32;
        if (ca != cb) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static CHAR16* luks_default_key_path(CHAR16 *key_path) {
    return (key_path && key_path[0]) ? key_path : L"/crypto_keyfile.bin";
}

static CHAR16* luks_make_key_cmdline(const CHAR16 *prefix, CHAR16 *key_path) {
    CHAR16 *path = luks_default_key_path(key_path);
    UINTN plen = 0, klen = 0;
    while (prefix[plen]) plen++;
    while (path[klen]) klen++;

    CHAR16 *out = efi_allocate_pool((plen + klen + 1) * sizeof(CHAR16));
    if (!out) return NULL;

    UINTN k = 0;
    for (UINTN i = 0; i < plen; i++) out[k++] = prefix[i];
    for (UINTN i = 0; i < klen; i++) out[k++] = path[i];
    out[k] = 0;
    return out;
}

static CHAR16* luks_cmdline_from_preset(CHAR16 *preset, CHAR16 *key_path) {
    if (!preset || !preset[0]) return NULL;
    if (str_eq_ci(preset, L"mkinitcpio") || str_eq_ci(preset, L"arch"))
        return luks_make_key_cmdline(L"cryptkey=rootfs:", key_path);
    if (str_eq_ci(preset, L"dracut") || str_eq_ci(preset, L"systemd"))
        return luks_make_key_cmdline(L"rd.luks.key=", key_path);
    efi_log(L"WARN: unknown luks_preset; set luks_cmdline manually");
    return NULL;
}

static EFI_STATUS parse_entry(config_t *config, CHAR16 **lines, UINTN *idx,
                              UINTN count, int win_hint) {
    CHAR16 *name = NULL;
    CHAR16 *icon_path = NULL;
    CHAR16 *kernel_path = NULL;
    CHAR16 *initrd_path = NULL;
    CHAR16 *cmdline = NULL;
    CHAR16 *uuid = NULL;
    int type = win_hint ? 1 : 0;
    color_t color; int has_color = 0;
    UINT8 sha256_buf[32]; int has_sha256 = 0;
    UINTN entry_icon_size = 0;
    int encrypted = 0, kernel_encrypted_set = 0, initrd_encrypted_set = 0;
    int kernel_encrypted = 0, initrd_encrypted = 0;
    int luks = 0;
    CHAR16 *luks_key_path = NULL;
    CHAR16 *luks_cmdline = NULL;
    CHAR16 *luks_preset = NULL;

    while (*idx < count) {
        CHAR16 *line = trim(lines[*idx]);

        if (line[0] == '}' || line[0] == '\0') {

            break;
        }

        CHAR16 *eq = efi_strchr(line, '=');
        if (eq) {
            *eq = '\0';
            CHAR16 *key = trim(line);
            strip_inline_comment(eq + 1);
            CHAR16 *value = trim(eq + 1);

            if (efi_strcmp(key, L"name") == 0) {
                name = efi_strdup(value);
            } else if (efi_strcmp(key, L"icon") == 0) {
                icon_path = dup_path(value);
            } else if (efi_strcmp(key, L"kernel") == 0) {
                kernel_path = dup_path(value);
            } else if (efi_strcmp(key, L"initrd") == 0) {
                initrd_path = dup_path(value);
            } else if (efi_strcmp(key, L"cmdline") == 0 || efi_strcmp(key, L"options") == 0) {
                cmdline = efi_strdup(value);
            } else if (efi_strcmp(key, L"uuid") == 0) {
                uuid = efi_strdup(value);
            } else if (efi_strcmp(key, L"color") == 0) {
                has_color = parse_color(value, &color);
                if (!has_color) efi_log(L"WARN: invalid entry color= (use #RRGGBB)");
            } else if (efi_strcmp(key, L"sha256") == 0) {
                has_sha256 = parse_sha256(value, sha256_buf);
                if (!has_sha256) efi_log(L"WARN: invalid sha256= (expect 64 hex chars)");
            } else if (efi_strcmp(key, L"icon_size") == 0) {
                entry_icon_size = parse_uint(value);
            } else if (efi_strcmp(key, L"encrypted") == 0) {
                encrypted = (*value == '1' || *value == 't' || *value == 'y');
            } else if (efi_strcmp(key, L"kernel_encrypted") == 0) {
                kernel_encrypted = (*value == '1' || *value == 't' || *value == 'y');
                kernel_encrypted_set = 1;
            } else if (efi_strcmp(key, L"initrd_encrypted") == 0) {
                initrd_encrypted = (*value == '1' || *value == 't' || *value == 'y');
                initrd_encrypted_set = 1;
            } else if (efi_strcmp(key, L"luks") == 0 ||
                       efi_strcmp(key, L"luks_password") == 0) {
                luks = (*value == '1' || *value == 't' || *value == 'y');
            } else if (efi_strcmp(key, L"luks_key_path") == 0) {
                luks_key_path = efi_strdup(value);
            } else if (efi_strcmp(key, L"luks_cmdline") == 0 ||
                       efi_strcmp(key, L"luks_options") == 0 ||
                       efi_strcmp(key, L"luks_options_append") == 0) {
                luks_cmdline = efi_strdup(value);
            } else if (efi_strcmp(key, L"luks_preset") == 0 ||
                       efi_strcmp(key, L"luks_initramfs") == 0) {
                luks_preset = efi_strdup(value);
            }
        }
        (*idx)++;
    }

    if (!type && looks_windows(kernel_path)) type = 1;
    if (!kernel_encrypted_set) kernel_encrypted = encrypted;
    if (!initrd_encrypted_set) initrd_encrypted = encrypted;
    if (luks && !luks_cmdline && luks_preset)
        luks_cmdline = luks_cmdline_from_preset(luks_preset, luks_key_path);

    int entry_added = 0;
    if (name && kernel_path) {
        boot_entry_t *e = config_add_entry(config, name, icon_path, kernel_path,
                                           initrd_path, cmdline, uuid, type,
                                           kernel_encrypted, initrd_encrypted);
        if (e) {
            entry_added = 1;
            e->luks = luks;
            e->luks_key_path = luks_key_path;
            e->luks_cmdline = luks_cmdline;
            e->luks_preset = luks_preset;
            luks_key_path = NULL;
            luks_cmdline = NULL;
            luks_preset = NULL;
        }
        if (e && has_color) { e->color = color; e->has_color = 1; }
        if (e) e->icon_size = entry_icon_size;
        if (e && has_sha256) {
            for (int i = 0; i < 32; i++) e->sha256[i] = sha256_buf[i];
            e->has_sha256 = 1;
        }
    }
    if (!entry_added) {
        free_char16(&name);
        free_char16(&icon_path);
        free_char16(&kernel_path);
        free_char16(&initrd_path);
        free_char16(&cmdline);
        free_char16(&uuid);
    }
    if (luks_key_path) efi_free_pool(luks_key_path);
    if (luks_cmdline) efi_free_pool(luks_cmdline);
    if (luks_preset) efi_free_pool(luks_preset);

    return EFI_SUCCESS;
}

static CHAR16 lc16(CHAR16 c) { return (c >= 'A' && c <= 'Z') ? (CHAR16)(c + 32) : c; }

static int contains_ci(CHAR16 *hay, const CHAR16 *needle) {
    for (UINTN i = 0; hay[i]; i++) {
        UINTN j = 0;
        while (needle[j] && lc16(hay[i + j]) == lc16(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static int ends_with_ci(CHAR16 *s, const CHAR16 *suf) {
    UINTN ls = 0, lf = 0;
    while (s[ls]) ls++;
    while (suf[lf]) lf++;
    if (lf > ls) return 0;
    for (UINTN i = 0; i < lf; i++)
        if (lc16(s[ls - lf + i]) != lc16(suf[i])) return 0;
    return 1;
}

static CHAR16* icon_path_for(const CHAR16 *file) {
    CHAR16 *out = efi_allocate_pool(MAX_PATH * sizeof(CHAR16));
    if (!out) return NULL;
    SPrint(out, MAX_PATH * sizeof(CHAR16), L"%s\\icons\\%s", CONFIG_DIR, file);
    return out;
}

static CHAR16* distro_icon(CHAR16 *hint) {
    static const struct { const CHAR16 *needle; const CHAR16 *file; } map[] = {
        { L"endeavour",  L"endeavouros.png" },
        { L"arch",       L"arch.png" },
        { L"fedora",     L"fedora.png" },
        { L"mint",       L"linuxmint.png" },
        { L"manjaro",    L"manjaro.png" },
        { L"suse",       L"opensuse.png" },
        { L"pop",        L"pop.png" },
        { L"ubuntu",     L"ubuntu.png" },
        { NULL, NULL }
    };
    if (!hint) return icon_path_for(L"linux.png");
    for (int i = 0; map[i].needle; i++)
        if (contains_ci(hint, map[i].needle)) return icon_path_for(map[i].file);
    return icon_path_for(L"linux.png");
}

static int is_loader_efi(CHAR16 *name) {
    static const CHAR16 *skip[] = {
        L"grub", L"refind", L"shim", L"systemd-boot", L"bootx64",
        L"bootia32", L"mmx64", L"fbx64", L"mokmanager", L"bootmgr", NULL
    };
    for (int i = 0; skip[i]; i++)
        if (contains_ci(name, skip[i])) return 1;
    return 0;
}

static int scan_uki_dir(config_t *config, EFI_FILE_PROTOCOL *root, CHAR16 *dir) {
    EFI_FILE_PROTOCOL *d = efi_open_dir(root, dir);
    if (!d) return 0;

    int added = 0;
    CHAR16 name[128];
    int is_dir;
    while (efi_read_dirent(d, name, 128, &is_dir)) {
        if (is_dir) continue;
        if (!ends_with_ci(name, L".efi")) continue;
        if (is_loader_efi(name)) continue;

        CHAR16 path[MAX_PATH];
        SPrint(path, sizeof(path), L"%s\\%s", dir, name);

        CHAR16 disp[128];
        UINTN n = 0;
        while (name[n] && n < 127) { disp[n] = name[n]; n++; }
        disp[n] = '\0';
        if (n > 4) disp[n - 4] = '\0';

        CHAR16 *entry_name = efi_strdup(disp);
        CHAR16 *entry_icon = distro_icon(disp);
        CHAR16 *entry_path = efi_strdup(path);
        if (!entry_name || !entry_path) {
            free_char16(&entry_name);
            free_char16(&entry_icon);
            free_char16(&entry_path);
        } else if (config_add_entry(config, entry_name, entry_icon, entry_path,
                             NULL, NULL, NULL, 0, 0, 0)) {
            added++;
        } else {
            free_char16(&entry_name);
            free_char16(&entry_icon);
            free_char16(&entry_path);
        }
    }
    d->Close(d);
    return added;
}

static int is_kernel_name(CHAR16 *name) {
    if (ends_with_ci(name, L".img")) return 0;
    if (contains_ci(name, L"initrd") || contains_ci(name, L"initramfs")) return 0;
    if (lc16(name[0]) == 'v' && lc16(name[1]) == 'm' && lc16(name[2]) == 'l') return 1;
    if (contains_ci(name, L"bzimage")) return 1;
    return 0;
}

static CHAR16* find_initrd(EFI_FILE_PROTOCOL *root, CHAR16 *dir, CHAR16 *kernel_name) {
    CHAR16 *suffix = efi_strchr(kernel_name, '-');
    CHAR16 sbuf[96];
    if (suffix) {
        UINTN i = 0;
        while (suffix[i] && i < 95) { sbuf[i] = suffix[i]; i++; }
        sbuf[i] = '\0';
    } else {
        sbuf[0] = '\0';
    }

    const CHAR16 *patterns[] = {
        L"%s\\initramfs%s.img", L"%s\\initrd.img%s", L"%s\\initramfs%s",
        L"%s\\initrd%s.img",    L"%s\\initrd%s",     L"%s\\initramfs.img",
        L"%s\\initrd.img",      NULL
    };
    for (int p = 0; patterns[p]; p++) {
        CHAR16 cand[MAX_PATH];
        SPrint(cand, sizeof(cand), patterns[p], dir, sbuf);
        if (efi_file_exists_root(root, cand)) return efi_strdup(cand);
    }
    return NULL;
}

static int scan_kernel_dir(config_t *config, EFI_FILE_PROTOCOL *root, CHAR16 *dir) {
    EFI_FILE_PROTOCOL *d = efi_open_dir(root, dir);
    if (!d) return 0;

    int added = 0;
    CHAR16 name[128];
    int is_dir;
    while (efi_read_dirent(d, name, 128, &is_dir)) {
        if (is_dir) continue;
        if (!is_kernel_name(name)) continue;

        CHAR16 path[MAX_PATH];
        SPrint(path, sizeof(path), L"%s\\%s", dir, name);
        CHAR16 *initrd = find_initrd(root, dir, name);

        efi_log(L"config: auto-detected raw kernel");
        efi_log(path);
        if (initrd) efi_log(initrd);

        CHAR16 *entry_name = efi_strdup(name);
        CHAR16 *entry_icon = icon_path_for(L"unknown.png");
        CHAR16 *entry_path = efi_strdup(path);
        if (!entry_name || !entry_path) {
            free_char16(&entry_name);
            free_char16(&entry_icon);
            free_char16(&entry_path);
            free_char16(&initrd);
        } else if (config_add_entry(config, entry_name, entry_icon,
                             entry_path, initrd, NULL, NULL, 0, 0, 0)) {
            added++;
        } else {
            free_char16(&entry_name);
            free_char16(&entry_icon);
            free_char16(&entry_path);
            free_char16(&initrd);
        }
    }
    d->Close(d);
    return added;
}

static EFI_FILE_PROTOCOL *root_from_handle(EFI_HANDLE h) {
    EFI_FILE_IO_INTERFACE *io = NULL;
    if (EFI_ERROR(BS->HandleProtocol(h, &gEfiSimpleFileSystemProtocolGuid, (void**)&io)) || !io)
        return NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (EFI_ERROR(io->OpenVolume(io, &root))) return NULL;
    return root;
}

#define MAX_BLS 48

typedef struct {
    CHAR16 *title, *version, *kernel, *initrd, *options, *machine, *conf;
    int tries_left, tries_done, ot_idx;
} bls_rec_t;

static CHAR16* read_text_from_root(EFI_FILE_PROTOCOL *root, CHAR16 *path) {
    EFI_FILE_PROTOCOL *fh = NULL;
    if (EFI_ERROR(root->Open(root, &fh, path, EFI_FILE_MODE_READ, 0)) || !fh) return NULL;
    UINT64 sz = 0;
    fh->SetPosition(fh, ~0ULL);
    fh->GetPosition(fh, &sz);
    fh->SetPosition(fh, 0);
    if (sz == 0 || sz > 1024 * 1024) { fh->Close(fh); return NULL; }
    UINT8 *raw = efi_allocate_pool((UINTN)sz);
    if (!raw) { fh->Close(fh); return NULL; }
    UINTN rd = (UINTN)sz;
    EFI_STATUS s = fh->Read(fh, &rd, raw);
    fh->Close(fh);
    if (EFI_ERROR(s) || rd != (UINTN)sz) { efi_free_pool(raw); return NULL; }
    CHAR16 *out = efi_allocate_pool((rd + 1) * sizeof(CHAR16));
    if (!out) { efi_free_pool(raw); return NULL; }
    for (UINTN i = 0; i < rd; i++) out[i] = (CHAR16)raw[i];
    out[rd] = 0;
    efi_free_pool(raw);
    return out;
}

static CHAR16* bls_field(CHAR16 *line, const CHAR16 *key) {
    UINTN i = 0;
    while (key[i] && line[i] == key[i]) i++;
    if (key[i] != '\0' || (line[i] != ' ' && line[i] != '\t')) return NULL;
    while (line[i] == ' ' || line[i] == '\t') i++;
    return &line[i];
}

static CHAR16* bls_path_dup(CHAR16 *p) {
    if (!p || !p[0]) return NULL;
    UINTN n = 0; while (p[n]) n++;
    CHAR16 *o = efi_allocate_pool((n + 1) * sizeof(CHAR16));
    if (!o) return NULL;
    for (UINTN i = 0; i < n; i++) o[i] = (p[i] == '/') ? '\\' : p[i];
    o[n] = 0;
    return o;
}

static void bls_tries(CHAR16 *name, int *left, int *done) {
    *left = -1; *done = 0;
    UINTN n = 0; while (name[n]) n++;
    INTN plus = -1;
    for (UINTN i = 0; i < n; i++) if (name[i] == '+') plus = (INTN)i;
    if (plus < 0) return;
    UINTN i = (UINTN)plus + 1;
    int l = 0, got = 0;
    while (name[i] >= '0' && name[i] <= '9') { l = l * 10 + (name[i] - '0'); i++; got = 1; }
    if (!got) return;
    int d = 0;
    if (name[i] == '-') { i++; while (name[i] >= '0' && name[i] <= '9') { d = d * 10 + (name[i] - '0'); i++; } }
    *left = l; *done = d;
}

static CHAR16* base_title_dup(CHAR16 *t) {
    UINTN n = 0; while (t[n]) n++;
    UINTN cut = n;
    for (UINTN i = 0; i + 8 <= n; i++)
        if (t[i]=='(' && t[i+1]=='o' && t[i+2]=='s' && t[i+3]=='t' &&
            t[i+4]=='r' && t[i+5]=='e' && t[i+6]=='e' && t[i+7]==':') { cut = i; break; }
    while (cut > 0 && (t[cut-1]==' ' || t[cut-1]=='\t')) cut--;
    CHAR16 *o = efi_allocate_pool((cut + 1) * sizeof(CHAR16));
    if (!o) return NULL;
    for (UINTN i = 0; i < cut; i++) o[i] = t[i];
    o[cut] = 0;
    return o;
}

static int bls_ostree_idx(CHAR16 *t) {
    UINTN n = 0; while (t[n]) n++;
    for (UINTN i = 0; i + 8 <= n; i++)
        if (t[i]=='(' && t[i+1]=='o' && t[i+2]=='s' && t[i+3]=='t' &&
            t[i+4]=='r' && t[i+5]=='e' && t[i+6]=='e' && t[i+7]==':') {
            UINTN j = i + 8; int v = 0, g = 0;
            while (t[j] >= '0' && t[j] <= '9') { v = v * 10 + (t[j] - '0'); j++; g = 1; }
            return g ? v : -1;
        }
    return -1;
}

static int bls_same_group(bls_rec_t *a, bls_rec_t *b) {
    if (a->machine && b->machine) return efi_strcmp(a->machine, b->machine) == 0;
    CHAR16 *ba = base_title_dup(a->title), *bb = base_title_dup(b->title);
    int eq = (ba && bb && efi_strcmp(ba, bb) == 0);
    if (ba) efi_free_pool(ba);
    if (bb) efi_free_pool(bb);
    return eq;
}

static void bls_scan(EFI_FILE_PROTOCOL *root, CHAR16 *dir, bls_rec_t *recs, int *nrec) {
    EFI_FILE_PROTOCOL *d = efi_open_dir(root, dir);
    if (!d) return;
    CHAR16 name[160];
    int is_dir;
    while (*nrec < MAX_BLS && efi_read_dirent(d, name, 160, &is_dir)) {
        if (is_dir || !ends_with_ci(name, L".conf")) continue;
        CHAR16 path[MAX_PATH];
        SPrint(path, sizeof(path), L"%s\\%s", dir, name);
        CHAR16 *buf = read_text_from_root(root, path);
        if (!buf) continue;

        bls_rec_t r = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, -1, 0, -1 };
        CHAR16 *start = buf;
        while (*start) {
            CHAR16 *end = start;
            while (*end && *end != '\n') end++;
            if (*end == '\n') *end = '\0';
            CHAR16 *cr = efi_strchr(start, '\r');
            if (cr) *cr = '\0';
            CHAR16 *line = trim(start);
            CHAR16 *v;
            if (line[0] && line[0] != '#') {
                if      ((v = bls_field(line, L"title")))      { if (!r.title)   r.title   = efi_strdup(v); }
                else if ((v = bls_field(line, L"version")))    { if (!r.version) r.version = efi_strdup(v); }
                else if ((v = bls_field(line, L"linux")))      { if (!r.kernel)  r.kernel  = efi_strdup(v); }
                else if ((v = bls_field(line, L"initrd")))     { if (!r.initrd)  r.initrd  = efi_strdup(v); }
                else if ((v = bls_field(line, L"options")))    { if (!r.options) r.options = efi_strdup(v); }
                else if ((v = bls_field(line, L"machine-id"))) { if (!r.machine) r.machine = efi_strdup(v); }
            }
            start = end + 1;
        }
        efi_free_pool(buf);

        if (r.title && r.kernel) {
            r.conf = efi_strdup(path);
            bls_tries(name, &r.tries_left, &r.tries_done);
            r.ot_idx = bls_ostree_idx(r.title);
            recs[(*nrec)++] = r;
        } else {
            if (r.title) efi_free_pool(r.title);
            if (r.version) efi_free_pool(r.version);
            if (r.kernel) efi_free_pool(r.kernel);
            if (r.initrd) efi_free_pool(r.initrd);
            if (r.options) efi_free_pool(r.options);
            if (r.machine) efi_free_pool(r.machine);
        }
    }
    d->Close(d);
}

static int bls_detect(config_t *config) {
    bls_rec_t recs[MAX_BLS];
    int nrec = 0;

    UINTN nvol = 0;
    EFI_HANDLE *vols = efi_locate_handle_buffer(&gEfiSimpleFileSystemProtocolGuid, &nvol);
    if (vols) {
        for (UINTN v = 0; v < nvol && nrec < MAX_BLS; v++) {
            EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
            if (!root) continue;
            bls_scan(root, L"\\loader\\entries", recs, &nrec);
            bls_scan(root, L"\\boot\\loader\\entries", recs, &nrec);
            root->Close(root);
        }
        efi_free_pool(vols);
    }
    if (nrec == 0) return 0;

    int used[MAX_BLS];
    for (int i = 0; i < nrec; i++) used[i] = 0;
    int groups = 0;

    for (int i = 0; i < nrec; i++) {
        if (used[i]) continue;
        int idx[MAX_BLS]; int n = 0;
        idx[n++] = i; used[i] = 1;
        for (int j = i + 1; j < nrec; j++)
            if (!used[j] && bls_same_group(&recs[i], &recs[j])) { idx[n++] = j; used[j] = 1; }

        for (int a = 1; a < n; a++) {
            int key = idx[a];
            int ra = recs[key].ot_idx >= 0 ? recs[key].ot_idx : 1000 + key;
            int b = a - 1;
            while (b >= 0) {
                int rb = recs[idx[b]].ot_idx >= 0 ? recs[idx[b]].ot_idx : 1000 + idx[b];
                if (rb <= ra) break;
                idx[b + 1] = idx[b]; b--;
            }
            idx[b + 1] = key;
        }

        deployment_t *deps = efi_allocate_pool((UINTN)n * sizeof(deployment_t));
        if (!deps) continue;
        int deps_ok = 1;
        for (int k = 0; k < n; k++) {
            bls_rec_t *r = &recs[idx[k]];
            deps[k].version    = r->version ? efi_strdup(r->version) : efi_strdup(L"unknown");
            deps[k].kernel     = bls_path_dup(r->kernel);
            deps[k].initrd     = bls_path_dup(r->initrd);
            deps[k].cmdline    = r->options ? efi_strdup(r->options) : NULL;
            deps[k].bls_path   = r->conf ? efi_strdup(r->conf) : NULL;
            deps[k].role       = (k == 0) ? DEPLOY_CURRENT : (k == 1 ? DEPLOY_ROLLBACK : DEPLOY_OLDER);
            deps[k].tries_left = r->tries_left;
            deps[k].tries_done = r->tries_done;
            if (!deps[k].version || !deps[k].kernel ||
                (r->options && !deps[k].cmdline) ||
                (r->conf && !deps[k].bls_path))
                deps_ok = 0;
        }
        if (!deps_ok) {
            free_deployments(deps, (UINTN)n);
            continue;
        }

        UINTN def = 0;
        if (n > 1 && deps[0].tries_left == 0) def = 1;

        CHAR16 *dispname = base_title_dup(recs[idx[0]].title);
        CHAR16 *dispicon = distro_icon(dispname);
        boot_entry_t *e = config_add_entry(config, dispname, dispicon,
                                           deps[def].kernel, deps[def].initrd,
                                           deps[def].cmdline, NULL, 0, 0, 0);
        if (e) {
            e->deployments    = deps;
            e->deploy_count   = (UINTN)n;
            e->deploy_default = def;
            e->deploy_sel     = def;
            groups++;
        } else {
            free_char16(&dispname);
            free_char16(&dispicon);
            free_deployments(deps, (UINTN)n);
        }
    }

    for (int i = 0; i < nrec; i++) {
        if (recs[i].title) efi_free_pool(recs[i].title);
        if (recs[i].version) efi_free_pool(recs[i].version);
        if (recs[i].kernel) efi_free_pool(recs[i].kernel);
        if (recs[i].initrd) efi_free_pool(recs[i].initrd);
        if (recs[i].options) efi_free_pool(recs[i].options);
        if (recs[i].machine) efi_free_pool(recs[i].machine);
        if (recs[i].conf) efi_free_pool(recs[i].conf);
    }

    if (groups) {
        CHAR16 m[64];
        SPrint(m, sizeof(m), L"bls: detected %d OSTree/BLS OS group(s)", groups);
        efi_log(m);
    }
    return groups;
}

void bls_decrement(boot_entry_t *e) {
    if (!e || e->deploy_count == 0) return;
    UINTN s = e->deploy_sel; if (s >= e->deploy_count) s = 0;
    deployment_t *d = &e->deployments[s];
    if (d->tries_left <= 0 || !d->bls_path) return;

    UINTN n = 0; while (d->bls_path[n]) n++;
    INTN plus = -1;
    for (UINTN i = 0; i < n; i++) if (d->bls_path[i] == '+') plus = (INTN)i;
    if (plus < 0) return;

    CHAR16 base[MAX_PATH];
    UINTN k = 0;
    for (INTN i = 0; i < plus && k < MAX_PATH - 1; i++) base[k++] = d->bls_path[i];
    base[k] = 0;

    CHAR16 newp[MAX_PATH];
    SPrint(newp, sizeof(newp), L"%s+%d-%d.conf", base, d->tries_left - 1, d->tries_done + 1);

    if (efi_rename_file(d->bls_path, newp))
        efi_log(L"bls: decremented boot-counter for selected deployment");
    else
        efi_log(L"WARN: could not update boot-counter (read-only /boot?)");
}

static EFI_STATUS detect_entries(config_t *config) {
    static CHAR16 *windows_paths[] = {
        L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
        L"\\EFI\\BOOT\\bootmgfw.efi",
        NULL
    };

    config->show_names = 0;
    config->center_info = 1;

    int bls = bls_detect(config);

    UINTN nvol = 0;
    EFI_HANDLE *vols = efi_locate_handle_buffer(&gEfiSimpleFileSystemProtocolGuid, &nvol);
    if (!vols || nvol == 0) {
        if (vols) efi_free_pool(vols);
        return EFI_NOT_FOUND;
    }

    int windows_found = 0;
    int uki_found = 0;

    for (UINTN v = 0; v < nvol; v++) {
        EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
        if (!root) continue;

        if (!windows_found) {
            for (int i = 0; windows_paths[i] != NULL; i++) {
                if (efi_file_exists_root(root, windows_paths[i])) {
                    CHAR16 *entry_name = efi_strdup(L"Windows Boot Manager");
                    CHAR16 *entry_icon = icon_path_for(L"windows.png");
                    CHAR16 *entry_path = efi_strdup(windows_paths[i]);
                    if (!entry_name || !entry_path) {
                        free_char16(&entry_name);
                        free_char16(&entry_icon);
                        free_char16(&entry_path);
                    } else if (config_add_entry(config, entry_name, entry_icon,
                                         entry_path, NULL, NULL, NULL, 1, 0, 0)) {
                        windows_found = 1;
                    } else {
                        free_char16(&entry_name);
                        free_char16(&entry_icon);
                        free_char16(&entry_path);
                    }
                    break;
                }
            }
        }

        uki_found += scan_uki_dir(config, root, L"\\EFI\\Linux");

        root->Close(root);
    }

    if (!uki_found && !bls) {
        for (UINTN v = 0; v < nvol; v++) {
            EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
            if (!root) continue;
            scan_kernel_dir(config, root, L"\\boot");
            scan_kernel_dir(config, root, L"\\");
            root->Close(root);
        }
    }

    efi_free_pool(vols);
    return EFI_SUCCESS;
}

#define CONFIG_TEXT_MAX (1024ULL * 1024ULL)

static CHAR16* read_text_file(CHAR16 *path) {
    efi_file_t *file = efi_fopen(path);
    if (!file) return NULL;

    UINT64 size = 0;
    file->handle->SetPosition(file->handle, ~0ULL);
    file->handle->GetPosition(file->handle, &size);
    file->handle->SetPosition(file->handle, 0);

    if (size > CONFIG_TEXT_MAX) {
        efi_log(L"WARN: config text file exceeds 1 MiB - ignoring");
        efi_fclose(file);
        return NULL;
    }

    UINT8 *raw = NULL;
    UINTN raw_len = (UINTN)size;
    if (raw_len) {
        raw = efi_allocate_pool(raw_len);
        if (!raw) { efi_fclose(file); return NULL; }

        UINTN total = 0;
        while (total < raw_len) {
            UINTN n = efi_fread(file, raw + total, raw_len - total);
            if (n == 0) break;
            total += n;
        }
        if (total != raw_len) {
            efi_log(L"WARN: short config text read - ignoring file");
            efi_free_pool(raw);
            efi_fclose(file);
            return NULL;
        }
    }
    efi_fclose(file);

    UINTN off = 0;
    int   utf16 = 0;
    if (raw_len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        utf16 = 1; off = 2;
    } else if (raw_len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        off = 3;
    } else if (raw_len >= 2 && raw[1] == 0x00 && raw[0] != 0x00) {
        utf16 = 1;
    }

    UINTN   char_count = utf16 ? (raw_len - off) / 2 : (raw_len - off);
    CHAR16 *buf = efi_allocate_pool((char_count + 1) * sizeof(CHAR16));
    if (!buf) { efi_free_pool(raw); return NULL; }

    if (utf16) {
        for (UINTN i = 0; i < char_count; i++)
            buf[i] = (CHAR16)(raw[off + i*2] | (raw[off + i*2 + 1] << 8));
    } else {
        for (UINTN i = 0; i < char_count; i++)
            buf[i] = (CHAR16)raw[off + i];
    }
    buf[char_count] = '\0';
    efi_free_pool(raw);
    return buf;
}

static void apply_global(config_t *config, CHAR16 *key, CHAR16 *value) {
    if (efi_strcmp(key, L"timeout") == 0) {
        INTN sign = 1;
        if (*value == '-') { sign = -1; value++; }
        INTN t = 0;
        while (*value >= '0' && *value <= '9') { t = t * 10 + (*value - '0'); value++; }
        config->timeout = (sign < 0) ? -1 : t;
    } else if (efi_strcmp(key, L"default") == 0) {
        config->default_entry = 0;
        while (*value >= '0' && *value <= '9') {
            config->default_entry = config->default_entry * 10 + (*value - '0');
            value++;
        }
    } else if (efi_strcmp(key, L"quiet") == 0) {
        config->quiet = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"text_menu") == 0 || efi_strcmp(key, L"text_mode") == 0) {
        config->text_menu = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"cmdline") == 0 || efi_strcmp(key, L"options") == 0) {
        config->def_cmdline = (value[0] == '\0') ? NULL : efi_strdup(value);
    } else if (efi_strcmp(key, L"show_names") == 0 || efi_strcmp(key, L"names") == 0) {
        config->show_names = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"center_info") == 0 || efi_strcmp(key, L"centre_info") == 0) {
        config->center_info = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"autoboot") == 0) {
        config->autoboot = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"remember_last") == 0 || efi_strcmp(key, L"remember") == 0) {
        config->remember_last = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"recovery_entries") == 0 || efi_strcmp(key, L"recovery") == 0) {
        config->recovery_entries = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"mouse") == 0 || efi_strcmp(key, L"pointer") == 0) {
        config->mouse = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"editor") == 0) {
        config->editor = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"box_radius") == 0 || efi_strcmp(key, L"corner_radius") == 0) {
        config->box_radius = parse_uint(value);
    } else if (efi_strcmp(key, L"resolution") == 0) {
        config->res_w = 0; config->res_h = 0; config->res_max = 0;
        if (efi_strcmp(value, L"max") == 0 || efi_strcmp(value, L"highest") == 0) {
            config->res_max = 1;
        } else if (efi_strcmp(value, L"native") == 0 || value[0] == '\0') {

        } else {
            UINTN w = 0;
            while (*value >= '0' && *value <= '9') { w = w * 10 + (*value - '0'); value++; }
            if (*value == 'x' || *value == 'X' || *value == '*') value++;
            UINTN h = 0;
            while (*value >= '0' && *value <= '9') { h = h * 10 + (*value - '0'); value++; }
            if (w && h) { config->res_w = w; config->res_h = h; }
            else efi_log(L"WARN: invalid resolution (use WxH, e.g. 1920x1080, or max/native)");
        }
    } else if (efi_strcmp(key, L"theme") == 0) {
        config->theme = (value[0] == '\0') ? NULL : efi_strdup(value);
    } else if (efi_strcmp(key, L"title") == 0) {
        if (efi_strcmp(value, L"none") == 0) {
            config->no_title = 1;
            config->title = NULL;
        } else if (value[0] == '\0') {
            config->no_title = 0;
            config->title = NULL;
        } else {
            config->no_title = 0;
            config->title = efi_strdup(value);
        }
    } else if (efi_strcmp(key, L"font") == 0) {
        config->font = (value[0] == '\0') ? NULL : efi_strdup(value);
    } else if (efi_strcmp(key, L"title_color") == 0) {
        if (!parse_color(value, &config->title_color))
            efi_log(L"WARN: invalid title_color (use #RRGGBB)");
    } else if (efi_strcmp(key, L"name_color") == 0) {
        if (!parse_color(value, &config->name_color))
            efi_log(L"WARN: invalid name_color (use #RRGGBB)");
    } else if (efi_strcmp(key, L"highlight_color") == 0) {
        if (!parse_color(value, &config->highlight_color))
            efi_log(L"WARN: invalid highlight_color (use #RRGGBB)");
    } else if (efi_strcmp(key, L"title_size") == 0) {
        config->title_size = parse_uint(value);
    } else if (efi_strcmp(key, L"name_size") == 0) {
        config->name_size = parse_uint(value);
    } else if (efi_strcmp(key, L"icon_size") == 0) {
        config->icon_size = parse_uint(value);
    } else if (efi_strcmp(key, L"icon_spacing") == 0) {
        config->icon_spacing = parse_uint(value);
    } else if (efi_strcmp(key, L"icon_y") == 0) {
        config->icon_y = parse_uint(value);
    } else if (efi_strcmp(key, L"underline_color") == 0) {
        if (parse_color(value, &config->underline_color))
            config->has_underline_color = 1;
        else
            efi_log(L"WARN: invalid underline_color (use #RRGGBB)");
    } else if (efi_strcmp(key, L"underline_thickness") == 0) {
        config->underline_thickness = parse_uint(value);
    } else if (efi_strcmp(key, L"underline_length") == 0) {
        config->underline_length = parse_uint(value);
    } else if (efi_strcmp(key, L"power_position") == 0) {
        if (efi_strcmp(value, L"topright") == 0)
            config->power_position = POWER_POS_TOPRIGHT;
        else if (efi_strcmp(value, L"topleft") == 0)
            config->power_position = POWER_POS_TOPLEFT;
        else if (efi_strcmp(value, L"bottomleft") == 0)
            config->power_position = POWER_POS_BOTTOMLEFT;
        else if (efi_strcmp(value, L"bottomright") == 0)
            config->power_position = POWER_POS_BOTTOMRIGHT;
        else
            efi_log(L"WARN: invalid power_position (topright/topleft/bottomleft/bottomright)");
    } else if (efi_strcmp(key, L"shutdown_color") == 0) {
        if (parse_color(value, &config->shutdown_color))
            config->has_shutdown_color = 1;
        else
            efi_log(L"WARN: invalid shutdown_color (use #RRGGBB)");
    } else if (efi_strcmp(key, L"reboot_color") == 0) {
        if (parse_color(value, &config->reboot_color))
            config->has_reboot_color = 1;
        else
            efi_log(L"WARN: invalid reboot_color (use #RRGGBB)");
    } else if (efi_strcmp(key, L"firmware_color") == 0) {
        if (parse_color(value, &config->firmware_color))
            config->has_firmware_color = 1;
        else
            efi_log(L"WARN: invalid firmware_color (use #RRGGBB)");
    } else if (efi_strcmp(key, L"background") == 0) {
        config->background = dup_path(value);
    } else if (efi_strcmp(key, L"power_icons") == 0) {
        config->power_icons = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"power_icon_size") == 0) {
        config->power_icon_size = parse_uint(value);
    } else if (efi_strcmp(key, L"shutdown_icon") == 0) {
        config->shutdown_icon = dup_path(value);
    } else if (efi_strcmp(key, L"reboot_icon") == 0) {
        config->reboot_icon = dup_path(value);
    } else if (efi_strcmp(key, L"firmware_icon") == 0) {
        config->firmware_icon = dup_path(value);
    } else if (efi_strcmp(key, L"blur") == 0) {
        if (*value == 'c' || *value == 'C') config->blur = 2;
        else config->blur = (*value == '1' || *value == 't' || *value == 'y' || *value == 'f') ? 1 : 0;
    } else if (efi_strcmp(key, L"animation") == 0) {
        config->animation = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"anim_speed") == 0) {
        config->anim_speed = (int)parse_uint(value);
    } else if (efi_strcmp(key, L"fade_speed") == 0) {
        config->fade_speed = (int)parse_uint(value);
    } else if (efi_strcmp(key, L"entries_per_page") == 0) {
        config->entries_per_page = parse_uint(value);
    } else if (efi_strcmp(key, L"blur_title") == 0) {
        config->blur_title = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"blur_color") == 0) {
        if (parse_color(value, &config->blur_color))
            config->has_blur_color = 1;
        else
            efi_log(L"WARN: invalid blur_color (use #RRGGBB)");
    }
}

static void apply_theme(config_t *config, CHAR16 *name) {
    CHAR16 path[MAX_PATH];
    SPrint(path, sizeof(path), L"%s\\themes\\%s.conf", CONFIG_DIR, name);
    efi_log(L"config: loading theme file");
    efi_log(path);

    CHAR16 *buf = read_text_file(path);
    if (!buf) { efi_log(L"WARN: theme file not found - keeping boot.conf values"); return; }

    CHAR16 *start = buf;
    while (*start) {
        CHAR16 *end = start;
        while (*end && *end != '\n') end++;
        if (*end == '\n') *end = '\0';
        CHAR16 *cr = efi_strchr(start, '\r');
        if (cr) *cr = '\0';

        CHAR16 *line = trim(start);
        if (line[0] != '#' && line[0] != '\0') {
            CHAR16 *eq = efi_strchr(line, '=');
            if (eq) {
                *eq = '\0';
                CHAR16 *key = trim(line);
                strip_inline_comment(eq + 1);
                CHAR16 *value = trim(eq + 1);
                if (efi_strcmp(key, L"theme") != 0)
                    apply_global(config, key, value);
            }
        }
        start = end + 1;
    }
    efi_free_pool(buf);
}

#define MAX_THEMES 32

static CHAR16* resolve_rotating_theme(int cycle) {
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return NULL;
    CHAR16 dir[MAX_PATH];
    SPrint(dir, sizeof(dir), L"%s\\themes", CONFIG_DIR);
    EFI_FILE_PROTOCOL *d = efi_open_dir(root, dir);
    root->Close(root);
    if (!d) return NULL;

    CHAR16 names[MAX_THEMES][64];
    int n = 0;
    CHAR16 name[128];
    int is_dir;
    while (n < MAX_THEMES && efi_read_dirent(d, name, 128, &is_dir)) {
        if (is_dir || !ends_with_ci(name, L".conf")) continue;
        UINTN k = 0;
        while (name[k] && k < 63) { names[n][k] = name[k]; k++; }
        names[n][k] = '\0';
        if (k > 5) names[n][k - 5] = '\0';
        n++;
    }
    d->Close(d);
    if (n == 0) return NULL;

    for (int a = 1; a < n; a++) {
        CHAR16 tmp[64];
        UINTN t = 0; while (names[a][t]) { tmp[t] = names[a][t]; t++; } tmp[t] = '\0';
        int b = a - 1;
        while (b >= 0 && efi_strcmp(names[b], tmp) > 0) {
            UINTN c = 0; while (names[b][c]) { names[b + 1][c] = names[b][c]; c++; } names[b + 1][c] = '\0';
            b--;
        }
        UINTN c = 0; while (tmp[c]) { names[b + 1][c] = tmp[c]; c++; } names[b + 1][c] = '\0';
    }

    UINT32 idx;
    if (cycle) {
        UINT32 saved = 0;
        efi_get_var_u32(L"VisorThemeIndex", &saved);
        idx = saved % (UINT32)n;
        efi_set_var_u32(L"VisorThemeIndex", (idx + 1) % (UINT32)n);
    } else {
        idx = efi_rand() % (UINT32)n;
    }
    efi_log(L"config: rotated theme selected");
    efi_log(names[idx]);
    return efi_strdup(names[idx]);
}

static void add_recovery_entries(config_t *config) {
    static const CHAR16 *suffix = L"systemd.unit=rescue.target nomodeset";

    boot_entry_t *orig[MAX_THEMES];
    int n = 0;
    for (boot_entry_t *e = config->entries; e && n < MAX_THEMES; e = e->next)
        orig[n++] = e;

    for (int i = 0; i < n; i++) {
        boot_entry_t *o = orig[i];
        if (o->type == 1 || !o->kernel_path) continue;

        UINTN nlen = 0; while (o->name[nlen]) nlen++;
        CHAR16 *rname = efi_allocate_pool((nlen + 16) * sizeof(CHAR16));
        if (!rname) continue;
        SPrint(rname, (nlen + 16) * sizeof(CHAR16), L"%s (recovery)", o->name);

        UINTN olen = 0; if (o->cmdline) while (o->cmdline[olen]) olen++;
        UINTN slen = 0; while (suffix[slen]) slen++;
        CHAR16 *rcmd = efi_allocate_pool((olen + slen + 2) * sizeof(CHAR16));
        if (!rcmd) { efi_free_pool(rname); continue; }
        if (olen) SPrint(rcmd, (olen + slen + 2) * sizeof(CHAR16), L"%s %s", o->cmdline, suffix);
        else      SPrint(rcmd, (olen + slen + 2) * sizeof(CHAR16), L"%s", suffix);

        CHAR16 *ricon = o->icon_path ? efi_strdup(o->icon_path) : NULL;
        CHAR16 *rkernel = efi_strdup(o->kernel_path);
        CHAR16 *rinitrd = o->initrd_path ? efi_strdup(o->initrd_path) : NULL;
        CHAR16 *ruuid = o->uuid ? efi_strdup(o->uuid) : NULL;
        if (!rkernel || (o->icon_path && !ricon) ||
            (o->initrd_path && !rinitrd) || (o->uuid && !ruuid)) {
            free_char16(&rname);
            free_char16(&ricon);
            free_char16(&rkernel);
            free_char16(&rinitrd);
            free_char16(&rcmd);
            free_char16(&ruuid);
            continue;
        }

        boot_entry_t *r = config_add_entry(config, rname, ricon, rkernel,
            rinitrd, rcmd, ruuid, o->type, o->encrypted, o->initrd_encrypted);
        if (r) {
            r->luks = o->luks;
            r->luks_key_path = o->luks_key_path ? efi_strdup(o->luks_key_path) : NULL;
            r->luks_cmdline = o->luks_cmdline ? efi_strdup(o->luks_cmdline) : NULL;
            r->luks_preset = o->luks_preset ? efi_strdup(o->luks_preset) : NULL;
            r->color = o->color;
            r->has_color = o->has_color;
            r->icon_size = o->icon_size;
            if (o->has_sha256) {
                for (int k = 0; k < 32; k++) r->sha256[k] = o->sha256[k];
                r->has_sha256 = 1;
            }
        } else {
            free_char16(&rname);
            free_char16(&ricon);
            free_char16(&rkernel);
            free_char16(&rinitrd);
            free_char16(&rcmd);
            free_char16(&ruuid);
        }
    }
}

EFI_STATUS config_parse(config_t *config) {

    config->timeout = 5;
    config->default_entry = 0;
    config->quiet = 0;
    config->text_menu = 0;
    config->res_w = 0;
    config->res_h = 0;
    config->res_max = 0;
    config->def_cmdline = NULL;
    config->show_names = 1;
    config->center_info = 0;
    config->box_radius = 0;
    config->remember_last = 0;
    config->recovery_entries = 0;
    config->autoboot = 0;
    config->mouse = 1;
    config->editor = 1;
    config->theme = NULL;
    config->title = NULL;
    config->no_title = 0;
    config->font = NULL;
    config->background = NULL;
    config->bg_color = (color_t){0x1a, 0x1a, 0x2e};
    config->fg_color = COLOR_WHITE;
    config->highlight_color = COLOR_BLUE;
    config->title_color = COLOR_WHITE;
    config->name_color = COLOR_WHITE;
    config->title_size = 0;
    config->name_size = 0;
    config->icon_size = 0;
    config->icon_spacing = 0;
    config->icon_y = 0;
    config->underline_color = COLOR_BLUE;
    config->has_underline_color = 0;
    config->underline_thickness = 0;
    config->underline_length = 0;
    config->power_position = POWER_POS_BOTTOMRIGHT;
    config->shutdown_color = COLOR_BLUE;
    config->reboot_color = COLOR_BLUE;
    config->firmware_color = COLOR_BLUE;
    config->has_shutdown_color = 0;
    config->has_reboot_color = 0;
    config->has_firmware_color = 0;
    config->blur = 0;
    config->blur_title = 0;
    config->blur_color = COLOR_WHITE;
    config->has_blur_color = 0;
    config->animation = 1;
    config->anim_speed = 0;
    config->fade_speed = 0;
    config->entries_per_page = 0;
    config->power_icons = 0;
    config->power_icon_size = 0;
    config->shutdown_icon = NULL;
    config->reboot_icon = NULL;
    config->firmware_icon = NULL;
    config->entries = NULL;
    config->tail = NULL;
    config->entry_count = 0;

    CHAR16 *buf = read_text_file(CONFIG_FILE);
    if (!buf) {
        efi_log(L"config: boot.conf not found, auto-detecting boot entries");
        return detect_entries(config);
    }

    CHAR16 *lines[256];
    UINTN line_count = 0;
    CHAR16 *start = buf;

    while (*start && line_count < 256) {
        CHAR16 *end = start;
        while (*end && *end != '\n') end++;
        if (*end == '\n') *end = '\0';

        CHAR16 *cr = efi_strchr(start, '\r');
        if (cr) *cr = '\0';

        lines[line_count++] = start;
        start = end + 1;
    }

    UINTN entry_count_before_parse = config->entry_count;

    for (UINTN i = 0; i < line_count; i++) {
        CHAR16 *line = trim(lines[i]);

        if (line[0] == '#' || line[0] == '\0') continue;

        CHAR16 *eq = efi_strchr(line, '=');
        if (eq) {
            *eq = '\0';
            CHAR16 *key = trim(line);
            strip_inline_comment(eq + 1);
            CHAR16 *value = trim(eq + 1);
            apply_global(config, key, value);
            continue;
        }

        if (line[0] == 'e' || line[0] == 'l' || line[0] == 'w') {
            int win_hint = is_header(line, L"windows");
            if (is_header(line, L"entry") ||
                is_header(line, L"linux") ||
                win_hint) {
                parse_entry(config, lines, &i, line_count, win_hint);
            }
        }
    }

    if (config->theme) {
        if (efi_strcmp(config->theme, L"random") == 0 ||
            efi_strcmp(config->theme, L"cycle") == 0) {
            int cycle = (efi_strcmp(config->theme, L"cycle") == 0);
            CHAR16 *chosen = resolve_rotating_theme(cycle);
            efi_free_pool(config->theme);
            config->theme = chosen;
        }
        if (config->theme) apply_theme(config, config->theme);
    }

    efi_free_pool(buf);

    if (config->entry_count == entry_count_before_parse) {
        detect_entries(config);
    }

    if (config->recovery_entries) add_recovery_entries(config);

    return EFI_SUCCESS;
}

static boot_entry_t* config_add_entry(config_t *config,
                                      CHAR16 *name,
                                      CHAR16 *icon_path,
                                      CHAR16 *kernel_path,
                                      CHAR16 *initrd_path,
                                      CHAR16 *cmdline,
                                      CHAR16 *uuid,
                                      int type,
                                      int encrypted,
                                      int initrd_encrypted) {
    boot_entry_t *entry = efi_allocate_pool(sizeof(boot_entry_t));
    if (!entry) { efi_log(L"ERROR: out of memory adding boot entry"); return NULL; }

    entry->name = name ? name : efi_strdup(L"Unknown");
    if (!entry->name) {
        efi_free_pool(entry);
        efi_log(L"ERROR: out of memory naming boot entry");
        return NULL;
    }
    entry->icon_path = icon_path;
    entry->kernel_path = kernel_path;
    entry->initrd_path = initrd_path;
    entry->cmdline = cmdline ? cmdline
                   : (config->def_cmdline ? efi_strdup(config->def_cmdline) : NULL);
    entry->uuid = uuid;
    entry->type = type;
    entry->index = config->entry_count;
    entry->icon = NULL;
    entry->icon_size = 0;
    entry->color = config->name_color;
    entry->has_color = 0;
    entry->has_sha256 = 0;
    entry->encrypted = encrypted;
    entry->initrd_encrypted = initrd_encrypted;
    entry->luks = 0;
    entry->luks_key_path = NULL;
    entry->luks_cmdline = NULL;
    entry->luks_preset = NULL;
    entry->decrypt_password = NULL;
    entry->next = NULL;
    entry->deployments = NULL;
    entry->deploy_count = 0;
    entry->deploy_default = 0;
    entry->deploy_sel = 0;

    efi_log(L"config: adding entry");
    efi_log(entry->name);

    if (icon_path) {
        entry->icon = gui_load_icon(icon_path);
        if (!entry->icon) efi_log(L"WARN: entry icon failed to load");
    }

    if (!config->entries) {
        config->entries = entry;
    } else {
        config->tail->next = entry;
    }
    config->tail = entry;

    config->entry_count++;
    return entry;
}

void config_free(config_t *config) {
    boot_entry_t *entry = config->entries;
    while (entry) {
        boot_entry_t *next = entry->next;
        if (entry->name) efi_free_pool(entry->name);
        if (entry->icon_path) efi_free_pool(entry->icon_path);
        if (entry->deployments) {
            for (UINTN i = 0; i < entry->deploy_count; i++) {
                deployment_t *d = &entry->deployments[i];
                if (d->version)  efi_free_pool(d->version);
                if (d->kernel)   efi_free_pool(d->kernel);
                if (d->initrd)   efi_free_pool(d->initrd);
                if (d->cmdline)  efi_free_pool(d->cmdline);
                if (d->bls_path) efi_free_pool(d->bls_path);
            }
            efi_free_pool(entry->deployments);
        } else {
            if (entry->kernel_path) efi_free_pool(entry->kernel_path);
            if (entry->initrd_path) efi_free_pool(entry->initrd_path);
            if (entry->cmdline) efi_free_pool(entry->cmdline);
        }
        if (entry->uuid) efi_free_pool(entry->uuid);
        if (entry->luks_key_path) efi_free_pool(entry->luks_key_path);
        if (entry->luks_cmdline) efi_free_pool(entry->luks_cmdline);
        if (entry->luks_preset) efi_free_pool(entry->luks_preset);
        if (entry->decrypt_password) {
            wipe16(entry->decrypt_password);
            efi_free_pool(entry->decrypt_password);
        }

        efi_free_pool(entry);
        entry = next;
    }
    config->entries = NULL;
    config->tail = NULL;
    config->entry_count = 0;

    if (config->background)    efi_free_pool(config->background);
    if (config->theme)         efi_free_pool(config->theme);
    if (config->title)         efi_free_pool(config->title);
    if (config->font)          efi_free_pool(config->font);
    if (config->def_cmdline)   efi_free_pool(config->def_cmdline);
    if (config->shutdown_icon) efi_free_pool(config->shutdown_icon);
    if (config->reboot_icon)   efi_free_pool(config->reboot_icon);
    if (config->firmware_icon) efi_free_pool(config->firmware_icon);
    config->background = NULL;
    config->theme = NULL;
    config->title = NULL;
    config->font = NULL;
    config->def_cmdline = NULL;
    config->shutdown_icon = NULL;
    config->reboot_icon = NULL;
    config->firmware_icon = NULL;
}
