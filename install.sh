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
ARCH=""
USE_COLOR=-1
EFIFS_VERSION="${EFIFS_VERSION:-v1.12}"
EFIFS_URL_OVERRIDDEN="${EFIFS_URL:-}"
EFIFS_URL="${EFIFS_URL:-https://github.com/pbatard/efifs/releases/download/$EFIFS_VERSION}"
CLI_DIR="${CLI_DIR:-/usr/local/bin}"
DATA_DIR="${DATA_DIR:-/usr/share/visor}"

VISOR_DIR_REL="EFI/visor"
CLI_NAME="visor"

setup_style() {
    if [ "$USE_COLOR" -eq -1 ]; then
        if [ -n "${NO_COLOR:-}" ] || [ "${TERM:-dumb}" = dumb ] || [ ! -t 1 ]; then
            USE_COLOR=0
        else
            USE_COLOR=1
        fi
    fi

    if [ "$USE_COLOR" -eq 1 ]; then
        C_ACCENT=$'\033[38;5;153m'; C_DIM=$'\033[38;5;245m'
        C_OK=$'\033[38;5;114m';     C_WARN=$'\033[38;5;179m'
        C_ERR=$'\033[38;5;203m';    C_ASK=$'\033[38;5;183m'
        C_BOLD=$'\033[1m';          C_OFF=$'\033[0m'
        C_LOGO=$'\033[38;5;231m'
    else
        C_ACCENT=""; C_DIM=""; C_OK=""; C_WARN=""
        C_ERR="";    C_ASK=""; C_BOLD=""; C_OFF=""
        C_LOGO=""
    fi

    case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
        *[Uu][Tt][Ff]8*|*[Uu][Tt][Ff]-8*) UNICODE=1 ;;
        *) UNICODE=0 ;;
    esac
    [ "$USE_COLOR" -eq 0 ] && [ ! -t 1 ] && UNICODE=0

    if [ "$UNICODE" -eq 1 ]; then
        S_STEP="▸"; S_OK="✓"; S_WARN="!"; S_ERR="✗"; S_ASK="?"; S_BUL="·"; S_RULE="─"
    else
        S_STEP=">"; S_OK="+"; S_WARN="!"; S_ERR="x"; S_ASK="?"; S_BUL="-"; S_RULE="-"
    fi
}

rule() {
    local n=54 out=""
    while [ "${#out}" -lt "$n" ]; do out="$out$S_RULE"; done
    printf '%s%s%s\n' "$C_DIM" "$out" "$C_OFF"
}

hdr()  { printf '\n%s%s%s %s%s\n' "$C_ACCENT" "$S_STEP" "$C_OFF" "$C_BOLD$*" "$C_OFF"; }
say()  { printf '  %s%s%s %s\n' "$C_DIM" "$S_BUL" "$C_OFF" "$*"; }
ok()   { printf '  %s%s%s %s\n' "$C_OK" "$S_OK" "$C_OFF" "$*"; }
warn() { printf '  %s%s%s %s\n' "$C_WARN" "$S_WARN" "$C_OFF" "$*" >&2; }
die()  { printf '\n%s%s%s %s\n\n' "$C_ERR" "$S_ERR" "$C_OFF" "$*" >&2; exit 1; }

kv() { printf '  %s%-16s%s %s\n' "$C_DIM" "$1" "$C_OFF" "$2"; }

on_err() {
    local rc=$? line=$1
    [ "$rc" -eq 0 ] && return 0
    printf '\n%s%s%s install.sh failed at line %s (exit %s)\n' \
        "$C_ERR" "$S_ERR" "$C_OFF" "$line" "$rc" >&2
    printf '  Nothing further was changed. Re-run with --help for options.\n\n' >&2
}
trap 'on_err $LINENO' ERR

