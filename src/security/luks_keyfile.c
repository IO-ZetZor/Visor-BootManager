/* luks_keyfile.c - cpio keyfile archive appended to the initrd (feature: luks) */
#include "linux_internal.h"

static UINTN utf8_size16(CHAR16 *s) {
    UINTN n = 0;
    if (!s) return 0;
    for (UINTN i = 0; s[i]; i++) {
        UINT16 c = s[i];
        if (c < 0x80) n += 1;
        else if (c < 0x800) n += 2;
        else n += 3;
    }
    return n;
}

static CHAR8* utf8_from16(CHAR16 *s, UINTN *out_len) {
    UINTN len = utf8_size16(s);
    CHAR8 *out = efi_allocate_pool(len ? len : 1);
    if (!out) return NULL;

    UINTN k = 0;
    if (s) {
        for (UINTN i = 0; s[i]; i++) {
            UINT16 c = s[i];
            if (c < 0x80) {
                out[k++] = (CHAR8)c;
            } else if (c < 0x800) {
                out[k++] = (CHAR8)(0xC0 | (c >> 6));
                out[k++] = (CHAR8)(0x80 | (c & 0x3F));
            } else {
                out[k++] = (CHAR8)(0xE0 | (c >> 12));
                out[k++] = (CHAR8)(0x80 | ((c >> 6) & 0x3F));
                out[k++] = (CHAR8)(0x80 | (c & 0x3F));
            }
        }
    }
    if (out_len) *out_len = k;
    return out;
}

static CHAR8* cpio_name_from_path(CHAR16 *path) {
    CHAR16 *src = (path && path[0]) ? path : LUKS_DEFAULT_KEY_PATH;
    while (*src == L'/' || *src == L'\\') src++;
    if (!*src) src = L"crypto_keyfile.bin";

    int last_slash_check = 1;
    UINTN cap = 0;
    while (src[cap]) {
        CHAR16 c = src[cap];
        int slash = (c == L'/' || c == L'\\');
        if (c > 0x7F || c < 0x20) return NULL;
        if (!slash && (c == L':' || c == L'*' || c == L'?' ||
                       c == L'"' || c == L'<' || c == L'>' || c == L'|'))
            return NULL;
        if (!slash && c == L'.' &&
            (last_slash_check || src[cap + 1] == 0 ||
             src[cap + 1] == L'/' || src[cap + 1] == L'\\'))
            return NULL;
        last_slash_check = slash;
        cap++;
    }

    CHAR8 *out = efi_allocate_pool(cap + 1);
    if (!out) return NULL;

    UINTN k = 0;
    int last_slash = 0;
    for (UINTN i = 0; src[i]; i++) {
        CHAR16 c = src[i];
        int slash = (c == L'/' || c == L'\\');
        if (slash) {
            if (last_slash) continue;
            out[k++] = '/';
            last_slash = 1;
        } else {
            out[k++] = (CHAR8)c;
            last_slash = 0;
        }
    }
    while (k && out[k - 1] == '/') k--;
    if (!k) {
        efi_free_pool(out);
        return NULL;
    }
    out[k] = '\0';
    return out;
}

static void cpio_hex8(UINT8 *p, UINT32 v) {
    static const CHAR8 hex[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        p[7 - i] = (UINT8)hex[(v >> (i * 4)) & 0xFu];
    }
}

static UINT8* cpio_header(UINT8 *p, UINT32 ino, UINT32 mode, UINT32 nlink,
                          UINT32 filesize, UINT32 namesize) {
    CopyMem(p, "070701", 6);
    p += 6;
    cpio_hex8(p, ino);       p += 8;
    cpio_hex8(p, mode);      p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, nlink);     p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, filesize);  p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, 0);         p += 8;
    cpio_hex8(p, namesize);  p += 8;
    cpio_hex8(p, 0);         p += 8;
    return p;
}

static UINT8* cpio_write_entry(UINT8 *p, CHAR8 *name, UINT32 mode,
                               UINT32 nlink, UINT32 ino, UINT8 *data,
                               UINTN data_size) {
    UINT8 *start = p;
    UINTN name_len = strlen8(name);

    p = cpio_header(p, ino, mode, nlink, (UINT32)data_size, (UINT32)(name_len + 1));
    CopyMem(p, name, name_len);
    p[name_len] = 0;
    p += name_len + 1;

    UINTN pad = pad4((UINTN)(p - start));
    for (UINTN i = 0; i < pad; i++) *p++ = 0;

    if (data_size) {
        CopyMem(p, data, data_size);
        p += data_size;
    }
    pad = pad4((UINTN)(p - start));
    for (UINTN i = 0; i < pad; i++) *p++ = 0;
    return p;
}

