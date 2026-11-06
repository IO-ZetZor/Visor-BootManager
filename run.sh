#!/bin/sh
set -e

ARCH=${1:-x86_64}
case "$ARCH" in
  x86_64)  EFI=${2:-visor_x64.efi}; FB=BOOTX64.EFI ;;
  aarch64) EFI=${2:-visor_aa64.efi}; FB=BOOTAA64.EFI ;;
  *) echo "usage: $0 [x86_64|aarch64] [file.efi]"; exit 1 ;;
esac
[ -f "$EFI" ] || { echo "$EFI not found, pass it as 2nd arg"; exit 1; }

ESP=esp-$ARCH
mkdir -p "$ESP/EFI/BOOT"

if [ ! -d "$ESP/EFI/visor" ]; then
  echo "$ESP/EFI/visor does not exist, copying assets."
  mkdir -p "$ESP/EFI/visor"
  cp -r assets/backgrounds assets/icons "$ESP/EFI/visor/"
fi

cp "$EFI" "$ESP/EFI/visor/"
cp "$EFI" "$ESP/EFI/BOOT/$FB"

if [ "$ARCH" = x86_64 ]; then
  for d in /usr/share/OVMF /usr/share/edk2/x64 /usr/share/edk2/ovmf /usr/share/qemu; do
    for s in "" ".4m"; do
      [ -f "$d/OVMF_CODE$s.fd" ] && CODE="$d/OVMF_CODE$s.fd" && VARS="$d/OVMF_VARS$s.fd" && break 2
    done
    for s in "" "_4M.fd"; do
      [ -f "$d/OVMF_CODE$s.fd" ] && CODE="$d/OVMF_CODE$s.fd" && VARS="$d/OVMF_VARS$s.fd" && break 2
    done
  done
  [ -n "$CODE" ] || { echo "OVMF not found"; exit 1; }
  [ -f vars.fd ] || cp "$VARS" vars.fd

  exec qemu-system-x86_64 -machine q35 -m 512M -enable-kvm \
    -drive if=pflash,format=raw,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,file=vars.fd \
    -drive format=raw,file=fat:rw:"$ESP" \
    -device VGA -display gtk,full-screen=on,grab-on-hover=on \
    -device qemu-xhci -device usb-kbd -device usb-mouse -device usb-tablet \
    -serial mon:stdio -net none
else
  for f in /usr/share/edk2/aarch64/QEMU_EFI.fd /usr/share/AAVMF/AAVMF_CODE.fd \
           /usr/share/qemu-efi-aarch64/QEMU_EFI.fd; do
    [ -f "$f" ] && FW=$f && break
  done
  [ -n "$FW" ] || { echo "aarch64 firmware not found (edk2-aarch64)"; exit 1; }

  exec qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 1G \
    -bios "$FW" \
    -drive format=raw,file=fat:rw:"$ESP" \
    -device virtio-gpu-pci,xres=1920,yres=1080 -display gtk,grab-on-hover=on \
    -device qemu-xhci -device usb-kbd -device usb-mouse -device usb-tablet \
    -serial mon:stdio -net none
fi