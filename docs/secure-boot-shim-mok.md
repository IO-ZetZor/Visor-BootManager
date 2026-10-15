# Secure Boot through shim + MOK (Fedora, Bazzite, other shim distros)

`install.sh --sign` signs Visor with **sbctl**, i.e. with your own key in the
firmware `db`. That works when every kernel you boot is signed with a key in
`db` too. Distro kernels on Fedora-family systems (Fedora, Bazzite, Silverblue,
RHEL and others) are signed with the *distro's* key, which only **shim**
trusts. Visor launched directly from firmware can therefore not boot them
under Secure Boot.

The route that does work is the one GRUB uses:

```
firmware ──(Microsoft UEFI CA)──▶ shimx64.efi ──(MOK)──▶ Visor (as grubx64.efi)
                                      │
                                      └─ SHIM_LOCK / loader protocol ──▶ distro kernel, EfiFs driver
```

Tested on: Bazzite 44 (Fedora 44 base), shim 16.1, AMD fTPM, Secure Boot on.

## Requirements in the Visor image

* An **`.sbat` section.** shim (≥ 15.3) refuses any image without one
  ("Verification failed", then MokManager). Built in since this change; see
  `sbat.csv.in`.
* **Drivers need `.sbat` too.** Upstream EfiFs releases ship without one. Add it
  before signing (below).
* With **shim ≥ 16** the drivers are loaded through shim's loader protocol,
  whose `StartImage` frees every image after its entry point returns. Visor
  now calls the entry point of shim-loaded drivers directly so they stay
  resident (`src/core/efi_fsdrv.c`). Older Visor builds hang on the second
  access to a filesystem served by an EfiFs driver.

## Steps

All paths assume the ESP is mounted at `/boot/efi`.

1. **Create a MOK key** (once):

   ```sh
   sudo install -d -m 700 /var/lib/visor/mok
   cd /var/lib/visor/mok
   sudo openssl req -new -x509 -newkey rsa:2048 -nodes -sha256 -days 36500 \
        -subj "/CN=Visor bootloader MOK/" -addext "extendedKeyUsage=codeSigning" \
        -keyout MOK.key -out MOK.crt
   sudo openssl x509 -in MOK.crt -outform DER -out MOK.der
   sudo chmod 600 MOK.key
   ```

2. **Add `.sbat` to EfiFs drivers** (Visor already has one):

   ```sh
   printf 'sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\nefifs,1,EfiFs,efifs,1,https://github.com/pbatard/efifs\n' > sbat-efifs.csv
   # first free, 4 KiB aligned address after the last section
   end=$(objdump -h ext2_x64.efi | awk '/^ *[0-9]+ /{v=strtonum("0x"$4)+strtonum("0x"$3); if(v>m)m=v} END{printf "0x%x", int((m+4095)/4096)*4096}')
   objcopy --add-section .sbat=sbat-efifs.csv \
           --set-section-flags .sbat=contents,alloc,load,readonly,data \
           --change-section-address .sbat="$end" ext2_x64.efi ext2_x64.sbat.efi
   ```

3. **Sign** Visor and every driver with the MOK key (`sbsigntools`):

   ```sh
   sbsign --key MOK.key --cert MOK.crt --output visor_x64.efi visor_x64.efi
   sbsign --key MOK.key --cert MOK.crt --output ext2_x64.efi ext2_x64.sbat.efi
   sbverify --cert MOK.crt visor_x64.efi
   ```

4. **Put shim in front of Visor.** shim loads `grubx64.efi` from its own
   directory, so:

   ```sh
   sudo cp /boot/efi/EFI/fedora/shimx64.efi /boot/efi/EFI/fedora/mmx64.efi /boot/efi/EFI/visor/
   sudo cp visor_x64.efi /boot/efi/EFI/visor/grubx64.efi
   sudo cp ext2_x64.efi  /boot/efi/EFI/visor/drivers/
   sudo efibootmgr --create --disk /dev/nvme0n1 --part 1 \
        --label "Visor (shim)" --loader '\EFI\visor\shimx64.efi'
   ```

   Visor finds its files at fixed paths under `\EFI\visor\`, so the
   `grubx64.efi` name does not matter to it. Keep the copy of shim in sync
   with the distro's shim when the distro updates it (e.g. a systemd `.path`
   unit on `/boot/efi/EFI/fedora/shimx64.efi`); an outdated shim can be
   revoked by SBAT/dbx updates.

5. **Enroll the key:** `sudo mokutil --import /var/lib/visor/mok/MOK.der`, pick a
   one-time password, reboot, and in the blue MokManager screen choose
   *Enroll MOK*, *Continue*, *Yes*, enter the password, *Reboot*.

6. **Enable Secure Boot** in the firmware setup.

Re-run steps 2 to 4 (add `.sbat`, sign and copy) after every Visor update.

## Windows and BitLocker

When Windows is chainloaded from Visor, shim and Visor are part of Windows'
measured boot. BitLocker then either loses its PCR 7 binding or binds to PCR 4,
and asks for the recovery key after every Visor or shim update. Set
`windows_bootnext=1` to let Visor hand Windows to the firmware's own
"Windows Boot Manager" entry (BootNext + reset) instead.

## TPM auto-unlock of LUKS (systemd-cryptenroll)

Booting through shim and Visor gives different PCR 7 values than shim and
GRUB (shim measures the certificate that verified each stage). Enroll the
TPM while booted through the path you normally use; the other path falls
back to the passphrase. Multiple TPM2 enrollments per LUKS device are possible.
