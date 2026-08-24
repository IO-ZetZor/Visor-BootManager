# Maintainer: IO-ZetZor <swag100iq111@gmail.com>

pkgname=visor
pkgver=1.5
pkgrel=1
pkgdesc="A minimal, fast, graphical UEFI boot manager"
arch=('x86_64' 'aarch64')
url="https://github.com/IO-ZetZor/Visor-BootManager"
license=('BSD-2-Clause')
depends=('bash')
makedepends=('gnu-efi')
optdepends=('sbctl: Secure Boot signing with sbctl'
            'efibootmgr: UEFI boot entry management'
            'python: kernel/initrd encryption (visor encrypt)'
            'python-pillow: font baking (make bakefont)'
            'ffmpeg: animated backgrounds (visor convert)'
            'python-numpy: VBG backgrounds (visor convert --vbg)')
install=visor.install
source=("$pkgname-$pkgver.tar.gz::$url/archive/refs/tags/v$pkgver.tar.gz")
sha256sums=('a5ea624df0a35a04043dcff06ad231c7c48490bf894eeb4a0d991b959e82e1ff')

_dir="Visor-BootManager-$pkgver"

build() {
  cd "$srcdir/$_dir"
  case "$CARCH" in
    aarch64) make ARCH=aarch64 ;;
    *)       make ARCH=x86_64  ;;
  esac
}

package() {
  cd "$srcdir/$_dir"

  case "$CARCH" in
    aarch64) _efi=visor_aa64.efi ;;
    *)       _efi=visor_x64.efi  ;;
  esac
  install -Dm644 "$_efi" "$pkgdir/usr/lib/visor/$_efi"

  install -Dm755 visor "$pkgdir/usr/bin/visor"
  install -Dm644 boot.conf.example "$pkgdir/usr/share/visor/boot.conf.example"
  install -Dm644 docs/boot.conf.schema.json "$pkgdir/usr/share/visor/boot.conf.schema.json"

  install -Dm755 tools/vbg_encode.py "$pkgdir/usr/share/visor/tools/vbg_encode.py"

  if [ -d assets/icons ]; then
    cp -r assets/icons "$pkgdir/usr/share/visor/icons"
  fi
  if [ -d assets/backgrounds ]; then
    cp -r assets/backgrounds "$pkgdir/usr/share/visor/backgrounds"
  fi
  if [ -f assets/logo.png ]; then
    install -Dm644 assets/logo.png "$pkgdir/usr/share/visor/logo.png"
  fi

  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