banner() {
    printf '\n%s' "$C_LOGO"
    if [ "$UNICODE" -eq 1 ]; then
        cat <<'EOF'
  ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
  ⠀⠀⠀⠀⠀⠀⠀⠀⢀⣀⠀⠀⠀⠀⢀⣺⣿⢀⠀⠀⠀⠀⢀⡀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
  ⠀⠀⠀⠀⠀⠀⠀⠀⢘⣿⣦⠶⠒⠛⠛⢿⣿⠛⠛⠓⠶⣦⣿⡟⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
  ⠀⠀⠀⠀⠀⠀⣠⠾⠛⠻⠿⠂⠀⠀⠀⠀⠁⠀⠀⠀⠀⠿⠟⠛⠷⣄⡀⠀⠀⠀⠀⠀⠀⠀
  ⠀⠀⠀⠸⣿⣾⣇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢨⣿⣿⠇⠀⠀⠀⠀⠀
  ⠀⠀⠀⢰⠟⠙⠃⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣀⠀⠀⠀⡈⠉⠙⣦⠖⣆⠀⠀⠀
  ⠀⠀⢠⡏⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠘⡆⠈⠓⢦⡞⠁⠀⣀⣹⠀⡞⣠⡀⠀
  ⠀⠀⣿⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣿⡄⣰⠋⠈⠳⠄⡇⠙⣷⠋⠀⡇⠀
  ⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢀⣿⣿⡄⠀⠀⠀⠀⠳⢺⢻⡟⠶⠃⠀
  ⠀⢸⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢸⣿⣿⣷⡀⠀⠀⠀⠠⡇⣿⡹⡄⠀⠀
  ⠀⢸⣷⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣼⣿⣿⣿⣷⠀⠀⠀⡀⠉⢸⡇⢧⠀⠀
  ⠀⠀⣿⡄⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⢠⣿⣿⣿⣿⡿⠀⠀⣸⡇⠀⢸⠇⢸⡄⠀
  ⠀⠀⠘⣷⡀⠀⠀⢠⣤⣤⣤⣤⣤⣤⣤⣤⣤⣤⣿⣿⣿⣿⣿⡇⢀⣴⣿⠁⠀⣸⠀⢸⡇⠀
  ⠀⠀⠀⠘⢿⣄⣄⠀⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⠃⠀⠀⡿⠀⢸⡇⠀
  ⠀⠀⠀⠀⠈⠛⢿⠦⣜⢿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⣿⡿⠟⣹⠟⠁⠀⠀⢰⠇⠀⢸⠇⠀
  ⠀⠀⠀⠀⠀⠀⠀⠀⠈⠀⠈⠛⠿⣿⣿⣿⣿⣿⡿⠟⠋⠀⠀⠀⠀⠀⠀⠀⡟⠀⠀⠈⠀⠀
  ⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠉⠉⠉⠁⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀
EOF
    else
        cat <<'EOF'
   __      __ _____  _____  ____  _____
   \ \    / /|_   _|/ ____|/ __ \|  __ \
    \ \  / /   | | | (___ | |  | | |__) |
     \ \/ /    | |  \___ \| |  | |  _  /
      \  /    _| |_ ____) | |__| | | \ \
       \/    |_____|_____/ \____/|_|  \_\
EOF
    fi
    printf '%s' "$C_OFF"
    printf '  %sVISOR%s  %sA minimal UEFI boot manager%s\n\n' \
        "$C_BOLD$C_ACCENT" "$C_OFF" "$C_DIM" "$C_OFF"
}

usage() {
    cat <<'EOF'
install.sh - install Visor to the EFI System Partition (ESP)

Usage: ./install.sh [options]

  --esp PATH         ESP mount point (auto-detected if omitted)
  --arch NAME        target architecture: x86_64 or aarch64 (default: this host)
  --no-build         skip 'make'; install the existing binary
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
  --data-dir PATH    directory for host-side tools (default: /usr/share/visor)
  --force-config     overwrite an existing boot.conf with the default
  --color / --no-color   force or disable coloured output
  -h, --help         show this help

Honours NO_COLOR. Colour and box-drawing are disabled automatically when the
output is not a terminal.
EOF
    exit 0
}

ask() {
    local prompt="$1" reply
    if [ -t 0 ]; then
        printf '  %s%s%s %s [y/n] ' "$C_ASK" "$S_ASK" "$C_OFF" "$prompt" >&2
        read -r reply || true
    elif [ -r /dev/tty ]; then
        printf '  %s%s%s %s [y/n] ' "$C_ASK" "$S_ASK" "$C_OFF" "$prompt" >&2
        read -r reply < /dev/tty || true
    else
        echo 0; return
    fi
    case "$reply" in [yY]|[yY][eE][sS]) echo 1 ;; *) echo 0 ;; esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        --esp)          ESP="${2:-}"; shift 2 ;;
        --arch)         ARCH="${2:-}"; shift 2 ;;
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
        --data-dir)     DATA_DIR="${2:-}"; shift 2 ;;
        --force-config) FORCE_CONFIG=1; shift ;;
        --color)        USE_COLOR=1; shift ;;
        --no-color)     USE_COLOR=0; shift ;;
        -h|--help)      usage ;;
        *)              setup_style; die "unknown option: $1 (try --help)" ;;
    esac
done

setup_style

if [ -z "$ARCH" ]; then
    case "$(uname -m)" in
        aarch64|arm64) ARCH=aarch64 ;;
        x86_64|amd64)  ARCH=x86_64 ;;
        *)             ARCH=x86_64 ;;
    esac
fi
case "$ARCH" in
    x86_64)  EFI_NAME="visor_x64.efi" ;;
    aarch64) EFI_NAME="visor_aa64.efi" ;;
    *)       die "unsupported --arch '$ARCH' (use x86_64 or aarch64)" ;;
