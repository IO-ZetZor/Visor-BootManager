# Visor 

A minimal, fast, graphical UEFI boot manager written in C.

Visor draws an icon-based boot menu which combines the efficiency and speed of grub with the beauty of refind, capable of booting **Linux** (EFI stub kernels / Unified Kernel Images) or chainloading **Windows** (Windows Boot Manager). It is intentionally small: a single ~260 KB .efi, no external dependencies, no scripting bs — just a config and ur imagination.

# Visor Menu

<p align="center">
  <img src="docs/ss2.png" width="49%">
  <img src="docs/ss1.png" width="49%">
</p> 

## Features

- **Graphical, double-buffered menu** — flicker-free rendering via the UEFI
  Graphics Output Protocol (GOP).
- **Fluid Animations** — smooth animations for switching between entries.  
- **Customizable UI** — almost everything you see can be customized.
- **Auto-detection** — if `boot.conf` is missing, Visor scans for common Linux
  and Windows loaders and builds a menu automatically.
- **Self-pruning logs** — `boot.log` keeps only the last 3 boots, with
  descriptive messages at every fallible step for easy debugging.

---

## Requirements

- An **x86_64 UEFI** system
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
**Options:**

| Flag               | Effect                                                        |
|--------------------|---------------------------------------------------------------|
| `--esp <path>`     | Use this ESP mount point instead of auto-detecting.           |
| `--no-build`       | Install the already-built `visor_x64.efi`.                    |
| `--boot-entry`     | Register a `Visor` UEFI boot entry via `efibootmgr`.          |
| `--force-config`   | Overwrite an existing `boot.conf` with the bundled example.   |

```

After installing, **edit `<ESP>/EFI/visor/boot.conf`** to point at your real
kernels and set your root partition.

```
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
| `entries_per_page`| Entries shown per page. Default `3`.                                                                       |
| `title`           | Menu title. Empty/absent = `Visor` · `none` = no title · else verbatim.                                    |
| `font`            | Text font. Currently `jetbrains`. Empty = default.                                                         |
| `theme`           | Load `themes/<name>.conf`; its UI values override those in `boot.conf`. See [Themes](#themes).             |
| `title_color`     | Title text color, `#RRGGBB`.                                                                               |
| `name_color`      | Default entry-name color, `#RRGGBB`.                                                                       |
| `highlight_color` | Selection accent/underline color, `#RRGGBB`.                                                               |
| `blur`            | Blurred-glass highlight that follows the selection (entries **and** power actions), replacing the flat card. `0`/absent = flat card · `1` = frosted (blur + tint) · `clear` = clear blur (no tint). Edges are feathered. |
| `blur_title`      | `1` = static blurred panel behind the title. Independent of `blur`.                                       |
| `blur_color`      | Tint colour for frosted mode, `#RRGGBB`. Absent = white. Ignored when `blur=clear`.                       |
| `anim_speed`      | Selection animation speed, `1` (slow) .. `10` (fast). Default `8`. Entry↔power cross-fades; within a row/column slides. |
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
| `power_icons`     | `1` = draw the power actions as icons instead of text (still triggered by S/R/F). Needs the `*_icon` keys.  |
| `power_icon_size` | Power-icon edge length in pixels. `0`/absent = `40`.                                                       |
| `shutdown_icon`   | PNG for the Shutdown action (used when `power_icons=1`). Falls back to text if missing.                     |
| `reboot_icon`     | PNG for the Reboot action (used when `power_icons=1`). Falls back to text if missing.                       |
| `firmware_icon`   | PNG for the Firmware action (used when `power_icons=1`). Falls back to text if missing.                     |
| `background`      | Full-screen background image (PNG, RGB or RGBA). Falls back to `backgrounds/default.png` if missing/corrupt.|

### Boot entries (Examples)

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
| `name`      | Display name under the icon.                                      |
| `type`      | `linux` or `windows`.                                             |
| `icon`      | PNG icon (square, e.g. 128×128, RGBA recommended).                |
| `icon_size` | Per-entry icon edge length in pixels; overrides global `icon_size`|
| `kernel`  | EFI stub kernel / UKI (`linux`) or `bootmgfw.efi` (`windows`).      |
| `initrd`  | initrd image — Linux only, optional (omit for a UKI).               |
| `cmdline` | Kernel command line — Linux only, optional (omit for a UKI).        |
| `color`   | Per-entry name color, `#RRGGBB` (overrides `name_color`).           |
| `uuid`    | Partition UUID hint for Windows chainloading, optional.             |
| `sha256`  | Pin the image's SHA-256 (64 hex). Boot is refused on mismatch.      |

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

### Themes

Instead of scattering UI tweaks through `boot.conf`, you can keep a whole look in
a theme file and switch with a single line.

1. Create `\EFI\visor\themes\<name>.conf` on the ESP (a `themes/` folder is made
   for you at install time, with a sample `nord.conf`).
2. Put any UI keys in it — the same keys used in `boot.conf` (colors, sizes,
   `icon_*`, `underline_*`, `power_position`, `background`, `title`, …).
3. In `boot.conf`, set `theme=<name>`.

When a theme is set, its values **override** whatever `boot.conf` specifies for
those keys. Keys the theme doesn't mention keep their `boot.conf`/default value.
Boot entries (`linux {}` / `windows {}`) always live in `boot.conf` — themes are
UI-only.

```conf
# boot.conf
theme=nord
```

```conf
# themes/nord.conf  (overrides boot.conf's UI)
title=Visor
title_color=#88C0D0
name_color=#ECEFF4
underline_color=#A3BE8C
underline_thickness=8
icon_size=128
power_position=bottomright
shutdown_color=#BF616A
reboot_color=#EBCB8B
firmware_color=#A3BE8C
background=\EFI\visor\backgrounds\default.png
```

---

## Controls

| Key            | Action                                                               |
|----------------|----------------------------------------------------------------------|
| `Left`/`Right` | Move between boot entries (wraps)                                    |
| `Down`         | Enter the power column (Shutdown), then move down it                 |
| `Up`           | Move up the power column; from the top, return to the boot entries   |
| `Enter`        | Boot the focused entry, or run the focused power action              |
| `1`–`9`        | Boot entry N directly                                                |
| `Esc`          | Boot the default entry                                               |
| `S`            | Shut down (works from anywhere)                                      |
| `R`            | Reboot (works from anywhere)                                         |
| `F`            | Enter firmware setup (works from anywhere)                           |



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

---

MASSIVELY INSPIRED BY rEFInd!!

