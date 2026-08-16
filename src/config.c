#include "config.h"
#include "efi_helpers.h"
#include "accent.h"
#include "arch.h"
#include "path_compat.h"
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
        if (!in_quote && s[i] == '#' && (i == 0 || is_space16(s[i - 1]))) {
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

static int parse_spec(CHAR16 *value, accent_spec_t *sp) {
    if (!value || !value[0]) return 0;

    color_t c;
    if (parse_color(value, &c)) {
        sp->mode = SPEC_COLOR;
        sp->color = c;
        return 1;
    }
    int role = accent_role_from_str(value);
    if (role >= 0) {
        sp->mode = SPEC_ROLE;
        sp->role = role;
        return 1;
    }
    if (efi_strcmp(value, L"on") == 0 ||
        *value == '1' || *value == 't' || *value == 'y' ||
        *value == 'T' || *value == 'Y') {
        sp->mode = SPEC_ON;
        return 1;
    }
    if (efi_strcmp(value, L"off") == 0 ||
        *value == '0' || *value == 'f' || *value == 'n' ||
        *value == 'F' || *value == 'N') {
        sp->mode = SPEC_OFF;
        return 1;
    }
    return 0;
}

static int parse_spec_color(CHAR16 *value, accent_spec_t *sp, color_t *fallback) {
    if (!parse_spec(value, sp)) return 0;
    if (sp->mode == SPEC_COLOR && fallback) *fallback = sp->color;
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

static void set_char16(CHAR16 **slot, CHAR16 *val) {
    if (*slot) efi_free_pool(*slot);
    *slot = val;
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

static void free_snapshots(snapshot_t *snaps, UINTN count);

static void free_entry_contents(boot_entry_t *e) {
    if (!e) return;
    if (e->name)      efi_free_pool(e->name);
    if (e->icon_path) efi_free_pool(e->icon_path);

    if (e->deployments) {
        int cmdline_aliased = 0;
        for (UINTN i = 0; i < e->deploy_count; i++)
            if (e->cmdline == e->deployments[i].cmdline) cmdline_aliased = 1;
        if (!cmdline_aliased && e->cmdline) efi_free_pool(e->cmdline);
        free_deployments(e->deployments, e->deploy_count);
    } else {
        if (e->kernel_path) efi_free_pool(e->kernel_path);
        if (e->initrd_path) efi_free_pool(e->initrd_path);
        if (e->cmdline)     efi_free_pool(e->cmdline);
    }

    if (e->uuid)          efi_free_pool(e->uuid);
    if (e->snapshots)     free_snapshots(e->snapshots, e->snap_count);
    if (e->luks_key_path) efi_free_pool(e->luks_key_path);
    if (e->luks_cmdline)  efi_free_pool(e->luks_cmdline);
    if (e->luks_preset)   efi_free_pool(e->luks_preset);
    if (e->decrypt_password) {
        wipe16(e->decrypt_password);
        efi_free_pool(e->decrypt_password);
        e->decrypt_password = NULL;
    }
    if (e->icon) {
        if (e->icon->scaled) efi_free_pool(e->icon->scaled);
        if (e->icon->pixels) efi_free_pool(e->icon->pixels);
        efi_free_pool(e->icon);
        e->icon = NULL;
    }
}

static void free_snapshots(snapshot_t *snaps, UINTN count) {
    if (!snaps) return;
    for (UINTN i = 0; i < count; i++) {
        if (snaps[i].id)      efi_free_pool(snaps[i].id);
        if (snaps[i].date)    efi_free_pool(snaps[i].date);
        if (snaps[i].desc)    efi_free_pool(snaps[i].desc);
        if (snaps[i].kernel)  efi_free_pool(snaps[i].kernel);
        if (snaps[i].initrd)  efi_free_pool(snaps[i].initrd);
        if (snaps[i].cmdline) efi_free_pool(snaps[i].cmdline);
    }
    efi_free_pool(snaps);
}

static UINTN parse_uint(CHAR16 *s) {
    UINTN n = 0;
    while (*s >= '0' && *s <= '9') {
        if (n >= 100000000u) n = 100000000u;
        else n = n * 10 + (UINTN)(*s - '0');
        s++;
    }
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
    color_t color; int has_color = 0; int color_role = -1;
    UINT8 sha256_buf[32]; int has_sha256 = 0;
    UINTN entry_icon_size = 0;
    int encrypted = 0, kernel_encrypted_set = 0, initrd_encrypted_set = 0;
    int kernel_encrypted = 0, initrd_encrypted = 0;
    int luks = 0;
    int luks_confirm = 0;
    int luks_verbose = 0;
    CHAR16 *luks_key_path = NULL;
    CHAR16 *luks_cmdline = NULL;
    CHAR16 *luks_preset = NULL;

    int braced = 0;
    for (CHAR16 *h = lines[*idx]; *h; h++)
        if (*h == '{') { braced = 1; break; }

    while (*idx < count) {
        CHAR16 *line = trim(lines[*idx]);

        if (line[0] == '}') break;
        if (line[0] == '\0') {
            if (!braced) break;
            (*idx)++;
            continue;
        }
        if (line[0] == '#') {
            (*idx)++;
            continue;
        }

        CHAR16 *eq = efi_strchr(line, '=');
        if (eq) {
            *eq = '\0';
            CHAR16 *key = trim(line);
            strip_inline_comment(eq + 1);
            CHAR16 *value = trim(eq + 1);

            if (efi_strcmp(key, L"name") == 0) {
                set_char16(&name, efi_strdup(value));
            } else if (efi_strcmp(key, L"icon") == 0) {
                set_char16(&icon_path, dup_path(value));
            } else if (efi_strcmp(key, L"kernel") == 0) {
                set_char16(&kernel_path, dup_path(value));
            } else if (efi_strcmp(key, L"initrd") == 0) {
                set_char16(&initrd_path, dup_path(value));
            } else if (efi_strcmp(key, L"cmdline") == 0 || efi_strcmp(key, L"options") == 0) {
                set_char16(&cmdline, efi_strdup(value));
            } else if (efi_strcmp(key, L"uuid") == 0) {
                set_char16(&uuid, efi_strdup(value));
            } else if (efi_strcmp(key, L"color") == 0) {
                int role = accent_role_from_str(value);
                if (role >= 0) {
                    color_role = role;
                    has_color = 0;
                } else {
                    has_color = parse_color(value, &color);
                    if (has_color) color_role = -1;
                    else efi_log(L"WARN: invalid entry color= (an accent role or #RRGGBB)");
                }
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
            } else if (efi_strcmp(key, L"luks_confirm") == 0 ||
                       efi_strcmp(key, L"luks_verify") == 0) {
                luks_confirm = (*value == '1' || *value == 't' || *value == 'y');
            } else if (efi_strcmp(key, L"luks_verbose") == 0 ||
                       efi_strcmp(key, L"luks_show_prompt") == 0) {
                luks_verbose = (*value == '1' || *value == 't' || *value == 'y');
            } else if (efi_strcmp(key, L"luks_key_path") == 0) {
                set_char16(&luks_key_path, efi_strdup(value));
            } else if (efi_strcmp(key, L"luks_cmdline") == 0 ||
                       efi_strcmp(key, L"luks_options") == 0 ||
                       efi_strcmp(key, L"luks_options_append") == 0) {
                set_char16(&luks_cmdline, efi_strdup(value));
            } else if (efi_strcmp(key, L"luks_preset") == 0 ||
                       efi_strcmp(key, L"luks_initramfs") == 0) {
                set_char16(&luks_preset, efi_strdup(value));
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
            e->luks_confirm = luks_confirm;
            e->luks_verbose = luks_verbose;
            e->luks_key_path = luks_key_path;
            e->luks_cmdline = luks_cmdline;
            e->luks_preset = luks_preset;
            luks_key_path = NULL;
            luks_cmdline = NULL;
            luks_preset = NULL;
        }
        if (e && has_color) { e->color = color; e->has_color = 1; }
        if (e && color_role >= 0) e->color_role = color_role;
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
        { L"void",       L"void.png" },
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

static CHAR16* dir_prefix(CHAR16 *dir) {
    return (dir[0] == '\\' && dir[1] == '\0') ? L"" : dir;
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
        SPrint(path, sizeof(path), L"%s\\%s", dir_prefix(dir), name);

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


static int entry_takes_default_cmdline(CHAR16 *kernel_path, int type) {
    static const CHAR16 *loaders[] = {
        L"grubx64.efi", L"grubaa64.efi", L"shimx64.efi", L"shimaa64.efi",
        L"bootmgfw.efi", L"refind_x64.efi", L"refind_aa64.efi",
        L"systemd-bootx64.efi", L"systemd-bootaa64.efi", NULL
    };
    if (!kernel_path || type != 0) return 0;
    if (looks_windows(kernel_path)) return 0;
    if (contains_ci(kernel_path, L"\\EFI\\BOOT\\")) return 0;
    for (int i = 0; loaders[i]; i++)
        if (ends_with_ci(kernel_path, loaders[i])) return 0;
    return 1;
}


static int dc_foreign_volume;

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
        SPrint(cand, sizeof(cand), patterns[p], dir_prefix(dir), sbuf);
        if (efi_file_exists_root(root, cand)) return efi_strdup(cand);
    }
    return NULL;
}

static EFI_FILE_PROTOCOL *root_from_handle(EFI_HANDLE h);

static UINT8* dc_read_file(EFI_FILE_PROTOCOL *root, CHAR16 *path,
                           UINTN max, UINTN *out_len) {
    EFI_FILE_PROTOCOL *fh = NULL;
    if (EFI_ERROR(root->Open(root, &fh, path, EFI_FILE_MODE_READ, 0)) || !fh)
        return NULL;
    UINT8 *buf = efi_allocate_pool(max + 1);
    if (!buf) { fh->Close(fh); return NULL; }
    UINTN len = max;
    if (EFI_ERROR(fh->Read(fh, &len, buf))) len = 0;
    fh->Close(fh);
    if (!len) { efi_free_pool(buf); return NULL; }
    buf[len] = 0;
    *out_len = len;
    return buf;
}

static int dc_is_space(UINT8 c) { return c == ' ' || c == '\t' || c == '\r'; }

static UINTN dc_token(UINT8 **p, UINT8 *end, CHAR16 *out, UINTN cap) {
    while (*p < end && dc_is_space(**p)) (*p)++;
    UINTN n = 0;
    while (*p < end && **p && !dc_is_space(**p) && **p != '\n') {
        if (n + 1 < cap) out[n++] = (CHAR16)**p;
        (*p)++;
    }
    out[n] = 0;
    return n;
}

static int dc_prefix(CHAR16 *s, const CHAR16 *pre) {
    UINTN i = 0;
    while (pre[i]) { if (lc16(s[i]) != lc16((CHAR16)pre[i])) return 0; i++; }
    return 1;
}

static CHAR16* dc_uki_cmdline_file(EFI_FILE_PROTOCOL *root, CHAR16 *path) {
    EFI_FILE_PROTOCOL *fh = NULL;
    if (EFI_ERROR(root->Open(root, &fh, path, EFI_FILE_MODE_READ, 0)) || !fh)
        return NULL;
    UINT8 hdr[4096];
    UINTN len = sizeof(hdr);
    CHAR16 *out = NULL;
    if (EFI_ERROR(fh->Read(fh, &len, hdr)) || len < 0x40 ||
        hdr[0] != 'M' || hdr[1] != 'Z') {
        fh->Close(fh);
        return NULL;
    }
    UINT32 pe_off;
    CopyMem(&pe_off, hdr + 0x3C, sizeof(pe_off));
    if ((UINTN)pe_off + 24 > len ||
        hdr[pe_off] != 'P' || hdr[pe_off + 1] != 'E' ||
        hdr[pe_off + 2] != 0 || hdr[pe_off + 3] != 0) {
        fh->Close(fh);
        return NULL;
    }
    UINT16 nsec, optsz;
    CopyMem(&nsec,  hdr + pe_off + 6,  sizeof(nsec));
    CopyMem(&optsz, hdr + pe_off + 20, sizeof(optsz));
    UINTN sect = (UINTN)pe_off + 24 + optsz;
    for (UINT16 s = 0; s < nsec && !out; s++) {
        UINTN off = sect + (UINTN)s * 40;
        if (off + 40 > len) break;
        if (CompareMem(hdr + off, (void*)".cmdline", 8) != 0) continue;
        UINT32 vsz, rsz, roff;
        CopyMem(&vsz,  hdr + off + 8,  sizeof(vsz));
        CopyMem(&rsz,  hdr + off + 16, sizeof(rsz));
        CopyMem(&roff, hdr + off + 20, sizeof(roff));
        UINTN csz = (vsz && vsz < rsz) ? vsz : rsz;
        if (!csz || csz > 2048) break;
        UINT8 *cbuf = efi_allocate_pool(csz);
        if (!cbuf) break;
        if (!EFI_ERROR(fh->SetPosition(fh, roff))) {
            UINTN rl = csz;
            if (!EFI_ERROR(fh->Read(fh, &rl, cbuf)) && rl) {
                while (rl && (cbuf[rl - 1] == 0 || cbuf[rl - 1] == '\n' ||
                              cbuf[rl - 1] == '\r' || cbuf[rl - 1] == ' '))
                    rl--;
                if (rl) {
                    out = efi_allocate_pool((rl + 1) * sizeof(CHAR16));
                    if (out) {
                        for (UINTN i = 0; i < rl; i++)
                            out[i] = (cbuf[i] == '\n' || cbuf[i] == '\r' ||
                                      cbuf[i] == '\t') ? L' ' : (CHAR16)cbuf[i];
                        out[rl] = 0;
                    }
                }
            }
        }
        efi_free_pool(cbuf);
    }
    fh->Close(fh);
    return out;
}

static CHAR16* dc_from_uki(EFI_FILE_PROTOCOL *root) {
    EFI_FILE_PROTOCOL *d = efi_open_dir(root, L"\\EFI\\Linux");
    if (!d) return NULL;
    CHAR16 name[128];
    int is_dir;
    CHAR16 *out = NULL;
    while (!out && efi_read_dirent(d, name, 128, &is_dir)) {
        if (is_dir || !ends_with_ci(name, L".efi")) continue;
        CHAR16 path[MAX_PATH];
        SPrint(path, sizeof(path), L"\\EFI\\Linux\\%s", name);
        out = dc_uki_cmdline_file(root, path);
        if (out) {
            efi_log(L"config: cmdline taken from UKI .cmdline section");
            efi_log(path);
            efi_log(out);
        }
    }
    d->Close(d);
    return out;
}

static int dc_ascii_key_is(const UINT8 *s, UINTN n, const char *key) {
    UINTN i = 0;
    while (key[i] && i < n) {
        UINT8 c = s[i];
        if (c >= 'A' && c <= 'Z') c = (UINT8)(c + 32);
        if (c != (UINT8)key[i]) return 0;
        i++;
    }
    return key[i] == 0 && i == n;
}

static int dc_basename_eq(CHAR16 *path_value, CHAR16 *kernel_name) {
    if (!path_value || !kernel_name) return 0;
    UINTN len = 0;
    while (path_value[len]) len++;
    UINTN start = len;
    while (start > 0 && path_value[start - 1] != '/' && path_value[start - 1] != '\\')
        start--;
    return str_eq_ci(path_value + start, kernel_name);
}


static CHAR16* dc_from_loader_entries(EFI_FILE_PROTOCOL *root, CHAR16 *kernel_name) {
    static CHAR16 *dirs[] = { L"\\loader\\entries", L"\\boot\\loader\\entries", NULL };
    if (!kernel_name || !kernel_name[0]) return NULL;

    for (int di = 0; dirs[di]; di++) {
        EFI_FILE_PROTOCOL *d = efi_open_dir(root, dirs[di]);
        if (!d) continue;

        CHAR16 name[160];
        int is_dir;
        CHAR16 *out = NULL;
        while (!out && efi_read_dirent(d, name, 160, &is_dir)) {
            if (is_dir || !ends_with_ci(name, L".conf")) continue;

            CHAR16 path[MAX_PATH];
            SPrint(path, sizeof(path), L"%s\\%s", dirs[di], name);
            UINTN len = 0;
            UINT8 *data = dc_read_file(root, path, 16384, &len);
            if (!data) continue;

            int kernel_match = 0;
            CHAR16 opts[512];
            opts[0] = 0;

            UINTN i = 0;
            while (i < len) {
                UINTN ls = i;
                while (i < len && data[i] != '\n') i++;
                UINTN le = i;
                if (i < len) i++;
                while (le > ls && (data[le - 1] == '\r' || data[le - 1] == ' ' ||
                                   data[le - 1] == '\t')) le--;
                while (ls < le && (data[ls] == ' ' || data[ls] == '\t')) ls++;

                UINTN ke = ls;
                while (ke < le && data[ke] != ' ' && data[ke] != '\t') ke++;
                UINTN vs = ke;
                while (vs < le && (data[vs] == ' ' || data[vs] == '\t')) vs++;

                if (dc_ascii_key_is(data + ls, ke - ls, "linux")) {
                    CHAR16 val[256];
                    UINTN w = 0;
                    for (UINTN k = vs; k < le && w + 1 < 256; k++)
                        val[w++] = (CHAR16)data[k];
                    val[w] = 0;
                    if (dc_basename_eq(val, kernel_name)) kernel_match = 1;
                } else if (dc_ascii_key_is(data + ls, ke - ls, "options")) {
                    UINTN w = 0;
                    for (UINTN k = vs; k < le && w + 1 < 512; k++)
                        opts[w++] = (data[k] == '\t') ? L' ' : (CHAR16)data[k];
                    opts[w] = 0;
                }
            }
            efi_free_pool(data);

            if (kernel_match && opts[0]) {
                out = efi_strdup(opts);
                if (out) {
                    efi_log(L"config: cmdline taken from this kernel's loader entry");
                    efi_log(path);
                    efi_log(out);
                }
            }
        }
        d->Close(d);
        if (out) return out;
    }
    return NULL;
}

static CHAR16* dc_from_fstab(EFI_FILE_PROTOCOL *root) {
    static CHAR16 *paths[] = { L"\\etc\\fstab", L"\\@\\etc\\fstab", NULL };
    for (int pi = 0; paths[pi]; pi++) {
        UINTN len = 0;
        UINT8 *data = dc_read_file(root, paths[pi], 65536, &len);
        if (!data) continue;
        UINT8 *p = data, *end = data + len;
        CHAR16 *result = NULL;
        while (p < end && !result) {
            while (p < end && (dc_is_space(*p) || *p == '\n')) p++;
            if (p >= end) break;
            if (*p == '#') { while (p < end && *p != '\n') p++; continue; }
            CHAR16 spec[160], mnt[80], type[40], opts[256];
            dc_token(&p, end, spec, 160);
            dc_token(&p, end, mnt, 80);
            dc_token(&p, end, type, 40);
            dc_token(&p, end, opts, 256);
            while (p < end && *p != '\n') p++;
            if (!(mnt[0] == '/' && mnt[1] == 0)) continue;
            if (!(dc_prefix(spec, L"UUID=") || dc_prefix(spec, L"PARTUUID=") ||
                  dc_prefix(spec, L"LABEL=") || dc_prefix(spec, L"PARTLABEL=") ||
                  dc_prefix(spec, L"/dev/")))
                continue;

            CHAR16 flags[160];
            UINTN fn = 0;
            int ro = 0;
            for (UINTN i = 0; opts[i]; ) {
                CHAR16 o[128];
                UINTN on = 0;
                while (opts[i] && opts[i] != ',') {
                    if (on + 1 < 128) o[on++] = opts[i];
                    i++;
                }
                o[on] = 0;
                if (opts[i] == ',') i++;
                if (o[0] == 'r' && o[1] == 'o' && !o[2]) ro = 1;
                if (dc_prefix(o, L"subvol=") || dc_prefix(o, L"subvolid=")) {
                    if (fn && fn + 1 < 160) flags[fn++] = ',';
                    for (UINTN k = 0; o[k] && fn + 1 < 160; k++) flags[fn++] = o[k];
                }
            }
            flags[fn] = 0;
            CHAR16 out[512];
            if (fn)
                SPrint(out, sizeof(out), L"root=%s rootflags=%s %s",
                       spec, flags, ro ? L"ro" : L"rw");
            else
                SPrint(out, sizeof(out), L"root=%s %s", spec, ro ? L"ro" : L"rw");
            result = efi_strdup(out);
            if (result) {
                efi_log(L"config: cmdline derived from fstab");
                efi_log(paths[pi]);
                efi_log(result);
            }
        }
        efi_free_pool(data);
        if (result) return result;
    }
    return NULL;
}

static CHAR16* dc_from_gpt(EFI_HANDLE prefer_volume) {
#if defined(__aarch64__)
    static const EFI_GUID root_type =
        { 0xb921b045, 0x1df0, 0x41c3, { 0xaf,0x44,0x4c,0x6f,0x28,0x0d,0x3f,0xae } };
#else
    static const EFI_GUID root_type =
        { 0x4f68bce3, 0xe8cd, 0x4db1, { 0x96,0xe7,0xfb,0xca,0xf9,0x84,0xb7,0x09 } };
#endif
    UINTN nh = 0;
    EFI_HANDLE *hs = efi_locate_handle_buffer(&gEfiBlockIoProtocolGuid, &nh);
    if (!hs) return NULL;
    CHAR16 *out = NULL;
    int candidates = 0;
    for (UINTN pass = 0; pass < 2 && !out; pass++)
    for (UINTN h = 0; h < nh && !out; h++) {
        EFI_BLOCK_IO *bio = NULL;
        if (EFI_ERROR(BS->HandleProtocol(hs[h], &gEfiBlockIoProtocolGuid,
                                         (void**)&bio)) || !bio || !bio->Media)
            continue;
        if (bio->Media->LogicalPartition || !bio->Media->MediaPresent) continue;
        int same_disk = prefer_volume && efi_handles_same_disk(hs[h], prefer_volume);
        if (pass == 0 ? !same_disk : same_disk) continue;
        UINT32 bs = bio->Media->BlockSize;
        if (bs < 512 || bs > 4096) continue;
        UINTN align = bio->Media->IoAlign > 1 ? bio->Media->IoAlign : 1;

        UINT8 *raw = efi_allocate_pool(bs + align);
        if (!raw) continue;
        UINT8 *hdr = raw + ((align - ((UINTN)raw & (align - 1))) & (align - 1));
        if (EFI_ERROR(bio->ReadBlocks(bio, bio->Media->MediaId, 1, bs, hdr)) ||
            CompareMem(hdr, (void*)"EFI PART", 8) != 0) {
            efi_free_pool(raw);
            continue;
        }
        UINT64 elba;
        UINT32 num, esz;
        CopyMem(&elba, hdr + 72, sizeof(elba));
        CopyMem(&num,  hdr + 80, sizeof(num));
        CopyMem(&esz,  hdr + 84, sizeof(esz));
        efi_free_pool(raw);
        if (esz < 128 || esz > 4096 || !num) continue;
        if (num > 128) num = 128;
        UINTN rdsz = (((UINTN)num * esz + bs - 1) / bs) * bs;

        raw = efi_allocate_pool(rdsz + align);
        if (!raw) continue;
        UINT8 *ents = raw + ((align - ((UINTN)raw & (align - 1))) & (align - 1));
        if (EFI_ERROR(bio->ReadBlocks(bio, bio->Media->MediaId, elba, rdsz, ents))) {
            efi_free_pool(raw);
            continue;
        }
        for (UINT32 i = 0; i < num; i++) {
            UINT8 *e = ents + (UINTN)i * esz;
            if (CompareMem(e, (void*)&root_type, 16) != 0) continue;
            candidates++;
            if (out) continue;
            EFI_GUID g;
            CopyMem(&g, e + 16, sizeof(g));
            UINT8 b[16] = {
                (UINT8)(g.Data1 >> 24), (UINT8)(g.Data1 >> 16),
                (UINT8)(g.Data1 >> 8),  (UINT8)g.Data1,
                (UINT8)(g.Data2 >> 8),  (UINT8)g.Data2,
                (UINT8)(g.Data3 >> 8),  (UINT8)g.Data3,
                g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]
            };
            static const CHAR16 hex[] = L"0123456789abcdef";
            CHAR16 u[37];
            UINTN w = 0;
            for (UINTN k = 0; k < 16; k++) {
                if (k == 4 || k == 6 || k == 8 || k == 10) u[w++] = '-';
                u[w++] = hex[b[k] >> 4];
                u[w++] = hex[b[k] & 0xF];
            }
            u[w] = 0;
            CHAR16 cmd[80];
            SPrint(cmd, sizeof(cmd), L"root=PARTUUID=%s rw", u);
            out = efi_strdup(cmd);
            if (out) {
                efi_log(pass == 0
                    ? L"config: cmdline derived from a GPT root partition on the kernel's own disk"
                    : L"WARN: cmdline derived from a GPT root partition on another disk - verify root=");
                efi_log(out);
            }
        }
        efi_free_pool(raw);
    }
    efi_free_pool(hs);
    if (candidates > 1)
        efi_log(L"WARN: this disk has more than one Linux root partition - "
                L"set root= explicitly in boot.conf if the guess is wrong");
    return out;
}

static CHAR16* dc_derive_cmdline(EFI_FILE_PROTOCOL *root, EFI_HANDLE volume,
                                CHAR16 *kernel_name, int global_src) {
    CHAR16 *cmd = dc_from_loader_entries(root, kernel_name);
    if (!cmd) cmd = dc_from_uki(root);
    if (!cmd) cmd = dc_from_fstab(root);

    for (int pass = 0; pass < 2 && !cmd && global_src; pass++) {
        UINTN n = 0;
        EFI_HANDLE *hs = efi_locate_handle_buffer(
            &gEfiSimpleFileSystemProtocolGuid, &n);
        if (!hs) break;
        for (UINTN i = 0; i < n && !cmd; i++) {
            if (hs[i] == volume) continue;
            int same_disk = efi_handles_same_disk(hs[i], volume);
            if (pass == 0 ? !same_disk : same_disk) continue;

            EFI_FILE_PROTOCOL *r = root_from_handle(hs[i]);
            if (!r) continue;
            cmd = dc_from_fstab(r);
            r->Close(r);
            if (cmd)
                efi_log(pass == 0
                    ? L"config: cmdline derived from another volume on the same disk"
                    : L"WARN: cmdline derived from an fstab on a different disk - verify root=");
        }
        efi_free_pool(hs);
    }

    if (!cmd && global_src) cmd = dc_from_gpt(volume);
    return cmd;
}

struct hp_kdir {
    EFI_FILE_PROTOCOL *root;
    EFI_HANDLE volume;
    CHAR16 *dir;
    int global_cmdline;
    EFI_FILE_PROTOCOL *d;
    CHAR16 *auto_cmd;
    int auto_tried;
};

static int add_kernel_entry(config_t *config, EFI_FILE_PROTOCOL *root,
                            EFI_HANDLE volume, CHAR16 *dir, CHAR16 *name,
                            int global_cmdline, CHAR16 **auto_cmd,
                            int *auto_tried) {
    CHAR16 path[MAX_PATH];
    SPrint(path, sizeof(path), L"%s\\%s", dir_prefix(dir), name);
    CHAR16 *initrd = find_initrd(root, dir, name);

    efi_log(L"config: auto-detected raw kernel");
    efi_log(path);
    if (initrd) efi_log(initrd);

    CHAR16 *entry_cmd = dc_from_loader_entries(root, name);
    if (!entry_cmd) {
        if (!*auto_tried) {
            *auto_tried = 1;
            *auto_cmd = dc_derive_cmdline(root, volume, NULL, global_cmdline);
        }
        entry_cmd = *auto_cmd ? efi_strdup(*auto_cmd) : NULL;
    }

    CHAR16 *entry_name = efi_strdup(name);
    CHAR16 *entry_icon = distro_icon(name);
    CHAR16 *entry_path = efi_strdup(path);
    if (entry_name && entry_path &&
        config_add_entry(config, entry_name, entry_icon,
                         entry_path, initrd, entry_cmd, NULL, 0, 0, 0))
        return 1;

    free_char16(&entry_name);
    free_char16(&entry_icon);
    free_char16(&entry_path);
    free_char16(&initrd);
    free_char16(&entry_cmd);
    return 0;
}

static int scan_kernel_dir(config_t *config, EFI_FILE_PROTOCOL *root,
                           EFI_HANDLE volume, CHAR16 *dir, int global_cmdline) {
    EFI_FILE_PROTOCOL *d = efi_open_dir(root, dir);
    if (!d) return 0;

    int added = 0;
    CHAR16 name[128];
    int is_dir;
    CHAR16 *auto_cmd = NULL;
    int auto_tried = 0;
    while (efi_read_dirent(d, name, 128, &is_dir)) {
        if (is_dir) continue;
        if (!is_kernel_name(name)) continue;
        added += add_kernel_entry(config, root, volume, dir, name,
                                  global_cmdline, &auto_cmd, &auto_tried);
    }
    if (auto_cmd) efi_free_pool(auto_cmd);
    d->Close(d);
    return added;
}

static void hp_kdir_begin(EFI_FILE_PROTOCOL *root, CHAR16 *dir,
                          EFI_HANDLE volume, int global_cmdline,
                          struct hp_kdir *w) {
    w->root = root;
    w->volume = volume;
    w->dir = dir;
    w->global_cmdline = global_cmdline;
    w->d = efi_open_dir(root, dir);
    w->auto_cmd = NULL;
    w->auto_tried = 0;
}

#define HP_DIRENT_SLICE_US 20000ULL

static int hp_kdir_next(config_t *config, struct hp_kdir *w, CHAR16 **out) {
    *out = NULL;
    if (!w->d) return 0;
    CHAR16 name[128];
    int is_dir;
    UINT64 t0 = arch_now_us();
    for (;;) {
        if (efi_key_pending() || arch_now_us() - t0 >= HP_DIRENT_SLICE_US)
            return 1;
        if (!efi_read_dirent(w->d, name, 128, &is_dir)) break;
        if (is_dir) continue;
        if (!is_kernel_name(name)) continue;
        if (add_kernel_entry(config, w->root, w->volume, w->dir, name,
                             w->global_cmdline, &w->auto_cmd, &w->auto_tried))
            *out = efi_strdup(name);
        return 1;
    }
    w->d->Close(w->d);
    w->d = NULL;
    if (w->auto_cmd) efi_free_pool(w->auto_cmd);
    w->auto_cmd = NULL;
    return 0;
}

static EFI_FILE_PROTOCOL *root_from_handle(EFI_HANDLE h) {
    EFI_FILE_IO_INTERFACE *io = NULL;
    if (EFI_ERROR(BS->HandleProtocol(h, &gEfiSimpleFileSystemProtocolGuid, (void**)&io)) || !io)
        return NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (EFI_ERROR(io->OpenVolume(io, &root))) return NULL;
    return root;
}

static int equals_ci(CHAR16 *a, const CHAR16 *b) {
    UINTN i = 0;
    while (a[i] && b[i] && lc16(a[i]) == lc16((CHAR16)b[i])) i++;
    return a[i] == '\0' && b[i] == '\0';
}

static int scan_vendor_loaders(config_t *config, EFI_FILE_PROTOCOL *root) {
    static const CHAR16 *skip_dirs[] = {
        L"BOOT", L"Microsoft", L"visor", L"Linux", L"systemd",
        L"refind", L"tools", NULL
    };
    static const CHAR16 *loaders[] = { L"shimx64.efi", L"grubx64.efi", NULL };

    EFI_FILE_PROTOCOL *d = efi_open_dir(root, L"\\EFI");
    if (!d) return 0;

    int added = 0;
    CHAR16 name[128];
    int is_dir;
    while (efi_read_dirent(d, name, 128, &is_dir)) {
        if (!is_dir) continue;
        int skip = 0;
        for (int i = 0; skip_dirs[i]; i++)
            if (equals_ci(name, skip_dirs[i])) { skip = 1; break; }
        if (skip) continue;

        for (int i = 0; loaders[i]; i++) {
            CHAR16 path[MAX_PATH];
            SPrint(path, sizeof(path), L"\\EFI\\%s\\%s", name, loaders[i]);
            if (!efi_file_exists_root(root, path)) continue;

            efi_log(L"config: auto-detected distro loader");
            efi_log(path);

            CHAR16 *entry_name = efi_strdup(name);
            CHAR16 *entry_icon = distro_icon(name);
            CHAR16 *entry_path = efi_strdup(path);
            if (!entry_name || !entry_path) {
                free_char16(&entry_name);
                free_char16(&entry_icon);
                free_char16(&entry_path);
            } else if (config_add_entry(config, entry_name, entry_icon,
                                 entry_path, NULL, NULL, NULL, 0, 0, 0)) {
                added++;
            } else {
                free_char16(&entry_name);
                free_char16(&entry_icon);
                free_char16(&entry_path);
            }
            break;
        }
    }
    d->Close(d);
    return added;
}

static int scope_wants_volume(int quick, EFI_HANDLE vol);

#define MAX_BLS 48

typedef struct {
    CHAR16 *title, *version, *kernel, *initrd, *options, *machine, *conf;
    int tries_left, tries_done, ot_idx;
    EFI_HANDLE volume;
} bls_rec_t;

static CHAR16* read_text_from_root(EFI_FILE_PROTOCOL *root, CHAR16 *path) {
    EFI_FILE_PROTOCOL *fh = NULL;
    if (EFI_ERROR(root->Open(root, &fh, path, EFI_FILE_MODE_READ, 0)) || !fh) return NULL;
    UINT64 sz = efi_file_size(fh);
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
    if (a->volume != b->volume) return 0;
    if (a->machine && b->machine) return efi_strcmp(a->machine, b->machine) == 0;
    CHAR16 *ba = base_title_dup(a->title), *bb = base_title_dup(b->title);
    int eq = (ba && bb && efi_strcmp(ba, bb) == 0);
    if (ba) efi_free_pool(ba);
    if (bb) efi_free_pool(bb);
    return eq;
}

static void bls_scan(EFI_FILE_PROTOCOL *root, EFI_HANDLE volume, CHAR16 *dir,
                     bls_rec_t *recs, int *nrec) {
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

        bls_rec_t r = {
            NULL, NULL, NULL, NULL, NULL, NULL, NULL, -1, 0, -1, volume
        };
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

static int bls_detect(config_t *config, int quick) {
    bls_rec_t recs[MAX_BLS];
    int nrec = 0;

    UINTN nvol = 0;
    EFI_HANDLE *vols = efi_locate_handle_buffer(&gEfiSimpleFileSystemProtocolGuid, &nvol);
    if (vols) {
        for (UINTN v = 0; v < nvol && nrec < MAX_BLS; v++) {
            if (!scope_wants_volume(quick, vols[v])) continue;
            EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
            if (!root) continue;
            bls_scan(root, vols[v], L"\\loader\\entries", recs, &nrec);
            bls_scan(root, vols[v], L"\\boot\\loader\\entries", recs, &nrec);
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
            e->hp_volume      = recs[idx[0]].volume;
            e->uuid           = efi_handle_partition_uuid(recs[idx[0]].volume);
            CHAR16 m[128];
            SPrint(m, sizeof(m), L"bls: attached %d deployment(s) to %s",
                   n, e->name);
            efi_log(m);
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

    if ((UINTN)plus + 32 >= MAX_PATH) {
        efi_log(L"WARN: bls path too long to update boot-counter");
        return;
    }
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

static void tag_entries_since(config_t *config, UINTN first,
                              EFI_HANDLE volume) {
    if (!config || !volume || first >= config->entry_count) return;

    CHAR16 *uuid = efi_handle_partition_uuid(volume);
    UINTN idx = 0;
    for (boot_entry_t *e = config->entries; e; e = e->next, idx++) {
        if (idx < first) continue;
        e->hp_volume = volume;
        if (uuid && !e->uuid) e->uuid = efi_strdup(uuid);
    }
    if (uuid) efi_free_pool(uuid);
}

static const EFI_GUID scope_type_esp =
    { 0xc12a7328, 0xf81f, 0x11d2, { 0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b } };
static const EFI_GUID scope_type_xbootldr =
    { 0xbc13c2ff, 0x59e6, 0x4262, { 0xa3,0x52,0xb2,0x75,0xfd,0x6f,0x71,0x72 } };

#define SCOPE_MAX_PARTS 64
static UINT8 scope_parts[SCOPE_MAX_PARTS][16];
static UINTN scope_part_n;
static int   scope_gpt_read;

static void scope_read_gpt(void) {
    if (scope_gpt_read) return;
    scope_gpt_read = 1;

    UINTN nh = 0;
    EFI_HANDLE *hs = efi_locate_handle_buffer(&gEfiBlockIoProtocolGuid, &nh);
    if (!hs) return;

    for (UINTN h = 0; h < nh && scope_part_n < SCOPE_MAX_PARTS; h++) {
        EFI_BLOCK_IO *bio = NULL;
        if (EFI_ERROR(BS->HandleProtocol(hs[h], &gEfiBlockIoProtocolGuid,
                                         (void**)&bio)) || !bio || !bio->Media)
            continue;
        if (bio->Media->LogicalPartition || !bio->Media->MediaPresent) continue;
        UINT32 bs = bio->Media->BlockSize;
        if (bs < 512 || bs > 4096) continue;
        UINTN align = bio->Media->IoAlign > 1 ? bio->Media->IoAlign : 1;

        UINT8 *raw = efi_allocate_pool(bs + align);
        if (!raw) continue;
        UINT8 *hdr = raw + ((align - ((UINTN)raw & (align - 1))) & (align - 1));
        if (EFI_ERROR(bio->ReadBlocks(bio, bio->Media->MediaId, 1, bs, hdr)) ||
            CompareMem(hdr, (void*)"EFI PART", 8) != 0) {
            efi_free_pool(raw);
            continue;
        }
        UINT64 elba;
        UINT32 num, esz;
        CopyMem(&elba, hdr + 72, sizeof(elba));
        CopyMem(&num,  hdr + 80, sizeof(num));
        CopyMem(&esz,  hdr + 84, sizeof(esz));
        efi_free_pool(raw);
        if (esz < 128 || esz > 4096 || !num) continue;
        if (num > 128) num = 128;
        UINTN rdsz = (((UINTN)num * esz + bs - 1) / bs) * bs;

        raw = efi_allocate_pool(rdsz + align);
        if (!raw) continue;
        UINT8 *ents = raw + ((align - ((UINTN)raw & (align - 1))) & (align - 1));
        if (EFI_ERROR(bio->ReadBlocks(bio, bio->Media->MediaId, elba, rdsz, ents))) {
            efi_free_pool(raw);
            continue;
        }
        for (UINT32 i = 0; i < num && scope_part_n < SCOPE_MAX_PARTS; i++) {
            UINT8 *e = ents + (UINTN)i * esz;
            if (CompareMem(e, &scope_type_esp, 16) != 0 &&
                CompareMem(e, &scope_type_xbootldr, 16) != 0)
                continue;
            CopyMem(scope_parts[scope_part_n++], e + 16, 16);
        }
        efi_free_pool(raw);
    }
    efi_free_pool(hs);

    { CHAR16 d[64]; SPrint(d, sizeof(d), L"config: %d boot partition(s) in GPT",
                           (int)scope_part_n); efi_log(d); }
}

static int scope_part_guid(EFI_HANDLE h, UINT8 out[16]) {
    EFI_DEVICE_PATH *dp = NULL;
    if (EFI_ERROR(BS->HandleProtocol(h, &gEfiDevicePathProtocolGuid, (void**)&dp)) || !dp)
        return 0;
    for (EFI_DEVICE_PATH *n = dp; !IsDevicePathEnd(n);
         n = (EFI_DEVICE_PATH*)((UINT8*)n + DevicePathNodeLength(n))) {
        if (DevicePathType(n) != MEDIA_DEVICE_PATH ||
            DevicePathSubType(n) != MEDIA_HARDDRIVE_DP)
            continue;
        HARDDRIVE_DEVICE_PATH *hd = (HARDDRIVE_DEVICE_PATH*)n;
        if (hd->SignatureType != SIGNATURE_TYPE_GUID) continue;
        CopyMem(out, hd->Signature, 16);
        return 1;
    }
    return 0;
}

static int scope_wants_volume(int quick, EFI_HANDLE vol) {
    if (!quick) return 1;
    if (vol == efi_boot_volume_handle()) return 1;

    scope_read_gpt();

    UINT8 g[16];
    if (scope_part_n && scope_part_guid(vol, g)) {
        for (UINTN i = 0; i < scope_part_n; i++)
            if (CompareMem(scope_parts[i], g, 16) == 0) return 1;
        return 0;
    }

    EFI_FILE_PROTOCOL *root = root_from_handle(vol);
    if (!root) return 0;
    int ok = efi_file_exists_root(root, L"\\EFI") ||
             efi_file_exists_root(root, L"\\loader\\entries");
    root->Close(root);
    return ok;
}

static EFI_HANDLE dc_scanned_vols[64];
static UINTN      dc_scanned_n;

static void dc_mark_scanned(EFI_HANDLE vol) {
    if (!vol) return;
    for (UINTN i = 0; i < dc_scanned_n; i++)
        if (dc_scanned_vols[i] == vol) return;
    if (dc_scanned_n < 64) dc_scanned_vols[dc_scanned_n++] = vol;
}

static int detect_scan_volumes(config_t *config, int bls, int quick) {
    static CHAR16 *windows_paths[] = {
        L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
        L"\\EFI\\BOOT\\bootmgfw.efi",
        NULL
    };

    UINTN start_count = config->entry_count;

    UINTN nvol = 0;
    EFI_HANDLE *vols = efi_locate_handle_buffer(&gEfiSimpleFileSystemProtocolGuid, &nvol);
    if (!vols || nvol == 0) {
        if (vols) efi_free_pool(vols);
        return 0;
    }

    int windows_found = 0;
    int uki_found = 0;

    for (UINTN v = 0; v < nvol; v++) {
        if (!scope_wants_volume(quick, vols[v])) continue;
        EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
        if (!root) continue;

        UINTN volume_start = config->entry_count;
        dc_mark_scanned(vols[v]);
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
        tag_entries_since(config, volume_start, vols[v]);

        root->Close(root);
    }

    int raw_found = 0;
    if (!bls) {
        for (UINTN v = 0; v < nvol; v++) {
            if (!scope_wants_volume(quick, vols[v])) continue;
            EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
            if (!root) continue;
            UINTN volume_start = config->entry_count;
            raw_found += scan_kernel_dir(config, root, vols[v], L"\\boot", 1);
            raw_found += scan_kernel_dir(config, root, vols[v], L"\\@\\boot", 1);
            raw_found += scan_kernel_dir(config, root, vols[v], L"\\", 1);
            tag_entries_since(config, volume_start, vols[v]);
            root->Close(root);
        }
        { CHAR16 d[64]; SPrint(d, sizeof(d), L"config: raw kernel scan found %d", raw_found); efi_log(d); }
    }
    if (!uki_found && !bls && !raw_found) {
        for (UINTN v = 0; v < nvol; v++) {
            if (!scope_wants_volume(quick, vols[v])) continue;
            EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
            if (!root) continue;
            UINTN volume_start = config->entry_count;
            scan_vendor_loaders(config, root);
            tag_entries_since(config, volume_start, vols[v]);
            root->Close(root);
        }
    }

    efi_free_pool(vols);
    return (int)(config->entry_count - start_count);
}

static int show_names_set;
static int center_info_set;

static EFI_STATUS detect_entries(config_t *config) {
    if (!show_names_set)  config->show_names = 0;
    if (!center_info_set) config->center_info = 1;

    if (efi_fs_drivers_pending()) {
        efi_log(L"config: starting FS drivers before auto-detection");
        efi_start_deferred_drivers();
    }

    int quick = (config->scan_mode == SCAN_MODE_QUICK);
    if (quick) efi_log(L"config: quick scan - ESP and /boot partitions only");

    int bls = bls_detect(config, quick);
    int found = detect_scan_volumes(config, bls, quick);

    if (quick && !found && !bls) {
        efi_log(L"config: quick scan found nothing - widening to a deep scan");
        bls = bls_detect(config, 0);
        found = detect_scan_volumes(config, bls, 0);
    }

    if (!found && !bls) return EFI_NOT_FOUND;
    return EFI_SUCCESS;
}

static EFI_HANDLE *hp_fs_known;  static UINTN hp_fs_n;
static int hp_pending_probe;
static UINTN hp_probe_cursor;
static int hp_probe_round;
static EFI_HANDLE *hp_blk_known; static UINTN hp_blk_n;

#define HP_TRIED_MAX 64
static EFI_HANDLE hp_blk_tried[HP_TRIED_MAX];
static UINTN      hp_blk_tried_n;

static int hp_in_set(EFI_HANDLE *set, UINTN n, EFI_HANDLE h) {
    for (UINTN i = 0; i < n; i++)
        if (set[i] == h) return 1;
    return 0;
}


static int hp_volume_hosts_entry(config_t *config, EFI_HANDLE vol) {
    EFI_FILE_PROTOCOL *root = root_from_handle(vol);
    if (!root) return 0;

    int hosts = 0;
    for (boot_entry_t *e = config->entries; e && !hosts; e = e->next) {
        if (!e->kernel_path || !e->kernel_path[0]) continue;
        if (efi_file_exists_root(root, e->kernel_path)) {
            hosts = 1;
            break;
        }
        CHAR16 *alt = visor_path_without_boot_mount(e->kernel_path);
        if (alt && efi_file_exists_root(root, alt)) hosts = 1;
    }
    root->Close(root);
    return hosts;
}

void config_hotplug_arm(config_t *config) {
    if (hp_fs_known)  efi_free_pool(hp_fs_known);
    if (hp_blk_known) efi_free_pool(hp_blk_known);
    hp_fs_n = hp_blk_n = 0;

    hp_blk_known = efi_locate_handle_buffer(
        &gEfiBlockIoProtocolGuid, &hp_blk_n);
    if (!hp_blk_known) hp_blk_n = 0;

    UINTN nf = 0;
    EFI_HANDLE *fs = efi_locate_handle_buffer(
        &gEfiSimpleFileSystemProtocolGuid, &nf);


    hp_fs_known = efi_allocate_pool((nf ? nf : 1) * sizeof(EFI_HANDLE));
    if (hp_fs_known) {
        EFI_HANDLE boot_volume = efi_boot_volume_handle();
        for (UINTN i = 0; i < nf; i++) {
            int seen = (fs[i] == boot_volume);
            for (UINTN k = 0; !seen && k < dc_scanned_n; k++)
                if (dc_scanned_vols[k] == fs[i]) seen = 1;
            if (seen) hp_fs_known[hp_fs_n++] = fs[i];
        }
    }


    int sweep;
    if (config->scan_existing >= 0)
        sweep = config->scan_existing;
    else if (config->scan_mode == SCAN_MODE_QUICK)
        sweep = 0;
    else
        sweep = !config->entries_from_config;
    if (!sweep && fs && hp_fs_known) {
        efi_free_pool(hp_fs_known);
        hp_fs_known = fs;
        hp_fs_n = nf;
        fs = NULL;
    }
    if (fs) efi_free_pool(fs);

    hp_pending_probe = sweep && !efi_fs_probe_exhausted();
    hp_probe_cursor = 0;
    hp_probe_round = 0;

    { CHAR16 d[128];
      SPrint(d, sizeof(d),
             L"hotplug: armed, %d of %d volume(s) known, existing-media sweep %s",
             (int)hp_fs_n, (int)nf, sweep ? L"on" : L"off");
      efi_log(d);
      if (sweep && !hp_pending_probe)
          efi_log(L"hotplug: block devices already probed at startup - "
                  L"skipping the re-probe"); }
}

static EFI_GUID hp_fs_info_guid = { 0x09576e93, 0x6d3f, 0x11d2,
    { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };

static CHAR16* hp_volume_label(EFI_FILE_PROTOCOL *root) {
    UINT8 buf[SIZE_OF_EFI_FILE_SYSTEM_INFO + 64 * sizeof(CHAR16)]
        __attribute__((aligned(8)));
    UINTN size = sizeof(buf);
    EFI_FILE_SYSTEM_INFO *info = (EFI_FILE_SYSTEM_INFO*)buf;
    if (EFI_ERROR(root->GetInfo(root, &hp_fs_info_guid, &size, info)))
        return NULL;
    if (size < SIZE_OF_EFI_FILE_SYSTEM_INFO + sizeof(CHAR16) ||
        !info->VolumeLabel[0])
        return NULL;
    return efi_strdup(info->VolumeLabel);
}

enum {
    HP_STAGE_WINDOWS = 0,
    HP_STAGE_UKI,
    HP_STAGE_BOOT,
    HP_STAGE_BTRFS_BOOT,
    HP_STAGE_ROOT,
    HP_STAGE_VENDOR,
    HP_STAGE_FALLBACK,
    HP_STAGE_DONE
};

static EFI_HANDLE hp_cur_vol;
static EFI_FILE_PROTOCOL *hp_cur_root;
static int   hp_cur_stage;
static UINTN hp_cur_before;
static int   hp_cur_uki;
static int   hp_cur_raw;
static struct hp_kdir hp_cur_walk;
static int   hp_cur_walking;

static void hp_scan_finish(config_t *config) {
    if (hp_cur_walk.d) { hp_cur_walk.d->Close(hp_cur_walk.d); hp_cur_walk.d = NULL; }
    if (hp_cur_walk.auto_cmd) {
        efi_free_pool(hp_cur_walk.auto_cmd);
        hp_cur_walk.auto_cmd = NULL;
    }
    if (hp_cur_root) { hp_cur_root->Close(hp_cur_root); hp_cur_root = NULL; }
    dc_foreign_volume = 0;

    if (hp_cur_vol) {
        int total = (int)(config->entry_count - hp_cur_before);
        if (total > 0) {
            CHAR16 d[64];
            SPrint(d, sizeof(d), L"hotplug: added %d boot entr%s", total,
                   total == 1 ? L"y" : L"ies");
            efi_log(d);
        }
    }
    hp_cur_vol = NULL;
    hp_cur_walking = 0;
    hp_cur_stage = HP_STAGE_DONE;
}

static CHAR16* hp_stage_dir(int stage) {
    switch (stage) {
    case HP_STAGE_BOOT:       return L"\\boot";
    case HP_STAGE_BTRFS_BOOT: return L"\\@\\boot";
    default:                  return L"\\";
    }
}

static int hp_scan_step(config_t *config, int *done) {
    *done = 0;
    EFI_HANDLE vol = hp_cur_vol;
    EFI_FILE_PROTOCOL *root = hp_cur_root;
    if (!root) { hp_scan_finish(config); *done = 1; return 0; }

    UINTN before = config->entry_count;
    dc_foreign_volume = 1;

    switch (hp_cur_stage) {
    case HP_STAGE_WINDOWS: {
        static CHAR16 *win_paths[] = {
            L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
            L"\\EFI\\BOOT\\bootmgfw.efi",
            NULL
        };
        for (int i = 0; win_paths[i]; i++) {
            if (!efi_file_exists_root(root, win_paths[i])) continue;
            CHAR16 *entry_name = efi_strdup(L"Windows Boot Manager");
            CHAR16 *entry_icon = icon_path_for(L"windows.png");
            CHAR16 *entry_path = efi_strdup(win_paths[i]);
            if (!entry_name || !entry_path ||
                !config_add_entry(config, entry_name, entry_icon,
                                  entry_path, NULL, NULL, NULL, 1, 0, 0)) {
                free_char16(&entry_name);
                free_char16(&entry_icon);
                free_char16(&entry_path);
            }
            break;
        }
        hp_cur_stage++;
        break;
    }
    case HP_STAGE_UKI:
        hp_cur_uki += scan_uki_dir(config, root, L"\\EFI\\Linux");
        hp_cur_stage++;
        break;
    case HP_STAGE_BOOT:
    case HP_STAGE_BTRFS_BOOT:
    case HP_STAGE_ROOT: {
        if (!hp_cur_walking) {
            hp_kdir_begin(root, hp_stage_dir(hp_cur_stage), vol, 0, &hp_cur_walk);
            hp_cur_walking = 1;
        }
        CHAR16 *added_name = NULL;
        if (!hp_kdir_next(config, &hp_cur_walk, &added_name)) {
            hp_cur_walking = 0;
            hp_cur_stage++;
        }
        if (added_name) { hp_cur_raw++; efi_free_pool(added_name); }
        break;
    }
    case HP_STAGE_VENDOR:
        if (!hp_cur_uki && !hp_cur_raw && config->entry_count == hp_cur_before)
            scan_vendor_loaders(config, root);
        hp_cur_stage++;
        break;
    case HP_STAGE_FALLBACK:
        if (config->entry_count == hp_cur_before) {
#if defined(__aarch64__)
            CHAR16 *fallback = L"\\EFI\\BOOT\\BOOTAA64.EFI";
#else
            CHAR16 *fallback = L"\\EFI\\BOOT\\BOOTX64.EFI";
#endif
            if (efi_file_exists_root(root, fallback)) {
                CHAR16 *label = hp_volume_label(root);
                CHAR16 *entry_name = label ? label : efi_strdup(L"USB Device");
                CHAR16 *entry_icon = icon_path_for(L"unknown.png");
                CHAR16 *entry_path = efi_strdup(fallback);
                if (!entry_name || !entry_path ||
                    !config_add_entry(config, entry_name, entry_icon,
                                      entry_path, NULL, NULL, NULL, 1, 0, 0)) {
                    free_char16(&entry_name);
                    free_char16(&entry_icon);
                    free_char16(&entry_path);
                }
            }
        }
        hp_cur_stage++;
        break;
    default:
        hp_cur_stage = HP_STAGE_DONE;
        break;
    }

    dc_foreign_volume = 0;

    int added = (int)(config->entry_count - before);
    if (added > 0) tag_entries_since(config, before, vol);

    if (hp_cur_stage >= HP_STAGE_DONE) {
        hp_scan_finish(config);
        *done = 1;
    }
    return added;
}

static int hp_scan_begin(config_t *config, EFI_HANDLE vol) {
    for (boot_entry_t *e = config->entries; e; e = e->next)
        if (e->hp_volume == vol ||
            (e->uuid && e->uuid[0] &&
             efi_handle_matches_partition_uuid(vol, e->uuid)))
            return 0;

    EFI_FILE_PROTOCOL *root = root_from_handle(vol);
    if (!root) return 0;

    hp_cur_vol = vol;
    hp_cur_root = root;
    hp_cur_stage = HP_STAGE_WINDOWS;
    hp_cur_before = config->entry_count;
    hp_cur_uki = 0;
    hp_cur_raw = 0;
    hp_cur_walking = 0;
    hp_cur_walk.d = NULL;
    hp_cur_walk.auto_cmd = NULL;
    return 1;
}

static void hp_free_entry(boot_entry_t *e) {
    free_entry_contents(e);
    efi_free_pool(e);
}

#define HP_MAX_PROBES_PER_POLL 1

#define HP_SLICE_BUDGET_US 40000ULL

static int hp_vol_present(EFI_HANDLE *fs, UINTN nf, EFI_HANDLE h) {
    if (!hp_in_set(fs, nf, h)) return 0;
    EFI_BLOCK_IO *bio = NULL;
    if (!EFI_ERROR(BS->HandleProtocol(h, &gEfiBlockIoProtocolGuid,
                                      (void**)&bio)) &&
        bio && bio->Media && !bio->Media->MediaPresent)
        return 0;
    return 1;
}

static int hp_vol_exists(EFI_HANDLE *fs, UINTN nf, EFI_HANDLE h) {
    return hp_in_set(fs, nf, h);
}

int config_hotplug_poll(config_t *config, UINTN *first_new) {
    UINT64 t_poll = arch_now_us();
    UINT64 us_probe = 0, us_blk = 0, us_fsloc = 0;
    UINT64 us_removal = 0, us_discover = 0, us_scan = 0;

    int probed_now = 0;
    UINT64 t_phase = arch_now_us();
    if (hp_pending_probe) {
        UINTN probed = 0;
        while (hp_probe_cursor < hp_blk_n && probed < HP_MAX_PROBES_PER_POLL) {
            EFI_HANDLE h = hp_blk_known[hp_probe_cursor++];
            if (efi_handle_has_filesystem(h)) continue;
            UINT64 t0 = arch_now_us();
            BS->ConnectController(h, NULL, NULL, FALSE);
            UINT64 dt = arch_now_us() - t0;
            probed++;
            if (dt > 100000) {
                CHAR16 d[112];
                SPrint(d, sizeof(d),
                       L"hotplug: probing block device %d took %d ms",
                       (int)(hp_probe_cursor - 1), (int)(dt / 1000));
                efi_log(d);
            }
        }
        hp_probe_round += (int)probed;
        if (hp_probe_cursor >= hp_blk_n) {
            hp_pending_probe = 0;
            if (hp_probe_round) {
                CHAR16 d[96];
                SPrint(d, sizeof(d),
                       L"hotplug: probed %d unreadable block device(s)",
                       hp_probe_round);
                efi_log(d);
            }
        }
        probed_now = (probed > 0);
    }
    us_probe = arch_now_us() - t_phase;

    t_phase = arch_now_us();
    if (!probed_now) {
        UINTN nb = 0;
        EFI_HANDLE *blk = efi_locate_handle_buffer(&gEfiBlockIoProtocolGuid, &nb);
        if (blk) {
            int fresh = 0;
            for (UINTN i = 0; i < nb; i++)
                if (!hp_in_set(hp_blk_known, hp_blk_n, blk[i])) {
                    if (hp_in_set(hp_blk_tried, hp_blk_tried_n, blk[i])) continue;
                    if (hp_blk_tried_n < HP_TRIED_MAX)
                        hp_blk_tried[hp_blk_tried_n++] = blk[i];
                    UINT64 t0 = arch_now_us();
                    BS->ConnectController(blk[i], NULL, NULL, FALSE);
                    UINT64 dt = arch_now_us() - t0;
                    if (dt > 100000) {
                        CHAR16 d[112];
                        SPrint(d, sizeof(d),
                               L"hotplug: connecting a new block device took %d ms",
                               (int)(dt / 1000));
                        efi_log(d);
                    }
                    fresh = 1;
                }
            if (fresh) {
                if (hp_blk_known) efi_free_pool(hp_blk_known);
                hp_blk_known = blk;
                hp_blk_n = nb;
                hp_probe_cursor = 0;
            } else {
                efi_free_pool(blk);
            }
        }
    }
    us_blk = arch_now_us() - t_phase;

    t_phase = arch_now_us();
    UINTN nf = 0;
    EFI_HANDLE *fs = efi_locate_handle_buffer(&gEfiSimpleFileSystemProtocolGuid, &nf);
    us_fsloc = arch_now_us() - t_phase;
    if (!fs) return 0;

    int mask = 0;

    t_phase = arch_now_us();
    UINTN removed_at = 0, idx = 0;
    int removed = 0;
    boot_entry_t **link = &config->entries;
    while (*link) {
        boot_entry_t *e = *link;
        if (e->hp_volume && !hp_vol_present(fs, nf, e->hp_volume)) {
            if (!removed) removed_at = idx;
            *link = e->next;
            efi_log(L"hotplug: volume removed - dropping boot entry");
            efi_log(e->name);
            hp_free_entry(e);
            config->entry_count--;
            removed++;
            continue;
        }
        link = &e->next;
        idx++;
    }
    if (removed) {
        boot_entry_t *t = config->entries;
        while (t && t->next) t = t->next;
        config->tail = t;
        UINTN i2 = 0;
        for (boot_entry_t *e = config->entries; e; e = e->next) e->index = i2++;
        mask |= 2;
    }

    if (hp_cur_vol && !hp_vol_present(fs, nf, hp_cur_vol)) {
        efi_log(L"hotplug: volume vanished mid-scan - abandoning it");
        hp_scan_finish(config);
    }
    us_removal = arch_now_us() - t_phase;

    int added = 0;
    UINTN before = config->entry_count;
    UINTN scanned_idx = nf;
    int discovery_ran = 0;
    if (!probed_now) {
        t_phase = arch_now_us();
        if (!hp_cur_vol) {
            discovery_ran = 1;
            for (UINTN i = 0; i < nf; i++) {
                if (hp_in_set(hp_fs_known, hp_fs_n, fs[i])) continue;
                if (hp_volume_hosts_entry(config, fs[i])) {
                    efi_log(L"hotplug: volume already backs a configured entry - not scanning");
                } else {
                    efi_log(L"hotplug: scanning a volume Visor has not seen yet");
                    hp_scan_begin(config, fs[i]);
                }
                scanned_idx = i;
                break;
            }
        }
        us_discover = arch_now_us() - t_phase;

        if (hp_cur_vol) {
            UINT64 t0 = arch_now_us();
            int done = 0;
            while (hp_cur_vol && !done) {
                added += hp_scan_step(config, &done);
                if (efi_key_pending()) {
                    efi_log(L"hotplug: key pressed - pausing the scan");
                    break;
                }
                if (arch_now_us() - t0 >= HP_SLICE_BUDGET_US) break;
            }
            us_scan = arch_now_us() - t0;
        }
    }


    if (scanned_idx < nf) {
        EFI_HANDLE *grown = efi_allocate_pool((hp_fs_n + 1) * sizeof(EFI_HANDLE));
        if (grown) {
            UINTN k = 0;
            for (UINTN i = 0; i < hp_fs_n; i++)
                if (hp_vol_exists(fs, nf, hp_fs_known[i]))
                    grown[k++] = hp_fs_known[i];
            grown[k++] = fs[scanned_idx];
            if (hp_fs_known) efi_free_pool(hp_fs_known);
            hp_fs_known = grown;
            hp_fs_n = k;
        }
        efi_free_pool(fs);
    } else if (discovery_ran) {
        if (hp_fs_known) efi_free_pool(hp_fs_known);
        hp_fs_known = fs;
        hp_fs_n = nf;
    } else {
        efi_free_pool(fs);
    }

    if (added > 0) mask |= 1;
    if (first_new) *first_new = (mask & 1) ? before : removed_at;

    UINT64 us_total = arch_now_us() - t_poll;
    if (us_total > 150000) {
        CHAR16 d[224];
        SPrint(d, sizeof(d),
               L"hotplug: SLOW poll %d ms (probe %d, blk %d, fs-locate %d, "
               L"removal %d, discover %d, scan %d)",
               (int)(us_total / 1000), (int)(us_probe / 1000),
               (int)(us_blk / 1000), (int)(us_fsloc / 1000),
               (int)(us_removal / 1000), (int)(us_discover / 1000),
               (int)(us_scan / 1000));
        efi_log(d);
    }
    return mask;
}

#define CONFIG_TEXT_MAX (1024ULL * 1024ULL)

static CHAR16* read_text_file(CHAR16 *path) {
    efi_file_t *file = efi_fopen(path);
    if (!file) return NULL;

    UINT64 size = efi_file_size(file->handle);

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


int config_early_file_log_enabled(void) {
    int previous = efi_log_file_enabled();
    efi_log_set_file(0);
    CHAR16 *buf = read_text_file(CONFIG_FILE);
    efi_log_set_file(previous);
    if (!buf) return 1;

    int enabled = 1;
    int in_entry = 0;
    CHAR16 *start = buf;
    while (*start) {
        CHAR16 *end = start;
        while (*end && *end != '\n') end++;
        int had_nl = (*end == '\n');
        if (had_nl) *end = '\0';

        CHAR16 *cr = efi_strchr(start, '\r');
        if (cr) *cr = '\0';
        CHAR16 *line = trim(start);

        if (in_entry) {
            if (line[0] == '}') in_entry = 0;
        } else if (is_header(line, L"entry") ||
                   is_header(line, L"linux") ||
                   is_header(line, L"windows")) {
            in_entry = 1;
        } else if (line[0] != '#' && line[0] != '\0') {
            CHAR16 *eq = efi_strchr(line, '=');
            if (eq) {
                *eq = '\0';
                CHAR16 *key = trim(line);
                strip_inline_comment(eq + 1);
                CHAR16 *value = trim(eq + 1);
                int truthy = (*value == '1' || *value == 't' || *value == 'y');
                if (efi_strcmp(key, L"log") == 0 ||
                    efi_strcmp(key, L"boot_log") == 0 ||
                    efi_strcmp(key, L"file_log") == 0)
                    enabled = truthy;
                else if (efi_strcmp(key, L"no_log") == 0)
                    enabled = !truthy;
            }
        }

        if (!had_nl) break;
        start = end + 1;
    }

    efi_free_pool(buf);
    return enabled;
}

static void apply_global(config_t *config, CHAR16 *key, CHAR16 *value) {
    if (efi_strcmp(key, L"timeout") == 0) {
        config->timeout = (*value == '-') ? -1 : (INTN)parse_uint(value);
    } else if (efi_strcmp(key, L"default") == 0) {
        config->default_entry = (INTN)parse_uint(value);
    } else if (efi_strcmp(key, L"quiet") == 0) {
        config->quiet = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"text_menu") == 0 || efi_strcmp(key, L"text_mode") == 0) {
        config->text_menu = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"cmdline") == 0 || efi_strcmp(key, L"options") == 0) {
        if (config->def_cmdline) efi_free_pool(config->def_cmdline);
        config->def_cmdline = (value[0] == '\0') ? NULL : efi_strdup(value);
    } else if (efi_strcmp(key, L"show_names") == 0 || efi_strcmp(key, L"names") == 0) {
        config->show_names = (*value == '1' || *value == 't' || *value == 'y');
        show_names_set = 1;
    } else if (efi_strcmp(key, L"center_info") == 0 || efi_strcmp(key, L"centre_info") == 0) {
        config->center_info = (*value == '1' || *value == 't' || *value == 'y');
        center_info_set = 1;
    } else if (efi_strcmp(key, L"autoboot") == 0) {
        config->autoboot = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"remember_last") == 0 || efi_strcmp(key, L"remember") == 0) {
        config->remember_last = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"recovery_entries") == 0 || efi_strcmp(key, L"recovery") == 0) {
        config->recovery_entries = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"snapshots") == 0) {
        if (efi_strcmp(value, L"manifest") == 0) config->snapshots_mode = 2;
        else config->snapshots_mode = (*value == '1' || *value == 't' || *value == 'y') ? 1 : 0;
    } else if (efi_strcmp(key, L"hotplug") == 0) {
        config->hotplug = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"mouse") == 0 || efi_strcmp(key, L"pointer") == 0) {
        config->mouse = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"mouse_speed") == 0 ||
               efi_strcmp(key, L"pointer_speed") == 0) {
        UINTN speed = parse_uint(value);
        if (speed < 1) speed = 1;
        if (speed > 20) speed = 20;
        config->pointer_speed = speed;
    } else if (efi_strcmp(key, L"scan_existing") == 0 ||
               efi_strcmp(key, L"hotplug_scan_existing") == 0) {
        config->scan_existing = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"scan") == 0 ||
               efi_strcmp(key, L"scan_mode") == 0) {
        if (efi_strcmp(value, L"deep") == 0 || efi_strcmp(value, L"deepscan") == 0 ||
            efi_strcmp(value, L"full") == 0)
            config->scan_mode = SCAN_MODE_DEEP;
        else if (efi_strcmp(value, L"quick") == 0 || efi_strcmp(value, L"quickscan") == 0 ||
                 efi_strcmp(value, L"fast") == 0)
            config->scan_mode = SCAN_MODE_QUICK;
        else
            efi_log(L"WARN: invalid scan mode (deep/quick)");
    } else if (efi_strcmp(key, L"deep_scan") == 0 || efi_strcmp(key, L"deepscan") == 0) {
        config->scan_mode = (*value == '1' || *value == 't' || *value == 'y')
                            ? SCAN_MODE_DEEP : SCAN_MODE_QUICK;
    } else if (efi_strcmp(key, L"quick_scan") == 0 || efi_strcmp(key, L"quickscan") == 0) {
        config->scan_mode = (*value == '1' || *value == 't' || *value == 'y')
                            ? SCAN_MODE_QUICK : SCAN_MODE_DEEP;
    } else if (efi_strcmp(key, L"log") == 0 ||
               efi_strcmp(key, L"boot_log") == 0 ||
               efi_strcmp(key, L"file_log") == 0) {
        config->file_log = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"no_log") == 0) {
        config->file_log = !(*value == '1' || *value == 't' || *value == 'y');
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
        if (config->theme) efi_free_pool(config->theme);
        config->theme = (value[0] == '\0') ? NULL : efi_strdup(value);
    } else if (efi_strcmp(key, L"title") == 0) {
        if (config->title) efi_free_pool(config->title);
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
    } else if (efi_strcmp(key, L"logo") == 0) {
        if (config->logo) efi_free_pool(config->logo);
        config->logo = NULL;
        if (efi_strcmp(value, L"none") == 0 || efi_strcmp(value, L"off") == 0) {
            config->no_logo = 1;
        } else {
            config->no_logo = 0;
            if (value[0] != '\0' && efi_strcmp(value, L"default") != 0)
                config->logo = dup_path(value);
        }
    } else if (efi_strcmp(key, L"logo_mode") == 0) {
        if (efi_strcmp(value, L"title") == 0 || efi_strcmp(value, L"beside") == 0)
            config->logo_mode = LOGO_MODE_TITLE;
        else if (efi_strcmp(value, L"only") == 0 || efi_strcmp(value, L"standalone") == 0)
            config->logo_mode = LOGO_MODE_ONLY;
        else if (efi_strcmp(value, L"above") == 0 || efi_strcmp(value, L"stacked") == 0)
            config->logo_mode = LOGO_MODE_ABOVE;
        else if (efi_strcmp(value, L"none") == 0 || efi_strcmp(value, L"off") == 0)
            config->logo_mode = LOGO_MODE_OFF;
        else
            efi_log(L"WARN: invalid logo_mode (title/only/above/none)");
    } else if (efi_strcmp(key, L"logo_size") == 0) {
        config->logo_size = parse_uint(value);
    } else if (efi_strcmp(key, L"logo_gap") == 0) {
        config->logo_gap = parse_uint(value);
    } else if (efi_strcmp(key, L"accent_logo") == 0 ||
               efi_strcmp(key, L"logo_color") == 0 ||
               efi_strcmp(key, L"accent_logo_color") == 0) {
        if (parse_spec(value, &config->sp_logo)) {
            config->accent_logo = (config->sp_logo.mode != SPEC_OFF);
            config->has_accent_logo = 1;
        } else {
            efi_log(L"WARN: invalid accent_logo (0/1, an accent role, or #RRGGBB)");
        }
    } else if (efi_strcmp(key, L"font") == 0) {
        if (config->font) efi_free_pool(config->font);
        config->font = (value[0] == '\0') ? NULL : efi_strdup(value);
    } else if (efi_strcmp(key, L"title_color") == 0) {
        if (!parse_spec_color(value, &config->sp_title, &config->title_color))
            efi_log(L"WARN: invalid title_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"name_color") == 0) {
        if (!parse_spec_color(value, &config->sp_name, &config->name_color))
            efi_log(L"WARN: invalid name_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"highlight_color") == 0) {
        if (!parse_spec_color(value, &config->sp_highlight, &config->highlight_color))
            efi_log(L"WARN: invalid highlight_color (0/1, an accent role, or #RRGGBB)");
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
        if (parse_spec_color(value, &config->sp_underline, &config->underline_color))
            config->has_underline_color = (config->sp_underline.mode == SPEC_COLOR);
        else
            efi_log(L"WARN: invalid underline_color (0/1, an accent role, or #RRGGBB)");
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
        if (parse_spec_color(value, &config->sp_shutdown, &config->shutdown_color))
            config->has_shutdown_color = (config->sp_shutdown.mode == SPEC_COLOR);
        else
            efi_log(L"WARN: invalid shutdown_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"reboot_color") == 0) {
        if (parse_spec_color(value, &config->sp_reboot, &config->reboot_color))
            config->has_reboot_color = (config->sp_reboot.mode == SPEC_COLOR);
        else
            efi_log(L"WARN: invalid reboot_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"firmware_color") == 0) {
        if (parse_spec_color(value, &config->sp_firmware, &config->firmware_color))
            config->has_firmware_color = (config->sp_firmware.mode == SPEC_COLOR);
        else
            efi_log(L"WARN: invalid firmware_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"background") == 0) {
        if (config->background) efi_free_pool(config->background);
        config->background = dup_path(value);
    } else if (efi_strcmp(key, L"power_icons") == 0) {
        config->power_icons = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"power_icon_size") == 0) {
        config->power_icon_size = parse_uint(value);
    } else if (efi_strcmp(key, L"shutdown_icon") == 0) {
        if (config->shutdown_icon) efi_free_pool(config->shutdown_icon);
        config->shutdown_icon = dup_path(value);
    } else if (efi_strcmp(key, L"reboot_icon") == 0) {
        if (config->reboot_icon) efi_free_pool(config->reboot_icon);
        config->reboot_icon = dup_path(value);
    } else if (efi_strcmp(key, L"firmware_icon") == 0) {
        if (config->firmware_icon) efi_free_pool(config->firmware_icon);
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
        if (parse_spec_color(value, &config->sp_blur, &config->blur_color))
            config->has_blur_color = (config->sp_blur.mode == SPEC_COLOR);
        else
            efi_log(L"WARN: invalid blur_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent") == 0) {
        if (parse_spec(value, &config->sp_all)) {
            config->accent_enabled = (config->sp_all.mode != SPEC_OFF);
        } else {
            efi_log(L"WARN: invalid accent (0/1, an accent role, or #RRGGBB)");
        }
    } else if (efi_strcmp(key, L"accent_icons") == 0) {
        if (parse_spec(value, &config->sp_g_icons))
            config->accent_icons = (config->sp_g_icons.mode != SPEC_OFF);
        else
            efi_log(L"WARN: invalid accent_icons (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent_underline") == 0) {
        if (parse_spec(value, &config->sp_g_underline))
            config->accent_underline = (config->sp_g_underline.mode != SPEC_OFF);
        else
            efi_log(L"WARN: invalid accent_underline (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent_text") == 0) {
        if (parse_spec(value, &config->sp_g_text))
            config->accent_text = (config->sp_g_text.mode != SPEC_OFF);
        else
            efi_log(L"WARN: invalid accent_text (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent_os_icons") == 0) {
        if (parse_spec(value, &config->sp_os_icons))
            config->accent_os_icons = (config->sp_os_icons.mode != SPEC_OFF);
        else
            efi_log(L"WARN: invalid accent_os_icons (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"info_color") == 0) {
        if (!parse_spec_color(value, &config->sp_info, &config->fg_color))
            efi_log(L"WARN: invalid info_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"bg_color") == 0 ||
               efi_strcmp(key, L"background_color") == 0) {
        if (!parse_spec_color(value, &config->sp_bg, &config->bg_color))
            efi_log(L"WARN: invalid bg_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent_variant") == 0) {
        config->accent_variant = accent_variant_from_str(value);
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

#define SNAPSHOTS_FILE     L"\\EFI\\visor\\snapshots.conf"
#define SNAPSHOTS_FILE_ALT L"\\EFI\\visor\\snapshot.conf"
#define MAX_SNAPS 64

static boot_entry_t* sole_linux_entry(config_t *config) {
    boot_entry_t *only = NULL;
    for (boot_entry_t *e = config->entries; e; e = e->next) {
        if (e->type != 0) continue;
        if (only) return NULL;
        only = e;
    }
    return only;
}


static boot_entry_t* snapshot_entry_match(config_t *config,
                                          CHAR16 *entry_name, CHAR16 *os,
                                          int *ambiguous) {
    if (ambiguous) *ambiguous = 0;

    if (entry_name && entry_name[0]) {
        boot_entry_t *match = NULL;
        UINTN matches = 0;
        for (boot_entry_t *e = config->entries; e; e = e->next) {
            if (e->type == 1 || !str_eq_ci(e->name, entry_name)) continue;
            match = e;
            matches++;
        }
        if (matches == 1) return match;
        if (matches > 1) {
            if (ambiguous) *ambiguous = 1;
            return NULL;
        }
    }

    if (os && os[0]) {
        for (boot_entry_t *e = config->entries; e; e = e->next)
            if (e->type != 1 && str_eq_ci(e->name, os))
                return e;

        boot_entry_t *match = NULL;
        UINTN matches = 0;
        for (boot_entry_t *e = config->entries; e; e = e->next) {
            if (e->type == 1 || !contains_ci(e->name, os)) continue;
            match = e;
            matches++;
        }
        if (matches == 1) return match;
        if (matches > 1) {
            if (ambiguous) *ambiguous = 1;
            return NULL;
        }
    }

    return sole_linux_entry(config);
}

static int load_snapshots(config_t *config) {
    CHAR16 *buf = read_text_file(SNAPSHOTS_FILE);
    if (!buf) {
        buf = read_text_file(SNAPSHOTS_FILE_ALT);
        if (buf)
            efi_log(L"config: loaded \\EFI\\visor\\snapshot.conf "
                    L"(the documented name is snapshots.conf)");
    }
    if (!buf) return 0;

    snapshot_t tmp[MAX_SNAPS];
    boot_entry_t *owner[MAX_SNAPS];
    UINTN n = 0;

    CHAR16 *lines[512];
    UINTN line_count = 0;
    CHAR16 *start = buf;
    while (*start && line_count < 512) {
        CHAR16 *end = start;
        while (*end && *end != '\n') end++;
        if (*end == '\n') *end = '\0';
        CHAR16 *cr = efi_strchr(start, '\r');
        if (cr) *cr = '\0';
        lines[line_count++] = start;
        start = end + 1;
    }

    for (UINTN i = 0; i < line_count && n < MAX_SNAPS; i++) {
        CHAR16 *line = trim(lines[i]);
        if (line[0] == '#' || line[0] == '\0') continue;
        if (!is_header(line, L"snapshot")) continue;

        snapshot_t s = { NULL, NULL, NULL, NULL, NULL, NULL };
        CHAR16 *os = NULL;
        CHAR16 *entry_name = NULL;
        i++;
        while (i < line_count) {
            CHAR16 *l = trim(lines[i]);
            if (l[0] == '}' || l[0] == '\0') break;
            CHAR16 *eq = efi_strchr(l, '=');
            if (eq) {
                *eq = '\0';
                CHAR16 *key = trim(l);
                strip_inline_comment(eq + 1);
                CHAR16 *value = trim(eq + 1);
                if      (efi_strcmp(key, L"entry") == 0 ||
                         efi_strcmp(key, L"boot_entry") == 0)
                                                           entry_name = value;
                else if (efi_strcmp(key, L"os") == 0)      os = value;
                else if (efi_strcmp(key, L"id") == 0)      { if (!s.id)   s.id   = efi_strdup(value); }
                else if (efi_strcmp(key, L"date") == 0)    { if (!s.date) s.date = efi_strdup(value); }
                else if (efi_strcmp(key, L"desc") == 0 ||
                         efi_strcmp(key, L"description") == 0)
                                                           { if (!s.desc) s.desc = efi_strdup(value); }
                else if (efi_strcmp(key, L"kernel") == 0)  { if (!s.kernel) s.kernel = dup_path(value); }
                else if (efi_strcmp(key, L"initrd") == 0)  { if (!s.initrd) s.initrd = dup_path(value); }
                else if (efi_strcmp(key, L"cmdline") == 0 ||
                         efi_strcmp(key, L"options") == 0)
                                                           { if (!s.cmdline) s.cmdline = efi_strdup(value); }
            }
            i++;
        }

        int ambiguous = 0;
        boot_entry_t *tgt = snapshot_entry_match(
            config, entry_name, os, &ambiguous);
        if (tgt && s.cmdline && s.id) {
            tmp[n] = s;
            owner[n] = tgt;
            n++;
        } else {
            if (ambiguous)
                efi_log(L"WARN: snapshot manifest selector matched multiple boot entries");
            else if (!tgt)
                efi_log(L"WARN: snapshot manifest record did not match a boot entry");
            else if (!s.id)
                efi_log(L"WARN: snapshot manifest record has no id");
            else if (!s.cmdline)
                efi_log(L"WARN: snapshot manifest record has no cmdline");
            if (entry_name && entry_name[0]) efi_log(entry_name);
            else if (os && os[0]) efi_log(os);
            if (s.id)      efi_free_pool(s.id);
            if (s.date)    efi_free_pool(s.date);
            if (s.desc)    efi_free_pool(s.desc);
            if (s.kernel)  efi_free_pool(s.kernel);
            if (s.initrd)  efi_free_pool(s.initrd);
            if (s.cmdline) efi_free_pool(s.cmdline);
        }
    }
    efi_free_pool(buf);
    if (!n) {
        efi_log(L"WARN: snapshot manifest contained no attachable records");
        return 1;
    }

    for (boot_entry_t *e = config->entries; e; e = e->next) {
        UINTN cnt = 0;
        for (UINTN i = 0; i < n; i++) if (owner[i] == e) cnt++;
        if (!cnt) continue;
        snapshot_t *arr = efi_allocate_pool(cnt * sizeof(snapshot_t));
        if (!arr) continue;
        UINTN k = 0;
        for (UINTN i = 0; i < n; i++)
            if (owner[i] == e) { arr[k++] = tmp[i]; owner[i] = NULL; }
        e->snapshots = arr;
        e->snap_count = cnt;
        e->snap_sel = 0;
        CHAR16 d[128];
        SPrint(d, sizeof(d), L"config: attached %d snapshot(s) to %s",
               (int)cnt, e->name);
        efi_log(d);
    }
    for (UINTN i = 0; i < n; i++) {
        if (!owner[i]) continue;
        if (tmp[i].id)      efi_free_pool(tmp[i].id);
        if (tmp[i].date)    efi_free_pool(tmp[i].date);
        if (tmp[i].desc)    efi_free_pool(tmp[i].desc);
        if (tmp[i].kernel)  efi_free_pool(tmp[i].kernel);
        if (tmp[i].initrd)  efi_free_pool(tmp[i].initrd);
        if (tmp[i].cmdline) efi_free_pool(tmp[i].cmdline);
    }
    efi_log(L"config: snapshot manifest loaded");
    return 1;
}

#define AUTO_SNAP_MAX 12

static CHAR16* find_sub(CHAR16 *hay, const CHAR16 *needle) {
    if (!hay || !needle) return NULL;
    for (UINTN i = 0; hay[i]; i++) {
        UINTN j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return hay + i;
    }
    return NULL;
}

static int is_digits(CHAR16 *s) {
    if (!s[0]) return 0;
    for (UINTN i = 0; s[i]; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

static CHAR16* xml_field(CHAR16 *xml, const CHAR16 *open, const CHAR16 *close, UINTN maxch) {
    CHAR16 *p = find_sub(xml, open);
    if (!p) return NULL;
    while (*p && *p != '>') p++;
    if (!*p) return NULL;
    p++;
    CHAR16 *q = find_sub(p, close);
    if (!q) return NULL;
    UINTN len = (UINTN)(q - p);
    if (len > maxch) len = maxch;
    CHAR16 buf[128];
    if (len > 127) len = 127;
    for (UINTN i = 0; i < len; i++) buf[i] = p[i];
    buf[len] = '\0';
    return efi_strdup(trim(buf));
}

static int cmdline_subvol(CHAR16 *cl, CHAR16 *out, UINTN cap) {
    out[0] = '\0';
    CHAR16 *rf = cl ? find_sub(cl, L"rootflags=") : NULL;
    if (!rf) return 0;
    CHAR16 *p = rf + 10;
    CHAR16 *sv = NULL;
    while (*p && *p != ' ') {
        if (find_sub(p, L"subvol=") == p) { sv = p + 7; break; }
        while (*p && *p != ' ' && *p != ',') p++;
        if (*p == ',') p++;
    }
    if (!sv) return 0;
    while (*sv == '/') sv++;
    UINTN o = 0;
    while (*sv && *sv != ',' && *sv != ' ' && o + 1 < cap) out[o++] = *sv++;
    while (o && out[o - 1] == '/') o--;
    out[o] = '\0';
    return out[0] != '\0';
}

static void base_to_pfx(const CHAR16 *base, CHAR16 *pfx, UINTN cap) {
    UINTN i = 0, o = 0;
    while (base[i] == '\\') i++;
    for (; base[i] && o + 1 < cap; i++)
        pfx[o++] = (base[i] == '\\') ? '/' : base[i];
    pfx[o] = '\0';
}

static INTN cmp16(const CHAR16 *a, const CHAR16 *b) {
    UINTN i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (INTN)a[i] - (INTN)b[i];
}

static CHAR16* json_field(CHAR16 *j, const CHAR16 *key, UINTN maxch) {
    CHAR16 pat[24];
    SPrint(pat, sizeof(pat), L"\"%s\"", key);
    CHAR16 *p = find_sub(j, pat);
    if (!p) return NULL;
    while (*p && *p != ':') p++;
    if (!*p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    CHAR16 buf[128];
    UINTN o = 0;
    while (*p && *p != '"' && o < maxch && o < 127) {
        if (*p == '\\' && p[1]) p++;
        buf[o++] = *p++;
    }
    buf[o] = '\0';
    CHAR16 *t = trim(buf);
    if (!t[0]) return NULL;
    return efi_strdup(t);
}

static int ts_tag_ok(CHAR16 *s) {
    static const CHAR16 pat[] = L"dddd-dd-dd_dd-dd-dd";
    for (UINTN i = 0; pat[i]; i++) {
        if (!s[i]) return 0;
        if (pat[i] == 'd') {
            if (s[i] < '0' || s[i] > '9') return 0;
        } else if (s[i] != pat[i]) {
            return 0;
        }
    }
    return s[19] == '\0';
}

static void snap_build_cmdline(CHAR16 *out, UINTN cap, CHAR16 *cl, CHAR16 *subvol) {
    UINTN o = 0;
    CHAR16 extra[MAX_CMDLINE];
    UINTN x = 0;
    extra[0] = '\0';
    UINTN i = 0;
    while (cl && cl[i]) {
        while (cl[i] == ' ' || cl[i] == '\t') i++;
        if (!cl[i]) break;
        UINTN j = i;
        while (cl[j] && cl[j] != ' ' && cl[j] != '\t') j++;
        if (find_sub(cl + i, L"rootflags=") == cl + i) {
            UINTN k = i + 10;
            while (k < j) {
                UINTN m = k;
                while (m < j && cl[m] != ',') m++;
                if (find_sub(cl + k, L"subvol") != cl + k) {
                    for (UINTN t = k; t < m && x + 2 < MAX_CMDLINE; t++) {
                        if (t == k && x) extra[x++] = ',';
                        else if (t == k) { }
                        extra[x++] = cl[t];
                    }
                    extra[x] = '\0';
                }
                k = m + (m < j ? 1 : 0);
            }
        } else {
            if (o && o + 1 < cap) out[o++] = ' ';
            for (UINTN t = i; t < j && o + 1 < cap; t++) out[o++] = cl[t];
        }
        i = j;
    }
    out[o] = '\0';
    CHAR16 tail[MAX_CMDLINE];
    if (extra[0])
        SPrint(tail, sizeof(tail), L"%srootflags=subvol=%s,%s", o ? L" " : L"", subvol, extra);
    else
        SPrint(tail, sizeof(tail), L"%srootflags=subvol=%s", o ? L" " : L"", subvol);
    for (UINTN t = 0; tail[t] && o + 1 < cap; t++) out[o++] = tail[t];
    out[o] = '\0';
}

static boot_entry_t* snap_autodetect_target(config_t *config) {
    boot_entry_t *found = NULL;
    for (boot_entry_t *e = config->entries; e; e = e->next) {
        if (looks_windows(e->kernel_path)) continue;
        CHAR16 *cl = e->cmdline ? e->cmdline : config->def_cmdline;
        if (!cl || !find_sub(cl, L"root=")) continue;
        if (found) return NULL;
        found = e;
    }
    return found;
}

typedef struct {
    EFI_FILE_PROTOCOL *root;
    CHAR16 *cl;
    snapshot_t arr[AUTO_SNAP_MAX];
    UINTN made;
} snap_ctx_t;

static void snap_add(snap_ctx_t *c, CHAR16 *sv, CHAR16 *id, CHAR16 *date, CHAR16 *desc) {
    if (c->made >= AUTO_SNAP_MAX || !id) {
        if (id)   efi_free_pool(id);
        if (date) efi_free_pool(date);
        if (desc) efi_free_pool(desc);
        return;
    }
    CHAR16 buf[MAX_CMDLINE];
    snap_build_cmdline(buf, MAX_CMDLINE, c->cl, sv);
    snapshot_t *s = &c->arr[c->made];
    s->kernel = NULL;
    s->initrd = NULL;
    s->id = id;
    s->date = date ? date : efi_strdup(L"?");
    s->desc = desc ? desc : efi_strdup(L"snapshot");
    s->cmdline = efi_strdup(buf);
    if (!s->cmdline) {
        if (s->id)   efi_free_pool(s->id);
        if (s->date) efi_free_pool(s->date);
        if (s->desc) efi_free_pool(s->desc);
        return;
    }
    c->made++;
}

static UINTN scan_snapper(snap_ctx_t *c, const CHAR16 *base) {
    CHAR16 dirp[MAX_PATH];
    SPrint(dirp, sizeof(dirp), L"%s\\.snapshots", base);
    EFI_FILE_PROTOCOL *d = efi_open_dir(c->root, dirp);
    if (!d) return 0;

    UINTN ids[MAX_SNAPS];
    UINTN n = 0;
    CHAR16 name[64];
    int is_dir;
    while (efi_read_dirent(d, name, 64, &is_dir) && n < MAX_SNAPS) {
        if (!is_dir || !is_digits(name)) continue;
        CHAR16 sp[MAX_PATH];
        SPrint(sp, sizeof(sp), L"%s\\%s\\snapshot", dirp, name);
        EFI_FILE_PROTOCOL *sd = efi_open_dir(c->root, sp);
        if (!sd) continue;
        sd->Close(sd);
        ids[n++] = parse_uint(name);
    }
    d->Close(d);
    if (!n) return 0;

    for (UINTN a = 1; a < n; a++) {
        UINTN v = ids[a];
        UINTN b = a;
        while (b > 0 && ids[b - 1] < v) { ids[b] = ids[b - 1]; b--; }
        ids[b] = v;
    }
    if (n > AUTO_SNAP_MAX) n = AUTO_SNAP_MAX;

    CHAR16 pfx[128];
    base_to_pfx(base, pfx, 128);

    UINTN before = c->made;
    for (UINTN a = 0; a < n && c->made < AUTO_SNAP_MAX; a++) {
        CHAR16 sv[MAX_PATH], buf[MAX_PATH];
        if (pfx[0])
            SPrint(sv, sizeof(sv), L"/%s/.snapshots/%d/snapshot", pfx, ids[a]);
        else
            SPrint(sv, sizeof(sv), L"/.snapshots/%d/snapshot", ids[a]);

        CHAR16 *date = NULL, *desc = NULL;
        SPrint(buf, sizeof(buf), L"%s\\%d\\info.xml", dirp, ids[a]);
        CHAR16 *xml = read_text_from_root(c->root, buf);
        if (xml) {
            date = xml_field(xml, L"<date", L"</date>", 16);
            desc = xml_field(xml, L"<description", L"</description>", 64);
            efi_free_pool(xml);
        }
        SPrint(buf, sizeof(buf), L"%d", ids[a]);
        snap_add(c, sv, efi_strdup(buf), date, desc);
    }
    return c->made - before;
}

static UINTN scan_timeshift(snap_ctx_t *c) {
    EFI_FILE_PROTOCOL *d = efi_open_dir(c->root, L"\\timeshift-btrfs\\snapshots");
    if (!d) return 0;

    CHAR16 tags[32][20];
    UINTN n = 0;
    CHAR16 name[64];
    int is_dir;
    while (efi_read_dirent(d, name, 64, &is_dir) && n < 32) {
        if (!is_dir || !ts_tag_ok(name)) continue;
        CHAR16 sp[MAX_PATH];
        SPrint(sp, sizeof(sp), L"\\timeshift-btrfs\\snapshots\\%s\\@", name);
        EFI_FILE_PROTOCOL *sd = efi_open_dir(c->root, sp);
        if (!sd) continue;
        sd->Close(sd);
        for (UINTN i = 0; i < 20; i++) tags[n][i] = name[i];
        n++;
    }
    d->Close(d);
    if (!n) return 0;

    for (UINTN a = 1; a < n; a++) {
        CHAR16 tmp[20];
        for (UINTN i = 0; i < 20; i++) tmp[i] = tags[a][i];
        UINTN b = a;
        while (b > 0 && cmp16(tags[b - 1], tmp) < 0) {
            for (UINTN i = 0; i < 20; i++) tags[b][i] = tags[b - 1][i];
            b--;
        }
        for (UINTN i = 0; i < 20; i++) tags[b][i] = tmp[i];
    }
    if (n > AUTO_SNAP_MAX) n = AUTO_SNAP_MAX;

    UINTN before = c->made;
    for (UINTN a = 0; a < n && c->made < AUTO_SNAP_MAX; a++) {
        CHAR16 sv[MAX_PATH], buf[MAX_PATH];
        SPrint(sv, sizeof(sv), L"/timeshift-btrfs/snapshots/%s/@", tags[a]);

        CHAR16 datebuf[17];
        for (UINTN i = 0; i < 10; i++) datebuf[i] = tags[a][i];
        datebuf[10] = ' ';
        datebuf[11] = tags[a][11];
        datebuf[12] = tags[a][12];
        datebuf[13] = ':';
        datebuf[14] = tags[a][14];
        datebuf[15] = tags[a][15];
        datebuf[16] = '\0';

        CHAR16 *desc = NULL;
        SPrint(buf, sizeof(buf), L"\\timeshift-btrfs\\snapshots\\%s\\info.json", tags[a]);
        CHAR16 *js = read_text_from_root(c->root, buf);
        if (js) {
            desc = json_field(js, L"comments", 64);
            if (!desc) desc = json_field(js, L"tags", 24);
            efi_free_pool(js);
        }
        if (!desc) desc = efi_strdup(L"timeshift");
        snap_add(c, sv, efi_strdup(tags[a]), efi_strdup(datebuf), desc);
    }
    return c->made - before;
}

static UINTN scan_plain_dir(snap_ctx_t *c, const CHAR16 *base, const CHAR16 *dirname) {
    CHAR16 dirp[MAX_PATH];
    SPrint(dirp, sizeof(dirp), L"%s\\%s", base, dirname);
    EFI_FILE_PROTOCOL *d = efi_open_dir(c->root, dirp);
    if (!d) return 0;

    CHAR16 names[32][64];
    UINTN n = 0;
    CHAR16 name[64];
    int is_dir;
    while (efi_read_dirent(d, name, 64, &is_dir) && n < 32) {
        if (!is_dir) continue;
        for (UINTN i = 0; i < 64; i++) names[n][i] = name[i];
        n++;
    }
    d->Close(d);
    if (!n) return 0;

    for (UINTN a = 1; a < n; a++) {
        CHAR16 tmp[64];
        for (UINTN i = 0; i < 64; i++) tmp[i] = names[a][i];
        UINTN b = a;
        while (b > 0 && cmp16(names[b - 1], tmp) < 0) {
            for (UINTN i = 0; i < 64; i++) names[b][i] = names[b - 1][i];
            b--;
        }
        for (UINTN i = 0; i < 64; i++) names[b][i] = tmp[i];
    }

    CHAR16 pfx[128];
    base_to_pfx(base, pfx, 128);

    UINTN before = c->made;
    for (UINTN a = 0; a < n && c->made < AUTO_SNAP_MAX; a++) {
        CHAR16 sv[MAX_PATH];
        if (pfx[0])
            SPrint(sv, sizeof(sv), L"/%s/%s/%s", pfx, dirname, names[a]);
        else
            SPrint(sv, sizeof(sv), L"/%s/%s", dirname, names[a]);
        snap_add(c, sv, efi_strdup(names[a]), NULL, efi_strdup(L"btrfs subvolume"));
    }
    return c->made - before;
}

static void snapshots_autodetect(config_t *config) {
    boot_entry_t *e = snap_autodetect_target(config);
    if (!e) return;

    CHAR16 *cl = e->cmdline ? e->cmdline : config->def_cmdline;

    CHAR16 bases[3][130];
    UINTN nb = 0;
    CHAR16 sv[128];
    if (cmdline_subvol(cl, sv, 128)) {
        UINTN o = 0;
        bases[nb][o++] = '\\';
        for (UINTN i = 0; sv[i] && o + 1 < 130; i++)
            bases[nb][o++] = (sv[i] == '/') ? '\\' : sv[i];
        bases[nb][o] = '\0';
        nb++;
    }
    if (nb == 0 || cmp16(bases[0], L"") != 0) {
        bases[nb][0] = '\0';
        nb++;
    }
    if (nb == 1 || cmp16(bases[0], L"\\@") != 0) {
        bases[nb][0] = '\\';
        bases[nb][1] = '@';
        bases[nb][2] = '\0';
        nb++;
    }

    UINTN nvol = 0;
    EFI_HANDLE *vols = efi_locate_handle_buffer(&gEfiSimpleFileSystemProtocolGuid, &nvol);
    if (!vols) return;

    snap_ctx_t ctx;
    ctx.cl = cl;
    ctx.made = 0;
    const CHAR16 *src = L"";

    int have_uuid = e->uuid && e->uuid[0];
    for (int pass = have_uuid ? 0 : 1; pass < 2 && !ctx.made; pass++) {
        for (UINTN v = 0; v < nvol && !ctx.made; v++) {
            if (pass == 0 &&
                !efi_handle_matches_partition_uuid(vols[v], e->uuid))
                continue;
            EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
            if (!root) continue;
            ctx.root = root;

            for (UINTN b = 0; b < nb; b++) scan_snapper(&ctx, bases[b]);
            if (ctx.made) src = L"snapper";

            if (!ctx.made && scan_timeshift(&ctx)) src = L"timeshift";

            if (!ctx.made) {
                for (UINTN b = 0; b < nb && !ctx.made; b++) {
                    scan_plain_dir(&ctx, bases[b], L".snapshots");
                    scan_plain_dir(&ctx, bases[b], L"snapshots");
                    scan_plain_dir(&ctx, bases[b], L"@snapshots");
                }
                if (ctx.made) src = L"btrfs";
            }
            root->Close(root);
        }
        if (pass == 0 && !ctx.made)
            efi_log(L"config: no snapshots on uuid= volume - trying all volumes");
    }
    efi_free_pool(vols);

    if (!ctx.made) return;

    snapshot_t *arr = efi_allocate_pool(ctx.made * sizeof(snapshot_t));
    if (!arr) {
        for (UINTN i = 0; i < ctx.made; i++) {
            if (ctx.arr[i].id)      efi_free_pool(ctx.arr[i].id);
            if (ctx.arr[i].date)    efi_free_pool(ctx.arr[i].date);
            if (ctx.arr[i].desc)    efi_free_pool(ctx.arr[i].desc);
            if (ctx.arr[i].cmdline) efi_free_pool(ctx.arr[i].cmdline);
        }
        return;
    }
    for (UINTN i = 0; i < ctx.made; i++) arr[i] = ctx.arr[i];
    e->snapshots = arr;
    e->snap_count = ctx.made;
    e->snap_sel = 0;

    CHAR16 d[96];
    SPrint(d, sizeof(d), L"config: auto-detected %d snapshot(s) (%s)", ctx.made, src);
    efi_log(d);
}

static void snapshots_apply(config_t *config) {
    if (config->snapshots_mode == 0) return;
    if (load_snapshots(config)) return;
    if (config->snapshots_mode != 2) snapshots_autodetect(config);
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
    config->snapshots_mode = 1;
    config->autoboot = 0;
    config->mouse = 1;
    config->pointer_speed = 4;
    config->file_log = 1;
    config->scan_existing = -1;
    config->scan_mode = SCAN_MODE_DEEP;
    config->entries_from_config = 0;
    config->editor = 1;
    config->theme = NULL;
    config->title = NULL;
    config->no_title = 0;
    config->logo = NULL;
    config->no_logo = 0;
    config->logo_mode = LOGO_MODE_TITLE;
    config->logo_size = 0;
    config->logo_gap = 0;
    config->accent_logo = 0;
    config->has_accent_logo = 0;
    config->sp_logo.mode = SPEC_UNSET;
    config->sp_underline.mode = SPEC_UNSET;
    config->sp_highlight.mode = SPEC_UNSET;
    config->sp_title.mode = SPEC_UNSET;
    config->sp_name.mode = SPEC_UNSET;
    config->sp_info.mode = SPEC_UNSET;
    config->sp_shutdown.mode = SPEC_UNSET;
    config->sp_reboot.mode = SPEC_UNSET;
    config->sp_firmware.mode = SPEC_UNSET;
    config->sp_os_icons.mode = SPEC_UNSET;
    config->sp_blur.mode = SPEC_UNSET;
    config->sp_bg.mode = SPEC_UNSET;
    config->sp_g_text.mode = SPEC_UNSET;
    config->sp_g_icons.mode = SPEC_UNSET;
    config->sp_g_underline.mode = SPEC_UNSET;
    config->sp_all.mode = SPEC_UNSET;
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
    config->accent_enabled = 0;
    config->accent_icons = 1;
    config->accent_underline = 1;
    config->accent_text = 0;
    config->accent_os_icons = 0;
    config->accent_variant = 0;
    config->animation = 1;
    config->anim_speed = 0;
    config->fade_speed = 0;
    config->entries_per_page = 0;
    config->hotplug = 1;
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
        EFI_STATUS st = detect_entries(config);
        snapshots_apply(config);
        return st;
    }

    UINTN max_lines = 1;
    for (CHAR16 *p = buf; *p; p++)
        if (*p == '\n') max_lines++;

    CHAR16 **lines = efi_allocate_pool(max_lines * sizeof(CHAR16 *));
    if (!lines) {
        efi_free_pool(buf);
        efi_log(L"ERROR: out of memory splitting config - auto-detecting");
        EFI_STATUS st = detect_entries(config);
        snapshots_apply(config);
        return st;
    }

    UINTN line_count = 0;
    CHAR16 *start = buf;

    while (*start && line_count < max_lines) {
        CHAR16 *end = start;
        while (*end && *end != '\n') end++;
        int had_nl = (*end == '\n');
        if (had_nl) *end = '\0';

        CHAR16 *cr = efi_strchr(start, '\r');
        if (cr) *cr = '\0';

        lines[line_count++] = start;
        if (!had_nl) break;
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
                efi_log(L"config: calling parse_entry");
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

    efi_free_pool(lines);
    efi_free_pool(buf);

    if (config->entry_count == entry_count_before_parse) {
        detect_entries(config);
    } else {
        config->entries_from_config = 1;
        efi_log(L"config: boot.conf supplied the entries - skipping volume scan");
    }

    snapshots_apply(config);

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
    entry->icon_path = icon_path ? icon_path
                     : (type == 1 || looks_windows(kernel_path)
                        ? icon_path_for(L"windows.png")
                        : distro_icon(name));
    entry->kernel_path = kernel_path;
    entry->initrd_path = initrd_path;
    entry->cmdline = cmdline ? cmdline
                   : (!dc_foreign_volume
                      && entry_takes_default_cmdline(kernel_path, type)
                      && config->def_cmdline
                      ? efi_strdup(config->def_cmdline) : NULL);
    entry->uuid = uuid;
    entry->type = type;
    entry->index = config->entry_count;
    entry->icon = NULL;
    entry->icon_size = 0;
    entry->color = config->name_color;
    entry->has_color = 0;
    entry->color_role = -1;
    entry->has_sha256 = 0;
    entry->encrypted = encrypted;
    entry->initrd_encrypted = initrd_encrypted;
    entry->luks = 0;
    entry->luks_confirm = 0;
    entry->luks_verbose = 0;
    entry->luks_key_path = NULL;
    entry->luks_cmdline = NULL;
    entry->luks_preset = NULL;
    entry->decrypt_password = NULL;
    entry->hp_volume = NULL;
    entry->next = NULL;
    entry->deployments = NULL;
    entry->deploy_count = 0;
    entry->deploy_default = 0;
    entry->deploy_sel = 0;
    entry->snapshots = NULL;
    entry->snap_count = 0;
    entry->snap_sel = 0;

    efi_log(L"config: adding entry");
    efi_log(entry->name);

    if (entry->icon_path) {
        entry->icon = gui_load_icon(entry->icon_path);
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
        free_entry_contents(entry);
        efi_free_pool(entry);
        entry = next;
    }
    config->entries = NULL;
    config->tail = NULL;
    config->entry_count = 0;

    if (config->background)    efi_free_pool(config->background);
    if (config->theme)         efi_free_pool(config->theme);
    if (config->title)         efi_free_pool(config->title);
    if (config->logo)          efi_free_pool(config->logo);
    if (config->font)          efi_free_pool(config->font);
    if (config->def_cmdline)   efi_free_pool(config->def_cmdline);
    if (config->shutdown_icon) efi_free_pool(config->shutdown_icon);
    if (config->reboot_icon)   efi_free_pool(config->reboot_icon);
    if (config->firmware_icon) efi_free_pool(config->firmware_icon);
    config->background = NULL;
    config->theme = NULL;
    config->title = NULL;
    config->logo = NULL;
    config->font = NULL;
    config->def_cmdline = NULL;
    config->shutdown_icon = NULL;
    config->reboot_icon = NULL;
    config->firmware_icon = NULL;
}

