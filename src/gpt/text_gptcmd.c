/* text_gptcmd.c - gpt subcommands of the recovery shell (feature: gptrepair) */
#include "text_menu_internal.h"

static int gpt_cmd_open(UINT32 media_id, gpt_dev_t *dev) {
    return gpt_disk_open_media_id(media_id, dev);
}

void gpt_cmd_scan(UINTN *row, UINTN rows, UINTN cols) {
    UINTN n = 0;
    EFI_HANDLE *hs = gpt_disk_enum(&n);
    if (!hs) {
        out_line(row, rows, cols, VT_DIM, L"gpt: no whole-disk block devices found");
        return;
    }
    out_line(row, rows, cols, VT_TITLE, L"GPT disks (whole-disk block devices):");
    CHAR16 line[220];
    for (UINTN i = 0; i < n; i++) {
        gpt_dev_t dev;
        if (!gpt_disk_from_bio(&dev, hs[i])) continue;
        gpt_diag_t dg;
        ZeroMem(&dg, sizeof(dg));
        EFI_STATUS st = gpt_diagnose(&dev, 0, &dg);
        if (EFI_ERROR(st)) {
            SPrint(line, sizeof(line), L"  media %d : diagnosis failed",
                   (int)dev.media_id);
        } else {
            gpt_status_t ws = gpt_table_status(&dg.primary);
            gpt_status_t wb = gpt_table_status(&dg.backup);
            SPrint(line, sizeof(line),
                   L"  media %d   %-34s  overall %s  (P:%s B:%s)",
                   (int)dev.media_id,
                   gpt_class_text(dg.klass), gpt_status_text(dg.overall),
                   gpt_status_text(ws), gpt_status_text(wb));
        }
        out_line(row, rows, cols, VT_NORMAL, line);
        gpt_diag_free(&dg);
        gpt_disk_close(&dev);
    }
    efi_free_pool(hs);
    out_line(row, rows, cols, VT_DIM,
             L"  'gpt <media>' shows detail; 'gpt repair <media>' rebuilds the primary GPT from the backup.");
}

