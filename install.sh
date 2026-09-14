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
DO_STUDIO=-1
STUDIO_REPO="${VISOR_STUDIO_REPO:-https://github.com/Versedcamel153/visor-studio.git}"
ARCH=""
USE_COLOR=-1
PROFILE="${VISOR_PROFILE:-full}"
FEATURES="${VISOR_FEATURES:-}"
EXPLICIT_PROFILE=0
EXPLICIT_FEATURES=0
FORCE_PROFILE=0
PROFILE_FROM_BINARY=0
EFIFS_VERSION="${EFIFS_VERSION:-v1.12}"
EFIFS_URL_OVERRIDDEN="${EFIFS_URL:-}"
EFIFS_URL="${EFIFS_URL:-https://github.com/pbatard/efifs/releases/download/$EFIFS_VERSION}"
CLI_DIR="${CLI_DIR:-/usr/local/bin}"
DATA_DIR="${DATA_DIR:-/usr/share/visor}"
DRY_RUN=0
ASSUME_YES=0
FILTER_CONFIG=1
WRITE_SIDECAR=1

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
        S_TL="╭"; S_TR="╮"; S_BL="╰"; S_BR="╯"; S_V="│"
        S_ON="●"; S_OFF="○"; S_ARR="→"
    else
        S_STEP=">"; S_OK="+"; S_WARN="!"; S_ERR="x"; S_ASK="?"; S_BUL="-"; S_RULE="-"
        S_TL="+"; S_TR="+"; S_BL="+"; S_BR="+"; S_V="|"
        S_ON="*"; S_OFF="."; S_ARR="->"
    fi
}

