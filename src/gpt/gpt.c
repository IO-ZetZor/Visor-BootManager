/* gpt.c - GPT header/entry parsing and cross-copy comparison */
#include "gpt_internal.h"

static UINT32 gpt_crc32_tab[256];
static int    gpt_crc32_ready;

static void gpt_crc32_init(void) {
    if (gpt_crc32_ready) return;
    for (UINT32 i = 0; i < 256; i++) {
        UINT32 c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0u);
        gpt_crc32_tab[i] = c;
    }
    gpt_crc32_ready = 1;
}

UINT32 gpt_crc32(UINT32 seed, const UINT8 *data, UINTN len) {
    gpt_crc32_init();
    UINT32 reg = ~seed;
    for (UINTN i = 0; i < len; i++)
        reg = gpt_crc32_tab[(reg ^ data[i]) & 0xFFu] ^ (reg >> 8);
    return ~reg;
}

void* gpt_alloc(UINTN size) {
    if (!size) return NULL;
    void *p = efi_allocate_pool(size);
    if (p) ZeroMem(p, size);
    return p;
}

void gpt_free(void *p) {
    efi_free_pool(p);
}

gpt_status_t gpt_parse_header(const gpt_dev_t *dev, UINT64 expected_lba,
                                     const UINT8 *sector, gpt_table_t *t) {
    gpt_header_t *h = &t->hdr;
    ZeroMem(h, sizeof(*h));

    const UINT8 *sig = sector + 0;
    if (CompareMem(sig, "EFI PART", 8) != 0) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_INVALID_SIGNATURE;
        return GPT_INVALID;
    }
    t->present = 1;

    h->signature     = le64(sector + 0);
    h->revision      = le32(sector + 8);
    h->header_size   = le32(sector + 12);
    h->header_crc32  = le32(sector + 16);
    h->reserved      = le32(sector + 20);
    h->current_lba   = le64(sector + 24);
    h->backup_lba    = le64(sector + 32);
    h->first_usable_lba = le64(sector + 40);
    h->last_usable_lba  = le64(sector + 48);
    RCOPY(h->disk_guid, sector + 56, 16);
    h->entry_lba     = le64(sector + 72);
    h->entry_count   = le32(sector + 80);
    h->entry_size    = le32(sector + 84);
    h->entries_crc32 = le32(sector + 88);

    t->header_crc_stored   = h->header_crc32;
    UINT32 saved_crc = h->header_crc32;

    UINT32 hsize = h->header_size;
    if (hsize < GPT_HEADER_SIZE_MIN ||
        hsize > dev->sector_size ||
        hsize > GPT_SECTOR_SIZE_MAX) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_INVALID_HEADER_SIZE;
        return GPT_INVALID;
    }

    UINT8 tmp[GPT_SECTOR_SIZE_MAX];
    RCOPY(tmp, sector, hsize);
    put_le32(tmp + 16, 0);
    UINT32 calc = gpt_crc32(0, tmp, hsize);
    t->header_crc_computed = calc;
    t->header_crc_ok = (calc == saved_crc);

    if (h->revision != GPT_REVISION_1_0) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_INVALID_REVISION;
        return GPT_INVALID;
    }

    if (h->current_lba != expected_lba) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_INVALID_MY_LBA;
        return GPT_INVALID;
    }
    if (h->backup_lba == 0 || h->backup_lba >= dev->total_sectors) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_INVALID_ALTERNATE_LBA;
        return GPT_INVALID;
    }
    if (h->first_usable_lba == 0 ||
        h->first_usable_lba > h->last_usable_lba ||
        h->last_usable_lba >= dev->total_sectors) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_INVALID_USABLE_RANGE;
        return GPT_INVALID;
    }

    if (h->entry_count == 0 || h->entry_count > GPT_ENTRY_COUNT_MAX) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_INVALID_ENTRY_COUNT;
        return GPT_INVALID;
    }
    if (h->entry_size < GPT_ENTRY_SIZE_MIN || h->entry_size > GPT_ENTRY_SIZE_MAX ||
        (h->entry_size % GPT_ENTRY_SIZE_MIN) != 0 ||
        (h->entry_size & (h->entry_size - 1)) != 0) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_INVALID_ENTRY_SIZE;
        return GPT_INVALID;
    }
    if ((UINT64)h->entry_count * (UINT64)h->entry_size > GPT_ARRAY_BYTES_MAX) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_ARRAY_TOO_LARGE;
        return GPT_INVALID;
    }

    {
        UINT64 abytes  = (UINT64)h->entry_count * (UINT64)h->entry_size;
        UINT64 asect   = (abytes + dev->sector_size - 1) / dev->sector_size;
        UINT64 last    = dev->total_sectors - 1;
        if (asect + 2 > dev->total_sectors ||
            h->first_usable_lba < 2 + asect ||
            h->last_usable_lba > last - 1 - asect) {
            t->header_status = GPT_INVALID;
            t->header_reason = GPT_R_INVALID_USABLE_RANGE;
            return GPT_INVALID;
        }
    }

    t->geometry_ok = 1;

    if (!t->header_crc_ok) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_HEADER_CRC_MISMATCH;
    } else {
        t->header_status = GPT_VALID;
        t->header_reason = GPT_R_NONE;
    }
    return t->header_status;
}

