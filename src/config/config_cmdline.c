/* config_cmdline.c - kernel cmdline derivation from UKI, BLS, fstab and GPT */
#include "config_internal.h"

int dc_foreign_volume;

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

CHAR16* dc_from_loader_entries(EFI_FILE_PROTOCOL *root, CHAR16 *kernel_name) {
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

CHAR16* dc_derive_cmdline(EFI_FILE_PROTOCOL *root, EFI_HANDLE volume,
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