term_width() {
    local w="${COLUMNS:-0}"
    if [ "$w" -le 0 ] && command -v tput >/dev/null 2>&1; then
        w="$(tput cols 2>/dev/null || echo 0)"
    fi
    [ "$w" -le 0 ] && w=80
    [ "$w" -gt 78 ] && w=78
    [ "$w" -lt 40 ] && w=40
    printf '%s\n' "$w"
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

OPTVAL=""
need_value() {
    [ -n "${2:-}" ] || { setup_style; die "$1 needs a value (try --help)"; }
    OPTVAL="$2"
}

vislen() {
    local s="$1"
    s="$(printf '%s' "$s" | sed $'s/\033\\[[0-9;]*m//g')"
    printf '%s\n' "${#s}"
}

panel_begin() {
    local title="$1" w pad
    w="$(term_width)"
    pad=$((w - 4 - ${#title}))
    [ "$pad" -lt 0 ] && pad=0
    printf '  %s%s%s %s%s%s ' "$C_DIM" "$S_TL" "$C_OFF" "$C_BOLD" "$title" "$C_OFF"
    printf '%s' "$C_DIM"
    local i=0
    while [ "$i" -lt "$pad" ]; do printf '%s' "$S_RULE"; i=$((i + 1)); done
    printf '%s%s\n' "$S_TR" "$C_OFF"
}

panel_line() {
    printf '  %s%s%s %s\n' "$C_DIM" "$S_V" "$C_OFF" "$*"
}

panel_wrap() {
    local indent="$1"; shift
    local text="$*" w avail word line=""
    w="$(term_width)"
    avail=$((w - 4 - ${#indent}))
    [ "$avail" -lt 20 ] && avail=20
    for word in $text; do
        if [ -z "$line" ]; then
            line="$word"
        elif [ "$(( ${#line} + 1 + ${#word} ))" -le "$avail" ]; then
            line="$line $word"
        else
            panel_line "$(printf '%s%s%s%s' "$C_DIM" "$indent" "$line" "$C_OFF")"
            line="$word"
        fi
    done
    if [ -n "$line" ]; then
        panel_line "$(printf '%s%s%s%s' "$C_DIM" "$indent" "$line" "$C_OFF")"
    fi
}

panel_end() {
    local w i=0
    w="$(term_width)"
    printf '  %s%s' "$C_DIM" "$S_BL"
    while [ "$i" -lt $((w - 2)) ]; do printf '%s' "$S_RULE"; i=$((i + 1)); done
    printf '%s%s\n' "$S_BR" "$C_OFF"
}

STEP_N=0
STEP_TOTAL=0
step() {
    STEP_N=$((STEP_N + 1))
    if [ "$STEP_TOTAL" -gt 0 ]; then
        printf '\n%s%s%s %s%s%s %s%s/%s%s\n' \
            "$C_ACCENT" "$S_STEP" "$C_OFF" "$C_BOLD" "$*" "$C_OFF" \
            "$C_DIM" "$STEP_N" "$STEP_TOTAL" "$C_OFF"
    else
        hdr "$@"
    fi
}

would() {
    printf '  %s%s%s %s%s%s\n' "$C_ASK" "$S_ARR" "$C_OFF" "$C_DIM" "$*" "$C_OFF"
}

run_or_skip() {
    local what="$1"; shift
    if [ "$DRY_RUN" -eq 1 ]; then
        would "$what"
        return 0
    fi
    "$@"
}

on_err() {
    local rc=$? line=$1
    [ "$rc" -eq 0 ] && return 0
    printf '\n%s%s%s install.sh failed at line %s (exit %s)\n' \
        "$C_ERR" "$S_ERR" "$C_OFF" "$line" "$rc" >&2
    printf '  Nothing further was changed. Re-run with --help for options.\n\n' >&2
}
trap 'on_err $LINENO' ERR

valid_visor_entries() {
    efibootmgr 2>/dev/null | awk '
        /^[Bb]oot[0-9A-Fa-f]{4}[^0-9A-Fa-f]/ && /[Vv]isor/ { print; found=1 }
        END { exit !found }
    '
}

corrupted_visor_entries() {
    efibootmgr 2>/dev/null | awk '
        /^[Bb]oot[0-9A-Fa-f]{5,}[^0-9A-Fa-f]/ && /[Vv]isor/ { print }
    '
}

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
  --profile NAME     feature profile: minimal, standard, hardened, ricer or full.
                     When neither --profile nor --features is given, install.sh
                     asks whether to reuse the profile from the last install
                     (recorded in DATA_DIR/profile) or prompts to choose one.
                     There is no silently assumed default profile.
  --features LIST    adjust the profile: "standard,+anim_mp4,-capture"
                     Bare names replace the profile; +name adds, -name removes.
  --list-features    show the feature set for the current profile and exit
  --list-profiles    list the available profiles and what they include, and exit
  --dry-run          show every action without writing anything
  -y, --yes          assume yes for the optional prompts (boot entry, signing,
                     filesystem drivers, Studio)
  --no-filter-config do not trim boot.conf.example/schema to the built features
  --no-sidecar       do not write \EFI\visor\build.json
  --force            skip the boot.conf pre-flight safety check
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
  --studio           pre-fetch the Visor Studio configurator (else prompted)
  --no-studio        do not install or prompt for Visor Studio
  --force-config     overwrite an existing boot.conf with the default
  --color / --no-color   force or disable coloured output
  -h, --help         show this help

Honours NO_COLOR. Colour and box-drawing are disabled automatically when the
output is not a terminal.
EOF
    exit 0
}

ask() {    local prompt="$1" reply
    if [ "${ASSUME_YES:-0}" -eq 1 ]; then
        printf '  %s%s%s %s [y/n] %sy (--yes)%s\n' \
            "$C_ASK" "$S_ASK" "$C_OFF" "$prompt" "$C_DIM" "$C_OFF" >&2
        echo 1; return
    fi
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

resolve_features() {
    local arch="${1:-$ARCH}" profile="${2:-$PROFILE}" features="${3:-$FEATURES}"
    make --no-print-directory ARCH="$arch" PROFILE="$profile" \
         "FEATURES=$features" list-features 2>/dev/null
}

resolve_features_checked() {
    local arch="$1" profile="$2" features="$3" out rc=0
    out="$(make --no-print-directory ARCH="$arch" PROFILE="$profile" \
                "FEATURES=$features" list-features 2>&1)" || rc=$?
    if [ "$rc" -ne 0 ]; then
        printf '%s\n' "$out" | sed 's/^/  /' >&2
        die "could not resolve profile '$profile'${features:+ with features '$features'} for $arch"
    fi
    printf '%s\n' "$out"
}

count_csv() {
    printf '%s\n' "$1" | tr ',' '\n' | sed '/^$/d' | wc -l
}

features_from() {
    local resolved="$1" which="$2"
    printf '%s\n' "$resolved" | awk -v want="$which" '
        /^on  \(/       { sect = "on";  next }
        /^off \(/       { sect = "off"; next }
        /^[^[:space:]]/ { sect = "";    next }   # any unindented line ends a list
        sect == want && NF == 1 { print $1 }
    '
}

feature_panel() {
    local resolved="$1" override="${2:-}" src="${3:-registry}"
    local on off all w cols cell line n=0 pulled arch_na
    if [ -n "$override" ] && [ "$src" = binary ]; then
        all="$(features_from "$resolved" on; features_from "$resolved" off)"
        on="$(printf '%s' "$override" | tr ',' '\n' | sed '/^$/d' | sort)"
        off="$(comm -23 <(printf '%s\n' "$all" | sed '/^$/d' | sort) <(printf '%s\n' "$on"))"
    else
        on="$(features_from "$resolved" on)"
        off="$(features_from "$resolved" off)"
    fi
    local non noff
    non="$(printf '%s\n' "$on" | sed '/^$/d' | wc -l)"
    noff="$(printf '%s\n' "$off" | sed '/^$/d' | wc -l)"

    if [ "$src" = binary ]; then
        panel_begin "Features (read from $EFI_NAME)"
    else
        panel_begin "Features"
    fi
    panel_line "$(printf '%s%s%s on   %s%s%s off   %sprofile%s %s%s%s   %sarch%s %s' \
        "$C_OK" "$non" "$C_OFF" "$C_DIM" "$noff" "$C_OFF" \
        "$C_DIM" "$C_OFF" "$C_BOLD" "$PROFILE" "$C_OFF" "$C_DIM" "$C_OFF" "$ARCH")"
    panel_line ""

    w="$(term_width)"
    cols=$(( (w - 4) / 18 ))
    [ "$cols" -lt 1 ] && cols=1

    line=""
    for f in $on; do
        cell="$(printf '%s%s%s %-14s' "$C_OK" "$S_ON" "$C_OFF" "$f")"
        line="$line$cell"
        n=$((n + 1))
        if [ "$((n % cols))" -eq 0 ]; then panel_line "$line"; line=""; fi
    done
    [ -n "$line" ] && panel_line "$line"

    if [ "$noff" -gt 0 ]; then
        panel_line ""
        line=""; n=0
        for f in $off; do
            cell="$(printf '%s%s %-14s%s' "$C_DIM" "$S_OFF" "$f" "$C_OFF")"
            line="$line$cell"
            n=$((n + 1))
            if [ "$((n % cols))" -eq 0 ]; then panel_line "$line"; line=""; fi
        done
        [ -n "$line" ] && panel_line "$line"
    fi

    pulled="$(printf '%s\n' "$resolved" | sed -n 's/^pulled in as dependencies: //p')"
    arch_na="$(printf '%s\n' "$resolved" | sed -n "s/^unavailable on $ARCH: //p")"
    if [ -n "$pulled" ] || [ -n "$arch_na" ]; then
        panel_line ""
        [ -n "$pulled" ] && panel_line "$(printf '%sadded as dependencies:%s %s' "$C_DIM" "$C_OFF" "$pulled")"
        [ -n "$arch_na" ] && panel_line "$(printf '%snot available on %s:%s %s' "$C_WARN" "$ARCH" "$C_OFF" "$arch_na")"
    fi
    panel_end
}

verify_manifest() {
    local efi="$1" resolved="$2" blob profile got profile_expected on_expected
    blob="$(grep -ao 'VISORFT1[^C]*VISORFTEND' "$efi" | head -1 || true)"
    [ -n "$blob" ] || { warn "Could not read embedded build manifest from $efi."; return; }
    got="$(printf '%s' "$blob" | python3 -c '
import json, sys
b = sys.stdin.read()
m = json.loads(b[b.index("{"): b.rindex("}") + 1])
print(m.get("profile", "?"), len(m.get("feat", [])))' 2>/dev/null)"
    [ -n "$got" ] || { warn "Embedded manifest in $efi is unreadable."; return; }
    profile_expected="$(printf '%s\n' "$resolved" | sed -n 's/^profile:  *\([a-z_]*\).*/\1/p')"
    on_expected="$(printf '%s' "$resolved" | sed -n 's/^on  *(\([0-9]*\)).*/\1/p')"
    if [ "$got" = "$profile_expected $on_expected" ]; then
        ok "Manifest verified: profile '${got% *}' with ${got##* } features."
    else
        warn "Embedded manifest ('$got') does not match build request ('$profile_expected $on_expected') — profile may not have been built in."
    fi
}

preflight_check() {
    local conf="$1" resolved="$2"
    local on_features off_features feature key
    on_features="$(features_from "$resolved" on)"
    off_features="$(features_from "$resolved" off)"

    local fatal=0

    if grep -qE '^[[:space:]]*sha256[[:space:]]*=' "$conf" 2>/dev/null; then
        if ! echo "$on_features" | grep -qw 'verify'; then
            die "boot.conf pins sha256 hashes but 'verify' is off in profile '$PROFILE'. Pick a profile with verify, or remove the sha256 entries."
        fi
    fi

    if grep -qE '^[[:space:]]*(kernel_|initrd_)?encrypted[[:space:]]*=' "$conf" 2>/dev/null; then
        if ! echo "$on_features" | grep -qw 'crypto'; then
            die "boot.conf has encrypted entries but 'crypto' is off in profile '$PROFILE'. Visor cannot boot encrypted kernels without the crypto feature."
        fi
    fi

    if grep -qE '^[[:space:]]*luks[a-z_]*[[:space:]]*=' "$conf" 2>/dev/null; then
        if ! echo "$on_features" | grep -qw 'luks'; then
            die "boot.conf has LUKS keys but 'luks' is off in profile '$PROFILE'. LUKS initrds cannot be embedded without this feature."
        fi
    fi

    local owners=""
    if command -v python3 >/dev/null 2>&1; then
        owners="$(python3 -c '
import json, sys
try:
    doc = json.load(open("features/features.json"))
except Exception:
    sys.exit(0)
for name, spec in (doc.get("features") or {}).items():
    keys = list(spec.get("keys") or []) + list(spec.get("entrykeys") or [])
    if keys:
        print("%s\t%s" % (name, "|".join(sorted(set(keys)))))
' 2>/dev/null || true)"
    fi
    [ -n "$owners" ] || return 0

    while IFS= read -r feature; do
        [ -z "$feature" ] && continue
        key="$(printf '%s\n' "$owners" | awk -F'\t' -v f="$feature" '$1==f {print $2; exit}')"
        [ -n "$key" ] || continue
        if grep -qE "^[[:space:]]*(${key})[[:space:]]*=" "$conf" 2>/dev/null; then
            warn "Config has a '$feature' key but that feature is off in profile '$PROFILE' — the key will be ignored."
        fi
    done <<< "$off_features"
}

profile_record_file() { printf '%s/profile\n' "$DATA_DIR"; }

PROFILE_LIST_CACHE=""
profile_list() {
    if [ -z "$PROFILE_LIST_CACHE" ]; then
        PROFILE_LIST_CACHE="$(make --no-print-directory list-features 2>/dev/null \
            | sed -n 's/^profiles: *//p' | head -1)"
        [ -n "$PROFILE_LIST_CACHE" ] || \
            PROFILE_LIST_CACHE="minimal standard hardened ricer full custom"
    fi
    printf '%s\n' "$PROFILE_LIST_CACHE"
}

is_profile() {
    local p
    for p in $(profile_list); do
        [ "$1" = "$p" ] && return 0
    done
    return 1
}

check_build_tools() {
    local missing="" c
    for c in make python3; do
        command -v "$c" >/dev/null 2>&1 || missing="$missing $c"
    done
    local cc_ok=0 cands
    case "$ARCH" in
        aarch64) cands="aarch64-linux-gnu-gcc" ;;
        *)       cands="x86_64-linux-gnu-gcc gcc" ;;
    esac
    for c in $cands; do
        command -v "$c" >/dev/null 2>&1 && { cc_ok=1; break; }
    done
    [ "$cc_ok" -eq 1 ] || missing="$missing ${cands%% *}"
    for c in objcopy; do
        command -v "$c" >/dev/null 2>&1 || command -v "aarch64-linux-gnu-$c" >/dev/null 2>&1 \
            || missing="$missing $c"
    done

    if [ -n "$missing" ]; then
        warn "Missing build tools:$missing"
        case "$ARCH" in
            aarch64) say "Arch: pacman -S aarch64-linux-gnu-gcc gnu-efi  |  Debian: apt install gcc-aarch64-linux-gnu" ;;
            *)       say "Arch: pacman -S base-devel gnu-efi  |  Debian: apt install build-essential gnu-efi" ;;
        esac
        die "cannot build without$missing (or pass --no-build to install an existing binary)"
    fi

    check_build_dir_writable
}

check_build_dir_writable() {
    local bd="${VISOR_BUILD_DIR:-build}"
    [ -e "$bd" ] || return 0
    [ "$(id -u)" -eq 0 ] && return 0

    local why=""
    if [ ! -w "$bd" ]; then
        why="it is not writable by $(id -un)"
    elif find "$bd" ! -user "$(id -u)" -print -quit 2>/dev/null | grep -q .; then
        why="it holds files owned by another user"
    fi
    [ -n "$why" ] || return 0

    warn "The build directory '$bd' cannot be used: $why."
    say "This is usually left over from an earlier 'sudo ./install.sh'."
    say "Clear it:      sudo rm -rf $bd"
    say "Or keep it:    sudo chown -R $(id -un) $bd"
    say "Or build elsewhere:  VISOR_BUILD_DIR=/tmp/visor-build ./install.sh ..."
    die "refusing to build into '$bd' - clear it first (see above)"
}

esp_free_kb() {
    df -Pk "$1" 2>/dev/null | awk 'NR==2 {print $4}'
}

check_esp() {
    local esp="$1" fstype free need
    fstype="$(findmnt -Uno FSTYPE "$esp" 2>/dev/null || true)"
    case "$fstype" in
        vfat|msdos|fat|fat32|"") ;;
        *)  warn "$esp is '$fstype', not FAT. UEFI firmware can normally only read FAT."
            if [ "$FORCE_PROFILE" -eq 0 ]; then
                die "refusing to install to a non-FAT ESP (use --force to override, or --esp PATH)"
            fi
            warn "--force: installing to a non-FAT filesystem anyway." ;;
    esac

    if ! mountpoint -q "$esp" 2>/dev/null; then
        warn "$esp is not a mount point - is the ESP mounted?"
    fi

    free="$(esp_free_kb "$esp")"
    if [ -n "$free" ]; then
        local binkb=1024
        if [ -f "$EFI_NAME" ]; then
            binkb="$(du -sk "$EFI_NAME" 2>/dev/null | cut -f1 || echo 1024)"
        fi
        need=$(( binkb + 4096 ))
        if [ "$free" -lt "$need" ]; then
            die "not enough free space on $esp: ${free}K available, about ${need}K needed"
        fi
        kv "Free space" "$(( free / 1024 )) MiB"
    fi
}

install_atomic() {
    local src="$1" dst="$2" mode="${3:-0644}" tmp
    tmp="$dst.new"
    if ! install -m "$mode" "$src" "$tmp" 2>/dev/null; then
        rm -f "$tmp" 2>/dev/null || true
        return 1
    fi
    sync "$tmp" 2>/dev/null || sync 2>/dev/null || true
    if ! mv -f "$tmp" "$dst" 2>/dev/null; then
        rm -f "$tmp" 2>/dev/null || true
        return 1
    fi
    return 0
}

ROLLBACK_FROM=""
ROLLBACK_TO=""
ROLLBACK_ARMED=0
rollback_loader() {
    [ "$ROLLBACK_ARMED" -eq 1 ] || return 0
    [ -n "$ROLLBACK_FROM" ] || return 0
    [ -f "$ROLLBACK_FROM" ] || return 0
    if cp -f "$ROLLBACK_FROM" "$ROLLBACK_TO" 2>/dev/null; then
        printf '  %s%s%s Restored the previous loader from %s\n' \
            "$C_WARN" "$S_WARN" "$C_OFF" "$(basename "$ROLLBACK_FROM")" >&2
    else
        printf '  %s%s%s Could not restore %s - the ESP may hold a half-installed loader.\n' \
            "$C_ERR" "$S_ERR" "$C_OFF" "$ROLLBACK_TO" >&2
        printf '      Recover with: cp %s %s\n' "$ROLLBACK_FROM" "$ROLLBACK_TO" >&2
    fi
}

rollback_on_exit() {
    local rc=$?
    [ "$rc" -eq 0 ] && return 0
    rollback_loader
}

write_esp_sidecar() {
    local dest="$1" feats="$2" out
    out="$dest/build.json"
    if [ "$DRY_RUN" -eq 1 ]; then
        would "write $out (feature manifest for Studio)"
        return 0
    fi
    local tmp="$out.new"
    if ! python3 - "$tmp" "$PROFILE" "$ARCH" "$feats" "$FEATURES" <<'PY' 2>/dev/null
import json, sys
path, profile, arch, feats, delta = sys.argv[1:6]
doc = {
    "v": 1,
    "profile": profile,
    "arch": arch,
    "features": [f for f in feats.split(",") if f],
    "features_delta": delta,
    "note": "Written by install.sh. The binary's embedded VISORFT1 manifest is "
            "authoritative; this is a convenience copy for tooling.",
}
with open(path, "w") as fh:
    json.dump(doc, fh, indent=2)
    fh.write("\n")
PY
    then
        rm -f "$tmp" 2>/dev/null || true
        warn "Could not write $out (tooling will fall back to reading the binary)."
        return 0
    fi
    if mv -f "$tmp" "$out" 2>/dev/null; then
        ok "Manifest: $out"
    else
        rm -f "$tmp" 2>/dev/null || true
        warn "Could not move the manifest into $out."
    fi
}

manifest_profile_of() {
    local efi="$1" blob
    blob="$(grep -ao 'VISORFT1[^C]*VISORFTEND' "$efi" 2>/dev/null | head -1 || true)"
    [ -n "$blob" ] || return 1
    printf '%s' "$blob" | python3 -c '
import json, sys
b = sys.stdin.read()
try:
    m = json.loads(b[b.index("{"): b.rindex("}") + 1])
except Exception:
    sys.exit(1)
print("%s\t%s" % (m.get("profile", ""), ",".join(m.get("feat", []))))
' 2>/dev/null
}

read_line() {
    local var="$1"
    if [ -t 0 ]; then
        read -r "$var" || true
    else
        read -r "$var" < /dev/tty || true
    fi
}

all_features_for_arch() {
    local arch="${1:-$ARCH}"
    make --no-print-directory ARCH="$arch" list-all-features 2>/dev/null \
        | sed -n 's/^features:  *//p'
}

pick_read_key() {
    local k="" intro="" fin="" junk=""
    IFS= read -r -s -N 1 k || return 1
    case "$k" in
        $'\e')
            IFS= read -r -s -N 1 -t 0.05 intro || { printf 'esc\n'; return 0; }
            case "$intro" in
                '['|'O') ;;
                *) printf 'esc\n'; return 0 ;;
            esac
            IFS= read -r -s -N 1 -t 0.05 fin || { printf 'esc\n'; return 0; }
            case "$fin" in
                A) printf 'up\n' ;;
                B) printf 'down\n' ;;
                C) printf 'ignore\n' ;;
                D) printf 'ignore\n' ;;
                H) printf 'home\n' ;;
                F) printf 'end\n' ;;
                5) IFS= read -r -s -N 1 -t 0.05 junk || true; printf 'pgup\n' ;;
                6) IFS= read -r -s -N 1 -t 0.05 junk || true; printf 'pgdn\n' ;;
                1|7) IFS= read -r -s -N 1 -t 0.05 junk || true; printf 'home\n' ;;
                4|8) IFS= read -r -s -N 1 -t 0.05 junk || true; printf 'end\n' ;;
                *) printf 'ignore\n' ;;
            esac ;;
        $'\r'|$'\n') printf 'enter\n' ;;
        ' ')         printf 'space\n' ;;
        [kK])        printf 'up\n' ;;
        [jJ])        printf 'down\n' ;;
        g)           printf 'top\n' ;;
        G)           printf 'bottom\n' ;;
        '/')         printf 'search\n' ;;
        [qQ]|[dD])   printf 'done\n' ;;
        [aA])        printf 'all\n' ;;
        [nN])        printf 'none\n' ;;
        [rR])        printf 'reset\n' ;;
        *)           printf 'ignore\n' ;;
    esac
}

