# Set up Visor on a USB drive

Visor is a UEFI boot manager, not an ISO-writing tool. This guide installs
Visor onto a USB drive so the computer's firmware can start it. It does not
turn Linux or Windows installer ISO files into bootable entries by itself.

## Prepare the USB drive

Use a USB drive with a FAT32 EFI System Partition that the target computer's
UEFI firmware can read. Back up any files you need before changing its
partitions or formatting it. Mount that partition in Linux and set `USB_ESP`
to its mount point:

```sh
USB_ESP="/run/media/$USER/VISOR"
```

Replace that example path with the mount point shown on your machine.

The installer writes Visor's files under `EFI/visor` on the selected
partition. From a Linux machine with network access, run the installer:

```sh
sh -c "$(curl -fsSL https://raw.githubusercontent.com/IO-ZetZor/Visor-BootManager/main/get.sh)" -- \
  --esp "$USB_ESP" \
  --no-boot-entry --no-sign --no-fs-drivers --no-studio --no-cli
```

The installer builds Visor and copies its files to the USB partition. The
`--no-boot-entry` option avoids adding an NVRAM boot entry on the computer
doing the installation. The other options skip host signing, filesystem
drivers, Studio, and the host-side `visor` command; they are not required for
Visor to start from a FAT32 USB partition.

## Add the removable-media boot path

UEFI firmware looks for a standard fallback filename when starting removable
media. Visor's installer places the binary at `EFI/visor/visor_x64.efi` on
x86-64 or `EFI/visor/visor_aa64.efi` on AArch64, so copy it to the matching
fallback path as well.

For x86-64:

```sh
sudo install -D -m 0644 \
  "$USB_ESP/EFI/visor/visor_x64.efi" \
  "$USB_ESP/EFI/BOOT/BOOTX64.EFI"
```

For AArch64:

```sh
sudo install -D -m 0644 \
  "$USB_ESP/EFI/visor/visor_aa64.efi" \
  "$USB_ESP/EFI/BOOT/BOOTAA64.EFI"
```

`install` replaces an existing file at the fallback path. If the USB already
contains another bootloader there, back it up first or use a separate USB
drive. Keep Visor's other installed files under `EFI/visor`; its config and
assets are read from that directory.

Safely eject the drive, connect it to the target computer, and choose its
UEFI USB entry from the firmware's one-time boot menu. With Secure Boot
enabled, the target computer must trust the Visor binary; signing it with a
key trusted only by the installation computer will not make it trusted on
another computer.

## Boot operating systems from USB

Visor can scan a USB volume for supported EFI boot files. It can also scan a
newly connected drive while the menu is open when hotplug is enabled. This
does not boot ISO files directly: the OS or installer USB must already contain
an EFI loader that Visor can find and chainload. For a one-time boot of an
installer USB, the firmware's own boot menu is also an option.
