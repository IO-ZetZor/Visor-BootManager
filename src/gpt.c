
#include <efi.h>
#include <efilib.h>

#include "gpt.h"

void* efi_allocate_pool(UINTN size);
void  efi_free_pool(void *ptr);

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

/* Standard CRC32 (poly 0xEDB88320), as required by the UEFI GPT spec.
 * `seed` is the running value from a previous call, or 0 to start a new one;
 * the result is always a finished CRC, so single-shot callers can just use
 * gpt_crc32(0, buf, len) and chained callers can feed the return value back in. */
UINT32 gpt_crc32(UINT32 seed, const UINT8 *data, UINTN len) {
    gpt_crc32_init();
    UINT32 reg = ~seed;
    for (UINTN i = 0; i < len; i++)
        reg = gpt_crc32_tab[(reg ^ data[i]) & 0xFFu] ^ (reg >> 8);
    return ~reg;
}

static UINT16 le16(const UINT8 *p) {
    return (UINT16)((UINT16)p[0] | ((UINT16)p[1] << 8));
}

static UINT32 le32(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) |
           ((UINT32)p[2] << 16) | ((UINT32)p[3] << 24);
}

static UINT64 le64(const UINT8 *p) {
    return (UINT64)le32(p) | ((UINT64)le32(p + 4) << 32);
}

static void put_le32(UINT8 *p, UINT32 v) {
    p[0] = (UINT8)(v);
    p[1] = (UINT8)(v >> 8);
    p[2] = (UINT8)(v >> 16);
    p[3] = (UINT8)(v >> 24);
}

static void put_le64(UINT8 *p, UINT64 v) {
    put_le32(p, (UINT32)v);
    put_le32(p + 4, (UINT32)(v >> 32));
}

static int guid_is_zero(const UINT8 g[16]) {
    for (int i = 0; i < 16; i++) if (g[i]) return 0;
    return 1;
}

static int guid_is_valid(const UINT8 g[16]) {

    return !guid_is_zero(g);
}

#define RCOPY(dest, src, n)  CopyMem((dest), (void*)(const void*)(src), (n))

const CHAR16* gpt_reason_text(gpt_reason_t reason) {
    switch (reason) {
    case GPT_R_NONE:                      return L"none";
    case GPT_R_DISK_READ_FAILED:          return L"disk read failed";
    case GPT_R_DISK_WRITE_FAILED:         return L"disk write failed";
    case GPT_R_DISK_FLUSH_FAILED:         return L"disk flush failed";
    case GPT_R_OUT_OF_MEMORY:             return L"out of memory";
    case GPT_R_BAD_SECTOR_SIZE:           return L"unsupported sector size";
    case GPT_R_DISK_TOO_SMALL:            return L"disk too small for GPT";
    case GPT_R_NOT_GPT:                   return L"not a GPT disk";
    case GPT_R_INVALID_SIGNATURE:         return L"invalid GPT signature";
    case GPT_R_INVALID_REVISION:          return L"unsupported GPT revision";
    case GPT_R_INVALID_HEADER_SIZE:       return L"invalid header size";
    case GPT_R_HEADER_CRC_MISMATCH:       return L"header CRC mismatch";
    case GPT_R_INVALID_MY_LBA:            return L"invalid current LBA";
    case GPT_R_INVALID_ALTERNATE_LBA:     return L"invalid backup LBA";
    case GPT_R_INVALID_USABLE_RANGE:      return L"invalid usable-LBA range";
    case GPT_R_INVALID_ENTRY_COUNT:       return L"invalid partition-entry count";
    case GPT_R_INVALID_ENTRY_SIZE:        return L"invalid partition-entry size";
    case GPT_R_ARRAY_TOO_LARGE:           return L"partition-entry array too large";
    case GPT_R_ARRAY_OFF_DISK:            return L"partition-entry array off disk";
    case GPT_R_ARRAY_IN_USABLE_RANGE:     return L"partition-entry array overlaps partitions";
    case GPT_R_ARRAY_CRC_MISMATCH:        return L"partition-entry array CRC mismatch";
    case GPT_R_INVALID_PARTITION_RANGE:   return L"partition spans an invalid range";
    case GPT_R_PARTITION_OFF_DISK:        return L"partition extends beyond the disk";
    case GPT_R_PARTITION_OVERLAP:         return L"partitions overlap each other";
    case GPT_R_METADATA_OVERLAP:          return L"partition overlaps GPT metadata";
    case GPT_R_INVALID_PARTITION_GUID:    return L"invalid partition GUID";
    case GPT_R_DUPLICATE_PARTITION_GUID:  return L"duplicate partition GUID";
    case GPT_R_HALF_USED_ENTRY:           return L"partition entry is half used";
    case GPT_R_MBR_NO_SIGNATURE:          return L"no MBR signature";
    case GPT_R_MBR_NOT_PROTECTIVE:        return L"MBR is not a protective MBR";
    case GPT_R_MBR_HYBRID:                return L"MBR has non-protective partitions";
    case GPT_R_MBR_WRONG_SIZE:            return L"MBR partition entry size wrong";
    case GPT_R_GPT_COPIES_DIFFER:         return L"primary and backup GPT differ";
    case GPT_R_BACKUP_NOT_AT_END:         return L"backup GPT is not at the last LBA";
    case GPT_R_UNSAFE_RECOVERY:           return L"recovery is not safe";
    case GPT_R_NO_SAFE_DESTINATION:       return L"no safe destination for primary GPT";
    case GPT_R_DISK_CHANGED:              return L"disk changed since diagnosis";
    case GPT_R_READ_ONLY_MEDIA:           return L"media is read-only";
    case GPT_R_POST_WRITE_VERIFY_FAILED:  return L"post-write verification failed";
    default:                              return L"unknown reason";
    }
}

const CHAR16* gpt_status_text(gpt_status_t status) {
    switch (status) {
    case GPT_UNKNOWN: return L"UNKNOWN";
    case GPT_VALID:   return L"VALID";
    case GPT_WARNING: return L"WARNING";
    case GPT_INVALID: return L"INVALID";
    case GPT_UNSAFE:  return L"UNSAFE";
    default:          return L"UNKNOWN";
    }
}

