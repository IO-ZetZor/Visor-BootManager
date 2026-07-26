#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE"

VER="${GNUEFI_VER:-4.0.4}"
SRC=gnu-efi-src
STAGE=gnu-efi/aarch64/gnuefi

if ! command -v aarch64-linux-gnu-gcc >/dev/null; then
  echo "ERROR: aarch64-linux-gnu-gcc not found. Install it first." >&2
  exit 1
fi

if [ ! -d "$SRC" ]; then
  git clone --depth 1 --branch "$VER" https://github.com/ncroxon/gnu-efi "$SRC" \
    || git clone --depth 1 https://github.com/ncroxon/gnu-efi "$SRC"
fi

make -C "$SRC" ARCH=aarch64 CROSS_COMPILE=aarch64-linux-gnu- lib gnuefi

mkdir -p "$STAGE"
cp "$SRC/aarch64/gnuefi/crt0-efi-aarch64.o" "$STAGE/"
cp "$SRC/aarch64/gnuefi/libgnuefi.a"        "$STAGE/"
cp "$SRC/aarch64/lib/libefi.a"              "$STAGE/"
cp "$SRC/gnuefi/elf_aarch64_efi.lds"        "$STAGE/"

echo "gnu-efi aarch64 staged in $STAGE"
echo "Now build with:  make ARCH=aarch64 GNU_EFI_INC=$SRC/inc"
