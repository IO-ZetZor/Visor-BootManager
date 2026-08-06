# Visor 1.3.3

A rendering and packaging release on top of 1.3.2: the menu text is
resampled properly at every size, and the installer learned which
architecture it is actually installing for.

*(The v1.3.2 changes — the scan-scope option, Arch packaging, and the
boot/config security fixes — are in the v1.3.2 release notes.)*

## Text rendering

The font pipeline was rebuilt around a correct downscale. Glyphs are now
baked from a 128px master and resampled with an area-average box filter
instead of point-sampling a single coverage value per pixel, so text at
normal menu sizes keeps its stroke weight instead of thinning and
breaking up.

- **Area-average glyph scaling.** Each destination pixel integrates the
  source coverage it actually covers, on both axes. Thin stems survive
  the downscale rather than dropping out between samples.
- **Subpixel positioning.** Advances are carried in 1/64 px and glyphs
  are rendered at one of four horizontal phases, so a run of text no
  longer accumulates rounding error and letter spacing stays even.
- **Glyph cache.** Rasterised glyphs are memoised per (codepoint, pixel
  size, phase) in a 512-slot table, so the extra filtering work happens
  once per distinct glyph rather than once per draw. The cache is
  flushed when the font or mode changes.

## Installer

- **Architecture detection.** `install.sh` derives the target from
  `uname -m` (overridable with `--arch x86_64|aarch64`) and uses it for
  the build, the installed binary name, the firmware boot entry and the
  EfiFs driver suffix. Previously the script always built and installed
  `visor_x64.efi` while the driver suffix followed the host, so an ARM64
  machine got an x86_64 loader and a mismatched driver.
- **Output restyle.** Step/OK/warn/error lines follow one scheme, colour
  is emitted only to a TTY (honouring `NO_COLOR` and `TERM=dumb`), and
  the Unicode symbols fall back to ASCII outside a UTF-8 locale — piped
  output and logs no longer contain escape sequences.
- **Safer failure.** A failed step reports the line and exit status and
  states that nothing further was changed. Re-installing keeps the
  previous binary alongside the new one.
- **New flags.** `--arch NAME`, `--color`, `--no-color`.

## Packaging

- **Arch package tracks releases.** The PKGBUILD now builds from the
  tagged release tarball with a real checksum, rather than fetching the
  repository URL as a plain download with checksum verification
  disabled. `pkgver` follows the release it builds.
- **aarch64 packaging.** The package builds for `aarch64` as well as
  `x86_64`, passing `ARCH` through to the build and installing the
  matching binary.
- **Void dependency name.** `get.sh` and the wiki install page install
  `gnu-efi-libs` on Void; the previous name does not exist in the
  repository.

## Configuration schema

`docs/boot.conf.schema.json` is a machine-readable description of every
key the parser accepts — types, defaults, enumerated values, aliases and
grouping — generated from `src/config.c` and validated against it. It is
a JSON Schema (draft 2020-12), intended as the single source of truth
for editors, `visor config validate`, and the wiki, so the three stop
drifting apart. It is installed to
`/usr/share/visor/boot.conf.schema.json`.

## Upgrading

```sh
visor update
```

or from a checkout:

```sh
git pull && make clean && make && sudo ./install.sh
```

Nothing in `boot.conf` changes — no keys were added, removed or
reinterpreted in this release.
