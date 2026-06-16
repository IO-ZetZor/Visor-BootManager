# Minimal EFI Boot Manager Makefile
# Targets x86_64 UEFI systems

ARCH = x86_64
TARGET = visor_x64.efi
BUILD_DIR = build

EFI_CFLAGS = -ffreestanding -fno-stack-protector -fno-strict-aliasing \
             -fno-asynchronous-unwind-tables -fno-unwind-tables \
             -fpic -fshort-wchar -fvisibility=hidden -DGNU_EFI_USE_MS_ABI \
             -mno-red-zone -Wall -Wextra -O2 -I include -MMD -MP

ifeq ($(origin CC),default)
CC := $(shell command -v x86_64-linux-gnu-gcc 2>/dev/null || command -v gcc 2>/dev/null || echo cc)
endif
OBJCOPY ?= objcopy

GNU_EFI_INC ?= $(firstword $(wildcard /usr/include/efi /usr/local/include/efi))
CRT0        ?= $(firstword $(wildcard \
                   /usr/lib/crt0-efi-x86_64.o \
                   /usr/lib64/gnuefi/crt0-efi-x86_64.o \
                   /usr/lib/gnuefi/crt0-efi-x86_64.o))
GNU_EFI_LIB ?= $(dir $(CRT0))

VERS = efi.vers
LDS  = visor_x86_64.lds

EFI_LDFLAGS = -nostdlib -znocombreloc -z notext -T $(LDS) -shared \
              -Bsymbolic -Wl,--version-script=$(VERS) -L $(GNU_EFI_LIB) \
              $(CRT0) -lefi -lgnuefi

SRCS = main.c efi_helpers.c gui.c config.c linux_boot.c windows_boot.c \
       png_decoder.c font_jetbrains.c sha256.c hash_verify.c
OBJS = $(SRCS:.c=.o)

FONT    ?= /usr/share/fonts/TTF/JetBrainsMono-Regular.ttf
FONT_PX ?= 64

.PHONY: all clean install bakefont check-env

all: check-env $(TARGET)

check-env:
	@test -n "$(GNU_EFI_INC)" || { \
	  echo "ERROR: gnu-efi headers not found (looked in /usr/include/efi)."; \
	  echo "  Install gnu-efi:  Arch: pacman -S gnu-efi | Debian/Ubuntu: apt install gnu-efi"; \
	  echo "                    Fedora: dnf install gnu-efi | openSUSE: zypper in gnu-efi-devel"; \
	  echo "  Or override:  make GNU_EFI_INC=/path/to/efi CRT0=/path/to/crt0-efi-x86_64.o"; \
	  exit 1; }
	@test -n "$(CRT0)" || { \
	  echo "ERROR: crt0-efi-x86_64.o not found. Install gnu-efi or set CRT0=..."; exit 1; }

bakefont:
	python3 tools/bake_font.py "$(FONT)" $(FONT_PX) jetbrains font_jetbrains.c

$(TARGET): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(EFI_LDFLAGS) -o $(BUILD_DIR)/visor.so $(OBJS)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
		   -j .dynsym  -j .dynstr -j .rel* -j .reloc \
		   -O pei-x86-64 --subsystem=10 $(BUILD_DIR)/visor.so $(TARGET)

%.o: %.c
	$(CC) $(EFI_CFLAGS) \
	      -I $(GNU_EFI_INC) \
	      -I $(GNU_EFI_INC)/$(ARCH) \
	      -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET) $(BUILD_DIR)/*

install: $(TARGET)
	./install.sh

#lowkeyamazingguynatsukisubaru
