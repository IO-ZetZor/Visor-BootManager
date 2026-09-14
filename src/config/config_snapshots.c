/* config_snapshots.c - snapper/timeshift snapshot entries (feature: snapshots) */
#include "config_internal.h"

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
    UINT64 deadline_ms;
    int    timed_out;
} snap_ctx_t;

#define SNAP_SCAN_BUDGET_MS 8000

static int snap_out_of_time(snap_ctx_t *c) {
    if (c->timed_out) return 1;
    if (!c->deadline_ms) return 0;
    if (efi_get_tick() < c->deadline_ms) return 0;
    c->timed_out = 1;
    efi_log(L"WARN: snapshot auto-detection hit its time budget - "
            L"giving up (set snapshots=0 or use snapshots.conf)");
    return 1;
}

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
    if (snap_out_of_time(c)) return 0;
    CHAR16 dirp[MAX_PATH];
    SPrint(dirp, sizeof(dirp), L"%s\\.snapshots", base);
    EFI_FILE_PROTOCOL *d = efi_open_dir(c->root, dirp);
    if (!d) return 0;

    UINTN ids[MAX_SNAPS];
    UINTN n = 0;
    CHAR16 name[64];
    int is_dir;
    while (efi_read_dirent(d, name, 64, &is_dir) && n < MAX_SNAPS) {
        if (snap_out_of_time(c)) break;
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
    if (snap_out_of_time(c)) return 0;
    EFI_FILE_PROTOCOL *d = efi_open_dir(c->root, L"\\timeshift-btrfs\\snapshots");
    if (!d) return 0;

    CHAR16 tags[32][20];
    UINTN n = 0;
    CHAR16 name[64];
    int is_dir;
    while (efi_read_dirent(d, name, 64, &is_dir) && n < 32) {
        if (snap_out_of_time(c)) break;
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
    if (snap_out_of_time(c)) return 0;
    CHAR16 dirp[MAX_PATH];
    SPrint(dirp, sizeof(dirp), L"%s\\%s", base, dirname);
    EFI_FILE_PROTOCOL *d = efi_open_dir(c->root, dirp);
    if (!d) return 0;

    CHAR16 names[32][64];
    UINTN n = 0;
    CHAR16 name[64];
    int is_dir;
    while (efi_read_dirent(d, name, 64, &is_dir) && n < 32) {
        if (snap_out_of_time(c)) break;
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
    ctx.timed_out = 0;
    ctx.deadline_ms = efi_get_tick() + SNAP_SCAN_BUDGET_MS;
    const CHAR16 *src = L"";

    int have_uuid = e->uuid && e->uuid[0];
    for (int pass = have_uuid ? 0 : 1; pass < 2 && !ctx.made && !ctx.timed_out; pass++) {
        for (UINTN v = 0; v < nvol && !ctx.made && !ctx.timed_out; v++) {
            if (pass == 0 &&
                !efi_handle_matches_partition_uuid(vols[v], e->uuid))
                continue;

            BS->SetWatchdogTimer(0, 0, 0, NULL);
            CHAR16 d[80];
            SPrint(d, sizeof(d), L"config: snapshot scan on volume %d of %d",
                   (int)v + 1, (int)nvol);
            efi_log(d);

            EFI_FILE_PROTOCOL *root = root_from_handle(vols[v]);
            if (!root) continue;
            ctx.root = root;

            for (UINTN b = 0; b < nb && !ctx.timed_out; b++)
                scan_snapper(&ctx, bases[b]);
            if (ctx.made) src = L"snapper";

            if (!ctx.made && scan_timeshift(&ctx)) src = L"timeshift";

            if (!ctx.made) {
                for (UINTN b = 0; b < nb && !ctx.made && !ctx.timed_out; b++) {
                    scan_plain_dir(&ctx, bases[b], L".snapshots");
                    scan_plain_dir(&ctx, bases[b], L"snapshots");
                    scan_plain_dir(&ctx, bases[b], L"@snapshots");
                }
                if (ctx.made) src = L"btrfs";
            }
            root->Close(root);
        }
        if (pass == 0 && !ctx.made && !ctx.timed_out)
            efi_log(L"config: no snapshots on uuid= volume - trying all volumes");
    }
    efi_free_pool(vols);

    if (!ctx.made) {
        if (!ctx.timed_out)
            efi_log(L"config: no snapshots found on any volume - continuing");
        return;
    }

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

void snapshots_apply(config_t *config) {
    if (config->snapshots_mode == 0) return;
    if (load_snapshots(config)) return;
    if (config->snapshots_mode != 2) snapshots_autodetect(config);
}