void gpt_cmd_detail(UINTN *row, UINTN rows, UINTN cols, UINT32 media_id) {
    gpt_dev_t dev;
    if (!gpt_cmd_open(media_id, &dev)) {
        out_line(row, rows, cols, VT_DIM, L"gpt: no disk with that media id (see 'gpt')");
        return;
    }
    gpt_diag_t dg;
    ZeroMem(&dg, sizeof(dg));
    CHAR16 line[220];
    if (EFI_ERROR(gpt_diagnose(&dev, 1, &dg))) {
        out_line(row, rows, cols, VT_DIM, L"gpt: diagnosis failed");
        gpt_disk_close(&dev);
        return;
    }

    SPrint(line, sizeof(line), L"disk media %d  %lld sectors @%d  read_only=%d removable=%d",
           (int)media_id, (long long)dg.total_sectors,
           (int)dg.sector_size, dg.read_only, dg.removable);
    out_line(row, rows, cols, VT_TITLE, line);
    SPrint(line, sizeof(line), L"  MBR: %s%s", gpt_status_text(dg.mbr_status),
           dg.mbr_protective ? L" (protective)" : L"");
    out_line(row, rows, cols, VT_NORMAL, line);
    SPrint(line, sizeof(line), L"  class: %s  capability: %s  overall: %s",
           gpt_class_text(dg.klass),
           dg.capability == GPT_RECOVER_PRIMARY_FROM_BACKUP ? L"recover primary from backup"
           : dg.capability == GPT_RECOVER_MANUAL_ONLY ? L"manual only" : L"none",
           gpt_status_text(dg.overall));
    out_line(row, rows, cols, VT_NORMAL, line);

    gpt_table_t *tables[2] = { &dg.primary, &dg.backup };
    const CHAR16 *names[2] = { L"primary", L"backup" };
    for (int c = 0; c < 2; c++) {
        gpt_table_t *t = tables[c];
        SPrint(line, sizeof(line), L"  %s: present=%d header=%s(%s) entries=%s(%s) layout=%s",
               names[c], t->present,
               gpt_status_text(t->header_status), gpt_reason_text(t->header_reason),
               gpt_status_text(t->entries_status), gpt_reason_text(t->entries_reason),
               gpt_status_text(t->layout_status));
        out_line(row, rows, cols, VT_NORMAL, line);
        SPrint(line, sizeof(line), L"    usable [%lld..%lld]  entries @%lld  %d x %d  crc=0x%08x",
               (long long)t->hdr.first_usable_lba, (long long)t->hdr.last_usable_lba,
               (long long)t->hdr.entry_lba, (int)t->hdr.entry_count,
               (int)t->hdr.entry_size, (unsigned)t->hdr.entries_crc32);
        out_line(row, rows, cols, VT_NORMAL, line);
        if (t->layout_reason != GPT_R_NONE) {
            SPrint(line, sizeof(line), L"    layout: %s", gpt_reason_text(t->layout_reason));
            out_line(row, rows, cols, VT_DIM, line);
        }
        if (*row >= rows - 2) break;
    }
    for (UINTN i = 0; i < dg.primary.ent_count && dg.primary.ents; i++) {
        gpt_entry_t *e = &dg.primary.ents[i];
        if (!e->used) continue;
        CHAR16 nm[40] = L"";
        CHAR16 tag[40] = L"";
        gpt_entry_name(e, nm, sizeof(nm) / sizeof(CHAR16));
        const CHAR16 *tn = gpt_type_name(e->type_guid);
        if (tn) SPrint(tag, sizeof(tag), L" [%s]", tn);
        SPrint(line, sizeof(line), L"  part %2d  %-24.24s  %12lld..%-12lld%s",
               (int)(i + 1), nm, (long long)e->first_lba, (long long)e->last_lba, tag);
        out_line(row, rows, cols, VT_NORMAL, line);
        if (*row >= rows - 2) break;
    }
    if (dg.klass == GPT_CLASS_PRIMARY_CORRUPT_BACKUP_VALID && !dg.read_only)
        out_line(row, rows, cols, VT_NORMAL, L"  repair available: 'gpt repair <media>'");
    else if (dg.read_only)
        out_line(row, rows, cols, VT_DIM, L"  read-only media - no repair.");
    gpt_diag_free(&dg);
    gpt_disk_close(&dev);
}

static int gpt_confirm_typed(UINTN rows, UINTN cols, const CHAR16 *question) {
    CHAR16 buf[80];
    SPrint(buf, sizeof(buf), L"%s  (type YES then Enter)", question);
    left_line(rows - 2, cols, VT_TITLE, buf);
    CHAR16 rep[16];
    UINTN rl = 0;
    rep[0] = 0;
    ST->ConOut->SetAttribute(ST->ConOut, VT_NORMAL);
    for (;;) {
        CHAR16 prompt[64];
        SPrint(prompt, sizeof(prompt), L"> %s", rep);
        left_line(rows - 1, cols, VT_SEL, prompt);
        EFI_INPUT_KEY key;
        EFI_STATUS ks = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_ERROR(ks)) { efi_sleep(20); continue; }
        CHAR16 uc = key.UnicodeChar;
        if (uc == 0x0D) break;
        if (uc == 0x1B) return 0;
        if (uc == 0x08) { if (rl) rl--; rep[rl] = 0; }
        else if (uc >= 0x20 && rl + 1 < sizeof(rep) / sizeof(rep[0])) {
            if (uc >= L'a' && uc <= L'z') uc = (CHAR16)(uc - 32);
            rep[rl++] = uc;
            rep[rl] = 0;
        }
        if (text_eq_ci(rep, L"NO")) return 0;
    }
    rep[rl] = 0;
    return text_eq_ci(rep, L"YES") || text_eq_ci(rep, L"Y");
}