_p_names=()
_p_count=0
_p_group=()
_p_size=()
_p_sum=()
_p_detail=()
_p_explicit=" "
_p_eff=" "
_p_seed=" "
_p_seed_name=""
_p_deps=()
_p_rows=()
_p_nrows=0
_p_cur=0
_p_top=0
_p_filter=""
_p_msg=""
_P_STTY_ORIG=""

declare -A _p_idx=()

_p_index_of() {
    local i="${_p_idx[$1]:-}"
    [ -n "$i" ] || return 1
    printf '%s\n' "$i"
}

_p_has() {
    case "$1" in *" $2 "*) return 0 ;; esac
    return 1
}

_p_add() {
    _p_has "$_p_explicit" "$1" || _p_explicit="$_p_explicit$1 "
}

_p_del() {
    _p_explicit="${_p_explicit// $1 / }"
}

_p_recompute() {
    local eff=" " f d changed=1 i
    for f in $_p_explicit; do eff="$eff$f "; done
    while [ "$changed" -eq 1 ]; do
        changed=0
        for f in $eff; do
            i="${_p_idx[$f]:-}"
            [ -n "$i" ] || continue
            for d in ${_p_deps[$i]:-}; do
                case "$eff" in
                    *" $d "*) ;;
                    *) eff="$eff$d "; changed=1 ;;
                esac
            done
        done
    done
    _p_eff="$eff"
}

_p_needs() {
    local start="$1" target="$2" seen=" " queue f i next
    i="${_p_idx[$start]:-}"
    [ -n "$i" ] || return 1
    queue="${_p_deps[$i]:-}"
    while [ -n "${queue// /}" ]; do
        next=""
        for f in $queue; do
            [ "$f" = "$target" ] && return 0
            case "$seen" in *" $f "*) continue ;; esac
            seen="$seen$f "
            i="${_p_idx[$f]:-}"
            [ -n "$i" ] && next="$next ${_p_deps[$i]:-}"
        done
        queue="$next"
    done
    return 1
}

_p_total_kb() {
    local f i sum=0
    for f in $_p_eff; do
        i="${_p_idx[$f]:-}"
        [ -n "$i" ] || continue
        sum=$((sum + ${_p_size[$i]:-0}))
    done
    printf '%s\n' "$sum"
}

_p_count_sel() {
    local t
    t="$(printf '%s' "$_p_eff" | tr -s ' ' | sed 's/^ //; s/ $//')"
    [ -z "$t" ] && { printf '0\n'; return; }
    printf '%s\n' "$t" | wc -w
}

picker_load() {
    local fts deps_raw line name deps n i row

    fts="$(all_features_for_arch "$ARCH")"
    if [ -z "$fts" ]; then
        die "could not read the feature list for $ARCH (make list-all-features failed)"
    fi

    _p_names=(); _p_idx=()
    for n in $fts; do
        _p_idx[$n]="${#_p_names[@]}"
        _p_names+=("$n")
    done
    _p_count="${#_p_names[@]}"

    _p_deps=()
    for ((i = 0; i < _p_count; i++)); do _p_deps[$i]=""; done
    deps_raw="$(make --no-print-directory ARCH="$ARCH" list-all-features-deps 2>/dev/null || true)"
    if [ -n "$deps_raw" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            name="${line%%:*}"
            deps="${line#*:}"
            i="${_p_idx[$name]:-}"
            [ -n "$i" ] || continue
            _p_deps[$i]="$deps"
        done <<< "$deps_raw"
    fi

    _p_group=(); _p_size=(); _p_sum=(); _p_detail=()
    local loaded=0
    if command -v python3 >/dev/null 2>&1; then
        while IFS=$'\t' read -r name row size summary detail; do
            i="${_p_idx[$name]:-}"
            [ -n "$i" ] || continue
            _p_group[$i]="$row"
            _p_size[$i]="$size"
            _p_sum[$i]="$summary"
            _p_detail[$i]="$detail"
            loaded=$((loaded + 1))
        done < <(
            printf '%s\n' "${_p_names[@]}" | python3 -c '
import json, sys
arch = sys.argv[1]
try:
    doc = json.load(open("features/features.json"))
    feats = doc["features"]
except Exception:
    sys.exit(1)
for raw in sys.stdin:
    n = raw.strip()
    if not n:
        continue
    f = feats.get(n) or {}
    size = (f.get("size_kb") or {}).get(arch, 0)
    def flat(v):
        return " ".join(str(v or "").split())
    print("\t".join([n, flat(f.get("group")) or "Features",
                     str(size), flat(f.get("summary")), flat(f.get("detail"))]))
' "$ARCH" 2>/dev/null || true
        )
    fi
    for ((i = 0; i < _p_count; i++)); do
        [ -n "${_p_group[$i]:-}" ] || _p_group[$i]="Features"
        [ -n "${_p_size[$i]:-}" ]  || _p_size[$i]=0
        [ -n "${_p_sum[$i]:-}" ]   || _p_sum[$i]=""
        [ -n "${_p_detail[$i]:-}" ] || _p_detail[$i]=""
    done
    [ "$loaded" -gt 0 ] || _p_msg="features.json unreadable - names only"
}

picker_seed() {
    local profile="$1" resolved on f
    _p_seed=" "; _p_seed_name=""
    if [ -n "$profile" ] && [ "$profile" != custom ]; then
        resolved="$(resolve_features "$ARCH" "$profile" "" || true)"
        on="$(features_from "$resolved" on)"
        for f in $on; do
            [ -n "${_p_idx[$f]:-}" ] || continue
            _p_seed="$_p_seed$f "
        done
        [ "$_p_seed" = " " ] || _p_seed_name="$profile"
    fi
    _p_explicit="$_p_seed"
    _p_recompute
}

_p_build_rows() {
    local i f g seen=" " groups=() want lc
    _p_rows=(); _p_nrows=0
    for ((i = 0; i < _p_count; i++)); do
        g="${_p_group[$i]}"
        case "$seen" in *" $g "*) continue ;; esac
        seen="$seen$g "
        groups+=("$g")
    done
    lc="$(printf '%s' "$_p_filter" | tr '[:upper:]' '[:lower:]')"
    for g in "${groups[@]}"; do
        local members=()
        for ((i = 0; i < _p_count; i++)); do
            [ "${_p_group[$i]}" = "$g" ] || continue
            if [ -n "$lc" ]; then
                want="$(printf '%s %s' "${_p_names[$i]}" "${_p_sum[$i]}" \
                        | tr '[:upper:]' '[:lower:]')"
                case "$want" in *"$lc"*) ;; *) continue ;; esac
            fi
            members+=("$i")
        done
        [ "${#members[@]}" -gt 0 ] || continue
        _p_rows+=("H$g")
        for i in "${members[@]}"; do _p_rows+=("F$i"); done
    done
    _p_nrows="${#_p_rows[@]}"
    if [ "$_p_nrows" -eq 0 ]; then
        _p_cur=0
    else
        [ "$_p_cur" -ge "$_p_nrows" ] && _p_cur=$((_p_nrows - 1))
        [ "$_p_cur" -lt 0 ] && _p_cur=0
        case "${_p_rows[$_p_cur]}" in
            H*) _p_move 1 || _p_move -1 || true ;;
        esac
    fi
}