static void gpt_parse_entry(const UINT8 *e, gpt_entry_t *out) {
    ZeroMem(out, sizeof(*out));
    RCOPY(out->type_guid, e + 0, 16);
    RCOPY(out->uniq_guid, e + 16, 16);
    out->first_lba = le64(e + 32);
    out->last_lba  = le64(e + 40);
    out->attributes = le64(e + 48);
    for (UINTN i = 0; i < GPT_NAME_CHARS; i++)
        out->name[i] = le16(e + 56 + i * 2);

    out->used = !guid_is_zero(out->type_guid) &&
                !(out->first_lba == 0 && out->last_lba == 0);
}

static gpt_reason_t gpt_check_entries_layout(const gpt_entry_t *ents,
                                             UINT32 ent_count,
                                             UINT64 first_us,
                                             UINT64 last_us,
                                             UINT64 total,
                                             UINT32 *used_out,
                                             gpt_reason_t *first_bad,
                                             INTN *bad_at) {
    UINT32 used = 0;
    *first_bad = GPT_R_NONE;
    if (bad_at) *bad_at = -1;
    for (UINT32 i = 0; i < ent_count; i++) {
        const gpt_entry_t *e = &ents[i];
        if (!e->used) continue;
        used++;

        if (!guid_is_valid(e->uniq_guid)) {
            *first_bad = GPT_R_INVALID_PARTITION_GUID;
            if (bad_at) *bad_at = (INTN)i;
            return GPT_R_INVALID_PARTITION_GUID;
        }

        if ((e->first_lba == 0) != (e->last_lba == 0)) {
            *first_bad = GPT_R_HALF_USED_ENTRY;
            if (bad_at) *bad_at = (INTN)i;
            return GPT_R_HALF_USED_ENTRY;
        }
        if (e->first_lba > e->last_lba) {
            *first_bad = GPT_R_INVALID_PARTITION_RANGE;
            if (bad_at) *bad_at = (INTN)i;
            return GPT_R_INVALID_PARTITION_RANGE;
        }
        if (e->last_lba >= total) {
            *first_bad = GPT_R_PARTITION_OFF_DISK;
            if (bad_at) *bad_at = (INTN)i;
            return GPT_R_PARTITION_OFF_DISK;
        }

        if (e->first_lba < first_us) {
            *first_bad = GPT_R_METADATA_OVERLAP;
            if (bad_at) *bad_at = (INTN)i;
            return GPT_R_METADATA_OVERLAP;
        }
        if (e->last_lba > last_us) {
            *first_bad = GPT_R_PARTITION_OFF_DISK;
            if (bad_at) *bad_at = (INTN)i;
            return GPT_R_PARTITION_OFF_DISK;
        }

        for (UINT32 j = 0; j < i; j++) {
            const gpt_entry_t *o = &ents[j];
            if (o->used && CompareMem(e->uniq_guid, o->uniq_guid, 16) == 0) {
                *first_bad = GPT_R_DUPLICATE_PARTITION_GUID;
                if (bad_at) *bad_at = (INTN)i;
                return GPT_R_DUPLICATE_PARTITION_GUID;
            }
        }
    }
    if (used_out) *used_out = used;
    return GPT_R_NONE;
}