int gpt_cmd_repair(UINTN *row, UINTN rows, UINTN cols, UINT32 media_id) {
    gpt_dev_t dev;
    if (!gpt_cmd_open(media_id, &dev)) {
        out_line(row, rows, cols, VT_DIM, L"gpt: no disk with that media id (see 'gpt')");
        return 0;
    }
    gpt_diag_t dg;
    ZeroMem(&dg, sizeof(dg));
    gpt_plan_t plan;
    ZeroMem(&plan, sizeof(plan));
    gpt_result_t res;
    ZeroMem(&res, sizeof(res));
    CHAR16 line[220];

    if (EFI_ERROR(gpt_diagnose(&dev, 1, &dg))) {
        out_line(row, rows, cols, VT_DIM, L"gpt: diagnosis failed");
        goto out;
    }
    if (dg.klass != GPT_CLASS_PRIMARY_CORRUPT_BACKUP_VALID) {
        SPrint(line, sizeof(line), L"gpt: media %d is not in the recoverable state (%s)",
               (int)media_id, gpt_class_text(dg.klass));
        out_line(row, rows, cols, VT_DIM, line);
        goto out;
    }
    if (dg.read_only) {
        out_line(row, rows, cols, VT_DIM, L"gpt: media is read-only - refusing to repair");
        goto out;
    }
    if (!gpt_build_primary_plan(&dg, &plan) || plan.safety != GPT_VALID) {
        SPrint(line, sizeof(line), L"gpt: no safe plan: %s", gpt_reason_text(plan.reason));
        out_line(row, rows, cols, VT_DIM, line);
        goto out;
    }

    SPrint(line, sizeof(line), L"plan: source backup @LBA %lld (entries @LBA %lld)",
           (long long)plan.src_header_lba, (long long)plan.src_entries_lba);
    out_line(row, rows, cols, VT_NORMAL, line);
    SPrint(line, sizeof(line), L"plan: rewrite primary header @LBA %lld and %d entries @LBA %lld",
           (long long)plan.dst_header_lba, (int)plan.entry_count,
           (long long)plan.dst_entries_lba);
    out_line(row, rows, cols, VT_NORMAL, line);
    SPrint(line, sizeof(line), L"plan: %d partitions, %lld usable sectors per copy",
           (int)plan.part_count,
           (long long)(plan.last_usable_lba - plan.first_usable_lba + 1));
    out_line(row, rows, cols, VT_NORMAL, line);

    SPrint(line, sizeof(line), L"WARNING: this rewrites the primary GPT of disk media %d.",
           (int)media_id);
    out_line(row, rows, cols, VT_TITLE, line);
    out_line(row, rows, cols, VT_DIM,   L"         Back up important data first. The backup GPT is never written.");
    if (!gpt_confirm_typed(rows, cols, L"Rebuild primary GPT from the backup copy?")) {
        out_line(row, rows, cols, VT_DIM, L"gpt: repair cancelled");
        goto out;
    }

    {
        CHAR16 logbuf[160];
        SPrint(logbuf, sizeof(logbuf), L"gpt: repairing primary GPT on disk media %d",
               (int)dev.media_id);
        efi_log(logbuf);
    }
    EFI_STATUS st = gpt_execute_plan(&dev, &plan, &res);
    if (!EFI_ERROR(st) && res.success) {
        out_line(row, rows, cols, VT_NORMAL, L"gpt: primary GPT rebuilt and verified from backup.");
        SPrint(line, sizeof(line), L"gpt: %d partitions restored; primary and backup now match.",
               (int)res.after.primary.used_count);
        out_line(row, rows, cols, VT_NORMAL, line);
        efi_log(L"gpt: primary GPT rebuilt and verified");
    } else {
        SPrint(line, sizeof(line), L"gpt: repair failed: %s", gpt_reason_text(res.reason));
        out_line(row, rows, cols, VT_DIM, line);
    }

out:
    gpt_result_free(&res);
    gpt_plan_free(&plan);
    gpt_diag_free(&dg);
    gpt_disk_close(&dev);
    return 0;
}