esac

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
    case "$ARCH" in
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
    ok "Filesystem driver: $DEST/drivers/$driver"
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

printf '\n  %sVisor%s %sinstaller%s\n' "$C_BOLD$C_ACCENT" "$C_OFF" "$C_DIM" "$C_OFF"
rule

hdr "Target"

if [ -z "$ESP" ]; then
    ESP="$(detect_esp || true)"
    [ -n "$ESP" ] || die "Could not find the ESP. Re-run with: --esp /your/esp/mount"
    kv "ESP" "$ESP $C_DIM(auto-detected)$C_OFF"
else
    kv "ESP" "$ESP"
fi
[ -d "$ESP" ] || die "ESP path does not exist: $ESP"

DEST="$ESP/$VISOR_DIR_REL"
kv "Architecture" "$ARCH"
kv "Binary" "$EFI_NAME"
kv "Install to" "$DEST"

if [ "$DO_BUILD" -eq 1 ]; then
    hdr "Build"
    say "Compiling $EFI_NAME for $ARCH ..."
    rm -f "$EFI_NAME"
    if ! make --no-print-directory ARCH="$ARCH"; then
        die "Build failed - not installing. Fix the errors above (see README 'Requirements')."
    fi
    [ -f "$EFI_NAME" ] || die "Build reported success but $EFI_NAME is missing - aborting."
    ok "Built $EFI_NAME ($(du -h "$EFI_NAME" | cut -f1))"
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

hdr "Install"

mkdir -p "$DEST/icons" "$DEST/backgrounds"

BACKUP=""
if [ -f "$DEST/$EFI_NAME" ]; then
    BACKUP="$DEST/$EFI_NAME.bak"
    cp -f "$DEST/$EFI_NAME" "$BACKUP"
    say "Previous binary kept as $(basename "$BACKUP")"
fi

install -m 0644 "$EFI_NAME" "$DEST/$EFI_NAME"
ok "Loader: $DEST/$EFI_NAME"