_p_move() {
    local step="$1" i="$_p_cur"
    while :; do
        i=$((i + step))
        [ "$i" -lt 0 ] && return 1
        [ "$i" -ge "$_p_nrows" ] && return 1
        case "${_p_rows[$i]}" in
            F*) _p_cur="$i"; return 0 ;;
        esac
    done
}

_p_first_feature() {
    local i
    for ((i = 0; i < _p_nrows; i++)); do
        case "${_p_rows[$i]}" in F*) _p_cur="$i"; return 0 ;; esac
    done
    return 1
}

_p_last_feature() {
    local i
    for ((i = _p_nrows - 1; i >= 0; i--)); do
        case "${_p_rows[$i]}" in F*) _p_cur="$i"; return 0 ;; esac
    done
    return 1
}

_p_toggle() {
    local idx="$1" name="${_p_names[$1]}" f dropped=""
    if _p_has "$_p_explicit" "$name"; then
        _p_del "$name"
        for f in $_p_explicit; do
            if _p_needs "$f" "$name"; then
                _p_del "$f"
                dropped="$dropped $f"
            fi
        done
        if [ -n "$dropped" ]; then
            _p_msg="also unticked:$dropped (they need $name)"
        fi
    else
        _p_add "$name"
    fi
    _p_recompute
}

_p_viewport_rows() {
    local h="${LINES:-0}"
    if [ "$h" -le 0 ] && command -v tput >/dev/null 2>&1; then
        h="$(tput lines 2>/dev/null || printf '0')"
    fi
    [ "$h" -le 0 ] && h=24
    local avail=$((h - 8))
    [ "$avail" -lt 3 ] && avail=3
    printf '%s\n' "$avail"
}

_p_scroll_into_view() {
    local avail="$1"
    [ "$_p_cur" -lt "$_p_top" ] && _p_top="$_p_cur"
    if [ "$_p_cur" -ge $((_p_top + avail)) ]; then
        _p_top=$((_p_cur - avail + 1))
    fi
    if [ "$_p_top" -gt 0 ]; then
        case "${_p_rows[$((_p_top - 1))]}" in
            H*) [ "$((_p_cur - _p_top))" -lt $((avail - 1)) ] && _p_top=$((_p_top - 1)) ;;
        esac
    fi
    [ "$_p_top" -lt 0 ] && _p_top=0
    if [ "$_p_nrows" -le "$avail" ]; then
        _p_top=0
    elif [ "$_p_top" -gt $((_p_nrows - avail)) ]; then
        _p_top=$((_p_nrows - avail))
    fi
}

_p_draw() {
    local avail w out="" i row g name mark mcol sym desc pad nsel kb
    avail="$(_p_viewport_rows)"
    _p_scroll_into_view "$avail"
    w="$(term_width)"

    out+="$(printf '%s%s%s %sFeatures for a custom profile%s   %s%s%s' \
        "$C_ACCENT" "$S_STEP" "$C_OFF" "$C_BOLD" "$C_OFF" "$C_DIM" "$ARCH" "$C_OFF")"$'\n'
    if [ -n "$_p_filter" ]; then
        out+="$(printf '  %ssearch:%s %s%s%s   %s%s matching - / to change, Esc-in-search to clear%s' \
            "$C_DIM" "$C_OFF" "$C_BOLD" "$_p_filter" "$C_OFF" \
            "$C_DIM" "$_p_nrows" "$C_OFF")"$'\n'
    elif [ -n "$_p_seed_name" ]; then
        out+="$(printf '  %sseeded from %s%s%s%s - r restores it%s' \
            "$C_DIM" "$C_OFF" "$C_BOLD" "$_p_seed_name" "$C_DIM" "$C_OFF")"$'\n'
    elif [ "$(_p_count_sel)" -eq 0 ]; then
        out+="$(printf '  %sNothing selected yet - tick what this machine needs.%s' \
            "$C_DIM" "$C_OFF")"$'\n'
    else
        out+="$(printf '  %sTick what this machine needs; dependencies follow automatically.%s' \
            "$C_DIM" "$C_OFF")"$'\n'
    fi

    if [ "$_p_nrows" -eq 0 ]; then
        out+=$'\n'"$(printf '  %sNo feature matches "%s".%s' "$C_WARN" "$_p_filter" "$C_OFF")"$'\n'
    fi

    for ((i = _p_top; i < _p_top + avail && i < _p_nrows; i++)); do
        row="${_p_rows[$i]}"
        case "$row" in
            H*)
                g="${row#H}"
                out+="$(printf '  %s%s%s' "$C_DIM" "$g" "$C_OFF")"$'\n'
                ;;
            F*)
                name="${_p_names[${row#F}]}"
                if _p_has "$_p_explicit" "$name"; then
                    mark="[x]"; mcol="$C_OK"
                elif _p_has "$_p_eff" "$name"; then
                    mark="[+]"; mcol="$C_ASK"
                else
                    mark="[ ]"; mcol="$C_DIM"
                fi
                if [ "$i" -eq "$_p_cur" ]; then sym="$C_ASK$S_STEP$C_OFF"
                else sym=" "; fi
                desc="${_p_sum[${row#F}]}"
                pad=$((w - 26))
                [ "$pad" -lt 8 ] && pad=8
                [ "${#desc}" -gt "$pad" ] && desc="${desc:0:$((pad - 1))}…"
                out+="$(printf '  %s %s%s%s %s%-13s%s %s%s%s' \
                    "$sym" "$mcol" "$mark" "$C_OFF" \
                    "$C_BOLD" "$name" "$C_OFF" "$C_DIM" "$desc" "$C_OFF")"$'\n'
                ;;
        esac
    done

    if [ "$_p_nrows" -gt "$avail" ]; then
        out+="$(printf '  %s%s%s-%s of %s%s' "$C_DIM" "   " \
            "$((_p_top + 1))" "$((_p_top + avail))" "$_p_nrows" "$C_OFF")"$'\n'
    fi

    out+=$'\n'
    nsel="$(_p_count_sel)"
    kb="$(_p_total_kb)"
    out+="$(printf '  %s%s%s selected   %s≈%s KiB%s   %s[x] picked  [+] pulled in as a dependency%s' \
        "$C_OK" "$nsel" "$C_OFF" "$C_DIM" "$kb" "$C_OFF" "$C_DIM" "$C_OFF")"$'\n'

    local ctx="" cidx cdeps
    if [ "$_p_nrows" -gt 0 ]; then
        case "${_p_rows[$_p_cur]}" in
            F*)
                cidx="${_p_rows[$_p_cur]#F}"
                ctx="${_p_detail[$cidx]}"
                cdeps="$(printf '%s' "${_p_deps[$cidx]:-}" | tr -s ' ' | sed 's/^ //; s/ $//')"
                pad=$((w - 6))
                [ "$pad" -lt 20 ] && pad=20
                [ "${#ctx}" -gt "$pad" ] && ctx="${ctx:0:$((pad - 1))}…"
                out+="$(printf '  %s%s%s' "$C_DIM" "$ctx" "$C_OFF")"$'\n'
                if [ -n "$cdeps" ]; then
                    out+="$(printf '  %sneeds:%s %s' "$C_DIM" "$C_OFF" "$cdeps")"$'\n'
                else
                    out+=$'\n'
                fi
                ;;
        esac
    else
        out+=$'\n'$'\n'
    fi

    if [ -n "$_p_msg" ]; then
        out+="$(printf '  %s%s %s%s' "$C_WARN" "$S_WARN" "$_p_msg" "$C_OFF")"$'\n'
    else
        out+="$(printf '  %s↑↓ move · space tick · / search · a all · n none · r reset · q done · Esc cancel%s' \
            "$C_DIM" "$C_OFF")"$'\n'
    fi

    printf '\e[H\e[2J%s' "$out"
}

_picker_cleanup() {
    [ -n "$_P_STTY_ORIG" ] && stty "$_P_STTY_ORIG" 2>/dev/null || true
    printf '\e[?25h\e[?1049l'
}

_p_prompt_search() {
    local reply=""
    stty "$_P_STTY_ORIG" 2>/dev/null || true
    printf '\e[?25h'
    printf '\e[H\e[2J'
    printf '\n  %sFilter features%s (name or summary; blank clears)\n  %s>%s ' \
        "$C_BOLD" "$C_OFF" "$C_ASK" "$C_OFF"
    IFS= read -r reply || reply=""
    _p_filter="$(printf '%s' "$reply" | tr -d '\t')"
    stty -icanon -echo 2>/dev/null || true
    printf '\e[?25l'
    _p_cur=0; _p_top=0
}

