
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

static int looks_windows(CHAR16 *kernel_path) {
    if (!kernel_path) return 0;
    return contains_ci(kernel_path, L"bootmgfw") ||
           contains_ci(kernel_path, L"\\Microsoft\\");
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
            }
        }
        (*idx)++;
    }

    if (!type && looks_windows(kernel_path)) type = 1;

    if (name && kernel_path) {
        boot_entry_t *e = config_add_entry(config, name, icon_path, kernel_path,
                                           initrd_path, cmdline, uuid, type);
        if (e && has_color) { e->color = color; e->has_color = 1; }
        if (e) e->icon_size = entry_icon_size;
        if (e && has_sha256) {
            for (int i = 0; i < 32; i++) e->sha256[i] = sha256_buf[i];
            e->has_sha256 = 1;
        }
    }

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

        config_add_entry(config, efi_strdup(disp), distro_icon(disp), efi_strdup(path),
                         NULL, NULL, NULL, 0);
        added++;
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

        config_add_entry(config, efi_strdup(name), icon_path_for(L"unknown.png"),
                         efi_strdup(path), initrd, NULL, NULL, 0);
        added++;
    }
    d->Close(d);
    return added;
}

static EFI_STATUS detect_entries(config_t *config) {
    static CHAR16 *windows_paths[] = {
        L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
        L"\\EFI\\BOOT\\bootmgfw.efi",
        NULL
    };

    config->show_names = 0;
    config->center_info = 1;

    UINTN nvol = efi_volume_count();
    if (nvol == 0) return EFI_NOT_FOUND;

    int windows_found = 0;
    int uki_found = 0;

    for (UINTN v = 0; v < nvol; v++) {
        EFI_FILE_PROTOCOL *root = efi_open_volume(v);
        if (!root) continue;

        if (!windows_found) {
            for (int i = 0; windows_paths[i] != NULL; i++) {
                if (efi_file_exists_root(root, windows_paths[i])) {
                    config_add_entry(config, L"Windows Boot Manager", icon_path_for(L"windows.png"),
                                     efi_strdup(windows_paths[i]), NULL, NULL, NULL, 1);
                    windows_found = 1;
                    break;
                }
            }
        }

        uki_found += scan_uki_dir(config, root, L"\\EFI\\Linux");

        root->Close(root);
    }

    if (!uki_found) {
        for (UINTN v = 0; v < nvol; v++) {
            EFI_FILE_PROTOCOL *root = efi_open_volume(v);
            if (!root) continue;
            scan_kernel_dir(config, root, L"\\boot");
            scan_kernel_dir(config, root, L"\\");
            root->Close(root);
        }
    }

    return EFI_SUCCESS;
}

static CHAR16* read_text_file(CHAR16 *path) {
    efi_file_t *file = efi_fopen(path);
    if (!file) return NULL;

    UINT8 *raw = NULL;
    UINTN  raw_len = 0;
    UINTN  raw_cap = 4096;
    raw = efi_allocate_pool(raw_cap);
    if (!raw) { efi_fclose(file); return NULL; }

    for (;;) {
        if (raw_len + 512 > raw_cap) {
            raw_cap *= 2;
            UINT8 *nb = efi_allocate_pool(raw_cap);
            if (!nb) break;
            for (UINTN i = 0; i < raw_len; i++) nb[i] = raw[i];
            efi_free_pool(raw);
            raw = nb;
        }
        UINTN n = efi_fread(file, raw + raw_len, 512);
        if (n == 0) break;
        raw_len += n;
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
    } else if (efi_strcmp(key, L"box_radius") == 0 || efi_strcmp(key, L"corner_radius") == 0) {
        config->box_radius = parse_uint(value);
    } else if (efi_strcmp(key, L"resolution") == 0) {
        config->res_w = 0; config->res_h = 0; config->res_max = 0;
        if (efi_strcmp(value, L"max") == 0 || efi_strcmp(value, L"highest") == 0) {
            config->res_max = 1;
        } else if (efi_strcmp(value, L"native") == 0 || value[0] == '\0') {
            /* keep firmware default */
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
    } else if (efi_strcmp(key, L"anim_speed") == 0) {
        config->anim_speed = (int)parse_uint(value);
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
    config->anim_speed = 0;
    config->entries_per_page = 0;
    config->power_icons = 0;
    config->power_icon_size = 0;
    config->shutdown_icon = NULL;
    config->reboot_icon = NULL;
    config->firmware_icon = NULL;
    config->entries = NULL;
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

    if (config->theme) apply_theme(config, config->theme);

    efi_free_pool(buf);

    if (config->entry_count == entry_count_before_parse) {
        detect_entries(config);
    }

    return EFI_SUCCESS;
}

boot_entry_t* config_add_entry(config_t *config,
                                CHAR16 *name,
                                CHAR16 *icon_path,
                                CHAR16 *kernel_path,
                                CHAR16 *initrd_path,
                                CHAR16 *cmdline,
                                CHAR16 *uuid,
                                int type) {
    boot_entry_t *entry = efi_allocate_pool(sizeof(boot_entry_t));
    if (!entry) { efi_log(L"ERROR: out of memory adding boot entry"); return NULL; }

    entry->name = name ? name : L"Unknown";
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
    entry->next = NULL;

    efi_log(L"config: adding entry");
    efi_log(entry->name);

    if (icon_path) {
        entry->icon = gui_load_icon(icon_path);
        if (!entry->icon) efi_log(L"WARN: entry icon failed to load");
    }

    if (!config->entries) {
        config->entries = entry;
    } else {
        boot_entry_t *last = config->entries;
        while (last->next) last = last->next;
        last->next = entry;
    }

    config->entry_count++;
    return entry;
}

void config_free(config_t *config) {
    boot_entry_t *entry = config->entries;
    while (entry) {
        boot_entry_t *next = entry->next;
        if (entry->name) efi_free_pool(entry->name);
        if (entry->icon_path) efi_free_pool(entry->icon_path);
        if (entry->kernel_path) efi_free_pool(entry->kernel_path);
        if (entry->initrd_path) efi_free_pool(entry->initrd_path);
        if (entry->cmdline) efi_free_pool(entry->cmdline);
        if (entry->uuid) efi_free_pool(entry->uuid);

        efi_free_pool(entry);
        entry = next;
    }
    config->entries = NULL;
    config->entry_count = 0;

    config->background = NULL;
}