static gpt_reason_t gpt_check_overlaps(const gpt_entry_t *ents,
                                       UINT32 ent_count,
                                       INTN *ia, INTN *ib) {
    if (ia) *ia = -1;
    if (ib) *ib = -1;
    for (UINT32 i = 0; i < ent_count; i++) {
        const gpt_entry_t *a = &ents[i];
        if (!a->used) continue;
        for (UINT32 j = i + 1; j < ent_count; j++) {
            const gpt_entry_t *b = &ents[j];
            if (!b->used) continue;
            if (a->first_lba <= b->last_lba && b->first_lba <= a->last_lba) {
                if (ia) *ia = (INTN)i;
                if (ib) *ib = (INTN)j;
                return GPT_R_PARTITION_OVERLAP;
            }
        }
    }
    return GPT_R_NONE;
}

static gpt_status_t gpt_load_entries(gpt_dev_t *dev, gpt_table_t *t, int copy) {
    (void)copy;
    gpt_header_t *h = &t->hdr;
    t->entries_status = GPT_UNKNOWN;
    t->entries_reason = GPT_R_NONE;

    if (!t->geometry_ok) {
        t->entries_status = GPT_INVALID;
        t->entries_reason = t->header_reason;
        return GPT_INVALID;
    }

    UINT64 count   = h->entry_count;
    UINT64 esz     = h->entry_size;
    UINT64 bytes   = count * esz;
    if (bytes > GPT_ARRAY_BYTES_MAX || bytes == 0) {
        t->entries_status = GPT_INVALID;
        t->entries_reason = GPT_R_ARRAY_TOO_LARGE;
        return GPT_INVALID;
    }
    UINT64 sectors = (bytes + dev->sector_size - 1) / dev->sector_size;
    if (sectors == 0) sectors = 1;

    if (h->entry_lba == 0 ||
        h->entry_lba > (UINT64)dev->total_sectors ||
        sectors > (UINT64)dev->total_sectors - h->entry_lba) {
        t->entries_status = GPT_INVALID;
        t->entries_reason = GPT_R_ARRAY_OFF_DISK;
        return GPT_INVALID;
    }

    if (h->entry_lba <= h->last_usable_lba &&
        h->entry_lba + sectors > h->first_usable_lba) {
        t->entries_status = GPT_INVALID;
        t->entries_reason = GPT_R_ARRAY_IN_USABLE_RANGE;
        return GPT_INVALID;
    }

    UINTN alloc_bytes = (UINTN)(sectors * dev->sector_size);
    UINT8 *raw = gpt_alloc(alloc_bytes);
    if (!raw) {
        t->entries_status = GPT_INVALID;
        t->entries_reason = GPT_R_OUT_OF_MEMORY;
        return GPT_INVALID;
    }

    EFI_STATUS st = dev->read(dev, h->entry_lba, (UINTN)sectors, raw);
    if (EFI_ERROR(st)) {
        gpt_free(raw);
        t->entries_status = GPT_INVALID;
        t->entries_reason = GPT_R_DISK_READ_FAILED;
        return GPT_INVALID;
    }
    t->raw = raw;
    t->raw_bytes = alloc_bytes;
    t->array_sectors = sectors;
    t->loaded = 1;
    t->best_effort = !t->header_crc_ok;

    t->entries_crc_stored = h->entries_crc32;
    t->entries_crc_computed = gpt_crc32(0, raw, (UINTN)bytes);
    t->entries_crc_ok = (t->entries_crc_computed == t->entries_crc_stored);

    t->ents = gpt_alloc((UINTN)count * sizeof(gpt_entry_t));
    if (!t->ents) {
        t->entries_status = GPT_INVALID;
        t->entries_reason = GPT_R_OUT_OF_MEMORY;
        return GPT_INVALID;
    }
    t->ent_count = (UINT32)count;
    for (UINT32 i = 0; i < count; i++) {
        gpt_parse_entry(t->raw + (UINTN)i * (UINTN)esz, &t->ents[i]);
    }
    t->used_count = 0;
    for (UINT32 i = 0; i < count; i++) if (t->ents[i].used) t->used_count++;

    if (!t->entries_crc_ok) {
        t->entries_status = GPT_INVALID;
        t->entries_reason = GPT_R_ARRAY_CRC_MISMATCH;
    } else {
        t->entries_status = GPT_VALID;
        t->entries_reason = GPT_R_NONE;
    }
    return t->entries_status;
}