if [ -d assets/icons ]; then
    cp -f assets/icons/*.png "$DEST/icons/" 2>/dev/null || true
fi
if [ -d assets/backgrounds ]; then
    cp -f assets/backgrounds/*.png "$DEST/backgrounds/" 2>/dev/null || true
fi
if [ -f assets/logo.png ]; then
    install -m 0644 assets/logo.png "$DEST/logo.png" 2>/dev/null || true
fi
ok "Assets: icons, backgrounds, logo"

CLI_INSTALLED=""
if [ "$INSTALL_CLI" -eq 1 ] && [ -f "$CLI_NAME" ]; then
    if mkdir -p "$CLI_DIR" 2>/dev/null && install -m 0755 "$CLI_NAME" "$CLI_DIR/$CLI_NAME" 2>/dev/null; then
        CLI_INSTALLED="$CLI_DIR/$CLI_NAME"
        ok "Command: $CLI_INSTALLED"
    else
        warn "Could not install $CLI_NAME command to $CLI_DIR"
    fi

    if [ -d tools ]; then
        if mkdir -p "$DATA_DIR/tools" 2>/dev/null; then
            for t in tools/vbg_encode.py tools/visor_encrypt.py; do
                [ -f "$t" ] || continue
                install -m 0755 "$t" "$DATA_DIR/tools/$(basename "$t")" 2>/dev/null || true
            done
            ok "Host tools: $DATA_DIR/tools"
        else
            warn "Could not install host tools to $DATA_DIR/tools"
        fi
    fi
fi

CONF="$DEST/boot.conf"
CONF_IS_NEW=0
if [ -f "$CONF" ] && [ "$FORCE_CONFIG" -eq 0 ]; then
    ok "Config: kept existing $CONF"
else
    install -m 0644 boot.conf.example "$CONF"
    CONF_IS_NEW=1
    ok "Config: wrote default $CONF"
fi

if [ ! -e "$DEST/boot.log" ]; then
    install -m 0644 /dev/null "$DEST/boot.log"
    say "Created boot log: $DEST/boot.log"
fi

DRIVER_DESTS=()
if [ -n "$FS_DRIVER" ]; then
    hdr "Filesystem drivers"
    mkdir -p "$DEST/drivers"
    install -m 0644 "$FS_DRIVER" "$DEST/drivers/$(basename "$FS_DRIVER")"
    DRIVER_DESTS+=("$DEST/drivers/$(basename "$FS_DRIVER")")
    ok "Filesystem driver: $DEST/drivers/$(basename "$FS_DRIVER")"
fi

BOOT_FS="$(boot_fs_type)"
if [ "$DO_FS_DRIVERS" -ne 0 ] && [ -z "$FS_DRIVER" ]; then
    case "$BOOT_FS" in
        vfat|msdos|"")
            if [ "$DO_FS_DRIVERS" -eq 1 ]; then
                hdr "Filesystem drivers"
                say "Kernels live on a FAT filesystem the firmware already reads - no driver needed."
            fi
            ;;
        *)
            hdr "Filesystem drivers"
            if [ "$DO_FS_DRIVERS" -eq -1 ]; then
                say "Your kernels appear to live on a '$BOOT_FS' filesystem, which UEFI cannot read."
                DO_FS_DRIVERS="$(ask "Install the EfiFs $BOOT_FS driver so Visor can load them?")"
            fi
            [ "$DO_FS_DRIVERS" -eq 1 ] && install_efifs_driver "$BOOT_FS" || true
            ;;
    esac
fi

BOOT_ENTRY_STATE="skipped"
if [ "$DO_BOOT_ENTRY" -eq -1 ]; then
    hdr "Firmware boot entry"
    DO_BOOT_ENTRY="$(ask 'Add a UEFI boot entry for Visor with efibootmgr?')"
elif [ "$DO_BOOT_ENTRY" -eq 1 ]; then
    hdr "Firmware boot entry"
fi
if [ "$DO_BOOT_ENTRY" -eq 1 ]; then
    if ! command -v efibootmgr >/dev/null 2>&1; then
        warn "efibootmgr not installed; skipping boot entry."
    else
        src="$(findmnt -no SOURCE "$ESP")" || die "Cannot resolve ESP device."
        disk="/dev/$(lsblk -no PKNAME "$src")"
        partnum="$(lsblk -no PARTN "$src" 2>/dev/null || \
                   echo "$src" | grep -o '[0-9]*$')"
        if [ -z "$partnum" ]; then
            warn "Could not determine ESP partition number; skipping boot entry."
        elif [ ! -b "$disk" ]; then
            warn "Could not determine ESP disk (got '$disk'); skipping boot entry."
        elif efibootmgr | grep -q 'Visor'; then
            ok "Boot entry 'Visor' already exists; left untouched."
            BOOT_ENTRY_STATE="already present"
        else
            loader="\\${VISOR_DIR_REL//\//\\}\\$EFI_NAME"
            efibootmgr --create --disk "$disk" --part "$partnum" \
                       --label "Visor" --loader "$loader" >/dev/null
            ok "Boot entry 'Visor' -> $disk part $partnum"
            BOOT_ENTRY_STATE="created"
        fi
    fi
fi

SIGN_STATE="skipped"
if [ "$DO_SIGN" -eq -1 ]; then
    hdr "Secure Boot"
    DO_SIGN="$(ask 'Sign Visor for Secure Boot with sbctl?')"
elif [ "$DO_SIGN" -eq 1 ]; then
    hdr "Secure Boot"
fi
if [ "$DO_SIGN" -eq 1 ]; then
    if ! command -v sbctl >/dev/null 2>&1; then
        warn "sbctl not installed; skipping signing."
    else
        if sbctl sign -s "$DEST/$EFI_NAME" >/dev/null 2>&1; then
            ok "Signed $EFI_NAME"
            SIGN_STATE="signed"
        else
            warn "sbctl sign failed (are keys enrolled?)."
            SIGN_STATE="failed"
        fi
        for d in ${DRIVER_DESTS[@]+"${DRIVER_DESTS[@]}"}; do
            if sbctl sign -s "$d" >/dev/null 2>&1; then
                ok "Signed $(basename "$d")"
            else
                warn "sbctl sign failed for $(basename "$d")."
            fi
        done
    fi
fi

printf '\n'
rule
printf '  %s%sVisor is installed%s\n\n' "$C_BOLD" "$C_OK" "$C_OFF"
kv "Location" "$DEST"
kv "Loader" "$EFI_NAME ($ARCH)"
kv "Config" "$CONF"
if [ -n "$CLI_INSTALLED" ]; then kv "Command" "$CLI_INSTALLED"; fi
kv "Boot entry" "$BOOT_ENTRY_STATE"
kv "Secure Boot" "$SIGN_STATE"
if [ -n "$BACKUP" ]; then kv "Rollback" "$(basename "$BACKUP")"; fi

printf '\n  %sNext%s\n' "$C_BOLD" "$C_OFF"
if [ "$CONF_IS_NEW" -eq 1 ]; then
    say "Edit $CONF - set your kernel paths and root PARTUUID."
    say "Or delete it and let Visor auto-detect your entries."
else
    say "Your existing boot.conf was kept. Run 'visor status' to check it."
fi
say "Docs: https://visor-bootmanager.vercel.app"

banner
exit 0