picker_loop_raw() {
    local k done_flag=0 cancelled=0 avail

    _P_STTY_ORIG="$(stty -g 2>/dev/null || true)"
    trap '_picker_cleanup; exit 130' INT TERM
    trap '_picker_cleanup' EXIT
    stty -icanon -echo 2>/dev/null || true
    printf '\e[?1049h\e[?25l'

    _p_build_rows
    _p_first_feature || true
    _p_draw

    while [ "$done_flag" -eq 0 ]; do
        if ! k="$(pick_read_key)"; then
            break
        fi
        _p_msg=""
        case "$k" in
            up)     _p_move -1 || true ;;
            down)   _p_move 1 || true ;;
            top)    _p_first_feature || true ;;
            bottom) _p_last_feature || true ;;
            home)   _p_first_feature || true ;;
            end)    _p_last_feature || true ;;
            pgup)
                avail="$(_p_viewport_rows)"
                local n="$avail"
                while [ "$n" -gt 0 ] && _p_move -1; do n=$((n - 1)); done ;;
            pgdn)
                avail="$(_p_viewport_rows)"
                local n="$avail"
                while [ "$n" -gt 0 ] && _p_move 1; do n=$((n - 1)); done ;;
            enter|space)
                if [ "$_p_nrows" -gt 0 ]; then
                    case "${_p_rows[$_p_cur]}" in
                        F*) _p_toggle "${_p_rows[$_p_cur]#F}"
                            _p_build_rows ;;
                    esac
                fi ;;
            all)
                _p_explicit=" ${_p_names[*]} "
                _p_recompute ;;
            none)
                _p_explicit=" "
                _p_recompute ;;
            reset)
                _p_explicit="$_p_seed"
                _p_recompute
                _p_msg="${_p_seed_name:+restored $_p_seed_name}" ;;
            search)
                _p_prompt_search
                _p_build_rows
                _p_first_feature || true ;;
            esc)
                if [ -n "$_p_filter" ]; then
                    _p_filter=""
                    _p_build_rows
                    _p_first_feature || true
                else
                    cancelled=1
                    done_flag=1
                fi ;;
            done)
                if [ "$(_p_count_sel)" -eq 0 ]; then
                    _p_msg="at least one feature must stay ticked (Esc cancels)"
                else
                    done_flag=1
                fi ;;
        esac
        [ "$done_flag" -eq 1 ] || _p_draw
    done

    _picker_cleanup
    trap - INT TERM EXIT
    [ "$cancelled" -eq 0 ] || return 1
    return 0
}

picker_loop_numeric() {
    local cmd="" i n t name nsel row idx mark
    while :; do
        _p_build_rows
        hdr "Features for a custom profile"
        printf '  %s%s features available on %s%s\n' "$C_DIM" "$_p_count" "$ARCH" "$C_OFF"
        [ -n "$_p_seed_name" ] && \
            printf '  %sseeded from %s - "reset" restores it%s\n' "$C_DIM" "$_p_seed_name" "$C_OFF"
        i=0
        declare -a numbered=()
        for ((row = 0; row < _p_nrows; row++)); do
            case "${_p_rows[$row]}" in
                H*) printf '  %s%s%s\n' "$C_DIM" "${_p_rows[$row]#H}" "$C_OFF" ;;
                F*)
                    idx="${_p_rows[$row]#F}"
                    name="${_p_names[$idx]}"
                    i=$((i + 1))
                    numbered+=("$idx")
                    if _p_has "$_p_explicit" "$name"; then mark="$C_OK[x]$C_OFF"
                    elif _p_has "$_p_eff" "$name"; then mark="$C_ASK[+]$C_OFF"
                    else mark="$C_DIM[ ]$C_OFF"; fi
                    printf '  %s%2d%s %s %s %s%s%s\n' "$C_DIM" "$i" "$C_OFF" "$mark" \
                        "$name" "$C_DIM" "${_p_sum[$idx]}" "$C_OFF" ;;
            esac
        done
        printf '\n  %s%s selected, ≈%s KiB%s   %s[+] = pulled in as a dependency%s\n' \
            "$C_OK" "$(_p_count_sel)" "$(_p_total_kb)" "$C_OFF" "$C_DIM" "$C_OFF"
        printf '  %s[numbers]%s toggle e.g. "1 5 9" or "-5"; all/none/reset; blank = done\n  %s>%s ' \
            "$C_ASK" "$C_OFF" "$C_ASK" "$C_OFF"
        read_line cmd
        cmd="$(printf '%s' "${cmd:-done}" | tr '[:upper:]' '[:lower:]' | tr ',' ' ')"

        case "$cmd" in
            all)   _p_explicit=" ${_p_names[*]} "; _p_recompute; continue ;;
            none)  _p_explicit=" "; _p_recompute; continue ;;
            reset) _p_explicit="$_p_seed"; _p_recompute; continue ;;
            done) ;;
            *)
                [ -z "$cmd" ] && continue
                for t in $cmd; do
                    n="${t#-}"
                    case "$n" in
                        ''|*[!0-9]*)
                            printf '  %s%s toggle by number, or all/none/reset/done.%s\n' \
                                "$C_WARN" "$S_WARN" "$C_OFF"
                            continue 2 ;;
                    esac
                    if [ "$n" -lt 1 ] || [ "$n" -gt "${#numbered[@]}" ]; then
                        printf '  %s%s %s is out of range (1-%s).%s\n' \
                            "$C_WARN" "$S_WARN" "$n" "${#numbered[@]}" "$C_OFF"
                        continue
                    fi
                    idx="${numbered[$((n - 1))]}"
                    name="${_p_names[$idx]}"
                    if [ "${t#-}" != "$t" ]; then
                        _p_has "$_p_explicit" "$name" && _p_toggle "$idx"
                    else
                        _p_toggle "$idx"
                    fi
                done
                continue ;;
        esac

        nsel="$(_p_count_sel)"
        if [ "$nsel" -eq 0 ]; then
            printf '  %s%s At least one feature must stay selected.%s\n' \
                "$C_WARN" "$S_WARN" "$C_OFF"
            continue
        fi
        break
    done
    return 0
}

custom_feature_picker() {
    local seed="${1:-}"
    picker_load
    picker_seed "$seed"

    if [ -t 0 ] && [ -t 1 ]; then
        picker_loop_raw || return 1
    else
        picker_loop_numeric || return 1
    fi

    FEATURES="$(printf '%s' "$_p_eff" | tr -s ' ' | sed 's/^ //; s/ $//' | tr ' ' ',')"
    local nsel
    nsel="$(_p_count_sel)"
    if [ "$nsel" -eq 1 ]; then
        ok "Profile: custom (1 feature, ≈$(_p_total_kb) KiB)"
    else
        ok "Profile: custom ($nsel features, ≈$(_p_total_kb) KiB)"
    fi
    return 0
}

PROFILE_BLURB_CACHE=""
profile_blurb() {
    if [ -z "$PROFILE_BLURB_CACHE" ]; then
        PROFILE_BLURB_CACHE="$(python3 -c '
import json, sys
try:
    d = json.load(open("features/features.json"))
except Exception:
    sys.exit(0)
for name, spec in (d.get("profiles") or {}).items():
    print("%s\t%s" % (name, (spec.get("summary") or "").strip()))
' 2>/dev/null || true)"
        [ -n "$PROFILE_BLURB_CACHE" ] || PROFILE_BLURB_CACHE="-"
    fi
    [ "$PROFILE_BLURB_CACHE" = "-" ] && return 0
    printf '%s\n' "$PROFILE_BLURB_CACHE" | awk -F'\t' -v p="$1" '$1==p {print $2; exit}'
}

DEFAULT_PROFILE_CACHE=""
default_profile_name() {
    if [ -z "$DEFAULT_PROFILE_CACHE" ]; then
        DEFAULT_PROFILE_CACHE="$(python3 -c '
import json, sys
try:
    d = json.load(open("features/features.json"))
except Exception:
    sys.exit(0)
for name, spec in (d.get("profiles") or {}).items():
    if spec.get("default"):
        print(name)
        break
' 2>/dev/null || true)"
        [ -n "$DEFAULT_PROFILE_CACHE" ] || DEFAULT_PROFILE_CACHE="-"
    fi
    [ "$DEFAULT_PROFILE_CACHE" = "-" ] && return 0
    printf '%s\n' "$DEFAULT_PROFILE_CACHE"
}

