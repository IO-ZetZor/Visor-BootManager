/* config_bls.c - Boot Loader Spec / ostree entry discovery (feature: bls) */
#include "config_internal.h"

#define MAX_BLS 48

typedef struct {
    CHAR16 *title, *version, *kernel, *efi, *initrd, *options, *machine, *conf;
    CHAR16 *arch, *sort_key;
    int tries_left, tries_done, ot_idx;
    EFI_HANDLE volume;
} bls_rec_t;

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

static void bls_accum(CHAR16 **dst, CHAR16 *val) {
    if (!val || !val[0]) return;
    if (!*dst) { *dst = efi_strdup(val); return; }
    UINTN la = 0; while ((*dst)[la]) la++;
    UINTN lb = 0; while (val[lb]) lb++;
    CHAR16 *buf = efi_allocate_pool((la + 1 + lb + 1) * sizeof(CHAR16));
    if (!buf) return;
    for (UINTN i = 0; i < la; i++) buf[i] = (*dst)[i];
    buf[la] = ' ';
    for (UINTN i = 0; i <= lb; i++) buf[la + 1 + i] = val[i];
    efi_free_pool(*dst);
    *dst = buf;
}

static int bls_arch_ok(CHAR16 *arch) {
    if (!arch || !arch[0]) return 1;
#if defined(__x86_64__)
    return (wcs16casecmp(arch, L"x64") == 0 ||
            wcs16casecmp(arch, L"IA32") == 0);
#elif defined(__aarch64__)
    return (wcs16casecmp(arch, L"AA64") == 0);
#elif defined(__riscv)
    return (wcs16casecmp(arch, L"RISCV64") == 0);
#else
    return 1;
#endif
}

static int bls_deploy_precedes(bls_rec_t *a, bls_rec_t *b, int ia, int ib) {
    int oa = a->ot_idx, ob = b->ot_idx;
    if (oa >= 0 || ob >= 0) {
        if (oa != ob) return oa > ob;
    }
    if (a->sort_key && b->sort_key) {
        int c = efi_strcmp(a->sort_key, b->sort_key);
        if (c != 0) return c < 0;
    }
    if (a->sort_key && !b->sort_key) return 1;
    if (!a->sort_key && b->sort_key) return 0;
    return ia < ib;
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
            NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, -1, 0, -1, volume
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
                if      ((v = bls_field(line, L"title")))         { if (!r.title)   r.title   = efi_strdup(v); }
                else if ((v = bls_field(line, L"version")))       { if (!r.version) r.version = efi_strdup(v); }
                else if ((v = bls_field(line, L"linux")))         { if (!r.kernel)  r.kernel  = efi_strdup(v); }
                else if ((v = bls_field(line, L"efi")))           { if (!r.efi)     r.efi     = efi_strdup(v); }
                else if ((v = bls_field(line, L"uki")))            { if (!r.efi)     r.efi     = efi_strdup(v); }
                else if ((v = bls_field(line, L"initrd")))        { bls_accum(&r.initrd, v); }
                else if ((v = bls_field(line, L"options")))       { bls_accum(&r.options, v); }
                else if ((v = bls_field(line, L"machine-id")))    { if (!r.machine) r.machine = efi_strdup(v); }
                else if ((v = bls_field(line, L"architecture")))  { if (!r.arch)    r.arch    = efi_strdup(v); }
                else if ((v = bls_field(line, L"sort-key")))      { if (!r.sort_key) r.sort_key = efi_strdup(v); }
            }
            start = end + 1;
        }
        efi_free_pool(buf);

        if (!r.kernel && r.efi) { r.kernel = r.efi; r.efi = NULL; }
        if (r.title && r.kernel && bls_arch_ok(r.arch)) {
            r.conf = efi_strdup(path);
            bls_tries(name, &r.tries_left, &r.tries_done);
            r.ot_idx = bls_ostree_idx(r.title);
            recs[(*nrec)++] = r;
        } else {
            if (r.title) efi_free_pool(r.title);
            if (r.version) efi_free_pool(r.version);
            if (r.kernel) efi_free_pool(r.kernel);
            if (r.efi) efi_free_pool(r.efi);
            if (r.initrd) efi_free_pool(r.initrd);
            if (r.options) efi_free_pool(r.options);
            if (r.machine) efi_free_pool(r.machine);
            if (r.arch) efi_free_pool(r.arch);
            if (r.sort_key) efi_free_pool(r.sort_key);
        }
    }
    d->Close(d);
}

int bls_detect(config_t *config, int quick) {
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
            int b = a - 1;
            while (b >= 0 && bls_deploy_precedes(&recs[key], &recs[idx[b]], key, idx[b])) {
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