static UINTN cpio_parent_dirs_size(CHAR8 *name) {
    UINTN total = 0;
    for (UINTN i = 0; name[i]; i++) {
        if (name[i] == '/' && i > 0) total += cpio_entry_size(i, 0);
    }
    return total;
}

static UINT8* cpio_write_parent_dirs(UINT8 *p, CHAR8 *name, UINT32 *ino) {
    for (UINTN i = 0; name[i]; i++) {
        if (name[i] != '/' || i == 0) continue;
        name[i] = '\0';
        p = cpio_write_entry(p, name, 0x000041EDu, 2, (*ino)++, NULL, 0);
        name[i] = '/';
    }
    return p;
}

EFI_STATUS luks_build_keyfile_archive(boot_entry_t *entry,
                                             efi_file_buffer_t **out_buf) {
    if (!entry || !out_buf) return EFI_INVALID_PARAMETER;
    *out_buf = NULL;
    if (!entry->decrypt_password) {
        efi_log(L"ERROR: luks=1 but no password was captured");
        efi_print(L"LUKS password missing\r\n");
        return EFI_SECURITY_VIOLATION;
    }

    CHAR8 *name = cpio_name_from_path(entry->luks_key_path);
    if (!name) {
        efi_log(L"ERROR: invalid luks_key_path (must be a safe ASCII relative path)");
        efi_print(L"Invalid LUKS key path\r\n");
        return EFI_INVALID_PARAMETER;
    }

    UINTN pass_len = 0;
    CHAR8 *pass = utf8_from16(entry->decrypt_password, &pass_len);
    if (!pass) {
        efi_free_pool(name);
        return EFI_OUT_OF_RESOURCES;
    }

    static CHAR8 trailer[] = "TRAILER!!!";
    UINTN name_len = strlen8(name);
    UINTN archive_size = 0;
    UINTN n1 = cpio_parent_dirs_size(name);
    UINTN n2 = cpio_entry_size(name_len, pass_len);
    UINTN n3 = cpio_entry_size(strlen8(trailer), 0);
    if (add_overflow_uintn(n1, n2, &archive_size) ||
        add_overflow_uintn(archive_size, n3, &archive_size)) {
        wipe_bytes(pass, pass_len);
        efi_free_pool(pass);
        efi_free_pool(name);
        return EFI_INVALID_PARAMETER;
    }

    efi_file_buffer_t *out = efi_allocate_pool(sizeof(efi_file_buffer_t));
    if (!out) {
        wipe_bytes(pass, pass_len);
        efi_free_pool(pass);
        efi_free_pool(name);
        return EFI_OUT_OF_RESOURCES;
    }
    out->data = efi_allocate_pool(archive_size);
    if (!out->data) {
        efi_free_pool(out);
        wipe_bytes(pass, pass_len);
        efi_free_pool(pass);
        efi_free_pool(name);
        return EFI_OUT_OF_RESOURCES;
    }
    out->size = archive_size;

    UINT8 *dst = (UINT8*)out->data;
    UINT32 ino = 1;
    dst = cpio_write_parent_dirs(dst, name, &ino);
    dst = cpio_write_entry(dst, name, 0x00008180u, 1, ino++, (UINT8*)pass, pass_len);
    dst = cpio_write_entry(dst, trailer, 0, 1, ino++, NULL, 0);

    wipe_bytes(pass, pass_len);
    efi_free_pool(pass);
    efi_free_pool(name);

    *out_buf = out;
    return EFI_SUCCESS;
}

