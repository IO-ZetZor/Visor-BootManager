#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ESP=""
DO_BUILD=1
DO_BOOT_ENTRY=-1
DO_SIGN=-1
FORCE_CONFIG=0

VISOR_DIR_REL="EFI/visor"          # install dir, relative to the ESP root
EFI_NAME="visor_x64.efi"

say()  { printf '\033[1;34m::\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

usage() { sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0; }

ask() {
    local prompt="$1" reply
    [ -t 0 ] || { echo 0; return; }
    printf '\033[1;36m??\033[0m %s [y/n] ' "$prompt" >&2
    read -r reply || true
    case "$reply" in [yY]|[yY][eE][sS]) echo 1 ;; *) echo 0 ;; esac
}

# --- parse args -------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --esp)          ESP="${2:-}"; shift 2 ;;
        --no-build)     DO_BUILD=0; shift ;;
        --boot-entry)   DO_BOOT_ENTRY=1; shift ;;
        --sign)         DO_SIGN=1; shift ;;
        --force-config) FORCE_CONFIG=1; shift ;;
        -h|--help)      usage ;;
        *)              die "unknown option: $1 (try --help)" ;;
    esac
done

# --- locate the ESP ---------------------------------------------------------
detect_esp() {
    # 1) systemd's bootctl knows the ESP if it is installed.
    if command -v bootctl >/dev/null 2>&1; then
        local p
        p="$(bootctl --print-esp-path 2>/dev/null || true)"
        [ -n "$p" ] && { echo "$p"; return; }
    fi
    # 2) Common mount points backed by a vfat filesystem.
    local m
    for m in /boot/efi /efi /boot; do
        if mountpoint -q "$m" 2>/dev/null && \
           [ "$(findmnt -no FSTYPE "$m" 2>/dev/null)" = vfat ]; then
            echo "$m"; return
        fi
    done
    # 3) Any mounted vfat partition flagged as an ESP.
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

# --- build ------------------------------------------------------------------
if [ "$DO_BUILD" -eq 1 ]; then
    say "Building $EFI_NAME ..."
    make --no-print-directory
fi
[ -f "$EFI_NAME" ] || die "$EFI_NAME not found - build first or drop --no-build."

# --- write access check -----------------------------------------------------
if [ ! -w "$ESP" ]; then
    die "No write permission on $ESP. Re-run with sudo."
fi

# --- install binary + assets ------------------------------------------------
say "Installing into $DEST"
mkdir -p "$DEST/icons" "$DEST/backgrounds"
install -m 0644 "$EFI_NAME" "$DEST/$EFI_NAME"

if [ -d assets/icons ]; then
    cp -f assets/icons/*.png "$DEST/icons/" 2>/dev/null || true
fi
if [ -d assets/backgrounds ]; then
    cp -f assets/backgrounds/*.png "$DEST/backgrounds/" 2>/dev/null || true
fi

# --- default config (do not clobber an existing one) ------------------------
CONF="$DEST/boot.conf"
if [ -f "$CONF" ] && [ "$FORCE_CONFIG" -eq 0 ]; then
    say "Keeping existing config: $CONF (use --force-config to replace)"
else
    install -m 0644 boot.conf.example "$CONF"
    say "Wrote default config: $CONF"
    warn "Edit $CONF and set your kernel paths / root PARTUUID before rebooting."
fi

# --- optional UEFI boot entry -----------------------------------------------
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

# --- optional Secure Boot signing (sbctl) -----------------------------------
[ "$DO_SIGN" -eq -1 ] && DO_SIGN="$(ask 'Sign Visor for Secure Boot with sbctl?')"
if [ "$DO_SIGN" -eq 1 ]; then
    if ! command -v sbctl >/dev/null 2>&1; then
        warn "sbctl not installed; skipping signing."
    else
        say "Signing $DEST/$EFI_NAME with sbctl"
        sbctl sign -s "$DEST/$EFI_NAME" || warn "sbctl sign failed (are keys enrolled?)."
    fi
fi

say "Done. Visor is installed at $DEST"
