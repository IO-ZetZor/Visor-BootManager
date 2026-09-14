#include "filebrowse.h"
#include "gui.h"

static void fb_strcpy(CHAR16 *dst, const CHAR16 *src) {
    UINTN i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int name_casefold(CHAR16 c) {
    if (c >= 'A' && c <= 'Z') return (int)(c + 32);
    return (int)c;
}

static int name_ci_cmp(const CHAR16 *a, const CHAR16 *b) {
    while (*a && *b) {
        int ca = name_casefold(*a), cb = name_casefold(*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (int)*a - (int)*b;
}

static int fb_compare(const void *pa, const void *pb) {
    const fb_entry_t *a = (const fb_entry_t*)pa;
    const fb_entry_t *b = (const fb_entry_t*)pb;
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;
    return name_ci_cmp(a->name, b->name);
}

int fb_is_efi(const CHAR16 *name) {
    UINTN n = 0;
    while (name && name[n]) n++;
    if (n < 4) return 0;
    CHAR16 a = name[n-4], b = name[n-3], c = name[n-2], d = name[n-1];
    if (b >= 'A' && b <= 'Z') b += 32;
    if (c >= 'A' && c <= 'Z') c += 32;
    if (d >= 'A' && d <= 'Z') d += 32;
    return a == '.' && b == 'e' && c == 'f' && d == 'i';
}

int fb_is_initrd(const CHAR16 *name) {
    if (!name || !name[0]) return 0;
    static const CHAR16 *pfx[] = { L"initrd", L"initramfs", L"initram" };
    for (UINTN i = 0; i < 3; i++) {
        UINTN k = 0;
        while (pfx[i][k] && name[k]) {
            if (name_casefold(name[k]) != name_casefold(pfx[i][k])) break;
            k++;
        }
        if (!pfx[i][k]) return 1;
    }
    return 0;
}

int fb_is_kernel(const CHAR16 *name) {
    if (fb_is_efi(name)) return 1;
    if (!name || !name[0]) return 0;
    static const CHAR16 *pfx[] = { L"vmlinuz", L"bzImage", L"zImage",
                                   L"uImage", L"kernel", L"Image" };
    for (UINTN i = 0; i < 6; i++) {
        UINTN k = 0;
        while (pfx[i][k] && name[k]) {
            if (name_casefold(name[k]) != name_casefold(pfx[i][k])) break;
            k++;
        }
        if (!pfx[i][k]) return 1;
    }
    return 0;
}

void fb_format_size(UINT64 size, CHAR16 *buf, UINTN cap) {
    if (!buf || cap < 4) return;
    UINT64 u = size;
    int i = 0;
    static const CHAR16 unit[] = { 'B', 'K', 'M', 'G', 'T' };
    while (u >= 1024 && i < 4) { u /= 1024; i++; }
    if (i == 0) {
        SPrint(buf, cap, L"%ld B", (long)size);
    } else {
        UINTN whole = u;
        UINT64 rem = size - (whole << (10 * i));
        UINTN frac = (UINTN)(rem * 10 / (UINT64)(1ULL << (10 * i)));
        if (frac >= 10) { frac = 0; whole++; }
        if (frac) SPrint(buf, cap, L"%d.%d %cB", (int)whole, (int)frac, unit[i]);
        else      SPrint(buf, cap, L"%d %cB", (int)whole, unit[i]);
    }
}

static EFI_FILE_PROTOCOL* open_root_on_handle(EFI_HANDLE h) {
    if (!h) return NULL;
    EFI_FILE_IO_INTERFACE *io = NULL;
    if (EFI_ERROR(BS->HandleProtocol(h, &gEfiSimpleFileSystemProtocolGuid, (void**)&io)) || !io)
        return NULL;
    EFI_FILE_PROTOCOL *root = NULL;
    if (EFI_ERROR(io->OpenVolume(io, &root))) return NULL;
    return root;
}

static int volume_has_path(EFI_HANDLE h, CHAR16 *path) {
    EFI_FILE_PROTOCOL *root = open_root_on_handle(h);
    if (!root) return 0;
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = root->Open(root, &f, path, EFI_FILE_MODE_READ, 0);
    if (f) f->Close(f);
    root->Close(root);
    return !EFI_ERROR(st);
}

static void fb_clear_entries(fb_t *s) {
    if (!s) return;
    for (UINTN i = 0; i < s->entry_count; i++) {
        if (s->entries[i].name) efi_free_pool(s->entries[i].name);
    }
    if (s->entries) { efi_free_pool(s->entries); s->entries = NULL; }
    s->entry_count = 0;
    s->cursor = 0;
    s->scroll = 0;
    s->truncated = 0;
}

static void fb_free_volumes(fb_t *s) {
    if (!s) return;
    for (UINTN i = 0; i < s->vol_count; i++) {
        if (s->vols[i].uuid) efi_free_pool(s->vols[i].uuid);
        if (s->vols[i].label) efi_free_pool(s->vols[i].label);
    }
    if (s->vols) { efi_free_pool(s->vols); s->vols = NULL; }
    s->vol_count = 0;
    s->vol_cur = 0;
}

void fb_free(fb_t *s) {
    if (!s) return;
    fb_clear_entries(s);
    fb_free_volumes(s);
    s->path[0] = 0;
}

int fb_init(fb_t *s) {
    if (!s) return 0;
    s->vols = NULL;
    s->vol_count = 0;
    s->vol_cur = 0;
    s->entries = NULL;
    s->entry_count = 0;
    s->cursor = 0;
    s->scroll = 0;
    s->path[0] = 0;

    UINTN n = 0;
    EFI_HANDLE *hs = efi_locate_handle_buffer(&gEfiSimpleFileSystemProtocolGuid, &n);
    if (!hs || n == 0) return 0;

    EFI_HANDLE boot_h = efi_boot_volume_handle();
    UINTN boot_idx = n;
    for (UINTN i = 0; i < n; i++) {
        if (hs[i] == boot_h) { boot_idx = i; break; }
    }
    if (boot_idx >= n)
        for (UINTN i = 0; i < n; i++) {
            if (volume_has_path(hs[i], L"\\EFI\\visor\\boot.conf")) { boot_idx = i; break; }
        }
    if (boot_idx >= n)
        for (UINTN i = 0; i < n; i++) {
            if (volume_has_path(hs[i], L"\\EFI")) { boot_idx = i; break; }
        }

    fb_volume_t *vols = efi_allocate_pool(n * sizeof(fb_volume_t));
    if (!vols) { efi_free_pool(hs); return 0; }

    UINTN out = 0;
    for (UINTN pass = 0; pass < 2 && out < n; pass++) {
        for (UINTN i = 0; i < n; i++) {
            if (pass == 0 && i != boot_idx) continue;
            if (pass == 1 && i == boot_idx) continue;
            if (!efi_handle_has_filesystem(hs[i])) continue;

            fb_volume_t *v = &vols[out++];
            v->handle = hs[i];
            v->uuid = efi_handle_partition_uuid(hs[i]);
            if (i == boot_idx) {
                v->label = efi_strdup(L"boot");
            } else if (v->uuid) {
                CHAR16 *lb = efi_allocate_pool(13 * sizeof(CHAR16));
                if (lb) {
                    UINTN k = 0;
                    while (v->uuid[k] && k < 8) { lb[k] = v->uuid[k]; k++; }
                    lb[k++] = '.'; lb[k++] = '.'; lb[k++] = '.';
                    lb[k] = 0;
                    v->label = lb;
                } else {
                    v->label = efi_strdup(L"vol");
                }
            } else {
                CHAR16 lb[16];
                SPrint(lb, sizeof(lb), L"vol %d", (int)out);
                v->label = efi_strdup(lb);
            }
        }
    }
    efi_free_pool(hs);

    s->vols = vols;
    s->vol_count = out;
    if (s->vol_count == 0) { efi_free_pool(vols); return 0; }
    if (s->vol_cur >= s->vol_count) s->vol_cur = 0;

    fb_strcpy(s->path, L"\\");
    return 1;
}

int fb_set_path(fb_t *s, CHAR16 *path) {
    if (!s || !path) return 0;
    UINTN n = 0;
    while (path[n]) n++;
    if (n == 0 || n >= FB_PATH_MAX) return 0;
    UINTN i = 0;
    while (i <= n) { s->path[i] = path[i]; i++; }
    while (n > 1 && s->path[n-1] == '\\') { s->path[n-1] = 0; n--; }
    if (s->path[0] != '\\') {
        CHAR16 tmp[FB_PATH_MAX];
        UINTN t = 0;
        tmp[t++] = '\\';
        for (i = 0; s->path[i] && t < FB_PATH_MAX - 1; i++) tmp[t++] = s->path[i];
        tmp[t] = 0;
        fb_strcpy(s->path, tmp);
    }
    return 1;
}

static int fb_grow(fb_entry_t **arr, UINTN *cap, UINTN need) {
    UINTN ncap = (*cap) ? *cap : 64;
    while (ncap < need) ncap *= 2;
    fb_entry_t *na = efi_allocate_pool(ncap * sizeof(fb_entry_t));
    if (!na) return 0;
    for (UINTN i = 0; i < *cap; i++) na[i] = (*arr)[i];
    if (*arr) efi_free_pool(*arr);
    *arr = na;
    *cap = ncap;
    return 1;
}

int fb_list(fb_t *s) {
    if (!s || s->vol_cur >= s->vol_count) return 0;
    fb_clear_entries(s);

    EFI_FILE_PROTOCOL *root = open_root_on_handle(s->vols[s->vol_cur].handle);
    if (!root) return 0;
    EFI_FILE_PROTOCOL *dir = NULL;
    EFI_STATUS os = root->Open(root, &dir, s->path, EFI_FILE_MODE_READ, 0);
    if ((EFI_ERROR(os) || !dir) && s->path[0] == '\\' && s->path[1] == 0) {
        os = root->Open(root, &dir, L"", EFI_FILE_MODE_READ, 0);
    }
    if (EFI_ERROR(os) || !dir) {
        root->Close(root);
        return 0;
    }

    UINTN cap = 0;
    fb_entry_t *arr = NULL;
    UINTN count = 0;
    int truncated = 0;
    int stop = 0;

    UINT8 sbuf[1024] __attribute__((aligned(8)));
    for (UINTN reads = 0; reads < FB_MAX_READS && !stop; reads++) {
        if (count >= FB_MAX_ENTRIES) { truncated = 1; break; }
        UINT8 *buf = sbuf;
        UINT8 *heap = NULL;
        UINTN size = sizeof(sbuf);
        UINTN bufsize = sizeof(sbuf);
        EFI_STATUS st = dir->Read(dir, &size, buf);
        if (st == EFI_BUFFER_TOO_SMALL && size > sizeof(sbuf)) {
            if (size > 65536) { truncated = 1; break; }
            heap = efi_allocate_pool(size);
            if (!heap) break;
            buf = heap;
            bufsize = size;
            st = dir->Read(dir, &size, buf);
        }
        if (EFI_ERROR(st) || size == 0) {
            if (heap) efi_free_pool(heap);
            break;
        }
        if (size > bufsize) size = bufsize;

        UINT8 *ptr = buf;
        UINT8 *end = buf + size;
        while (ptr + SIZE_OF_EFI_FILE_INFO + sizeof(CHAR16) <= end &&
               count < FB_MAX_ENTRIES && !stop) {
            EFI_FILE_INFO *info = (EFI_FILE_INFO *)ptr;
            UINTN avail = (UINTN)(end - ptr);
            CHAR16 *fn = info->FileName;
            UINTN fn_max = (avail - SIZE_OF_EFI_FILE_INFO) / sizeof(CHAR16);
            int keep = 1;
            if (fn_max >= 2 && fn[0] == '.' &&
                (fn[1] == 0 || (fn_max >= 3 && fn[1] == '.' && fn[2] == 0)))
                keep = 0;
            if (keep) {
                UINTN nlen = 0;
                while (nlen < fn_max && fn[nlen]) nlen++;
                if (nlen > 0 && nlen < FB_NAME_MAX) {
                    if (count == cap && !fb_grow(&arr, &cap, count + 1)) {
                        truncated = 1; stop = 1; break;
                    }
                    fb_entry_t *e = &arr[count++];
                    e->name = efi_allocate_pool((nlen + 1) * sizeof(CHAR16));
                    if (!e->name) {
                        count--; truncated = 1; stop = 1; break;
                    }
                    for (UINTN i = 0; i <= nlen; i++) e->name[i] = fn[i];
                    e->size = info->FileSize;
                    e->is_dir = (info->Attribute & EFI_FILE_DIRECTORY) != 0;
                }
            }
            UINTN step = info->Size;
            if (step < SIZE_OF_EFI_FILE_INFO + sizeof(CHAR16) ||
                step > (UINTN)(end - ptr)) break;
            ptr += step;
        }
        if (heap) efi_free_pool(heap);
        if (count >= FB_MAX_ENTRIES) truncated = 1;
    }

    dir->Close(dir);
    root->Close(root);

    if (count > 1) {
        fb_entry_t *tmp = efi_allocate_pool(count * sizeof(fb_entry_t));
        if (tmp) {
            for (UINTN i = 0; i < count; i++) tmp[i] = arr[i];
            for (UINTN w = 1; w < count; w *= 2) {
                for (UINTN lo = 0; lo < count - w; lo += 2 * w) {
                    UINTN mid = lo + w;
                    UINTN hi = mid + w;
                    if (hi > count) hi = count;
                    UINTN i = lo, j = mid, k = lo;
                    while (i < mid && j < hi)
                        tmp[k++] = (fb_compare(&arr[i], &arr[j]) <= 0) ? arr[i++] : arr[j++];
                    while (i < mid) tmp[k++] = arr[i++];
                    while (j < hi)  tmp[k++] = arr[j++];
                }
                for (UINTN i = 0; i < count; i++) arr[i] = tmp[i];
            }
            efi_free_pool(tmp);
        }
    }

    s->entries = arr;
    s->entry_count = count;
    s->truncated = truncated;
    return count > 0;
}

int fb_enter(fb_t *s) {
    if (!s || s->cursor >= s->entry_count) return 0;
    fb_entry_t *e = &s->entries[s->cursor];
    if (!e->is_dir) return 0;

    UINTN plen = 0;
    while (s->path[plen]) plen++;
    UINTN nlen = 0;
    while (e->name[nlen]) nlen++;
    if (plen + nlen + 2 >= FB_PATH_MAX) return 0;

    if (plen == 1 && s->path[0] == '\\') {
        plen = 0;
    }
    CHAR16 tmp[FB_PATH_MAX];
    UINTN t = 0;
    for (UINTN i = 0; i < plen; i++) tmp[t++] = s->path[i];
    if (t == 0 || tmp[t-1] != '\\') tmp[t++] = '\\';
    for (UINTN i = 0; i < nlen; i++) tmp[t++] = e->name[i];
    tmp[t++] = '\\';
    tmp[t] = 0;
    for (UINTN i = 0; i <= t; i++) s->path[i] = tmp[i];

    fb_list(s);
    return 1;
}

int fb_up(fb_t *s) {
    if (!s) return 0;
    UINTN plen = 0;
    while (s->path[plen]) plen++;
    if (plen <= 1) return 0;
    UINTN cut = plen - 1;
    while (cut > 0 && s->path[cut-1] != '\\') cut--;
    if (cut == 0) {
        s->path[0] = '\\';
        s->path[1] = 0;
    } else {
        s->path[cut] = 0;
    }
    fb_list(s);
    return 1;
}

void fb_switch_volume(fb_t *s, int dir) {
    if (!s || s->vol_count < 2) return;
    INTN nv = (INTN)s->vol_cur + dir;
    if (nv < 0) nv = (INTN)s->vol_count - 1;
    if (nv >= (INTN)s->vol_count) nv = 0;
    s->vol_cur = (UINTN)nv;
    fb_strcpy(s->path, L"\\");
    fb_list(s);
}

void fb_move(fb_t *s, int delta) {
    if (!s || s->entry_count == 0) return;
    INTN nc = (INTN)s->cursor + delta;
    if (nc < 0) nc = 0;
    if (nc >= (INTN)s->entry_count) nc = (INTN)s->entry_count - 1;
    s->cursor = (UINTN)nc;
}

fb_entry_t* fb_cursor(fb_t *s) {
    if (!s || s->cursor >= s->entry_count) return NULL;
    return &s->entries[s->cursor];
}

static void fb_set_override_uuid(fb_t *s, struct gui_state *gui) {
    CHAR16 *uuid = s->vols[s->vol_cur].uuid;
    if (gui->override_uuid) { efi_free_pool(gui->override_uuid); gui->override_uuid = NULL; }
    if (uuid && uuid[0]) gui->override_uuid = efi_strdup(uuid);
}

int fb_boot_apply(fb_t *s, struct gui_state *gui) {
    if (!s || !gui) return 0;
    fb_entry_t *e = fb_cursor(s);
    if (!e || e->is_dir) return 0;

    UINTN plen = 0;
    while (s->path[plen]) plen++;
    UINTN nlen = 0;
    while (e->name[nlen]) nlen++;
    CHAR16 full[FB_PATH_MAX + FB_NAME_MAX];
    if (plen + nlen + 2 >= FB_PATH_MAX + FB_NAME_MAX) return 0;
    UINTN t = 0;
    for (UINTN i = 0; i < plen; i++) full[t++] = s->path[i];
    if (t == 0 || full[t-1] != '\\') full[t++] = '\\';
    for (UINTN i = 0; i < nlen; i++) full[t++] = e->name[i];
    full[t] = 0;

    if (fb_is_initrd(e->name)) {
        if (gui->override_initrd_path) efi_free_pool(gui->override_initrd_path);
        gui->override_initrd_path = efi_strdup(full);
        gui->override_initrd_set = 1;
        return 2;
    }

    if (gui->override_kernel_path) efi_free_pool(gui->override_kernel_path);
    gui->override_kernel_path = efi_strdup(full);
    fb_set_override_uuid(s, gui);
    gui->override_volume = s->vols[s->vol_cur].handle;

    if (fb_is_kernel(e->name) && !fb_is_efi(e->name)) {
        for (UINTN i = 0; i < s->entry_count; i++) {
            fb_entry_t *cand = &s->entries[i];
            if (cand == e || cand->is_dir || !fb_is_initrd(cand->name)) continue;
            UINTN cn = 0;
            while (cand->name[cn]) cn++;
            if (plen + cn + 2 >= FB_PATH_MAX + FB_NAME_MAX) break;
            CHAR16 ip[FB_PATH_MAX + FB_NAME_MAX];
            UINTN k = 0;
            for (UINTN j = 0; j < plen; j++) ip[k++] = s->path[j];
            if (k == 0 || ip[k-1] != '\\') ip[k++] = '\\';
            for (UINTN j = 0; j < cn; j++) ip[k++] = cand->name[j];
            ip[k] = 0;
            if (gui->override_initrd_path) efi_free_pool(gui->override_initrd_path);
            gui->override_initrd_path = efi_strdup(ip);
            gui->override_initrd_set = 1;
            break;
        }
    }
    return 1;
}