EFI_STATUS luks_append_keyfile(boot_entry_t *entry, efi_file_buffer_t **buf_io) {
    if (!entry || !entry->luks) return EFI_SUCCESS;
    if (!buf_io || !*buf_io || !(*buf_io)->data || !(*buf_io)->size)
        return EFI_INVALID_PARAMETER;

    efi_file_buffer_t *archive = NULL;
    EFI_STATUS status = luks_build_keyfile_archive(entry, &archive);
    if (EFI_ERROR(status)) return status;

    UINTN prefix_pad = pad4((*buf_io)->size);
    UINTN new_size = 0;
    if (add_overflow_uintn((*buf_io)->size, prefix_pad, &new_size) ||
        add_overflow_uintn(new_size, archive->size, &new_size)) {
        free_file_buffer_wipe(archive);
        return EFI_INVALID_PARAMETER;
    }

    efi_file_buffer_t *out = efi_allocate_pool(sizeof(efi_file_buffer_t));
    if (!out) {
        free_file_buffer_wipe(archive);
        return EFI_OUT_OF_RESOURCES;
    }
    out->data = efi_allocate_pool(new_size);
    if (!out->data) {
        efi_free_pool(out);
        free_file_buffer_wipe(archive);
        return EFI_OUT_OF_RESOURCES;
    }
    out->size = new_size;

    UINT8 *dst = (UINT8*)out->data;
    CopyMem(dst, (*buf_io)->data, (*buf_io)->size);
    dst += (*buf_io)->size;
    for (UINTN i = 0; i < prefix_pad; i++) *dst++ = 0;
    CopyMem(dst, archive->data, archive->size);

    free_file_buffer_wipe(archive);
    free_file_buffer_maybe_wipe(*buf_io, entry->initrd_encrypted);
    *buf_io = out;
    efi_log(L"luks: passphrase keyfile appended to initrd");
    return EFI_SUCCESS;
}

static void luks_strip_quiet(CHAR16 *cmdline) {
    if (!cmdline) return;
    static const CHAR16 *hide[] = { L"quiet", L"splash", NULL };

    UINTN i = 0;
    while (cmdline[i]) {
        while (cmdline[i] == L' ') i++;
        UINTN start = i;
        while (cmdline[i] && cmdline[i] != L' ') i++;
        UINTN len = i - start;
        for (int h = 0; hide[h]; h++) {
            UINTN hl = efi_strlen16((CHAR16*)hide[h]);
            if (hl != len) continue;
            UINTN k = 0;
            while (k < len && cmdline[start + k] == hide[h][k]) k++;
            if (k == len) {
                UINTN dst = start, src = start + len;
                while (cmdline[src] == L' ') src++;
                if (dst > 0 && cmdline[dst - 1] == L' ' && cmdline[src] == 0) dst--;
                while (cmdline[src]) cmdline[dst++] = cmdline[src++];
                cmdline[dst] = 0;
                i = start;
                break;
            }
        }
    }
}

EFI_STATUS luks_effective_cmdline(boot_entry_t *entry,
                                         CHAR16 **cmdline_out,
                                         int *owned_out) {
    *cmdline_out = entry->cmdline;
    *owned_out = 0;

    if (!entry->luks) return EFI_SUCCESS;
    if (!entry->luks_cmdline || !entry->luks_cmdline[0]) {
        efi_log(L"WARN: luks=1 without luks_cmdline; initramfs may still prompt");
        return EFI_SUCCESS;
    }
    if (!entry->luks_verbose && entry->cmdline &&
        (visor_cmdline_has_word(entry->cmdline, L"quiet") ||
         visor_cmdline_has_word(entry->cmdline, L"splash")))
        efi_log(L"WARN: luks=1 with quiet/splash - a rejected passphrase will "
                L"reprompt invisibly; set luks_verbose=1");

    UINTN base_len = entry->cmdline ? efi_strlen16(entry->cmdline) : 0;
    UINTN extra_len = efi_strlen16(entry->luks_cmdline);
    UINTN sep = base_len ? 1 : 0;
    CHAR16 *out = efi_allocate_pool((base_len + sep + extra_len + 1) * sizeof(CHAR16));
    if (!out) return EFI_OUT_OF_RESOURCES;

    UINTN k = 0;
    for (UINTN i = 0; i < base_len; i++) out[k++] = entry->cmdline[i];
    if (sep) out[k++] = L' ';
    for (UINTN i = 0; i < extra_len; i++) out[k++] = entry->luks_cmdline[i];
    out[k] = 0;

    if (entry->luks_verbose) {
        luks_strip_quiet(out);
        efi_log(L"luks: luks_verbose=1 - removed quiet/splash so the initramfs "
                L"can prompt if the passphrase is rejected");
    }

    *cmdline_out = out;
    *owned_out = 1;
    efi_log(L"luks: appended initramfs keyfile cmdline");
    return EFI_SUCCESS;
}
