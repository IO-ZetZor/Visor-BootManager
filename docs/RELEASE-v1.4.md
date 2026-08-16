# Visor — 1.4

Animated backgrounds, a file browser in both interfaces, and an installer
that finally works straight out of an AUR package — plus a round of
audit-driven hardening.

## Animated backgrounds

`background=` now accepts a GIF as well as PNG and BMP. Animated GIFs play
full-screen in a loop behind the menu.

- **Built-in GIF decoder.** A self-contained GIF89a parser (LZW
  decompression, frame disposal, loop handling) runs inside the boot
  manager, so no firmware or driver support is needed. Decoding is
  bounded so a huge or hostile GIF cannot exhaust memory.
- **Loop with a sane budget.** Frames advance on a wall-clock schedule;
  a slow redraw drops time instead of spiralling, and the animation
  stops cleanly when its loop budget is spent.
- **Accent palette from frame one.** The dynamic accent extraction reads
  the first frame, so the colour scheme still matches your wallpaper.
- **Fast rendering.** The background is drawn with precomputed
  nearest-neighbour maps (two lookups per pixel instead of two divides),
  and the frosted-glass panels reuse their blur while the animation
  plays instead of re-blurring the whole screen on every frame.
- **A clock that actually ticks.** Timing now comes from the
  architecture clock (calibrated TSC on x86, the generic counter on
  ARM) instead of the firmware's periodic timer, which some firmwares —
  and QEMU under TCG — never fire. Countdowns and animations stay
  accurate everywhere.

## File browser

Browse every readable volume from the menu (press `B`) or the recovery
shell (`browse [PATH]`), and boot what you pick.

- **All readable filesystems.** The browser enumerates every volume the
  firmware exposes, labelled by partition UUID, with the boot volume
  first. It finds the ESP by probing for Visor's own install, then
  `\EFI`, rather than trusting a firmware handle match that doesn't hold
  on all boards.
- **Real listing.** Sizes come from `EFI_FILE_INFO` in one pass;
  entries sort directories-first, case-insensitively, with a merge sort
  so even very large directories don't stall the menu (the previous
  insertion sort was quadratic).
- **Navigation.** Enter to descend, Backspace/← to go up, ↑/↓ and
  PgUp/PgDn to page, Tab/→ to switch volumes, Esc to close. The GUI
  panel also scrolls with the mouse wheel.
- **Boot what you pick.** Enter on a kernel sets a one-shot override and
  boots it, auto-pairing a sibling `initrd*`; Enter on an initrd sets
  the initrd override; Enter on a `.efi` chainloads it.
- **Booted from where you picked it.** A browsed boot is pinned to the
  volume it came from — by partition GUID *and* by volume handle — so an
  MBR stick without a GUID can't be answered by a same-named file on the
  ESP. This is the same pinning the regular entry loader uses, so the
  two paths can never disagree.

Known rough edges: booting still needs a selected entry to attach the
override to (a zero-entry config has nothing to override), very slow USB
media can make large listings take a moment, dates aren't shown yet, and
filesystems without a loaded driver (ext4 without the EfiFs driver) won't
appear.

## Installer and CLI

`visor install` and `visor doctor` now work immediately after an AUR
install, where no source tree exists.

- **Packaged install.** With no checkout present, `visor install`
  installs from the packaged files (`/usr/lib/visor` +
  `/usr/share/visor`): binary and backup, icons/backgrounds/logo, the
  config (kept unless `--force-config`), `boot.log`, and the UEFI boot
  entry — mirroring `install.sh`. `--packaged` forces the mode;
  `--source-dir` always wins for checkout users.
- **Package-aware doctor.** `visor doctor` recognises packaged installs
  and reports `ok packaged …` instead of a wall of `miss`es.
- **Root handling.** Non-root runs re-exec through sudo, and the target
  user's home is resolved via `getent` so `sudo visor …` (and
  `pkexec`) no longer looks in `/root` for your config.

## Hardening

- **PNG decoder validates the zlib header.** The declared length must
  match the one's complement of the stored length before anything is
  allocated against it (audit finding).
- **No mid-menu filesystem sweep.** The browser no longer force-connects
  every block controller from inside the menu loop — a source of freezes
  on real hardware. Drivers are connected once at boot as before.
- **Bounded directory reads.** The listing loop and its retry buffer are
  capped, so a misbehaving driver can't hang the menu.
- **No more colliding panels.** The centre-info panel and the "Booting
  in Xs" countdown are suppressed while the browser is open, and an
  empty directory says so instead of drawing nothing.

## Upgrading

```sh
visor update
```

or from a checkout:

```sh
git pull && make clean && make && sudo ./install.sh
```

Existing configs and encrypted files keep working.