profile_choose_interactive() {
    local seed="${1:-}"
    local choice="" reply="" p blurb letter letters="" i
    local -a names=() keys=()

    for p in $(profile_list); do
        names+=("$p")
        letter=""
        for ((i = 0; i < ${#p}; i++)); do
            if [[ "$letters" != *"${p:$i:1}"* ]]; then
                letter="${p:$i:1}"
                letters="$letters$letter"
                break
            fi
        done
        keys+=("$letter")
    done

    while :; do
        printf '\n  %sChoose a feature profile for this build:%s\n\n' "$C_BOLD" "$C_OFF"
        for ((i = 0; i < ${#names[@]}; i++)); do
            p="${names[$i]}"
            blurb="$(profile_blurb "$p")"
            [ -z "$blurb" ] && [ "$p" = custom ] && blurb="choose individual features yourself"
            if [ -n "${keys[$i]}" ]; then
                printf '  %s[%s]%s ' "$C_ASK" "${keys[$i]}" "$C_OFF"
            else
                printf '      '
            fi
            printf '%s%-10s%s %s%s%s\n' \
                "$C_BOLD" "$p" "$C_OFF" "$C_DIM" "${blurb:+— $blurb}" "$C_OFF"
        done
        printf '\n  %sChoice%s (a letter, or a full name): ' "$C_ASK" "$C_OFF"
        read_line choice
        choice="$(printf '%s' "${choice:-}" | tr '[:upper:]' '[:lower:]' | tr -d ' \t.,()[]')"

        PROFILE=""
        for ((i = 0; i < ${#names[@]}; i++)); do
            if [ "$choice" = "${names[$i]}" ] || \
               { [ -n "${keys[$i]}" ] && [ "$choice" = "${keys[$i]}" ]; }; then
                PROFILE="${names[$i]}"
                break
            fi
        done
        if [ -z "$PROFILE" ]; then
            printf '  %s%s Unknown choice "%s" - pick a letter or a profile name.%s\n' \
                "$C_WARN" "$S_WARN" "$choice" "$C_OFF"
            continue
        fi

        if [ "$PROFILE" = custom ]; then
            if custom_feature_picker "${seed:-$(default_profile_name)}"; then
                return 0
            fi
            say "Cancelled - pick a profile again."
            continue
        fi

        printf '\n  %sFeature tweaks?%s (blank = plain %s, or e.g. "+anim_mp4,-capture") %s>%s ' \
            "$C_ASK" "$C_OFF" "$PROFILE" "$C_ASK" "$C_OFF"
        read_line reply
        FEATURES="$reply"
        ok "Profile: $PROFILE"
        return 0
    done
}

profile_resolve_auto() {
    if [ "${EXPLICIT_PROFILE:-0}" -eq 1 ] || [ "${EXPLICIT_FEATURES:-0}" -eq 1 ]; then
        return 0
    fi

    local rec_profile="" rec_features="" rec_date=""
    local rec_file
    rec_file="$(profile_record_file)"
    if [ -r "$rec_file" ]; then
        rec_profile="$(grep -E '^profile=' "$rec_file" | head -1 | cut -d= -f2- | tr -d ' \r' || true)"
        rec_features="$(grep -E '^features=' "$rec_file" | head -1 | cut -d= -f2- | tr -d ' \r' || true)"
        rec_date="$(grep -E '^date=' "$rec_file" | head -1 | cut -d= -f2- | tr -d ' \r' || true)"
    fi

    local interactive=0
    if [ -t 0 ]; then
        interactive=1
    elif ( : < /dev/tty ) 2>/dev/null; then
        interactive=1
    fi

    if [ -n "$rec_profile" ] && [ "$interactive" -eq 1 ]; then
        hdr "Profile"
        local reply=""
        while :; do
            printf '  %s%s%s Last used profile %s' "$C_ASK" "$S_ASK" "$C_OFF" "$C_BOLD$rec_profile$C_OFF"
            [ -n "$rec_features" ] && printf ' (%s)' "$rec_features"
            [ -n "$rec_date" ] && printf ' - installed %s' "$rec_date"
            printf '\n  Reuse it? [y/n] (n = choose a different profile) '
            read_line reply
            case "$reply" in
                [yY]|[yY][eE][sS])
                    PROFILE="$rec_profile"
                    FEATURES="$rec_features"
                    ok "Reusing last profile: $PROFILE"
                    return 0 ;;
                [nN]|[nN][oO]|"")
                    profile_choose_interactive "$rec_profile"
                    return 0 ;;
                *)  printf '  %sAnswer y or n.%s\n' "$C_WARN" "$C_OFF" ;;
            esac
        done
    fi

    if [ -n "$rec_profile" ] && [ "$interactive" -eq 0 ]; then
        PROFILE="$rec_profile"
        FEATURES="$rec_features"
        say "Reusing last used profile: $PROFILE"
        return 0
    fi

    if [ "$interactive" -eq 1 ]; then
        hdr "Profile"
        say "No profile was recorded from a previous install."
        profile_choose_interactive
        return 0
    fi

    die "no --profile given and no previous install record to reuse - re-run with --profile NAME (or VISOR_PROFILE=NAME); there is no default profile"
}

profile_record_write() {
    local f tmp
    f="$(profile_record_file)"
    if ! mkdir -p "$DATA_DIR" 2>/dev/null; then
        warn "Could not create $DATA_DIR - not recording the chosen profile."
        return
    fi
    tmp="$(mktemp "${DATA_DIR}/profile.XXXXXX" 2>/dev/null)" || {
        warn "Could not write the chosen profile to $DATA_DIR."
        return
    }
    local rec_features="$FEATURES"
    if [ "$PROFILE_FROM_BINARY" -eq 1 ] && [ -n "$FEATURES" ]; then
        warn "Not recording FEATURES=$FEATURES: it adjusted a profile the installed binary does not use."
        rec_features=""
    fi
    {
        printf 'version=1\n'
        printf 'arch=%s\n' "$ARCH"
        printf 'profile=%s\n' "$PROFILE"
        printf 'features=%s\n' "$rec_features"
        printf 'date=%s\n' "$(date -Is 2>/dev/null || date 2>/dev/null || printf '?')"
    } > "$tmp"
    chmod 0644 "$tmp" 2>/dev/null || true
    if mv -f "$tmp" "$f" 2>/dev/null; then
        ok "Profile recorded: $f"
    else
        warn "Could not move the profile record into $f - the previous run's profile will not be offered next time."
        rm -f "$tmp"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --esp)          need_value "$1" "${2:-}"; ESP="$OPTVAL"; shift 2 ;;
        --arch)         need_value "$1" "${2:-}"; ARCH="$OPTVAL"; shift 2 ;;
        --profile)      need_value "$1" "${2:-}"; EXPLICIT_PROFILE=1; PROFILE="$OPTVAL"; shift 2 ;;
        --features)     need_value "$1" "${2:-}"; EXPLICIT_FEATURES=1; FEATURES="$OPTVAL"; shift 2 ;;
        --list-features) DO_LIST_FEATURES=1; shift ;;
        --list-profiles) DO_LIST_PROFILES=1; shift ;;
        --force)        FORCE_PROFILE=1; shift ;;
        --no-build)     DO_BUILD=0; shift ;;
        --dry-run)      DRY_RUN=1; shift ;;
        -y|--yes)       ASSUME_YES=1; shift ;;
        --no-filter-config) FILTER_CONFIG=0; shift ;;
        --no-sidecar)   WRITE_SIDECAR=0; shift ;;
        --boot-entry)   DO_BOOT_ENTRY=1; shift ;;
        --no-boot-entry) DO_BOOT_ENTRY=0; shift ;;
        --sign)         DO_SIGN=1; shift ;;
        --no-sign)      DO_SIGN=0; shift ;;
        --fs-drivers)   DO_FS_DRIVERS=1; shift ;;
        --no-fs-drivers) DO_FS_DRIVERS=0; shift ;;
        --fs-driver)    need_value "$1" "${2:-}"; FS_DRIVER="$OPTVAL"; shift 2 ;;
        --no-cli)       INSTALL_CLI=0; shift ;;
        --cli-dir)      need_value "$1" "${2:-}"; CLI_DIR="$OPTVAL"; shift 2 ;;
        --data-dir)     need_value "$1" "${2:-}"; DATA_DIR="$OPTVAL"; shift 2 ;;
        --studio)       DO_STUDIO=1; shift ;;
        --no-studio)    DO_STUDIO=0; shift ;;
        --force-config) FORCE_CONFIG=1; shift ;;
        --color)        USE_COLOR=1; shift ;;
        --no-color)     USE_COLOR=0; shift ;;
        -h|--help)      usage ;;
        *)              setup_style; die "unknown option: $1 (try --help)" ;;
    esac
done

setup_style

[ -n "${VISOR_PROFILE:-}" ] && EXPLICIT_PROFILE=1
[ -n "${VISOR_FEATURES:-}" ] && EXPLICIT_FEATURES=1

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

if [ "${DO_LIST_PROFILES:-0}" -eq 1 ]; then
    printf '\n  %sProfiles available for %s%s\n\n' "$C_BOLD" "$ARCH" "$C_OFF"
    for p in $(profile_list); do
        if [ "$p" = custom ]; then
            printf '  %s%-10s%s %s%s%s\n' "$C_BOLD" "$p" "$C_OFF" \
                "$C_DIM" "chosen feature by feature (--features)" "$C_OFF"
            continue
        fi
        n="$(resolve_features "$ARCH" "$p" "" | sed -n 's/^on  *(\([0-9]*\)).*/\1/p')"
        printf '  %s%-10s%s %s%2s features%s  %s%s%s\n' \
            "$C_BOLD" "$p" "$C_OFF" "$C_OK" "${n:-?}" "$C_OFF" \
            "$C_DIM" "$(profile_blurb "$p")" "$C_OFF"
    done
    printf '\n  %sSee the full set for one of them:%s ./install.sh --profile NAME --list-features\n\n' \
        "$C_DIM" "$C_OFF"
    exit 0
fi

profile_resolve_auto

case "$PROFILE" in
    "") die "--profile needs a name - one of: $(profile_list)" ;;
    *)  is_profile "$PROFILE" || \
            die "unknown profile '$PROFILE' — use one of: $(profile_list)" ;;
esac
if [ "$PROFILE" = custom ] && [ -z "$FEATURES" ]; then
    die "profile 'custom' has no fixed feature set — pass --features 'name,+name2,-name3'"
fi

if [ -n "$FEATURES" ]; then
    leftover=""
    matched=""
    FEATURES="$(echo "$FEATURES" | tr ' ' ',')"
    IFS=',' read -ra _ftoks <<< "$FEATURES"
    for tok in "${_ftoks[@]}"; do
        tok="$(echo "$tok" | tr -d '[:space:]')"
        [ -z "$tok" ] && continue
        bare="${tok//[+-]/}"
        match=0
        for p in $(profile_list); do
            if [ "$bare" = "$p" ]; then
                PROFILE="$p"
                match=1; break
            fi
        done
        if [ "$match" -eq 0 ]; then
            [ -n "$leftover" ] && leftover="${leftover},${tok}" || leftover="${tok}"
        else
            matched="${matched:+$matched,}$tok"
        fi
    done
    FEATURES="$leftover"
    [ -n "$matched" ] && [ -n "$leftover" ] && \
        say "Profile set to $PROFILE (features delta: $leftover)" || true
fi

if [ "$PROFILE" = custom ]; then
    has_bare=0
    IFS=',' read -ra _ftoks <<< "$FEATURES"
    for tok in "${_ftoks[@]}"; do
        case "$tok" in
            +*|-*|"") ;;
            *) has_bare=1 ;;
        esac
    done
    if [ "$has_bare" -eq 0 ]; then
        die "profile 'custom' needs at least one bare feature name, e.g. --features 'bls,pointer +anim_mp4,-capture'"
    fi
fi

if [ "$EXPLICIT_PROFILE" -eq 0 ] && [ -n "$FEATURES" ]; then
    _has_bare=0
    IFS=',' read -ra _ftoks <<< "$FEATURES"
    for tok in "${_ftoks[@]}"; do
        case "$tok" in
            +*|-*|"") ;;
            *) _has_bare=1 ;;
        esac
    done
    if [ "$_has_bare" -eq 1 ] && [ "$PROFILE" != custom ]; then
        say "Bare feature names replace the profile's set - recording this build as 'custom'."
        PROFILE=custom
    fi
fi

if [ "${DO_LIST_FEATURES:-0}" -eq 1 ]; then
    RESOLVED="$(resolve_features_checked "$ARCH" "$PROFILE" "$FEATURES")"
    printf '\n'
    feature_panel "$RESOLVED"
    printf '\n'
    exit 0
