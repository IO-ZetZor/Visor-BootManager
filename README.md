# Visor

A minimal, fast, graphical UEFI boot manager written in C.

Visor draws an icon-based boot menu with a full-screen PNG background and a
crisp scalable font, then boots **Linux** (EFI stub kernels / Unified Kernel
Images) or chainloads **Windows** (Windows Boot Manager). It is intentionally
small: a single ~260 KB `.efi`, no external runtime dependencies, no scripting
engine — just a config file and a handful of PNG assets.

# Visor Menu

<p align="center">
  <img src="docs/ss1.png" width="50%">
  <img src="docs/ss3.png" width="50%">
</p>)

## Features

- **Graphical, double-buffered menu** — flicker-free rendering via the UEFI
  Graphics Output Protocol (GOP).
- **PNG backgrounds and icons** — built-in PNG decoder (RGB and RGBA, all five
  filter types). No BMP conversion needed.
- **Scalable text** — a baked JetBrains Mono atlas, bilinearly scaled at
  runtime. Configurable title and entry-name sizes.
- **Themable colors** — title color, default entry-name color, selection accent
  color, and an optional per-entry name color, all as `#RRGGBB`.
- **Configurable title** — your own text, the default `Visor`, or none at all.
- **Linux boot** — EFI stub kernels and Unified Kernel Images (UKIs). Kernel
  command line is passed correctly as UTF-16; the stub handles
  `ExitBootServices` itself.
- **Windows boot** — chainloads `bootmgfw.efi` via its device path so the BCD
  is found (a common failure mode when loading from a memory buffer).
- **Auto-detection** — if `boot.conf` is missing, Visor scans for common Linux
  and Windows loaders and builds a menu automatically.
- **Power actions** — Shutdown, Reboot, and Firmware Setup from the menu.
- **Quiet mode** — optional black-screen hand-off instead of progress text.
- **Self-pruning logs** — `boot.log` keeps only the last 3 boots, with
  descriptive messages at every fallible step for easy debugging.

---

## Requirements

- An **x86_64 UEFI** system (GOP-capable firmware — virtually all modern UEFI).
- **gnu-efi** development files.
- **GCC** and **binutils** (`objcopy`).
- *(Optional)* **Python 3 + Pillow** — only to re-bake a different font; the
  default font is committed, so normal builds need neither.

### Install dependencies

| Distro            | Command                                            |
|-------------------|----------------------------------------------------|
| Arch              | `sudo pacman -S gnu-efi base-devel`                |
| Debian / Ubuntu   | `sudo apt install gnu-efi build-essential`         |
| Fedora            | `sudo dnf install gnu-efi gnu-efi-devel gcc make`  |
| openSUSE          | `sudo zypper in gnu-efi-devel gcc make`            |

---

## Building

```bash
make
```

This produces `visor_x64.efi` in the project root. The Makefile auto-detects
the gnu-efi headers and `crt0-efi-x86_64.o` across distros; if yours installs
them somewhere unusual, override the paths:

```bash
make GNU_EFI_INC=/path/to/efi CRT0=/path/to/crt0-efi-x86_64.o
```

If gnu-efi is missing, `make` stops early with an explanatory message instead
of a cryptic compiler error.

---

## Installation

### Quick install

```bash
sudo ./install.sh
```

`install.sh` will:

1. Build `visor_x64.efi` (skip with `--no-build`).
2. Locate your EFI System Partition (ESP) automatically — or pass
   `--esp /your/esp/mount`.
