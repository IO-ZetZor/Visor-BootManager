#!/bin/sh
set -e
ARCH=${1:-x86_64}
[ $# -gt 0 ] && shift
IMG=visor-build

docker image inspect "$IMG" >/dev/null 2>&1 || docker build -t "$IMG" .

case "$ARCH" in
  x86_64)  EXTRA="" ;;
  aarch64) EXTRA="CROSS_COMPILE=aarch64-linux-gnu- \
GNU_EFI_INC=/opt/gnu-efi-aarch64/usr/include/efi \
CRT0=/opt/gnu-efi-aarch64/usr/lib/crt0-efi-aarch64.o" ;;
  *) echo "usage: $0 [x86_64|aarch64] [make args]"; exit 1 ;;
esac

# shellcheck disable=SC2086
exec docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/src "$IMG" \
  make ARCH="$ARCH" $EXTRA "$@"