static void gpt_check_layout(gpt_table_t *t, UINT64 total_sectors) {
    t->layout_bad = -1;
    t->layout_bad2 = -1;
    if (!t->ents || t->ent_count == 0) {
        t->layout_status = (t->geometry_ok && t->entries_crc_ok)
            ? GPT_VALID : GPT_INVALID;
        t->layout_reason = GPT_R_NONE;
        return;
    }
    UINT32 used = 0;
    gpt_reason_t res = gpt_check_entries_layout(t->ents, t->ent_count,
                                                t->hdr.first_usable_lba,
                                                t->hdr.last_usable_lba,
                                                total_sectors,
                                                &used, &t->layout_reason,
                                                &t->layout_bad);
    if (res == GPT_R_NONE)
        res = gpt_check_overlaps(t->ents, t->ent_count,
                                 &t->layout_bad, &t->layout_bad2);
    if (res != GPT_R_NONE) {
        t->layout_status = GPT_INVALID;
        t->layout_reason = res;
        return;
    }
    t->layout_status = GPT_VALID;
    t->layout_reason = GPT_R_NONE;
}

void gpt_check_mbr(gpt_diag_t *diag, const UINT8 *lba0) {
    diag->mbr_status = GPT_VALID;
    diag->mbr_reason = GPT_R_NONE;
    diag->mbr_protective = 1;
    diag->mbr_other_parts = 0;

    if (lba0[510] != 0x55 || lba0[511] != 0xAA) {
        diag->mbr_status = GPT_WARNING;
        diag->mbr_reason = GPT_R_MBR_NO_SIGNATURE;
        diag->mbr_protective = 0;
        return;
    }

    UINT8 ptype = lba0[446 + 4];
    int protective = (ptype == 0xEE);
    if (!protective) {
        diag->mbr_status = GPT_WARNING;
        diag->mbr_reason = GPT_R_MBR_NOT_PROTECTIVE;
        diag->mbr_protective = 0;
    } else if (le32(lba0 + 446 + 8) == 0) {
        diag->mbr_status = GPT_WARNING;
        diag->mbr_reason = GPT_R_MBR_WRONG_SIZE;
    }

    for (int i = 1; i < 4; i++) {
        const UINT8 *e = lba0 + 446 + 16 * i;
        if (e[0] != 0x00 || e[4] != 0x00 ||
            e[8] != 0 || e[12] != 0) {
            diag->mbr_other_parts = 1;
            if (protective) {
                diag->mbr_status = GPT_WARNING;
                diag->mbr_reason = GPT_R_MBR_HYBRID;
            }
        }
    }
}

void gpt_load_ct(gpt_dev_t *dev, UINT64 lba, gpt_table_t *t, int copy) {
    (void)copy;
    ZeroMem(t, sizeof(*t));
    t->header_status = GPT_UNKNOWN;
    t->entries_status = GPT_UNKNOWN;
    t->layout_status = GPT_UNKNOWN;

    if (dev->sector_size < GPT_SECTOR_SIZE_MIN ||
        dev->sector_size > GPT_SECTOR_SIZE_MAX) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_BAD_SECTOR_SIZE;
        return;
    }
    if (dev->total_sectors <= (UINT64)33) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_DISK_TOO_SMALL;
        return;
    }
    if (lba == 0 || lba >= dev->total_sectors) {
        t->present = 0;
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_ARRAY_OFF_DISK;
        return;
    }

    UINT8 *sector = gpt_alloc(dev->sector_size);
    if (!sector) {
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_OUT_OF_MEMORY;
        return;
    }
    EFI_STATUS st = dev->read(dev, lba, 1, sector);
    if (EFI_ERROR(st)) {
        gpt_free(sector);
        t->header_status = GPT_INVALID;
        t->header_reason = GPT_R_DISK_READ_FAILED;
        return;
    }

    gpt_status_t hs = gpt_parse_header(dev, lba, sector, t);

    if (hs == GPT_VALID && !t->hdr_raw) {
        UINT32 hsize = t->hdr.header_size;
        UINT8 *hr = gpt_alloc(hsize);
        if (hr) {
            RCOPY(hr, sector, hsize);
            t->hdr_raw = hr;
            t->hdr_raw_bytes = hsize;
        }
    }
    gpt_free(sector);

    if (hs != GPT_VALID) {

        if (t->geometry_ok)
            gpt_load_entries(dev, t, copy);
        if (t->loaded) gpt_check_layout(t, dev->total_sectors);
        return;
    }

    gpt_load_entries(dev, t, copy);
    gpt_check_layout(t, dev->total_sectors);
}