3. Copy the binary and the bundled icons/background into `\EFI\visor\`.
4. Write a starter `boot.conf` **without overwriting an existing one**.

**Options:**

| Flag               | Effect                                                        |
|--------------------|---------------------------------------------------------------|
| `--esp <path>`     | Use this ESP mount point instead of auto-detecting.           |
| `--no-build`       | Install the already-built `visor_x64.efi`.                    |
| `--boot-entry`     | Register a `Visor` UEFI boot entry via `efibootmgr`.          |
| `--fallback`       | Also install as `\EFI\BOOT\BOOTX64.EFI` (removable default).  |
| `--force-config`   | Overwrite an existing `boot.conf` with the bundled example.   |

Example — install, register a firmware boot entry, and set up the fallback path:

```bash
sudo ./install.sh --boot-entry --fallback
```

After installing, **edit `<ESP>/EFI/visor/boot.conf`** to point at your real
kernels and set your root partition.

### Secure Boot

Visor is unsigned, so with Secure Boot enabled you must sign it with your own
keys. The easiest way is [`sbctl`](https://github.com/Foxboron/sbctl):

```bash
sudo sbctl sign -s <ESP>/EFI/visor/visor_x64.efi
```

(Re-run that after every rebuild, since signing covers the exact binary.)

### Manual installation

```bash
# 1. Mount the ESP (skip if already mounted at /boot/efi).
sudo mount /dev/sdXn /mnt/esp

