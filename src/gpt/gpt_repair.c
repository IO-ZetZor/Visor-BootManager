/* gpt_repair.c - repair planning and execution (feature: gptrepair) */
#include "gpt_internal.h"

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

    if (plan->first_usable_lba <= plan->dst_entries_lba ||
        plan->entry_array_sectors > plan->first_usable_lba - plan->dst_entries_lba) {
        plan->reason = GPT_R_NO_SAFE_DESTINATION;
        return 0;
    }

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

    if (plan->new_entries_crc != plan->src_entries_crc) {
        gpt_plan_free(plan);
        plan->safety = GPT_UNKNOWN;
        plan->reason = GPT_R_ARRAY_CRC_MISMATCH;
        return 0;
    }

    UINT8 *h = gpt_alloc(plan->sector_size);
    if (!h) { gpt_plan_free(plan); plan->reason = GPT_R_OUT_OF_MEMORY; return 0; }

    UINT32 hsize = b->hdr.header_size;
    ZeroMem(h, plan->sector_size);
    RCOPY(h, b->hdr_raw, hsize);
    put_le64(h + 24, plan->dst_header_lba);
    put_le64(h + 32, plan->backup_lba);
    put_le64(h + 72, plan->dst_entries_lba);
    put_le32(h + 80, plan->entry_count);
    put_le32(h + 84, plan->entry_size);
    put_le32(h + 88, plan->new_entries_crc);
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
