# Visor 1.4

Animated wallpapers, a file browser in both interfaces, `visor install` that
works straight out of an AUR package, and a round of hardening.

## What's new

- **Animated GIF backgrounds.** Drop a GIF on the ESP and Visor plays it
  full-screen in a loop. The accent palette is taken from frame one, and the
  frosted-glass panels reuse their blur while it plays so the menu stays
  fast. `background=` now accepts PNG, BMP or GIF.
- **File browser.** Press `B` in the graphics menu, or run `browse` in the
  recovery shell. Browse every readable volume, page through directories
  with sizes, and boot what you pick: a kernel (paired with a sibling
  initrd automatically), an initrd, or a `.efi` image. Booting through the
  browser pins the load to the volume you picked it from.
- **`visor install` and `visor doctor` after an AUR install.** The package
  ships no source tree, so both commands now operate on the packaged files
  instead of demanding a checkout. Non-root runs re-exec through sudo.
- **Menu clock no longer depends on the firmware timer interrupt.** Timing
  now comes from the architecture clock (TSC / generic counter), so
  countdowns and animations stay accurate even on firmwares that don't fire
  the periodic timer — QEMU under TCG included.

## Fixes

- **PNG decoder validates the zlib header length** (audit finding) before
  allocating against it.
- **Large directories no longer stall the menu.** Listings sort with a
  merge sort instead of an insertion sort.
- **The browser finds the boot volume reliably.** It probes for Visor's own
  install, then `\EFI`, instead of relying on a firmware handle match that
  doesn't hold on all boards.
- **Opening a filesystem root no longer freezes on some hardware.** The
  browser no longer force-connects every block controller mid-menu, and the
  directory read loop is bounded.
- **The centre info panel no longer collides with the browser panel** (and
  the boot countdown pauses while browsing).

## File browser — known rough edges

- Booting a picked file still needs a boot entry to attach the override to;
  with a zero-entry config there is nothing to override.
- Very slow USB media can make large listings take a moment to appear.
- Directory dates are not shown yet; sizes only.
- The browser lists what the firmware's filesystem drivers expose — a
  filesystem without a loaded driver (for example ext4 without the EfiFs
  driver) won't be readable from the browser.

## Upgrading

```sh
visor update
```

or from a checkout:

```sh
git pull && make clean && make && sudo ./install.sh
```

Existing configs and encrypted files keep working.
