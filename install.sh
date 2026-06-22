#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ESP=""
DO_BUILD=1
DO_BOOT_ENTRY=-1
DO_SIGN=-1
FORCE_CONFIG=0
FS_DRIVER=""
INSTALL_CLI=1
CLI_DIR="${CLI_DIR:-/usr/local/bin}"

VISOR_DIR_REL="EFI/visor"          
EFI_NAME="visor_x64.efi"
CLI_NAME="visor"

say()  { printf '\033[1;34m::\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
install.sh - install Visor to the EFI System Partition (ESP)

Usage: ./install.sh [options]

  --esp PATH         ESP mount point (auto-detected if omitted)
  --no-build         skip 'make'; install the existing visor_x64.efi
  --boot-entry       add a UEFI boot entry via efibootmgr (else prompted)
  --no-boot-entry    do not add or prompt for a UEFI boot entry
  --sign             sign Visor for Secure Boot via sbctl (else prompted)
  --no-sign          do not sign or prompt for Secure Boot signing
  --fs-driver PATH   copy an EFI filesystem driver into \EFI\visor\drivers
  --no-cli           do not install the host-side 'visor' command
  --cli-dir PATH     directory for the host-side command (default: /usr/local/bin)
  --force-config     overwrite an existing boot.conf with the default
  -h, --help         show this help
EOF
    exit 0
}

ask() {
    local prompt="$1" reply
    [ -t 0 ] || { echo 0; return; }
    printf '\033[1;36m??\033[0m %s [y/n] ' "$prompt" >&2
    read -r reply || true
    case "$reply" in [yY]|[yY][eE][sS]) echo 1 ;; *) echo 0 ;; esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        --esp)          ESP="${2:-}"; shift 2 ;;
        --no-build)     DO_BUILD=0; shift ;;
        --boot-entry)   DO_BOOT_ENTRY=1; shift ;;
        --no-boot-entry) DO_BOOT_ENTRY=0; shift ;;
        --sign)         DO_SIGN=1; shift ;;
        --no-sign)      DO_SIGN=0; shift ;;
        --fs-driver)    FS_DRIVER="${2:-}"; shift 2 ;;
        --no-cli)       INSTALL_CLI=0; shift ;;
        --cli-dir)      CLI_DIR="${2:-}"; shift 2 ;;
        --force-config) FORCE_CONFIG=1; shift ;;
        -h|--help)      usage ;;
        *)              die "unknown option: $1 (try --help)" ;;
    esac
done

if [ -n "$FS_DRIVER" ]; then
    [ -f "$FS_DRIVER" ] || die "fs-driver not found: $FS_DRIVER"
    case "$FS_DRIVER" in
        *.efi|*.EFI) ;;
        *) die "fs-driver must be an .efi file: $FS_DRIVER" ;;
    esac
fi

detect_esp() {
    if command -v bootctl >/dev/null 2>&1; then
        local p
        p="$(bootctl --print-esp-path 2>/dev/null || true)"
        [ -n "$p" ] && { echo "$p"; return; }
    fi
    local m
    for m in /boot/efi /efi /boot; do
        if mountpoint -q "$m" 2>/dev/null && \
           [ "$(findmnt -no FSTYPE "$m" 2>/dev/null)" = vfat ]; then
            echo "$m"; return
        fi
    done
    if command -v lsblk >/dev/null 2>&1; then
        lsblk -o MOUNTPOINT,PARTTYPENAME -rn 2>/dev/null | \
            awk -F' ' '/EFI System/ && $1!="" {print $1; exit}'
    fi
}

if [ -z "$ESP" ]; then
    say "Detecting EFI System Partition..."
    ESP="$(detect_esp || true)"
fi
[ -n "$ESP" ] || die "Could not find the ESP. Re-run with: --esp /your/esp/mount"
[ -d "$ESP" ] || die "ESP path does not exist: $ESP"
say "Using ESP: $ESP"

DEST="$ESP/$VISOR_DIR_REL"

if [ "$DO_BUILD" -eq 1 ]; then
    say "Building $EFI_NAME ..."
    make --no-print-directory
