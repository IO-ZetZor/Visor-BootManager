/* gpt_diagnose.c - damage classification and diagnostic notes */
#include "gpt_internal.h"

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

void gpt_diag_reset(gpt_diag_t *diag) {
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
