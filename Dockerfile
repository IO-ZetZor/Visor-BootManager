FROM debian:bookworm
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential make git ca-certificates \
      gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
      libc6-dev-arm64-cross gnu-efi \
 && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 https://git.code.sf.net/p/gnu-efi/code /tmp/gnu-efi \
 && cd /tmp/gnu-efi \
 && make ARCH=aarch64 CROSS_COMPILE=aarch64-linux-gnu- \
 && make ARCH=aarch64 CROSS_COMPILE=aarch64-linux-gnu- \
      INSTALLROOT=/opt/gnu-efi-aarch64 PREFIX=/usr install \
 && rm -rf /tmp/gnu-efi
WORKDIR /src