const CHAR16* gpt_class_text(gpt_class_t klass) {
    switch (klass) {
    case GPT_CLASS_NOT_GPT:                      return L"not a GPT disk";
    case GPT_CLASS_HEALTHY:                      return L"healthy";
    case GPT_CLASS_PRIMARY_CORRUPT_BACKUP_VALID: return L"primary corrupt, backup valid";
    case GPT_CLASS_BACKUP_CORRUPT_PRIMARY_VALID: return L"backup corrupt, primary valid";
    case GPT_CLASS_BOTH_COPIES_CORRUPT:          return L"both GPT copies corrupt";
    case GPT_CLASS_COPIES_DIFFER:                return L"GPT copies differ";
    case GPT_CLASS_INVALID_LAYOUT:               return L"invalid partition layout";
    case GPT_CLASS_UNSAFE_TO_RECOVER:            return L"unsafe to recover";
    default:                                     return L"unknown";
    }
}

static void* gpt_alloc(UINTN size) {
    if (!size) return NULL;
    void *p = efi_allocate_pool(size);
    if (p) ZeroMem(p, size);
    return p;
}

static void gpt_free(void *p) {
    efi_free_pool(p);
}

static gpt_status_t gpt_parse_header(const gpt_dev_t *dev, UINT64 expected_lba,
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

    /* header_size is attacker-controlled: it comes straight off the disk.  Bound
     * it by the CRC scratch buffer as well as the sector, never just the sector. */
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
    /* UEFI requires the entry size to be 128 * 2^n. */
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

    /* The usable range must leave room for both metadata copies.  Without this
     * a header can claim usable space that physically covers its own backup
     * GPT, and a partition spanning it would still validate. */
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

    /* The entry array must live in a metadata region, never inside the usable
     * range - otherwise the "partition table" we trust as a recovery source
     * could actually be filesystem content sitting inside a partition. */
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

static void gpt_check_mbr(gpt_diag_t *diag, const UINT8 *lba0) {
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
        /* A protective entry with a zero size field describes nothing and
         * cannot protect the disk - flag it rather than trusting the layout. */
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

static void gpt_load_ct(gpt_dev_t *dev, UINT64 lba, gpt_table_t *t, int copy) {
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

    /* Keep the raw header bytes we just parsed (header_size covers any
     * vendor-specific extension beyond the 92-byte base).  A rebuild of the
     * other copy must reproduce these bytes exactly, not shrink back to 92. */
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

static void gpt_compare(const gpt_diag_t *diag, gpt_cmp_t *cmp) {
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
        /* GPT metadata exists on the disk but neither copy can be trusted -
         * there is no way to say which (if either) reflects reality. */
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

static void gpt_classify(gpt_diag_t *diag) {
    gpt_table_t *p = &diag->primary;
    gpt_table_t *b = &diag->backup;
    diag->klass = GPT_CLASS_UNKNOWN;
    diag->capability = GPT_RECOVER_NONE;

    int p_present = p->present;
    int b_present = b->present;

    if (!p_present && !b_present) {
        diag->klass = GPT_CLASS_NOT_GPT;
        diag->overall = GPT_INVALID;
        return;
    }

    int p_ok = p_present && p->header_status == GPT_VALID &&
               p->entries_status == GPT_VALID && p->layout_status == GPT_VALID;
    int b_ok = b_present && b->header_status == GPT_VALID &&
               b->entries_status == GPT_VALID && b->layout_status == GPT_VALID;

    if (p_ok && b_ok) {
        if (diag->cmp.kind == GPT_CMP_IDENTICAL) {
            diag->klass = GPT_CLASS_HEALTHY;
            diag->overall = GPT_VALID;
        } else {
            diag->klass = GPT_CLASS_COPIES_DIFFER;
            diag->capability = GPT_RECOVER_MANUAL_ONLY;
            diag->overall = GPT_WARNING;
        }
        return;
    }

    if (p_ok && !b_ok) {
        diag->klass = GPT_CLASS_BACKUP_CORRUPT_PRIMARY_VALID;
        diag->capability = GPT_RECOVER_MANUAL_ONLY;
        diag->overall = GPT_WARNING;
        return;
    }

    if (!p_ok && b_ok) {
        /* Spec CASE B: the primary is unusable and the backup independently
         * validates.  This covers a merely-corrupt primary AND a primary that
         * was wiped outright (signature gone) - a zeroed first sector is one of
         * the most common ways a primary GPT dies, and the backup is what
         * proves the disk is really GPT.
         *
         * The one case we refuse is a disk that now carries a genuine MBR
         * partition table with a stale GPT backup left at the end: restoring
         * there would resurrect a layout the user replaced.  A protective MBR
         * (or an absent/blank one alongside a surviving primary signature) is
         * what tells the two apart. */
        if (!p_present && !diag->mbr_protective) {
            diag->klass = GPT_CLASS_UNSAFE_TO_RECOVER;
            diag->capability = GPT_RECOVER_MANUAL_ONLY;
            diag->overall = GPT_UNSAFE;
            return;
        }
        diag->klass = GPT_CLASS_PRIMARY_CORRUPT_BACKUP_VALID;
        diag->capability = GPT_RECOVER_PRIMARY_FROM_BACKUP;
        diag->overall = GPT_WARNING;
        return;
    }

    /* Neither copy is usable.  If a copy verifies structurally but its
     * partition layout does not, say so specifically - it is a different
     * problem from a failed CRC and needs different handling. */
    if ((p_present && p->header_status == GPT_VALID &&
         p->entries_status == GPT_VALID && p->layout_status == GPT_INVALID) ||
        (b_present && b->header_status == GPT_VALID &&
         b->entries_status == GPT_VALID && b->layout_status == GPT_INVALID)) {
        diag->klass = GPT_CLASS_INVALID_LAYOUT;
        diag->capability = GPT_RECOVER_MANUAL_ONLY;
        diag->overall = GPT_UNSAFE;
        return;
    }

    if (p_present || b_present) {
        diag->klass = GPT_CLASS_BOTH_COPIES_CORRUPT;
        diag->capability = GPT_RECOVER_MANUAL_ONLY;
        diag->overall = GPT_UNSAFE;
        return;
    }

    diag->klass = GPT_CLASS_UNSAFE_TO_RECOVER;
    diag->overall = GPT_UNSAFE;
}

static void gpt_diag_reset(gpt_diag_t *diag) {
    ZeroMem(diag, sizeof(*diag));
    diag->overall = GPT_UNKNOWN;
    diag->mbr_status = GPT_UNKNOWN;
}

static void gpt_note_push(gpt_diag_t *diag, gpt_reason_t reason,
                          gpt_status_t severity, int copy,
                          INTN index, INTN index2) {
    if (!diag) return;
    if (diag->note_count >= GPT_NOTE_MAX) {
        diag->notes_truncated = 1;
        return;
    }
    gpt_note_t *n = &diag->notes[diag->note_count++];
    n->reason = reason;
    n->severity = severity;
    n->copy = copy;
    n->index = index;
    n->index2 = index2;
}

static void gpt_copy_notes(gpt_diag_t *diag, const gpt_table_t *t, int copy) {
    if (!t->present) return;

    if (t->header_status != GPT_VALID) {
        gpt_reason_t r = t->header_reason;
        if (r == GPT_R_NONE) r = GPT_R_INVALID_SIGNATURE;
        gpt_note_push(diag, r, GPT_INVALID, copy, -1, -1);
    }
    if (t->entries_status != GPT_VALID && t->entries_reason != GPT_R_NONE) {
        gpt_note_push(diag, t->entries_reason, GPT_INVALID, copy, -1, -1);
    }
    if (t->layout_status == GPT_INVALID && t->layout_reason != GPT_R_NONE) {
        gpt_note_push(diag, t->layout_reason, GPT_INVALID, copy,
                      t->layout_bad, t->layout_bad2);
    }
}

static void gpt_build_notes(gpt_diag_t *diag) {
    diag->note_count = 0;
    diag->notes_truncated = 0;

    if (diag->mbr_reason != GPT_R_NONE)
        gpt_note_push(diag, diag->mbr_reason, diag->mbr_status,
                      GPT_COPY_NONE, -1, -1);

    if (diag->primary.present) {
        gpt_copy_notes(diag, &diag->primary, GPT_COPY_PRIMARY);
    } else if (diag->primary.header_reason != GPT_R_NONE) {
        gpt_note_push(diag, diag->primary.header_reason, GPT_INVALID,
                      GPT_COPY_PRIMARY, -1, -1);
    }

    if (diag->backup.present) {
        gpt_copy_notes(diag, &diag->backup, GPT_COPY_BACKUP);
    } else if (diag->backup.header_reason != GPT_R_NONE) {
        gpt_note_push(diag, diag->backup.header_reason, GPT_INVALID,
                      GPT_COPY_BACKUP, -1, -1);
    }

    if (diag->cmp.kind == GPT_CMP_DIFFERENT) {
        gpt_note_push(diag, GPT_R_GPT_COPIES_DIFFER, GPT_WARNING,
                      GPT_COPY_NONE, diag->cmp.first_diff, -1);
        gpt_note_push(diag, GPT_R_UNSAFE_RECOVERY, GPT_WARNING,
                      GPT_COPY_NONE, -1, -1);
    }

    if (diag->klass == GPT_CLASS_INVALID_LAYOUT)
        gpt_note_push(diag, GPT_R_UNSAFE_RECOVERY, GPT_UNSAFE,
                      GPT_COPY_NONE, -1, -1);
}

void gpt_diag_free(gpt_diag_t *diag) {
    if (!diag) return;
    if (diag->primary.raw)  { gpt_free(diag->primary.raw);  diag->primary.raw  = NULL; }
    if (diag->primary.ents) { gpt_free(diag->primary.ents); diag->primary.ents = NULL; }
    if (diag->primary.hdr_raw) { gpt_free(diag->primary.hdr_raw); diag->primary.hdr_raw = NULL; }
    if (diag->backup.raw)   { gpt_free(diag->backup.raw);   diag->backup.raw   = NULL; }
    if (diag->backup.ents)  { gpt_free(diag->backup.ents);  diag->backup.ents  = NULL; }
    if (diag->backup.hdr_raw) { gpt_free(diag->backup.hdr_raw); diag->backup.hdr_raw = NULL; }
    gpt_diag_reset(diag);
}

EFI_STATUS gpt_diagnose(gpt_dev_t *dev, int full, gpt_diag_t *out) {
    if (!dev || !out) return EFI_INVALID_PARAMETER;
    gpt_diag_reset(out);

    out->sector_size  = dev->sector_size;
    out->total_sectors = dev->total_sectors;
    out->media_id     = dev->media_id;
    out->read_only    = dev->read_only;
    out->removable    = dev->removable;
    out->overall      = GPT_UNKNOWN;

    if (!dev->read) return EFI_INVALID_PARAMETER;

    /* Reject geometry we cannot represent before doing any arithmetic on it.
     * Everything below assumes total_sectors >= 2 and a sane sector size. */
    if (dev->sector_size < GPT_SECTOR_SIZE_MIN ||
        dev->sector_size > GPT_SECTOR_SIZE_MAX) {
        out->primary.header_status = out->backup.header_status = GPT_INVALID;
        out->primary.header_reason = out->backup.header_reason = GPT_R_BAD_SECTOR_SIZE;
        out->klass = GPT_CLASS_NOT_GPT;
        out->overall = GPT_INVALID;
        return EFI_SUCCESS;
    }
    if (dev->total_sectors <= (UINT64)GPT_MIN_DISK_SECTORS) {
        out->primary.header_status = out->backup.header_status = GPT_INVALID;
        out->primary.header_reason = out->backup.header_reason = GPT_R_DISK_TOO_SMALL;
        out->klass = GPT_CLASS_NOT_GPT;
        out->overall = GPT_INVALID;
        return EFI_SUCCESS;
    }

    UINT8 *lba0 = gpt_alloc(dev->sector_size);
    if (lba0) {
        if (dev->sector_size >= 512) {
            EFI_STATUS st = dev->read(dev, 0, 1, lba0);
            if (EFI_ERROR(st)) out->mbr_status = GPT_UNKNOWN;
            else gpt_check_mbr(out, lba0);
        }
        gpt_free(lba0);
    } else {
        out->mbr_status = GPT_UNKNOWN;
    }

    UINT8 *plba1 = gpt_alloc(dev->sector_size);
    if (plba1) {
        EFI_STATUS st = dev->read(dev, 1, 1, plba1);
        if (EFI_ERROR(st)) {
            out->primary.header_status = GPT_UNKNOWN;
            out->primary.header_reason = GPT_R_DISK_READ_FAILED;
        } else if (CompareMem(plba1, "EFI PART", 8) == 0) {
            gpt_load_ct(dev, 1, &out->primary, GPT_COPY_PRIMARY);
        } else {

            out->primary.present = 0;
            out->primary.header_status = GPT_INVALID;
            out->primary.header_reason = GPT_R_INVALID_SIGNATURE;
        }
        gpt_free(plba1);
    }

    UINT64 last = dev->total_sectors - 1;
    UINT8 *plast = gpt_alloc(dev->sector_size);
    int primary_full = 0;
    if (plast) {
        EFI_STATUS st = dev->read(dev, last, 1, plast);
        if (EFI_ERROR(st)) {
            out->backup.header_status = GPT_UNKNOWN;
            out->backup.header_reason = GPT_R_DISK_READ_FAILED;
            gpt_free(plast);
            plast = NULL;
        } else if (CompareMem(plast, "EFI PART", 8) == 0) {
            if (!full &&
                out->primary.present &&
                out->primary.header_status == GPT_VALID &&
                out->primary.entries_status == GPT_VALID &&
                out->primary.layout_status == GPT_VALID) {

                gpt_table_t bhdr;
                ZeroMem(&bhdr, sizeof(bhdr));
                gpt_status_t hs = gpt_parse_header(dev, last, plast, &bhdr);
                primary_full = (hs == GPT_VALID);
                if (primary_full) {
                    out->backup.hdr = bhdr.hdr;
                    out->backup.present = 1;
                    out->backup.header_status = bhdr.header_status;
                    out->backup.header_reason = bhdr.header_reason;
                    out->backup.header_crc_stored = bhdr.header_crc_stored;
                    out->backup.header_crc_computed = bhdr.header_crc_computed;
                    out->backup.header_crc_ok = bhdr.header_crc_ok;
                    out->backup.geometry_ok = bhdr.geometry_ok;

                    if (out->backup.header_status == GPT_VALID &&
                        out->primary.hdr.entry_count == bhdr.hdr.entry_count &&
                        out->primary.hdr.entry_size  == bhdr.hdr.entry_size &&
                        out->primary.hdr.first_usable_lba == bhdr.hdr.first_usable_lba &&
                        out->primary.hdr.last_usable_lba  == bhdr.hdr.last_usable_lba &&
                        out->primary.hdr.entries_crc32    == bhdr.hdr.entries_crc32 &&
                        CompareMem(out->primary.hdr.disk_guid, bhdr.hdr.disk_guid, 16) == 0) {
                        out->backup.entries_status = GPT_VALID;
                        out->backup.entries_reason = GPT_R_NONE;
                        out->backup.entries_crc_stored = out->primary.hdr.entries_crc32;
                        out->backup.entries_crc_computed = out->primary.hdr.entries_crc32;
                        out->backup.entries_crc_ok = 1;
                        out->backup.layout_status = out->primary.layout_status;
                        out->backup.layout_reason = GPT_R_NONE;
                        out->backup.ent_count = out->primary.ent_count;
                        out->backup.used_count = out->primary.used_count;

                        out->cmp.crc_only = 1;
                    } else {
                        primary_full = 0;
                    }
                }
            }
            if (primary_full) {
                gpt_free(plast);
                plast = NULL;
            } else {
gpt_load_ct(dev, last, &out->backup, GPT_COPY_BACKUP);
                gpt_free(plast);
                plast = NULL;
            }
        } else {
            out->backup.present = 0;
            out->backup.header_status = GPT_INVALID;
            out->backup.header_reason = GPT_R_INVALID_SIGNATURE;
            gpt_free(plast);
            plast = NULL;
        }
    }

    gpt_compare(out, &out->cmp);
    gpt_classify(out);
    gpt_build_notes(out);
    return EFI_SUCCESS;
}

gpt_status_t gpt_table_status(const gpt_table_t *t) {
    if (!t) return GPT_UNKNOWN;

    int worst = 0;
    gpt_status_t vals[3];
    vals[0] = t->header_status;
    vals[1] = t->entries_status;
    vals[2] = t->layout_status;
    for (UINTN i = 0; i < 3; i++) {
        int r = 0;
        if (vals[i] == GPT_WARNING) r = 1;
        else if (vals[i] == GPT_INVALID) r = 2;
        else if (vals[i] == GPT_UNSAFE) r = 3;
        if (r > worst) worst = r;
    }
    switch (worst) {
        case 1:  return GPT_WARNING;
        case 2:  return GPT_INVALID;
        case 3:  return GPT_UNSAFE;
        default: return GPT_VALID;
    }
}

/* True only for a table we may copy FROM: fully validated *and* backed by real
 * entry bytes we actually read.  The fast path fills in a header-only backup
 * (raw == NULL) when the primary is pristine; that table is fine to report but
 * must never be used as a recovery source. */
int gpt_table_is_source(const gpt_table_t *t) {
    if (!t) return 0;
    return t->present &&
           t->header_status == GPT_VALID &&
           t->entries_status == GPT_VALID &&
           t->layout_status == GPT_VALID &&
           t->loaded && t->raw && t->ents && t->hdr_raw &&
           t->raw_bytes >= (UINTN)t->hdr.entry_count * (UINTN)t->hdr.entry_size &&
           t->hdr_raw_bytes >= (UINTN)t->hdr.header_size;
}

/* Partition type GUIDs as they appear ON DISK: the first three fields are
 * little-endian, so the byte order here is not the textual GUID order.
 * These are generated from the canonical text form - do not hand-edit. */
static const struct { const UINT8 g[16]; const CHAR16 *name; } KNOWN_TYPES[] = {
    { { 0x28,0x73,0x2a,0xc1,0x1f,0xf8,0xd2,0x11,0xba,0x4b,0x00,0xa0,0xc9,0x3e,0xc9,0x3b },
      L"EFI System Partition" },
    { { 0x41,0xee,0x4d,0x02,0xe7,0x33,0xd3,0x11,0x9d,0x69,0x00,0x08,0xc7,0x81,0xf3,0x9f },
      L"MBR partition scheme" },
    { { 0x48,0x61,0x68,0x21,0x49,0x64,0x6f,0x6e,0x74,0x4e,0x65,0x65,0x64,0x45,0x46,0x49 },
      L"BIOS boot" },
    { { 0x16,0xe3,0xc9,0xe3,0x5c,0x0b,0xb8,0x4d,0x81,0x7d,0xf9,0x2d,0xf0,0x02,0x15,0xae },
      L"Microsoft Reserved" },
    { { 0xa2,0xa0,0xd0,0xeb,0xe5,0xb9,0x33,0x44,0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7 },
      L"Microsoft Basic Data" },
    { { 0xa4,0xbb,0x94,0xde,0xd1,0x06,0x40,0x4d,0xa1,0x6a,0xbf,0xd5,0x01,0x79,0xd6,0xac },
      L"Windows Recovery" },
    { { 0xaa,0xc8,0x08,0x58,0x8f,0x7e,0xe0,0x42,0x85,0xd2,0xe1,0xe9,0x04,0x34,0xcf,0xb3 },
      L"Windows LDM metadata" },
    { { 0xa0,0x60,0x9b,0xaf,0x31,0x14,0x62,0x4f,0xbc,0x68,0x33,0x11,0x71,0x4a,0x69,0xad },
      L"Windows LDM data" },
    { { 0xaf,0x3d,0xc6,0x0f,0x83,0x84,0x72,0x47,0x8e,0x79,0x3d,0x69,0xd8,0x47,0x7d,0xe4 },
      L"Linux filesystem" },
    { { 0x6d,0xfd,0x57,0x06,0xab,0xa4,0xc4,0x43,0x84,0xe5,0x09,0x33,0xc8,0x4b,0x4f,0x4f },
      L"Linux swap" },
    { { 0x79,0xd3,0xd6,0xe6,0x07,0xf5,0xc2,0x44,0xa2,0x3c,0x23,0x8f,0x2a,0x3d,0xf9,0x28 },
      L"Linux LVM" },
    { { 0x0f,0x88,0x9d,0xa1,0xfc,0x05,0x3b,0x4d,0xa0,0x06,0x74,0x3f,0x0f,0x84,0x91,0x1e },
      L"Linux RAID" },
    { { 0xe1,0xc7,0x3a,0x93,0xb4,0x2e,0x13,0x4f,0xb8,0x44,0x0e,0x14,0xe2,0xae,0xf9,0x15 },
      L"Linux /home" },
    { { 0xff,0xc2,0x13,0xbc,0xe6,0x59,0x62,0x42,0xa3,0x52,0xb2,0x75,0xfd,0x6f,0x71,0x72 },
      L"Linux extended boot" },
    { { 0xe3,0xbc,0x68,0x4f,0xcd,0xe8,0xb1,0x4d,0x96,0xe7,0xfb,0xca,0xf9,0x84,0xb7,0x09 },
      L"Linux x86-64 root" },
    { { 0x45,0xb0,0x21,0xb9,0xf0,0x1d,0xc3,0x41,0xaf,0x44,0x4c,0x6f,0x28,0x0d,0x3f,0xae },
      L"Linux aarch64 root" },
    { { 0xcb,0x7c,0x7d,0xca,0xed,0x63,0x53,0x4c,0x86,0x1c,0x17,0x42,0x53,0x60,0x59,0xcc },
      L"Linux LUKS" },
    { { 0x00,0x53,0x46,0x48,0x00,0x00,0xaa,0x11,0xaa,0x11,0x00,0x30,0x65,0x43,0xec,0xac },
      L"Apple HFS+" },
    { { 0xef,0x57,0x34,0x7c,0x00,0x00,0xaa,0x11,0xaa,0x11,0x00,0x30,0x65,0x43,0xec,0xac },
      L"Apple APFS" },
    { { 0xb4,0x7c,0x6e,0x51,0xcf,0x6e,0xd6,0x11,0x8f,0xf8,0x00,0x02,0x2d,0x09,0x71,0x2b },
      L"FreeBSD disklabel" },
};

const CHAR16* gpt_type_name(const UINT8 type_guid[16]) {
    if (!type_guid) return NULL;
    for (UINTN i = 0; i < sizeof(KNOWN_TYPES) / sizeof(KNOWN_TYPES[0]); i++) {
        if (CompareMem(type_guid, KNOWN_TYPES[i].g, 16) == 0)
            return KNOWN_TYPES[i].name;
    }
    return NULL;
}

/* Render an on-disk GUID in canonical text form.  The first three fields are
 * stored little-endian on disk, so they are emitted byte-reversed. */
void gpt_format_guid(const UINT8 guid[16], CHAR16 *out, UINTN cap) {
    static const CHAR16 hex[] = L"0123456789abcdef";
    static const int order[16] = { 3,2,1,0, -1, 5,4, -1, 7,6, -1, 8,9, -1,
                                   10,11 };
    if (!out || cap == 0) return;
    if (!guid) { out[0] = 0; return; }

    CHAR16 u[40];
    UINTN w = 0;
    for (UINTN k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
        if (order[k] < 0) { u[w++] = '-'; continue; }
        UINT8 b = guid[order[k]];
        u[w++] = hex[b >> 4];
        u[w++] = hex[b & 0xF];
    }
    for (UINTN k = 12; k < 16; k++) {
        u[w++] = hex[guid[k] >> 4];
        u[w++] = hex[guid[k] & 0xF];
    }
    u[w] = 0;

    UINTN n = 0;
    while (u[n] && n + 1 < cap) { out[n] = u[n]; n++; }
    out[n] = 0;
}

void gpt_entry_name(const gpt_entry_t *entry, CHAR16 *out, UINTN cap) {
    if (!out || cap == 0) return;
    UINTN n = 0;
    if (entry) {
        for (UINTN i = 0; i < GPT_NAME_CHARS && entry->name[i] && n + 1 < cap; i++)
            out[n++] = entry->name[i];
    }
    out[n] = 0;
}

static void note16_put(CHAR16 *out, UINTN *w, UINTN cap, const CHAR16 *s) {
    for (UINTN i = 0; s[i] && *w + 1 < cap; i++) out[(*w)++] = s[i];
}

static void note16_num(CHAR16 *out, UINTN *w, UINTN cap, INTN v) {
    UINT64 val;
    if (v < 0) {
        if (*w + 1 < cap) out[(*w)++] = L'-';
        val = (UINT64)(-(v + 1)) + 1;
    } else {
        val = (UINT64)v;
    }
    CHAR16 tmp[24];
    UINTN n = 0;
    do { tmp[n++] = (CHAR16)(L'0' + (val % 10)); val /= 10; } while (val && n < 23);
    while (n && *w + 1 < cap) out[(*w)++] = tmp[--n];
}

void gpt_note_text(const gpt_note_t *note, CHAR16 *out, UINTN cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!note) return;

    static const CHAR16 *copy_name[2] = { L"primary GPT", L"backup GPT" };
    UINTN w = 0;

    if (note->copy == GPT_COPY_PRIMARY || note->copy == GPT_COPY_BACKUP)
        note16_put(out, &w, cap, copy_name[note->copy]);
    else
        note16_put(out, &w, cap, L"disk");

    if (note->reason != GPT_R_NONE) {
        note16_put(out, &w, cap, L": ");
        note16_put(out, &w, cap, gpt_reason_text(note->reason));
    }
    if (note->index >= 0) {
        note16_put(out, &w, cap, L" [entry ");
        note16_num(out, &w, cap, note->index);
        if (note->index2 >= 0) {
            note16_put(out, &w, cap, L", ");
            note16_num(out, &w, cap, note->index2);
        }
        note16_put(out, &w, cap, L"]");
    }
    out[w] = 0;
}

void gpt_plan_free(gpt_plan_t *plan) {
    if (!plan) return;
    if (plan->entry_bytes) { gpt_free(plan->entry_bytes); plan->entry_bytes = NULL; }
    if (plan->header_sector) { gpt_free(plan->header_sector); plan->header_sector = NULL; }
    ZeroMem(plan, sizeof(*plan));
}

int gpt_build_primary_plan(const gpt_diag_t *diag, gpt_plan_t *plan) {
    if (!diag || !plan) return 0;
    ZeroMem(plan, sizeof(*plan));
    plan->safety = GPT_UNKNOWN;
    plan->reason = GPT_R_UNSAFE_RECOVERY;

    if (diag->klass != GPT_CLASS_PRIMARY_CORRUPT_BACKUP_VALID) {
        plan->reason = GPT_R_UNSAFE_RECOVERY;
        return 0;
    }

    const gpt_table_t *b = &diag->backup;
    if (!gpt_table_is_source(b)) {
        plan->reason = GPT_R_UNSAFE_RECOVERY;
        return 0;
    }
    if (diag->read_only) {
        plan->reason = GPT_R_READ_ONLY_MEDIA;
        return 0;
    }
    if (diag->sector_size < GPT_SECTOR_SIZE_MIN ||
        diag->sector_size > GPT_SECTOR_SIZE_MAX) {
        plan->reason = GPT_R_BAD_SECTOR_SIZE;
        return 0;
    }
    if (diag->total_sectors <= (UINT64)GPT_MIN_DISK_SECTORS) {
        plan->reason = GPT_R_DISK_TOO_SMALL;
        return 0;
    }

    /* The source must be the real backup: sitting on the last LBA and pointing
     * back at LBA 1.  A "backup" that claims to live elsewhere describes a
     * different geometry than the one we are about to write, so recovering
     * from it is ambiguous - refuse here rather than letting the pre-commit
     * re-read reject it later and report it as a disk change. */
    if (b->hdr.current_lba != diag->total_sectors - 1) {
        plan->reason = GPT_R_BACKUP_NOT_AT_END;
        return 0;
    }
    if (b->hdr.backup_lba != 1) {
        plan->reason = GPT_R_INVALID_ALTERNATE_LBA;
        return 0;
    }

    plan->sector_size   = diag->sector_size;
    plan->total_sectors = diag->total_sectors;
    plan->media_id      = diag->media_id;

    plan->src_header_lba  = b->hdr.current_lba;
    plan->src_entries_lba = b->hdr.entry_lba;
    plan->src_header_crc  = b->hdr.header_crc32;
    plan->src_entries_crc = b->hdr.entries_crc32;
    RCOPY(plan->src_disk_guid, b->hdr.disk_guid, 16);

    plan->entry_count = b->hdr.entry_count;
    plan->entry_size  = b->hdr.entry_size;
    plan->entry_array_bytes = (UINT64)b->hdr.entry_count * (UINT64)b->hdr.entry_size;
    plan->entry_array_sectors =
        (plan->entry_array_bytes + plan->sector_size - 1) / plan->sector_size;
    if (plan->entry_array_sectors == 0) plan->entry_array_sectors = 1;

    plan->first_usable_lba = b->hdr.first_usable_lba;
    plan->last_usable_lba  = b->hdr.last_usable_lba;
    plan->backup_lba       = plan->total_sectors - 1;
    plan->part_count       = b->used_count;

    plan->dst_header_lba = 1;
    plan->dst_entries_lba = 2;

    if (plan->dst_header_lba >= plan->total_sectors) {
        plan->reason = GPT_R_NO_SAFE_DESTINATION;
        return 0;
    }

    if (plan->dst_entries_lba > plan->total_sectors ||
        plan->entry_array_sectors > plan->total_sectors - plan->dst_entries_lba) {
        plan->reason = GPT_R_NO_SAFE_DESTINATION;
        return 0;
    }

    /* The destination metadata region must fit strictly before the first
     * usable LBA.  This is what proves the rewrite cannot touch partition
     * data - it must hold before any write is even contemplated. */
    if (plan->first_usable_lba <= plan->dst_entries_lba ||
        plan->entry_array_sectors > plan->first_usable_lba - plan->dst_entries_lba) {
        plan->reason = GPT_R_NO_SAFE_DESTINATION;
        return 0;
    }

    /* Belt and braces: walk the actual partition list and prove no partition
     * intersects the sectors we are going to write, including LBA 0/1. */
    {
        UINT64 wr_first = plan->dst_header_lba;
        UINT64 wr_last  = plan->dst_entries_lba + plan->entry_array_sectors - 1;
        for (UINT32 i = 0; i < b->ent_count; i++) {
            const gpt_entry_t *e = &b->ents[i];
            if (!e->used) continue;
            if (e->first_lba <= wr_last && wr_first <= e->last_lba) {
                plan->reason = GPT_R_METADATA_OVERLAP;
                return 0;
            }
            /* And nothing may sit on top of the backup copy we read from. */
            if (e->first_lba <= plan->src_header_lba &&
                plan->src_header_lba <= e->last_lba) {
                plan->reason = GPT_R_METADATA_OVERLAP;
                return 0;
            }
        }
    }

    UINTN ebytes = (UINTN)(plan->entry_array_sectors * plan->sector_size);
    UINT8 *e = gpt_alloc(ebytes);
    if (!e) { plan->reason = GPT_R_OUT_OF_MEMORY; return 0; }
    CopyMem(e, b->raw, (UINTN)plan->entry_array_bytes);
    ZeroMem(e + (UINTN)plan->entry_array_bytes, ebytes - (UINTN)plan->entry_array_bytes);
    plan->entry_bytes = e;
    plan->new_entries_crc = gpt_crc32(0, e, (UINTN)plan->entry_array_bytes);

    /* The rebuilt array must reproduce the source's own recorded CRC - if it
     * does not, the bytes we hold are not the ones the backup header vouches
     * for and nothing may be written. */
    if (plan->new_entries_crc != plan->src_entries_crc) {
        gpt_plan_free(plan);
        plan->safety = GPT_UNKNOWN;
        plan->reason = GPT_R_ARRAY_CRC_MISMATCH;
        return 0;
    }

    UINT8 *h = gpt_alloc(plan->sector_size);
    if (!h) { gpt_plan_free(plan); plan->reason = GPT_R_OUT_OF_MEMORY; return 0; }

    /* Start from the source header's exact bytes (preserving header_size and
     * any vendor-specific tail beyond the 92-byte base), then patch only the
     * fields that describe the copy's own location.  The header CRC is then
     * taken over the preserved size. */
    UINT32 hsize = b->hdr.header_size;
    ZeroMem(h, plan->sector_size);
    RCOPY(h, b->hdr_raw, hsize);
    put_le64(h + 24, plan->dst_header_lba);
    put_le64(h + 32, plan->backup_lba);
    put_le64(h + 72, plan->dst_entries_lba);
    put_le32(h + 80, plan->entry_count);
    put_le32(h + 84, plan->entry_size);
    put_le32(h + 88, plan->new_entries_crc);
    /* The stored field must read zero while the header CRC is being computed -
     * never leave the source copy's own CRC in the field for the math. */
    put_le32(h + 16, 0);
    UINT32 new_hdr_crc = gpt_crc32(0, h, hsize);
    put_le32(h + 16, new_hdr_crc);
    plan->header_sector = h;
    plan->new_header_crc = le32(h + 16);

    plan->safety = GPT_VALID;
    plan->reason = GPT_R_NONE;
    return 1;
}

EFI_STATUS gpt_read_preimage(gpt_dev_t *dev, const gpt_plan_t *plan,
                             UINT8 **out, UINTN *out_bytes) {
    if (!dev || !plan || !out || !out_bytes) return EFI_INVALID_PARAMETER;
    *out = NULL;
    *out_bytes = 0;

    UINTN hs = plan->sector_size;
    UINTN es = (UINTN)(plan->entry_array_sectors * plan->sector_size);
    UINTN total = (UINTN)((UINT64)hs + hs + es); 
    UINT8 *buf = gpt_alloc(total);
    if (!buf) return EFI_OUT_OF_RESOURCES;

    EFI_STATUS st;
    st = dev->read(dev, 0, 1, buf);
    if (EFI_ERROR(st)) { gpt_free(buf); return st; }
    st = dev->read(dev, plan->dst_header_lba, 1, buf + hs);
    if (EFI_ERROR(st)) { gpt_free(buf); return st; }
    st = dev->read(dev, plan->dst_entries_lba, (UINTN)plan->entry_array_sectors,
                   buf + hs + hs);
    if (EFI_ERROR(st)) { gpt_free(buf); return st; }

    *out = buf;
    *out_bytes = total;
    return EFI_SUCCESS;
}

void gpt_result_free(gpt_result_t *result) {
    if (!result) return;
    if (result->preimage) {
        gpt_free(result->preimage);
        result->preimage = NULL;
    }
    result->preimage_bytes = 0;
    result->have_preimage = 0;
    if (result->have_after) {
        gpt_diag_free(&result->after);
        result->have_after = 0;
    }
    ZeroMem(result, sizeof(*result));
}

/* Re-read the recovery source immediately before committing and prove it is
 * still bit-for-bit what the plan was built from.  This closes the
 * time-of-check/time-of-use window: anything that differs means the disk (or
 * the medium) changed under us and the repair must abort. */
static int gpt_revalidate_source(gpt_dev_t *dev, const gpt_plan_t *plan) {
    UINT8 *sec = gpt_alloc(dev->sector_size);
    if (!sec) return 0;
    UINT8 *arr = NULL;
    int ok = 0;

    if (dev->sector_size   != plan->sector_size ||
        dev->total_sectors != plan->total_sectors ||
        dev->media_id      != plan->media_id ||
        dev->read_only) {

        goto done;
    }

    if (EFI_ERROR(dev->read(dev, plan->src_header_lba, 1, sec))) goto done;
    if (CompareMem(sec, "EFI PART", 8) != 0) goto done;
    if (le64(sec + 24) != plan->src_header_lba) goto done;
    if (le64(sec + 32) != plan->dst_header_lba) goto done;
    if (le32(sec + 16) != plan->src_header_crc) goto done;
    if (CompareMem(sec + 56, plan->src_disk_guid, 16) != 0) goto done;
    if (le64(sec + 72) != plan->src_entries_lba) goto done;
    if (le32(sec + 84) != plan->entry_size) goto done;
    if (le32(sec + 80) != plan->entry_count) goto done;
    if (le64(sec + 40) != plan->first_usable_lba) goto done;
    if (le64(sec + 48) != plan->last_usable_lba) goto done;
    if (le32(sec + 88) != plan->src_entries_crc) goto done;

    /* The header must still be self-consistent, not merely unchanged. */
    {
        UINT32 hsize = le32(sec + 12);
        if (hsize < GPT_HEADER_SIZE_MIN || hsize > dev->sector_size ||
            hsize > GPT_SECTOR_SIZE_MAX)
            goto done;
        UINT8 tmp[GPT_SECTOR_SIZE_MAX];
        RCOPY(tmp, sec, hsize);
        put_le32(tmp + 16, 0);
        if (gpt_crc32(0, tmp, hsize) != plan->src_header_crc) goto done;
    }

    arr = gpt_alloc((UINTN)(plan->entry_array_sectors * dev->sector_size));
    if (!arr) goto done;
    if (EFI_ERROR(dev->read(dev, plan->src_entries_lba,
                            (UINTN)plan->entry_array_sectors, arr))) goto done;
    if (gpt_crc32(0, arr, (UINTN)plan->entry_array_bytes) != plan->src_entries_crc)
        goto done;
    /* And the bytes must be exactly the ones staged in the plan. */
    if (plan->entry_bytes &&
        CompareMem(arr, plan->entry_bytes, (UINTN)plan->entry_array_bytes) != 0)
        goto done;

    ok = 1;
done:
    gpt_free(sec);
    gpt_free(arr);
    return ok;
}

EFI_STATUS gpt_execute_plan(gpt_dev_t *dev, const gpt_plan_t *plan,
                            gpt_result_t *result) {
    if (!dev || !plan || !result) return EFI_INVALID_PARAMETER;
    ZeroMem(result, sizeof(*result));
    if (plan->safety != GPT_VALID) {
        result->reason = plan->reason ? plan->reason : GPT_R_UNSAFE_RECOVERY;
        return EFI_INVALID_PARAMETER;
    }
    if (!dev->read || !dev->write || !plan->entry_bytes || !plan->header_sector) {
        result->reason = GPT_R_UNSAFE_RECOVERY;
        return EFI_INVALID_PARAMETER;
    }
    if (dev->read_only) {
        result->reason = GPT_R_READ_ONLY_MEDIA;
        return EFI_WRITE_PROTECTED;
    }

    if (!gpt_revalidate_source(dev, plan)) {
        result->reason = GPT_R_DISK_CHANGED;
        return EFI_DEVICE_ERROR;
    }

    /* Snapshot the sectors we are about to overwrite (LBA 0, LBA 1 and the
     * primary entry array) so the caller can log or restore the exact bytes
     * that were replaced.  Purely read-only; failure to snapshot is not fatal
     * to the repair, but is recorded. */
    result->preimage_bytes = 0;
    if (!EFI_ERROR(gpt_read_preimage(dev, plan, &result->preimage,
                                     &result->preimage_bytes)))
        result->have_preimage = 1;

    EFI_STATUS st;

    st = dev->write(dev, plan->dst_entries_lba, (UINTN)plan->entry_array_sectors,
                    plan->entry_bytes);
    if (EFI_ERROR(st)) { result->reason = GPT_R_DISK_WRITE_FAILED; result->status = st; return st; }
    result->wrote_entries = 1;

    if (dev->flush) {
        st = dev->flush(dev);
        if (EFI_ERROR(st)) { result->reason = GPT_R_DISK_FLUSH_FAILED; result->status = st; return st; }
        result->flushed = 1;
    }

    st = dev->write(dev, plan->dst_header_lba, 1, plan->header_sector);
    if (EFI_ERROR(st)) { result->reason = GPT_R_DISK_WRITE_FAILED; result->status = st; return st; }
    result->wrote_header = 1;

    if (dev->flush) {
        st = dev->flush(dev);
        if (EFI_ERROR(st)) { result->reason = GPT_R_DISK_FLUSH_FAILED; result->status = st; return st; }
    }

    gpt_diag_t after;
    gpt_diag_reset(&after);
    st = gpt_diagnose(dev, 1, &after);
    if (EFI_ERROR(st)) { gpt_diag_free(&after); result->reason = GPT_R_POST_WRITE_VERIFY_FAILED; return st; }

    result->have_after = 1;
    CopyMem(&result->after, &after, sizeof(after));

    gpt_table_t *p = &after.primary;
    gpt_table_t *b = &after.backup;
    int p_ok = p->present && p->header_status == GPT_VALID &&
               p->entries_status == GPT_VALID && p->layout_status == GPT_VALID;
    int b_ok = b->present && b->header_status == GPT_VALID &&
               b->entries_status == GPT_VALID && b->layout_status == GPT_VALID;
    int match = after.cmp.kind == GPT_CMP_IDENTICAL;

    result->verify_primary_ok = p_ok;
    result->verify_backup_ok  = b_ok;
    result->verify_match      = match;

    if (p_ok && b_ok && match) {
        result->success = 1;
        result->reason = GPT_R_NONE;
        return EFI_SUCCESS;
    }
    result->success = 0;
    result->reason = GPT_R_POST_WRITE_VERIFY_FAILED;
    return EFI_DEVICE_ERROR;
}