fi
[ -f "$EFI_NAME" ] || die "$EFI_NAME not found - build first or drop --no-build."

if [ ! -w "$ESP" ]; then
    die "No write permission on $ESP. Re-run with sudo."
fi

say "Installing into $DEST"
mkdir -p "$DEST/icons" "$DEST/backgrounds"
install -m 0644 "$EFI_NAME" "$DEST/$EFI_NAME"

if [ -d assets/icons ]; then
    cp -f assets/icons/*.png "$DEST/icons/" 2>/dev/null || true
fi
if [ -d assets/backgrounds ]; then
    cp -f assets/backgrounds/*.png "$DEST/backgrounds/" 2>/dev/null || true
fi

if [ "$INSTALL_CLI" -eq 1 ] && [ -f "$CLI_NAME" ]; then
    if mkdir -p "$CLI_DIR" 2>/dev/null && install -m 0755 "$CLI_NAME" "$CLI_DIR/$CLI_NAME" 2>/dev/null; then
        say "Installed command: $CLI_DIR/$CLI_NAME"
    else
        warn "Could not install $CLI_NAME command to $CLI_DIR"
    fi
fi

CONF="$DEST/boot.conf"
if [ -f "$CONF" ] && [ "$FORCE_CONFIG" -eq 0 ]; then
    say "Keeping existing config: $CONF (use --force-config to replace)"
else
    install -m 0644 boot.conf.example "$CONF"
    say "Wrote default config: $CONF"
    warn "Edit $CONF and set your kernel paths / root PARTUUID before rebooting."
fi

DRIVER_DEST=""
if [ -n "$FS_DRIVER" ]; then
    mkdir -p "$DEST/drivers"
    DRIVER_DEST="$DEST/drivers/$(basename "$FS_DRIVER")"
    install -m 0644 "$FS_DRIVER" "$DRIVER_DEST"
    say "Installed filesystem driver: $DRIVER_DEST"
fi

[ "$DO_BOOT_ENTRY" -eq -1 ] && DO_BOOT_ENTRY="$(ask 'Add a UEFI boot entry for Visor with efibootmgr?')"
if [ "$DO_BOOT_ENTRY" -eq 1 ]; then
    if ! command -v efibootmgr >/dev/null 2>&1; then
        warn "efibootmgr not installed; skipping boot entry."
    else
        src="$(findmnt -no SOURCE "$ESP")" || die "Cannot resolve ESP device."
        disk="/dev/$(lsblk -no PKNAME "$src")"
        partnum="$(lsblk -no PARTN "$src" 2>/dev/null || \
                   echo "$src" | grep -o '[0-9]*$')"
        if [ ! -b "$disk" ]; then
            warn "Could not determine ESP disk (got '$disk'); skipping boot entry."
        elif efibootmgr | grep -q 'Visor'; then
            say "A 'Visor' boot entry already exists; leaving it untouched."
        else
            loader="\\${VISOR_DIR_REL//\//\\}\\$EFI_NAME"
            say "Creating UEFI boot entry 'Visor' -> $disk part $partnum"
            efibootmgr --create --disk "$disk" --part "$partnum" \
                       --label "Visor" --loader "$loader" >/dev/null
        fi
    fi
fi

[ "$DO_SIGN" -eq -1 ] && DO_SIGN="$(ask 'Sign Visor for Secure Boot with sbctl?')"
if [ "$DO_SIGN" -eq 1 ]; then
    if ! command -v sbctl >/dev/null 2>&1; then
        warn "sbctl not installed; skipping signing."
    else
        say "Signing $DEST/$EFI_NAME with sbctl"
        sbctl sign -s "$DEST/$EFI_NAME" || warn "sbctl sign failed (are keys enrolled?)."
        if [ -n "$DRIVER_DEST" ]; then
            say "Signing $DRIVER_DEST with sbctl"
            sbctl sign -s "$DRIVER_DEST" || warn "sbctl sign failed for the driver."
        fi
    fi
fi

say "Done. Visor is installed at $DEST"
