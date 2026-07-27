#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ESP=""
DO_BUILD=1
DO_BOOT_ENTRY=-1
DO_SIGN=-1
DO_FS_DRIVERS=-1
FORCE_CONFIG=0
FS_DRIVER=""
INSTALL_CLI=1
EFIFS_VERSION="${EFIFS_VERSION:-v1.12}"
EFIFS_URL_OVERRIDDEN="${EFIFS_URL:-}"
EFIFS_URL="${EFIFS_URL:-https://github.com/pbatard/efifs/releases/download/$EFIFS_VERSION}"
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
  --fs-drivers       auto-detect the /boot filesystem and install the matching
                     EfiFs driver into \EFI\visor\drivers (else prompted)
  --no-fs-drivers    do not install or prompt for filesystem drivers
  --fs-driver PATH   copy a local EFI filesystem driver into \EFI\visor\drivers
  --no-cli           do not install the host-side 'visor' command
  --cli-dir PATH     directory for the host-side command (default: /usr/local/bin)
  --force-config     overwrite an existing boot.conf with the default
  -h, --help         show this help
EOF
    exit 0
}

ask() {
    local prompt="$1" reply
    if [ -t 0 ]; then
        printf '\033[1;36m??\033[0m %s [y/n] ' "$prompt" >&2
        read -r reply || true
    elif [ -r /dev/tty ]; then
        printf '\033[1;36m??\033[0m %s [y/n] ' "$prompt" >&2
        read -r reply < /dev/tty || true
    else
        echo 0; return
    fi
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
        --fs-drivers)   DO_FS_DRIVERS=1; shift ;;
        --no-fs-drivers) DO_FS_DRIVERS=0; shift ;;
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

boot_fs_type() {
    local t=""
    if mountpoint -q /boot 2>/dev/null; then
        t="$(findmnt -no FSTYPE /boot 2>/dev/null || true)"
    fi
    [ -z "$t" ] && t="$(findmnt -no FSTYPE / 2>/dev/null || true)"
    echo "$t"
}

efifs_sha256_for() {
    case "$1" in
        btrfs_aa64.efi) echo 92dae5f1d0f6055afb6fd851a438e6173e7a452582f9ff13038ad4f2bf5088b9 ;;
        btrfs_x64.efi) echo 8ae24aa9f38f71a1e347fb6d0646b4678e04466aadab9919fb2ad133d5ee879c ;;
        exfat_aa64.efi) echo 629e567847ba028cb6ba1f75af12b1ace2094a6b1e70cddbfe1a99a82cdd0511 ;;
        exfat_x64.efi) echo 21a5969dcd7b6c149b1dc9408c591749ba9c62fb264e2852cc70061fe3defff6 ;;
        ext2_aa64.efi) echo a472ec2641475dfcc2dff290472186c9d2424ae005a1a3c46809aac2785d146b ;;
        ext2_x64.efi) echo e009f02f25b9c5ad3beaf0d3a04f89042985eea1d90187b888130a708c35ca61 ;;
        f2fs_aa64.efi) echo df07c2bc9f485e8b01e707552852a6ee129e74e7085bd5547b29734ae4848beb ;;
        f2fs_x64.efi) echo 74490317fbbb4c3f37072c1c1ab93557d1c8834c533690970c41b4310b4527cb ;;
        hfsplus_aa64.efi) echo fa23cc880464ec5daeb669b7a3a373e524136e87459a5585c2ba23e59ddfe1fb ;;
        hfsplus_x64.efi) echo 894d5b2985808d92ae8a5476fd942d39075f4730ac86d9c182e421208af5fbaf ;;
        jfs_aa64.efi) echo 8f8d8388f34342eca7cf566f2b5bdcadd44bc6862d0ffb9cde3d3535e8d58a51 ;;
        jfs_x64.efi) echo 716d6328ba85d29faa7de377dc61e5a154f3455b3680037b06ec2b025e8eba82 ;;
        nilfs2_aa64.efi) echo 33a74897a89828fb5ad9f311b0942716438fadd2ba19cf642b692bcec30d970b ;;
        nilfs2_x64.efi) echo 2c43afe61c5d1fa309cadf79b39a789eca7424b9ed0363efbb246664d6b51a4e ;;
        ntfs_aa64.efi) echo 5eb1827942bdc8006a714d719b9c80268bb57095d7e484e9529f47781d68c672 ;;
        ntfs_x64.efi) echo 59c37d5026ca14553a158939e3f2cf20286b6135a713a62c08b569ac9caedcb7 ;;
        reiserfs_aa64.efi) echo 5cb5300186487fbb497497889674c585a6e93c9a189a3fcdaf9ec411e3c85439 ;;
        reiserfs_x64.efi) echo d14fd72d34cd04163cd810d465394c6664f30b8c4b45e96fa8b1b974b2933ac0 ;;
        ufs2_aa64.efi) echo aba8cc7949b77986071c37a9e49502b884a09700d4006913fe39c4840aefc0e1 ;;
        ufs2_x64.efi) echo 5f916e9263fc32bccd4f1e82b62d4fb72bc4b99b19010b4d4962a87d30854157 ;;
        xfs_aa64.efi) echo 82af528c35f464f106af40c39336e18ec6a8972bab16054d7e4b4959d11d80f7 ;;
        xfs_x64.efi) echo f75d595d8037d0f5612b4eaec2d6f4a581353530a792d904c2c6988d358e07f6 ;;
        zfs_aa64.efi) echo 8c710d400bb4d57129cc96363d21e42ea2ab1835ca52182840ef3ebcdf6c0b53 ;;
        zfs_x64.efi) echo e61dae69979979977f2ffa30283b86526e54fed8ad240ee4c0529690b1e5342d ;;
        *) echo "" ;;
    esac
}