fi

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
        t="$(findmnt -Uno FSTYPE /boot 2>/dev/null || true)"
    fi
    [ -z "$t" ] && t="$(findmnt -Uno FSTYPE / 2>/dev/null || true)"
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
    if [ "$DRY_RUN" -eq 1 ]; then
        would "download EfiFs driver $driver and install it to $DEST/drivers/"
        DRIVER_DESTS+=("$DEST/drivers/$driver")
        return 0
    fi
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
           [ "$(findmnt -Uno FSTYPE "$m" 2>/dev/null)" = vfat ]; then
            echo "$m"; return
        fi
    done

    if command -v lsblk >/dev/null 2>&1; then
        lsblk -o MOUNTPOINT,PARTTYPENAME -rn 2>/dev/null | \
            awk -F' ' '/EFI System/ && $1!="" {print $1; exit}'
    fi
}

printf '\n  %sVisor%s %sinstaller%s' "$C_BOLD$C_ACCENT" "$C_OFF" "$C_DIM" "$C_OFF"
if [ "$DRY_RUN" -eq 1 ]; then
    printf '  %s[dry run - nothing will be written]%s' "$C_WARN" "$C_OFF"
fi
printf '\n'
rule

BOOT_FS="$(boot_fs_type)"
STEP_TOTAL=3
[ "$DO_BUILD" -eq 1 ] && STEP_TOTAL=$((STEP_TOTAL + 1))
[ "$DO_STUDIO" -ne 0 ] && STEP_TOTAL=$((STEP_TOTAL + 1))
[ "$DO_BOOT_ENTRY" -ne 0 ] && STEP_TOTAL=$((STEP_TOTAL + 1))
[ "$DO_SIGN" -ne 0 ] && STEP_TOTAL=$((STEP_TOTAL + 1))
if [ -n "$FS_DRIVER" ]; then
    STEP_TOTAL=$((STEP_TOTAL + 1))
elif [ "$DO_FS_DRIVERS" -ne 0 ]; then
    case "$BOOT_FS" in
        vfat|msdos|"") [ "$DO_FS_DRIVERS" -eq 1 ] && STEP_TOTAL=$((STEP_TOTAL + 1)) || true ;;
        *)             STEP_TOTAL=$((STEP_TOTAL + 1)) ;;
    esac
fi

step "Target"

if [ -z "$ESP" ]; then
    ESP="$(detect_esp || true)"
    [ -n "$ESP" ] || die "Could not find the ESP. Re-run with: --esp /your/esp/mount"
    kv "ESP" "$ESP $C_DIM(auto-detected)$C_OFF"
else
    kv "ESP" "$ESP"
fi
[ -d "$ESP" ] || die "ESP path does not exist: $ESP"

DEST="$ESP/$VISOR_DIR_REL"
CONF="$DEST/boot.conf"
kv "Architecture" "$ARCH"
kv "Binary" "$EFI_NAME"
kv "Install to" "$DEST"

if [ "$DO_BUILD" -eq 0 ] && [ -f "$EFI_NAME" ]; then
    _mp="$(manifest_profile_of "$EFI_NAME" || true)"
    if [ -n "$_mp" ]; then
        _bin_profile="${_mp%%	*}"
        _bin_feats="${_mp#*	}"
        if [ "$EXPLICIT_PROFILE" -eq 1 ] && [ "$_bin_profile" != "$PROFILE" ]; then
            warn "--no-build: $EFI_NAME was built as '$_bin_profile', not '$PROFILE'."
            warn "Installing it as-is; drop --no-build to build '$PROFILE'."
        fi
        PROFILE="$_bin_profile"
        PROFILE_FROM_BINARY=1
        BIN_FEATURES="$_bin_feats"
        kv "From binary" "profile $PROFILE ($(count_csv "$_bin_feats") features)"
    else
        warn "--no-build: $EFI_NAME has no embedded manifest (a pre-1.5.6 build)."
        warn "Assuming it has every feature; the config will not be filtered."
        BIN_FEATURES=""
        LEGACY_BINARY=1
    fi
fi

RESOLVED="$(resolve_features_checked "$ARCH" "$PROFILE" "$FEATURES")"

EFF_FEATURES="$(features_from "$RESOLVED" on | tr '\n' ',' | sed 's/,$//')"
EFF_SOURCE="registry"
if [ -n "${BIN_FEATURES:-}" ]; then
    if [ "$BIN_FEATURES" != "$EFF_FEATURES" ]; then
        warn "$EFI_NAME was built against a different feature registry than this checkout."
        warn "Using the binary's own list; the config will be filtered to match it."
    fi
    EFF_FEATURES="$BIN_FEATURES"
    EFF_SOURCE="binary"
fi

printf '\n'
feature_panel "$RESOLVED" "$EFF_FEATURES" "$EFF_SOURCE"

if [ "$DO_BUILD" -eq 1 ]; then
    check_build_tools
fi
check_esp "$ESP"

if [ "$FORCE_PROFILE" -eq 0 ] && [ -f "$CONF" ]; then
    preflight_check "$CONF" "$RESOLVED"
elif [ "$FORCE_PROFILE" -eq 1 ] && [ -f "$CONF" ]; then
    warn "--force: skipping boot.conf feature compatibility check."
fi

if [ "$DO_BUILD" -eq 1 ]; then
    step "Build"
    say "Compiling $EFI_NAME for $ARCH (profile $PROFILE) ..."
    if [ "$DRY_RUN" -eq 1 ]; then
        would "make ARCH=$ARCH PROFILE=$PROFILE FEATURES=$FEATURES"
    else
        rm -f "$EFI_NAME"
        if ! make --no-print-directory ARCH="$ARCH" PROFILE="$PROFILE" \
                  FEATURES="$FEATURES" ${VISOR_BUILD_DIR:+BUILD_DIR="$VISOR_BUILD_DIR"}; then
            die "Build failed - not installing. Fix the errors above (see README 'Requirements')."
        fi
        [ -f "$EFI_NAME" ] || die "Build reported success but $EFI_NAME is missing - aborting."
        ok "Built $EFI_NAME ($(du -h "$EFI_NAME" | cut -f1))"
        verify_manifest "$EFI_NAME" "$RESOLVED"
    fi
fi
if [ "$DRY_RUN" -eq 0 ]; then
    [ -f "$EFI_NAME" ] || die "$EFI_NAME not found - build first or drop --no-build."
fi

if command -v objdump >/dev/null 2>&1; then
    if ! objdump -h "$EFI_NAME" 2>/dev/null | grep -q '\.text'; then
        die "$EFI_NAME looks malformed (no .text section) - refusing to install."
    fi
fi

if [ ! -w "$ESP" ] && [ "$DRY_RUN" -eq 0 ]; then
    die "No write permission on $ESP. Re-run with sudo."
fi

step "Install"

run_or_skip "create $DEST/{icons,backgrounds}" mkdir -p "$DEST/icons" "$DEST/backgrounds"

BACKUP=""
if [ -f "$DEST/$EFI_NAME" ]; then
    BACKUP="$DEST/$EFI_NAME.bak"
    if [ "$DRY_RUN" -eq 1 ]; then
        would "back up the current loader to $(basename "$BACKUP")"
    else
        cp -f "$DEST/$EFI_NAME" "$BACKUP"
        say "Previous binary kept as $(basename "$BACKUP")"
        ROLLBACK_FROM="$BACKUP"
        ROLLBACK_TO="$DEST/$EFI_NAME"
        ROLLBACK_ARMED=1
        trap 'rollback_on_exit' EXIT
    fi
fi

if [ "$DRY_RUN" -eq 1 ]; then
    would "install $EFI_NAME to $DEST/$EFI_NAME"
else
    install_atomic "$EFI_NAME" "$DEST/$EFI_NAME" 0644 \
        || die "Could not write $DEST/$EFI_NAME (out of space on the ESP?)"
    ok "Loader: $DEST/$EFI_NAME"
fi

if [ "$DRY_RUN" -eq 1 ]; then
    would "copy icons, backgrounds and logo"
else
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
fi

CLI_INSTALLED=""
if [ "$INSTALL_CLI" -eq 1 ] && [ -f "$CLI_NAME" ] && [ "$DRY_RUN" -eq 1 ]; then
    would "install $CLI_NAME to $CLI_DIR/$CLI_NAME"
    would "install host tools to $DATA_DIR/tools"
    CLI_INSTALLED="$CLI_DIR/$CLI_NAME"
elif [ "$INSTALL_CLI" -eq 1 ] && [ -f "$CLI_NAME" ]; then
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

STUDIO_STATE="skipped"
if [ "$DO_STUDIO" -eq -1 ]; then
    step "Visor Studio"
    DO_STUDIO="$(ask 'Pre-fetch the Visor Studio configurator so it runs offline?')"
elif [ "$DO_STUDIO" -eq 1 ]; then
    step "Visor Studio"
fi
if [ "$DO_STUDIO" -eq 1 ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        would "clone Visor Studio from $STUDIO_REPO into the user cache"
        STUDIO_STATE="dry run"
    elif ! command -v git >/dev/null 2>&1; then
        warn "git not installed; skipping Visor Studio."
    else
        user="${SUDO_USER:-${PKEXEC_UID:-}}"
        home="${HOME:-/tmp}"
        case "$user" in
            *[!A-Za-z0-9_.-]*|"")
                user="$(id -un)" ;;
        esac
        if [ "$(id -u)" -eq 0 ] && [ -n "$user" ] && command -v getent >/dev/null 2>&1; then
            h="$(getent passwd "$user" 2>/dev/null | cut -d: -f6 || true)"
            [ -n "$h" ] && [ -d "$h" ] && home="$h"
        fi
        dir="${VISOR_STUDIO_DIR:-${XDG_CACHE_HOME:-$home/.cache}/visor-studio}"
        if [ ! -d "$dir/.git" ]; then
            say "Cloning Visor Studio into $dir"
            mkdir -p "$(dirname "$dir")"
            if [ "$(id -u)" -eq 0 ] && [ -n "$SUDO_USER" ]; then
                chown "$SUDO_USER" "$(dirname "$dir")" 2>/dev/null || true
            fi
            if git clone --depth 1 "$STUDIO_REPO" "$dir"; then
                if [ "$(id -u)" -eq 0 ] && [ -n "$SUDO_USER" ]; then
                    chown -R "$SUDO_USER" "$dir" 2>/dev/null || true
                fi
                STUDIO_STATE="$dir"
                ok "Visor Studio: run 'visor studio'"
            else
                STUDIO_STATE="failed"
                warn "Could not fetch Visor Studio from $STUDIO_REPO"
            fi
        else
            say "Visor Studio already fetched at $dir"
            STUDIO_STATE="$dir"
        fi
    fi
