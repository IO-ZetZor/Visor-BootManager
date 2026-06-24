#!/usr/bin/env sh
set -eu

REPO="${VISOR_REPO:-https://github.com/IO-ZetZor/Visor-BootManager}"
SRC="${VISOR_SRC:-${XDG_CACHE_HOME:-$HOME/.cache}/visor-src}"

say()  { printf '\033[1;34m::\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

SUDO=""
[ "$(id -u)" = 0 ] || SUDO="sudo"

have() { command -v "$1" >/dev/null 2>&1; }

install_deps() {
    say "Installing build dependencies..."
    if   have pacman;  then $SUDO pacman -S --needed --noconfirm gnu-efi base-devel git
    elif have apt-get; then $SUDO apt-get update && $SUDO apt-get install -y gnu-efi build-essential binutils git
    elif have dnf;     then $SUDO dnf install -y gnu-efi gnu-efi-devel gcc binutils make git
    elif have zypper;  then $SUDO zypper install -y gnu-efi-devel gcc binutils make git
    elif have xbps-install; then $SUDO xbps-install -Sy gnu-efi gcc binutils make git
    elif have eopkg;   then $SUDO eopkg install -y gnu-efi gcc binutils make git
    else
        warn "Unknown package manager - install these yourself: gnu-efi, gcc, binutils, make, git"
    fi
}

need_deps() {
    for t in gcc make objcopy git; do have "$t" || return 0; done
    [ -d /usr/include/efi ] || [ -d /usr/local/include/efi ] || return 0
    return 1
}

say "Visor installer"
[ "$(uname -s)" = Linux ] || die "This installer is for Linux. (Visor itself runs in UEFI firmware.)"

if need_deps; then
    install_deps
else
    say "Build tools already present."
fi

if [ -d "$SRC/.git" ]; then
    say "Updating existing source in $SRC"
    git -C "$SRC" pull --ff-only || die "Could not update $SRC - delete it and retry."
else
    say "Downloading Visor into $SRC"
    mkdir -p "$(dirname "$SRC")"
    git clone --depth 1 "$REPO" "$SRC" || die "git clone failed."
fi

cd "$SRC"
say "Building..."
make clean >/dev/null 2>&1 || true
make || die "Build failed - see the errors above."

say "Installing (this needs administrator access)..."
if [ -n "$SUDO" ]; then
    $SUDO ./install.sh "$@"
else
    ./install.sh "$@"
fi

say "Done. You can re-run 'visor update' anytime to grab the latest version."