efifs_verify_sha256() {
    local want got
    want="$(efifs_sha256_for "$2")"
    if [ -z "$want" ]; then
        warn "No pinned SHA-256 for $2 - skipping checksum verification."
        return 0
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        got="$(sha256sum "$1" | cut -d" " -f1)"
    elif command -v shasum >/dev/null 2>&1; then
        got="$(shasum -a 256 "$1" | cut -d" " -f1)"
    else
        warn "sha256sum not available - skipping checksum verification."
        return 0
    fi
    [ "$got" = "$want" ]
}

efifs_name_for() {
    case "$1" in
        ext2|ext3|ext4) echo ext2 ;;
        btrfs)          echo btrfs ;;
        xfs)            echo xfs ;;
        f2fs)           echo f2fs ;;
        zfs)            echo zfs ;;
        ntfs|ntfs3)     echo ntfs ;;
        reiserfs)       echo reiserfs ;;
        nilfs2)         echo nilfs2 ;;
        jfs)            echo jfs ;;
        exfat)          echo exfat ;;
        hfsplus)        echo hfsplus ;;
        ufs2)           echo ufs2 ;;
        *)              echo "" ;;
    esac
}

efifs_arch_suffix() {
    case "$(uname -m)" in
        aarch64) echo aa64 ;;
        *)       echo x64 ;;
    esac
}

fetch_file() {
    local url="$1" out="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --connect-timeout 15 -o "$out" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -T 15 -O "$out" "$url"
    else
        return 2
    fi
}

install_efifs_driver() {
    local fstype="$1" name driver tmp
    name="$(efifs_name_for "$fstype")"
    if [ -z "$name" ]; then
        warn "No EfiFs driver known for filesystem '$fstype' - copy one manually with --fs-driver."
        return 1
    fi
    driver="${name}_$(efifs_arch_suffix).efi"
    tmp="$(mktemp)"
    say "Downloading EfiFs driver $driver ..."
    if ! fetch_file "$EFIFS_URL/$driver" "$tmp"; then
        rm -f "$tmp"
        warn "Download failed ($EFIFS_URL/$driver). Install it later with --fs-driver PATH."
        return 1
    fi
    if [ "$(head -c2 "$tmp" 2>/dev/null)" != "MZ" ]; then
        rm -f "$tmp"
        warn "Downloaded file is not an EFI binary - skipping driver install."
        return 1
    fi
    if [ -z "${EFIFS_URL_OVERRIDDEN:-}" ] && [ "$EFIFS_VERSION" = "v1.12" ]; then
        if ! efifs_verify_sha256 "$tmp" "$driver"; then
            rm -f "$tmp"
            warn "SHA-256 mismatch for $driver - refusing to install it."
            return 1
        fi
        say "Driver checksum verified (EfiFs $EFIFS_VERSION)"
    else
        warn "EFIFS_URL/EFIFS_VERSION overridden - checksum table does not apply."
    fi
    mkdir -p "$DEST/drivers"
    install -m 0644 "$tmp" "$DEST/drivers/$driver"
    rm -f "$tmp"
    DRIVER_DESTS+=("$DEST/drivers/$driver")
    say "Installed filesystem driver: $DEST/drivers/$driver"
}

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
    rm -f "$EFI_NAME"
    if ! make --no-print-directory; then
        die "Build failed - not installing. Fix the errors above (see README 'Requirements')."
    fi
    [ -f "$EFI_NAME" ] || die "Build reported success but $EFI_NAME is missing - aborting."
fi
[ -f "$EFI_NAME" ] || die "$EFI_NAME not found - build first or drop --no-build."

if command -v objdump >/dev/null 2>&1; then
    if ! objdump -h "$EFI_NAME" 2>/dev/null | grep -q '\.text'; then
        die "$EFI_NAME looks malformed (no .text section) - refusing to install."
    fi
fi

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
if [ -f assets/logo.png ]; then
    install -m 0644 assets/logo.png "$DEST/logo.png" 2>/dev/null || true
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

DRIVER_DESTS=()
if [ -n "$FS_DRIVER" ]; then
    mkdir -p "$DEST/drivers"
    install -m 0644 "$FS_DRIVER" "$DEST/drivers/$(basename "$FS_DRIVER")"
    DRIVER_DESTS+=("$DEST/drivers/$(basename "$FS_DRIVER")")
    say "Installed filesystem driver: $DEST/drivers/$(basename "$FS_DRIVER")"
fi

BOOT_FS="$(boot_fs_type)"
if [ "$DO_FS_DRIVERS" -ne 0 ] && [ -z "$FS_DRIVER" ]; then
    case "$BOOT_FS" in
        vfat|msdos|"")
            [ "$DO_FS_DRIVERS" -eq 1 ] && \
                say "Kernels live on a FAT filesystem the firmware already reads - no driver needed."
            ;;
        *)
            if [ "$DO_FS_DRIVERS" -eq -1 ]; then
                say "Your kernels appear to live on a '$BOOT_FS' filesystem, which UEFI cannot read."
                DO_FS_DRIVERS="$(ask "Install the EfiFs $BOOT_FS driver so Visor can load them?")"
            fi
            [ "$DO_FS_DRIVERS" -eq 1 ] && install_efifs_driver "$BOOT_FS" || true
            ;;
    esac
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
        for d in ${DRIVER_DESTS[@]+"${DRIVER_DESTS[@]}"}; do
            say "Signing $d with sbctl"
            sbctl sign -s "$d" || warn "sbctl sign failed for the driver."
        done
    fi
fi

say "Done. Visor is installed at $DEST"