fi

CONF="$DEST/boot.conf"
CONF_IS_NEW=0
CONF_FILTERED=""

FILTER_SRC="boot.conf.example"
SCHEMA_SRC="docs/boot.conf.schema.json"
FILTERED_CONF=""
FILTERED_SCHEMA=""
if [ "$FILTER_CONFIG" -eq 1 ] && [ "${LEGACY_BINARY:-0}" -eq 0 ] && \
   command -v python3 >/dev/null 2>&1 && [ -f tools/filter_config.py ]; then
    _on="$EFF_FEATURES"
    _tmpdir="$(mktemp -d 2>/dev/null || true)"
    if [ -n "$_tmpdir" ] && [ -n "$_on" ]; then
        FILTERED_CONF="$_tmpdir/boot.conf.example"
        FILTERED_SCHEMA="$_tmpdir/boot.conf.schema.json"
        if _fout="$(python3 tools/filter_config.py --features "$_on" \
                        --profile "$PROFILE" --config "$FILTERED_CONF" \
                        --schema "$FILTERED_SCHEMA" 2>&1)"; then
            FILTER_SRC="$FILTERED_CONF"
            SCHEMA_SRC="$FILTERED_SCHEMA"
            CONF_FILTERED="$(printf '%s' "$_fout" | tr '\n' '@' | sed 's/@$//; s/@/, /g')"
            if [ -n "$CONF_FILTERED" ]; then
                say "Tailored to $PROFILE — $CONF_FILTERED"
            else
                say "Config and schema already match this build — installed as-is."
            fi
        else
            warn "Could not filter the example config; installing the full one."
            printf '%s\n' "$_fout" | sed 's/^/      /' >&2
            FILTERED_CONF=""; FILTERED_SCHEMA=""
        fi
    fi
fi

if [ -f "$CONF" ] && [ "$FORCE_CONFIG" -eq 0 ]; then
    ok "Config: kept existing $CONF"
elif [ "$DRY_RUN" -eq 1 ]; then
    would "write $CONF from $(basename "$FILTER_SRC")"
    CONF_IS_NEW=1
else
    install_atomic "$FILTER_SRC" "$CONF" 0644 \
        || die "Could not write $CONF"
    CONF_IS_NEW=1
    ok "Config: wrote default $CONF"
fi

if [ -f "$SCHEMA_SRC" ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        would "write $DEST/boot.conf.schema.json"
    elif install_atomic "$SCHEMA_SRC" "$DEST/boot.conf.schema.json" 0644; then
        ok "Schema: $DEST/boot.conf.schema.json"
    else
        warn "Could not write the schema to $DEST (Studio will use its own copy)."
    fi
fi

if [ "$ROLLBACK_ARMED" -eq 1 ]; then
    ROLLBACK_ARMED=0
    trap - EXIT
fi

if [ "$WRITE_SIDECAR" -eq 1 ] && [ "${LEGACY_BINARY:-0}" -eq 0 ]; then
    write_esp_sidecar "$DEST" "$EFF_FEATURES"
fi

if [ -n "${_tmpdir:-}" ]; then
    rm -rf "$_tmpdir" 2>/dev/null || true
fi

if [ ! -e "$DEST/boot.log" ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        would "create $DEST/boot.log"
    else
        install -m 0644 /dev/null "$DEST/boot.log"
        say "Created boot log: $DEST/boot.log"
    fi
fi

DRIVER_DESTS=()
if [ -n "$FS_DRIVER" ]; then
    step "Filesystem drivers"
    if [ "$DRY_RUN" -eq 1 ]; then
        would "install $FS_DRIVER to $DEST/drivers/$(basename "$FS_DRIVER")"
    else
        mkdir -p "$DEST/drivers"
        install -m 0644 "$FS_DRIVER" "$DEST/drivers/$(basename "$FS_DRIVER")"
        ok "Filesystem driver: $DEST/drivers/$(basename "$FS_DRIVER")"
    fi
    DRIVER_DESTS+=("$DEST/drivers/$(basename "$FS_DRIVER")")
fi

if [ "$DO_FS_DRIVERS" -ne 0 ] && [ -z "$FS_DRIVER" ]; then
    case "$BOOT_FS" in
        vfat|msdos|"")
            if [ "$DO_FS_DRIVERS" -eq 1 ]; then
                step "Filesystem drivers"
                say "Kernels live on a FAT filesystem the firmware already reads - no driver needed."
            fi
            ;;
        *)
            step "Filesystem drivers"
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
    step "Firmware boot entry"
    DO_BOOT_ENTRY="$(ask 'Add a UEFI boot entry for Visor with efibootmgr?')"
elif [ "$DO_BOOT_ENTRY" -eq 1 ]; then
    step "Firmware boot entry"
fi
if [ "$DO_BOOT_ENTRY" -eq 1 ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        would "add a UEFI boot entry 'Visor' pointing at $VISOR_DIR_REL/$EFI_NAME"
        BOOT_ENTRY_STATE="dry run"
    elif ! command -v efibootmgr >/dev/null 2>&1; then
        warn "efibootmgr not installed; skipping boot entry."
    else
        src="$(findmnt -Uno SOURCE "$ESP")" || die "Cannot resolve ESP device."
        disk="/dev/$(lsblk -no PKNAME "$src")"
        partnum="$(lsblk -no PARTN "$src" 2>/dev/null || \
                   echo "$src" | grep -o '[0-9]*$')"
        if [ -z "$partnum" ]; then
            warn "Could not determine ESP partition number; skipping boot entry."
        elif [ ! -b "$disk" ]; then
            warn "Could not determine ESP disk (got '$disk'); skipping boot entry."
        elif valid_visor_entries >/dev/null; then
            ok "Boot entry 'Visor' already exists; left untouched."
            BOOT_ENTRY_STATE="already present"
        else
            if corrupted_visor_entries >/dev/null; then
                warn "Malformed UEFI boot entry 'Visor' detected; creating a fresh valid one."
                warn "Old unusable entries may need manual cleanup (see '#32')."
            fi
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
    step "Secure Boot"
    DO_SIGN="$(ask 'Sign Visor for Secure Boot with sbctl?')"
elif [ "$DO_SIGN" -eq 1 ]; then
    step "Secure Boot"
fi
if [ "$DO_SIGN" -eq 1 ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
        would "sbctl sign $DEST/$EFI_NAME"
        for d in ${DRIVER_DESTS[@]+"${DRIVER_DESTS[@]}"}; do
            would "sbctl sign $d"
        done
        SIGN_STATE="dry run"
    elif ! command -v sbctl >/dev/null 2>&1; then
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
step "Summary"

if [ "$DRY_RUN" -eq 1 ]; then
    would "record profile '$PROFILE' in $(profile_record_file)"
else
    profile_record_write
fi

printf '\n'
if [ "$DRY_RUN" -eq 1 ]; then
    panel_begin "Dry run complete - nothing was written"
else
    panel_begin "Visor is installed"
fi
panel_line ""
panel_line "$(printf '%s%-14s%s %s' "$C_DIM" "Location" "$C_OFF" "$DEST")"
panel_line "$(printf '%s%-14s%s %s %s(%s)%s' "$C_DIM" "Loader" "$C_OFF" "$EFI_NAME" "$C_DIM" "$ARCH" "$C_OFF")"
EFF_N="$(count_csv "$EFF_FEATURES")"
ALL_N=$(( $(features_from "$RESOLVED" on | wc -l) + $(features_from "$RESOLVED" off | wc -l) ))
panel_line "$(printf '%s%-14s%s %s%s%s %s(%s of %s features)%s' \
    "$C_DIM" "Profile" "$C_OFF" "$C_BOLD" "$PROFILE" "$C_OFF" \
    "$C_DIM" "$EFF_N" "$ALL_N" "$C_OFF")"
[ -n "$FEATURES" ] && panel_line "$(printf '%s%-14s%s %s' "$C_DIM" "Adjustments" "$C_OFF" "$FEATURES")"
panel_line "$(printf '%s%-14s%s %s' "$C_DIM" "Config" "$C_OFF" "$CONF")"
if [ -n "$CONF_FILTERED" ]; then
    panel_wrap "               " "$CONF_FILTERED"
fi
[ -n "$CLI_INSTALLED" ] && \
    panel_line "$(printf '%s%-14s%s %s' "$C_DIM" "Command" "$C_OFF" "$CLI_INSTALLED")"
panel_line ""

_state_line() {
    local label="$1" state="$2" col="$C_DIM" sym="$S_BUL"
    case "$state" in
        signed|created|"already present") col="$C_OK"; sym="$S_OK" ;;
        failed) col="$C_ERR"; sym="$S_ERR" ;;
        skipped) col="$C_DIM"; sym="$S_OFF" ;;
        "dry run") col="$C_ASK"; sym="$S_ARR" ;;
        *) col="$C_OK"; sym="$S_OK" ;;
    esac
    panel_line "$(printf '%s%-14s%s %s%s%s %s' "$C_DIM" "$label" "$C_OFF" "$col" "$sym" "$C_OFF" "$state")"
}
_state_line "Boot entry" "$BOOT_ENTRY_STATE"
_state_line "Secure Boot" "$SIGN_STATE"
_state_line "Visor Studio" "$STUDIO_STATE"
[ -n "$BACKUP" ] && \
    panel_line "$(printf '%s%-14s%s %s' "$C_DIM" "Rollback" "$C_OFF" "$(basename "$BACKUP")")"
panel_end

printf '\n  %sNext%s\n' "$C_BOLD" "$C_OFF"
if [ "$DRY_RUN" -eq 1 ]; then
    say "Re-run without --dry-run to apply the above."
elif [ "$CONF_IS_NEW" -eq 1 ]; then
    say "Edit $CONF - set your kernel paths and root PARTUUID."
    say "Or delete it and let Visor auto-detect your entries."
else
    say "Your existing boot.conf was kept. Run 'visor status' to check it."
fi
say "Check what this build can do: visor features"
say "Docs: https://visor-bootmanager.vercel.app"

banner
exit 0
