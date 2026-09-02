# GPT diagnostics & recovery

Visor can detect a damaged GPT partition table at boot and offer to repair it
safely. This page explains how the detector classifies a disk, what it is ever
willing to overwrite, and how the two user-facing flows (boot-menu warning and
recovery console command) work.

## Background

A GPT lives on a disk as **two copies** of the same metadata, each with its own
header CRC32 and partition-entry-array CRC32:

```
+-------+-------+---------------------+---------------------+
| LBA 0 | LBA 1 | LBA 2 .. entries    |  ...    | LBA N-1   |
| MBR   | hdr P | entries P           | backups | hdr B     |
+-------+-------+---------------------+         +-----------+
         primary copy                                 backup copy
```

The two copies should be byte-for-byte identical (modulo the header fields that
record each copy's own location). The redundancy is what makes safe repair
possible: when exactly one copy is corrupt and the other passes all checks, the
healthy copy can be copied over the damaged one.

Everything here lives in `src/gpt.c` (detection, verification, planning,
execution) with a device abstraction in `src/gpt_disk.c`. Neither touches the
*data* partitions. Only the metadata areas described below can ever be written.

## Detection & classification

`gpt_diagnose(dev, full)` reads both GPT copies and produces a `gpt_diag_t`:

* `primary` / `backup` — per-copy breakdown:
  * `present` — did the copy even exist (signature + MBR protective entry)?
  * `header_status` (`GPT_VALID` / `GPT_WARNING` / `GPT_INVALID`) — header
    CRC, signature, revision, `my_lba`/`alternate_lba` fields, usable range,
    entry count/size, on-disk bounds.
  * `entries_status` — entry array present, in-bounds, not overlapping the
    usable partition range, CRC32 matches the header's recorded value.
  * `layout_status` — every partition entry lies within the usable range, on
    the disk, with no overlaps and no duplicate GUIDs.
* `cmp` — how the primary and backup compare (`GPT_CMP_IDENTICAL`,
  `GPT_CMP_DIFFERENT`, `GPT_CMP_AMBIGUOUS`, plus the single-copy invalid
  kinds). `diff` is a bitmask of which fields differ and `raw_identical`
  records a byte-for-byte entry-array match.
* `mbr_status` — protective/hybrid MBR sanity (including the "protective
  entry claims size 0" check).
* `klass` — the overall classification.
* `capability` — what recovery is safe to do, if anything.

The `full == 0` fast path only reads LBAs 0–1 and the last sector; it falls
through to a full analysis whenever the primary header is not pristine. In
practice the boot-menu scan uses the fast path and the console commands use the
full scan.

### Classes

| `klass` | Meaning | Recoverable? |
|---|---|---|
| `GPT_CLASS_NOT_GPT` | No GPT present (single flag, empty, plain MBR, foreign). | No |
| `GPT_CLASS_HEALTHY` | Both copies present, valid, identical. | No (nothing to do) |
| `GPT_CLASS_PRIMARY_CORRUPT_BACKUP_VALID` | Primary damaged, backup fully valid. | **Yes, automatically** |
| `GPT_CLASS_BACKUP_CORRUPT_PRIMARY_VALID` | Backup damaged, primary fully valid. | Manual only with care |
| `GPT_CLASS_BOTH_COPIES_CORRUPT` | Neither copy verifies. | No |
| `GPT_CLASS_COPIES_DIFFER` | Both verify but are not identical. | Manual only |
| `GPT_CLASS_INVALID_LAYOUT` | Valid CRC but partitions out of bounds/overlapping. | No |
| `GPT_CLASS_UNSAFE_TO_RECOVER` | State is ambiguous or data could be lost. | No (never guessed) |

Classification is deliberately **conservative**. The one combination that is
ever auto-repaired is: **backup copy fully valid and MBR protective**, with the
primary unusable. That combination is what proves the disk is really a GPT disk
— a stale backup alone is not enough. Specifically:

* a primary that fails CRC but is still *present* (signature readable) with a
  valid backup → recoverable;
* a primary that is **wiped entirely** (signature gone, `present == 0`) with a
  valid backup and a **protective MBR** → still recoverable: a zeroed first
  sector is one of the most common ways a primary GPT dies, and the backup plus
  protective MBR is what proves the disk is GPT rather than freshly wiped;
* a disk whose MBR is a **genuine** (non-protective) partition table with only
  a stale GPT backup left behind → `GPT_CLASS_UNSAFE_TO_RECOVER`. Rewriting the
  primary there would resurrect a layout the user has since replaced.
* The MBR "protective" sanity check also flags a protective entry whose size
  field is zero (`GPT_R_MBR_WRONG_SIZE`), which makes the disk class
  `GPT_CLASS_UNSAFE_TO_RECOVER` rather than guessing.

Only when the write is provably confined to the small metadata area (see
below) is automatic recovery considered.

### Why "primary corrupt, backup valid" is the only auto-reply

1. The primary copy lives in a small, fixed metadata area (LBA 1 + entries),
   so overwriting it cannot collide with user data.
2. The backup copy is a complete, independently validated source whose header
   and entries both pass CRC.
3. The write is *verified after* by re-running the full diagnostic and
   requiring primary + backup to be valid and identical. Anything short of
   that is reported as failure, not success.

`GPT_CLASS_BACKUP_CORRUPT_PRIMARY_VALID` is deliberately *not* auto-repaired:
the backup metadata area sits at the end of the disk immediately before the
last usable sector, and regenerating it would require grinding over the
backup-header location fields. It is listed for informational purposes.

## The plan

A `gpt_plan_t` is only ever produced when the disk is in the auto-recoverable
class and the source passes. Building the plan (`gpt_build_primary_plan`):

* verifies `klass == GPT_CLASS_PRIMARY_CORRUPT_BACKUP_VALID`,
* verifies the backup `gpt_table_is_source` (signature, header CRC, bounds),
* refuses `read_only` media,
* computes `dst_header_lba = 1`, `dst_entries_lba = 2` and the entry-array
  span from the backup's `entry_count * entry_size`,
* rejects the plan unless the metadata area fits strictly *before*
  `first_usable_lba` (`GPT_R_NO_SAFE_DESTINATION`) — i.e. the rewrite can
  never touch a data partition,
* rebuilds the primary header from the backup's raw header bytes,
  preserving the backup's `header_size` and any vendor-specific tail bytes
  beyond the standard 92, then recomputes the entry-array CRC32 and header
  CRC32.

The plan records the source's identifying data (backup LBA, entry LBA, CRCs,
disk GUID) so the source can be re-verified right before writing.

## Execution & safety

`gpt_execute_plan(dev, plan, result)` refuses anything not marked
`GPT_VALID`, then:

1. **Re-validates the source** (`gpt_revalidate_source`): re-reads the backup
   header and entry array and re-checks CRCs/identity against the plan. The
   underlying device (see below) also refuses I/O if the media identity moved
   since it was opened.
2. Captures a **preimage** of the exact bytes the plan is about to overwrite
   (LBA 0/1 and the whole primary entry area) so an operator can inspect or
   restore them.
3. Writes the **entry array** to LBA 2 and flushes.
4. Writes the **primary header** to LBA 1 and flushes. (Entries-then-header
   ordering means an interrupted write leaves a detectable pair mismatch, not
   a plausible-but-wrong table.)
5. Re-runs the full diagnostic on the disk and requires, for success:
   primary present + header/entries/layout valid **and** backup present +
   valid **and** primary == backup. Any deviation →
   `GPT_R_POST_WRITE_VERIFY_FAILED`.

### Media identity (`gpt_disk.c`)

`src/gpt_disk.c` opens each block device once and remembers its **MediaId,
block size and last block at open time**. Every read/write/flush re-checks
those three values and returns `EFI_MEDIA_CHANGED` if any moved — so a USB key
pulled and replaced between diagnosis and repair cannot be silently written to
under the same identity. All user commands and the in-memory plan compare
against this stable identity rather than re-reading the current `MediaId`
every call.

If a write or flush fails, the result reports exactly which step failed; the
backup copy is never among the write targets. The only LBAs ever written are
`dst_header_lba` (1) and `dst_entries_lba .. +entry_array_sectors` (2..); the
backup header at the last LBA and all data partitions are unwritable by this
code path, and the plan builder proves the entry span sits strictly before
`first_usable_lba`.

## User flows

### Recovery console (`esc` at the boot menu)

The console `gpt` command family is implemented in `src/text_menu.c`. Disks
are addressed by their stable **media id** (printed by the scan), never a
transient enumeration order, so `gpt repair 7` cannot hit a different drive
than the scan reported:

* `gpt` — enumerate every GPT disk and print its class, capability and
  overall status without touching anything.
* `gpt <media>` — full per-copy report for one disk (MBR, header, entries,
  layout, partition list).
* `gpt repair <media>` — diagnose; if the disk is auto-recoverable, print the
  plan (source backup LBA, target LBAs, partition count), require you to type
  `YES`, execute, then report the verified result
  (`gpt: N partitions restored; primary and backup now match.`).

### Boot-menu warning modal

On entering the boot menu, Visor runs the fast-path scan over every
whole-disk block device **once, before the menu loop starts and regardless of
the hotplug setting**. If a disk is in the auto-recoverable class with a valid
plan, a modal appears. The scan is *not* repeated on the fixed hotplug poll
timer; it only runs again when a hotplug event reports that the disk set
changed. Any pending keystrokes are drained before the modal opens so a stray
Enter cannot trip the destructive step.

> **Possible disk corruption detected**
> Disk: media N — <size> sectors x <block size> bytes
> The primary partition table on this disk fails validation. A verified backup
> copy exists and has passed all safety checks. Repair restores the primary
> table from the backup; the backup copy is never written.

* **Enter** — go to the *confirm* step.
* **D** — recovery *details* (per-copy header/entries/layout status, usable
  range, and the diagnostic notes).
* **Esc** / **B** — *boot anyway*. The warning stays dismissed for this
  session and the disk is left as-is.

The **confirm** step shows exactly what will be written (primary header at LBA
1 plus the entry array) and requires you to type **YES** and press Enter;
typing `NO` or pressing Esc returns to the overview. After the write (which
uses the same `gpt_execute_plan` path — source re-validation and post-write
verify), the modal reports the post-repair state:

> Repair completed and verified.
> Primary GPT : VALID
> Backup GPT  : VALID
> Partition tables : MATCH

The modal only appears when the disk passes **all** checks: correct class,
not read-only, *protective* MBR, backup fully valid, and a `GPT_VALID`
buildable plan. In every other state nothing is offered.

## Tests

`tools/gpt_host.c` is a self-contained host harness (no firmware, no gnu-efi
toolchain) that `#include`s `gpt.c` and drives a synthetic in-memory disk with
fault injection. It builds with `gcc -fshort-wchar -Isrc/include -Itools
-Itools/fakeefi tools/gpt_host.c` and must end with:

```
86 passed, 0 failed
```

ASan/UBSan are enabled in CI. The suite covers: valid disks; corrupt/missing
primary and backup header+entries (alone and together); CRC-only vs. content
differences (`GPT_CMP_DIFFERENT`); the ambiguous interpretation when GPT
metadata is present but neither copy verifies; partitions off-disk,
overlapping, metadata-overlap; integer overflow in the 32-bit math; non-512
sector sizes; large disks; write failures; disks changed between diagnosis and
repair; simulated power loss between the two writes; the fast path; header
CRC recomputation when vendor bytes beyond the standard 92 are preserved; the
MBR protective-entry size==0 check; and a deterministic fuzz pass. A
self-contained `notes` rendering check validates the operator-facing diagnostic
strings.

End-to-end VM verification runs Visor against a corrupt QEMU disk image
(`vm/esp` ESP, `gptdisk_*sample*.img` as the data disk) and confirms, after a
repair, that the disk is byte-identical to its pristine snapshot and that
`vm/esp/EFI/visor/boot.log` logs `gpt: primary GPT rebuilt and verified from
backup`.