# 2. Copy the binary and assets.
sudo mkdir -p /mnt/esp/EFI/visor/icons /mnt/esp/EFI/visor/backgrounds
sudo cp visor_x64.efi              /mnt/esp/EFI/visor/
sudo cp assets/icons/*.png         /mnt/esp/EFI/visor/icons/
sudo cp assets/backgrounds/*.png   /mnt/esp/EFI/visor/backgrounds/
sudo cp boot.conf.example          /mnt/esp/EFI/visor/boot.conf

# 3. Register a boot entry (disk = whole disk, part = ESP partition number).
sudo efibootmgr --create --disk /dev/sdX --part n \
                --label "Visor" --loader '\EFI\visor\visor_x64.efi'
```

---

## Configuration

Config lives at `\EFI\visor\boot.conf` on the ESP. A fully-commented reference
is in [`boot.conf.example`](boot.conf.example).

**Path rules:** all paths are relative to the **root of the ESP** and use
back-slashes, e.g. `\EFI\visor\icons\arch.png`. Colors are `#RRGGBB`.

### Global settings

| Key               | Values / meaning                                                                                           |
|-------------------|------------------------------------------------------------------------------------------------------------|
| `timeout`         | `N` = auto-boot the default after N seconds · `-1` = wait forever · `0` = boot default instantly (no menu) |
| `default`         | Index of the default entry (0-based).                                                                      |
| `quiet`           | `1` = black screen during hand-off · `0` = show progress text.                                             |
| `title`           | Menu title. Empty/absent = `Visor` · `none` = no title · else verbatim.                                    |
| `font`            | Text font. Currently `jetbrains`. Empty = default.                                                         |
| `title_color`     | Title text color, `#RRGGBB`.                                                                               |
| `name_color`      | Default entry-name color, `#RRGGBB`.                                                                       |
| `highlight_color` | Selection accent/underline color, `#RRGGBB`.                                                               |
| `title_size`      | Title height in pixels. `0`/absent = `screen_height / 12`.                                                 |
| `name_size`       | Entry-name height in pixels. `0`/absent = `16`.                                                            |
| `icon_size`       | Icon edge length in pixels (square). `0`/absent = `64`.                                                    |
| `icon_spacing`    | Horizontal gap between icons in pixels. `0`/absent = `60`.                                                 |
| `icon_y`          | Vertical center of the icon row in pixels. `0`/absent = screen middle.                                     |
| `underline_color` | Selection underline color, `#RRGGBB`. Absent = `highlight_color`.                                          |
| `underline_thickness` | Underline height in pixels. `0`/absent = `4`.                                                          |
| `underline_length`| Underline width in pixels. `0`/absent = icon width + margin.                                               |
| `power_position`  | Corner for the power actions: `bottomright` (default), `bottomleft`, `topright`, `topleft`.                |
| `shutdown_color`  | Color of the **S**hutdown hotkey letter, `#RRGGBB`. Absent = `highlight_color`.                            |
| `reboot_color`    | Color of the **R**eboot hotkey letter, `#RRGGBB`. Absent = `highlight_color`.                              |
| `firmware_color`  | Color of the **F**irmware hotkey letter, `#RRGGBB`. Absent = `highlight_color`.                            |
| `background`      | Full-screen background image (PNG, RGB or RGBA). Falls back to `backgrounds/default.png` if missing/corrupt.|

### Boot entries

```conf
linux {
    name    = "Arch Linux"      # shown under the icon
    type    = linux             # linux | windows
    icon    = \EFI\visor\icons\arch.png
    color   = #1793D1           # optional: overrides name_color for this entry
    kernel  = \vmlinuz-linux    # EFI stub kernel or UKI .efi
    initrd  = \initramfs-linux.img        # optional (omit for a UKI)
    cmdline = "root=PARTUUID=... rw quiet" # optional (omit for a UKI)
}

windows {
    name   = "Windows 11"
    type   = windows
    icon   = \EFI\visor\icons\windows.png
    kernel = \EFI\Microsoft\Boot\bootmgfw.efi
}
```

| Entry key | Meaning                                                             |
|-----------|---------------------------------------------------------------------|
| `name`    | Display name under the icon.                                        |
| `type`    | `linux` or `windows`.                                               |
| `icon`    | PNG icon (square, e.g. 128×128, RGBA recommended).                  |
| `kernel`  | EFI stub kernel / UKI (`linux`) or `bootmgfw.efi` (`windows`).      |
| `initrd`  | initrd image — Linux only, optional (omit for a UKI).               |
| `cmdline` | Kernel command line — Linux only, optional (omit for a UKI).        |
| `color`   | Per-entry name color, `#RRGGBB` (overrides `name_color`).           |
| `uuid`    | Partition UUID hint for Windows chainloading, optional.             |

### Changing the font size

Set `title_size` and `name_size` (pixels) in the global section. For example:

```conf
title_size=120
name_size=28
```

Leave them out (or `0`) for the defaults (`screen_height/12` and `16`).

### Using a different font

The font is baked into the binary from a TTF, so a normal build needs no font
tooling. To swap it, regenerate the atlas (needs Python 3 + Pillow) and rebuild:

```bash
make bakefont FONT=/usr/share/fonts/TTF/YourFont.ttf FONT_PX=64
make
```

---

## Controls

| Key                          | Action                          |
|------------------------------|---------------------------------|
| `Left` / `Right` (or `Up` / `Down`) | Move selection (wraps)   |
| `Enter`                      | Boot the selected entry         |
| `1`–`9`                      | Boot entry N directly           |
| `Esc`                        | Boot the default entry          |
| `S`                          | Shut down                       |
| `R`                          | Reboot                          |
| `F`                          | Enter firmware setup            |

---

## Linux EFI stub requirements

To boot via the EFI stub, the kernel must be built with:

```
CONFIG_EFI=y
CONFIG_EFI_STUB=y
```

Most distributions enable these by default. Unified Kernel Images (e.g. from
`systemd-ukify` or `dracut --uefi`) already bundle the stub, initrd, and
cmdline — point `kernel` at the `.efi` and omit `initrd`/`cmdline`.

---

## Troubleshooting

Visor writes a log to **`\EFI\visor\boot.log`** on the ESP. It is overwritten
each boot but retains the **last 3 boots**, and logs every fallible step
(config parse, PNG decode, image load, hand-off). Read it first when something
fails — most problems are one descriptive line away.

| Symptom                         | Likely cause / fix                                                            |
|---------------------------------|-------------------------------------------------------------------------------|
| **Black screen, no menu**       | Check `boot.log`. Often a bad `background` path or a corrupt PNG.             |
| **Background shows color noise**| The PNG decoder needs a valid PNG; re-export as 8-bit RGB/RGBA.               |
| **Linux hangs after selecting** | Wrong `root=` in `cmdline`, or initrd/cmdline supplied for a UKI (drop them). |
| **Windows "failed to boot"**    | `kernel` must point at the real `\EFI\Microsoft\Boot\bootmgfw.efi`.           |
| **"Failed to load kernel"**     | Verify the path exists on the ESP and is an EFI stub / UKI.                   |
| **No graphics at all**          | Firmware must provide GOP; disable CSM / Legacy boot in firmware setup.       |

---

MASSIVELY INSPIRED BY rEFInd!!

