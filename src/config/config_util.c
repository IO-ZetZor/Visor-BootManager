/* config_util.c - string, colour and file helpers shared by the config units */
#include "config_internal.h"

CHAR16* trim(CHAR16 *s) {
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

void strip_inline_comment(CHAR16 *s) {
    int in_quote = 0;
    for (UINTN i = 0; s[i]; i++) {
        if (s[i] == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote && s[i] == '#' && i > 0 && is_space16(s[i - 1])) {
            s[i] = '\0';
            return;
        }
    }
}

CHAR16* dup_path(CHAR16 *value) {
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

int parse_sha256(CHAR16 *s, UINT8 out[32]) {
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

int parse_color(CHAR16 *s, color_t *out) {
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

int parse_spec(CHAR16 *value, accent_spec_t *sp) {
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

int parse_spec_color(CHAR16 *value, accent_spec_t *sp, color_t *fallback) {
    if (!parse_spec(value, sp)) return 0;
    if (sp->mode == SPEC_COLOR && fallback) *fallback = sp->color;
    return 1;
}

void wipe16(CHAR16 *s) {
    if (!s) return;
    volatile CHAR16 *p = (volatile CHAR16*)s;
    while (*p) *p++ = 0;
}

void free_char16(CHAR16 **p) {
    if (!p || !*p) return;
    efi_free_pool(*p);
    *p = NULL;
}

void set_char16(CHAR16 **slot, CHAR16 *val) {
    if (*slot) efi_free_pool(*slot);
    *slot = val;
}

UINTN parse_uint(CHAR16 *s) {
    UINTN n = 0;
    while (*s >= '0' && *s <= '9') {
        if (n >= 100000000u) n = 100000000u;
        else n = n * 10 + (UINTN)(*s - '0');
        s++;
    }
    return n;
}

int is_header(CHAR16 *line, const CHAR16 *kw) {
    UINTN i = 0;
    while (kw[i] && line[i] == kw[i]) i++;
    if (kw[i] != '\0') return 0;
    CHAR16 *r = line + i;
    while (*r == ' ' || *r == '\t' || *r == '{') r++;
    return (*r == '\0');
}

int looks_windows(CHAR16 *kernel_path) {
    if (!kernel_path) return 0;
    return contains_ci(kernel_path, L"bootmgfw") ||
           contains_ci(kernel_path, L"\\Microsoft\\");
}

int str_eq_ci(CHAR16 *a, const CHAR16 *b) {
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

CHAR16 lc16(CHAR16 c) { return (c >= 'A' && c <= 'Z') ? (CHAR16)(c + 32) : c; }

int contains_ci(CHAR16 *hay, const CHAR16 *needle) {
    for (UINTN i = 0; hay[i]; i++) {
        UINTN j = 0;
        while (needle[j] && lc16(hay[i + j]) == lc16(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

int ends_with_ci(CHAR16 *s, const CHAR16 *suf) {
    UINTN ls = 0, lf = 0;
    while (s[ls]) ls++;
    while (suf[lf]) lf++;
    if (lf > ls) return 0;
    for (UINTN i = 0; i < lf; i++)
        if (lc16(s[ls - lf + i]) != lc16(suf[i])) return 0;
    return 1;
}

CHAR16* icon_path_for(const CHAR16 *file) {
    CHAR16 *out = efi_allocate_pool(MAX_PATH * sizeof(CHAR16));
    if (!out) return NULL;
    SPrint(out, MAX_PATH * sizeof(CHAR16), L"%s\\icons\\%s", CONFIG_DIR, file);
    return out;
}

CHAR16* distro_icon(CHAR16 *hint) {
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

int equals_ci(CHAR16 *a, const CHAR16 *b) {
    UINTN i = 0;
    while (a[i] && b[i] && lc16(a[i]) == lc16((CHAR16)b[i])) i++;
    return a[i] == '\0' && b[i] == '\0';
}

CHAR16* read_text_from_root(EFI_FILE_PROTOCOL *root, CHAR16 *path) {
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

int wcs16casecmp(CHAR16 *a, CHAR16 *b) {
    while (*a && *b) {
        CHAR16 ca = *a, cb = *b;
        if (ca >= L'A' && ca <= L'Z') ca += 32;
        if (cb >= L'A' && cb <= L'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)*a - (int)*b;
}

#define CONFIG_TEXT_MAX (1024ULL * 1024ULL)

CHAR16* read_text_file(CHAR16 *path) {
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

CHAR16* find_sub(CHAR16 *hay, const CHAR16 *needle) {
    if (!hay || !needle) return NULL;
    for (UINTN i = 0; hay[i]; i++) {
        UINTN j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return hay + i;
    }
    return NULL;
}

int is_digits(CHAR16 *s) {
    if (!s[0]) return 0;
    for (UINTN i = 0; s[i]; i++)
        if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

INTN cmp16(const CHAR16 *a, const CHAR16 *b) {
    UINTN i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (INTN)a[i] - (INTN)b[i];
}