static int gpt_entries_equal(const gpt_table_t *a, const gpt_table_t *b) {
    if (a->raw_bytes != b->raw_bytes) return 0;
    return a->raw && b->raw && CompareMem(a->raw, b->raw, a->raw_bytes) == 0;
}

void gpt_compare(const gpt_diag_t *diag, gpt_cmp_t *cmp) {
    const gpt_table_t *p = &diag->primary;
    const gpt_table_t *b = &diag->backup;

    if (diag->cmp.crc_only) {
        ZeroMem(cmp, sizeof(*cmp));
        cmp->kind = GPT_CMP_IDENTICAL;
        cmp->crc_only = 1;
        cmp->raw_identical = 1;
        cmp->first_diff = -1;
        cmp->diff = 0;
        return;
    }

    ZeroMem(cmp, sizeof(*cmp));
    cmp->kind = GPT_CMP_UNKNOWN;

    int p_ok = p->present && p->header_status == GPT_VALID &&
               p->entries_status == GPT_VALID && p->layout_status == GPT_VALID;
    int b_ok = b->present && b->header_status == GPT_VALID &&
               b->entries_status == GPT_VALID && b->layout_status == GPT_VALID;

    if (!p_ok && !b_ok) {
        cmp->kind = (p->present || b->present) ? GPT_CMP_AMBIGUOUS
                                               : GPT_CMP_BOTH_INVALID;
        return;
    }
    if (!p_ok)          { cmp->kind = GPT_CMP_PRIMARY_INVALID; return; }
    if (!b_ok)          { cmp->kind = GPT_CMP_BACKUP_INVALID; return; }

    cmp->kind = GPT_CMP_IDENTICAL;
    cmp->raw_identical = gpt_entries_equal(p, b);
    cmp->first_diff = -1;

    if (CompareMem(p->hdr.disk_guid, b->hdr.disk_guid, 16) != 0) cmp->diff |= GPT_DIFF_DISK_GUID;
    if (p->hdr.entry_count  != b->hdr.entry_count)  cmp->diff |= GPT_DIFF_ENTRY_COUNT;
    if (p->hdr.entry_size   != b->hdr.entry_size)   cmp->diff |= GPT_DIFF_ENTRY_SIZE;
    if (p->hdr.first_usable_lba != b->hdr.first_usable_lba) cmp->diff |= GPT_DIFF_FIRST_USABLE;
    if (p->hdr.last_usable_lba  != b->hdr.last_usable_lba)  cmp->diff |= GPT_DIFF_LAST_USABLE;
    if (p->hdr.entries_crc32 != b->hdr.entries_crc32) cmp->diff |= GPT_DIFF_ARRAY_CRC;
    if (p->used_count != b->used_count) cmp->diff |= GPT_DIFF_PART_COUNT;

    UINT32 n = p->ent_count < b->ent_count ? p->ent_count : b->ent_count;
    for (UINT32 i = 0; i < n; i++) {
        const gpt_entry_t *a = &p->ents[i];
        const gpt_entry_t *c = &b->ents[i];
        int differ = 0;
        if (CompareMem(a->type_guid, c->type_guid, 16) != 0) { cmp->diff |= GPT_DIFF_PART_TYPE; differ = 1; }
        if (CompareMem(a->uniq_guid, c->uniq_guid, 16) != 0) { cmp->diff |= GPT_DIFF_PART_GUID; differ = 1; }
        if (a->first_lba != c->first_lba || a->last_lba != c->last_lba) { cmp->diff |= GPT_DIFF_PART_RANGE; differ = 1; }
        if (a->attributes != c->attributes) { cmp->diff |= GPT_DIFF_PART_ATTRS; differ = 1; }
        if (CompareMem(a->name, c->name, sizeof(a->name)) != 0) { cmp->diff |= GPT_DIFF_PART_NAME; differ = 1; }
        if (differ && cmp->first_diff < 0) cmp->first_diff = (INTN)i;
    }
    if (cmp->diff != 0) cmp->kind = GPT_CMP_DIFFERENT;
}
