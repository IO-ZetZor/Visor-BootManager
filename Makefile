ARCH ?= x86_64
BUILD_DIR = build
SRC_DIR = src

ifeq ($(ARCH),x86_64)
  TARGET       ?= visor_x64.efi
  CC_CANDIDATES = x86_64-linux-gnu-gcc gcc
  ARCH_CFLAGS   = -mno-red-zone -DGNU_EFI_USE_MS_ABI
  LDS           = $(SRC_DIR)/visor_x86_64.lds
  ARCH_SRC      = arch_x86_64.c
  EFI_OBJCOPY   = -O pei-x86-64 --subsystem=10
  OBJCOPY_DEF   = objcopy
  CRT0_NAME     = crt0-efi-x86_64.o
  RELOC_FIXUP   = 1
else ifeq ($(ARCH),aarch64)
  TARGET       ?= visor_aa64.efi
  CC_CANDIDATES = aarch64-linux-gnu-gcc
  ARCH_CFLAGS   = -mstrict-align
  LDS           = $(GNU_EFI_LIB)elf_aarch64_efi.lds
  ARCH_SRC      = arch_aarch64.c
  GNU_EFI_INC_PREF = gnu-efi-src/inc
  EFI_OBJCOPY   = -O pei-aarch64-little --subsystem=10
  OBJCOPY_DEF   = aarch64-linux-gnu-objcopy
  CRT0_NAME     = crt0-efi-aarch64.o
  RELOC_FIXUP   =
else
  $(error unsupported ARCH=$(ARCH); use x86_64 or aarch64)
endif

ifeq ($(origin CC),default)
CC := $(shell for c in $(CC_CANDIDATES); do command -v $$c 2>/dev/null && break; done)
CC := $(if $(CC),$(CC),cc)
endif
OBJCOPY ?= $(OBJCOPY_DEF)

CF_PROTECTION := $(shell echo 'int main(void){return 0;}' | $(CC) -fcf-protection=none -x c -c - -o /dev/null 2>/dev/null && echo -fcf-protection=none)

EFI_CFLAGS = -ffreestanding -fno-stack-protector -fno-strict-aliasing \
             -fno-asynchronous-unwind-tables -fno-unwind-tables \
             $(CF_PROTECTION) -fno-PIE \
             -fpic -fshort-wchar -fvisibility=hidden $(ARCH_CFLAGS) \
             -Wall -Wextra -O2 -I $(SRC_DIR)/include -MMD -MP

GNU_EFI_INC ?= $(firstword $(wildcard \
                   $(GNU_EFI_INC_PREF) \
                   /usr/include/efi \
                   /usr/local/include/efi \
                   /usr/include/gnuefi/efi))
CRT0        ?= $(firstword $(wildcard \
                   /usr/lib/$(CRT0_NAME) \
                   /usr/lib64/gnuefi/$(CRT0_NAME) \
                   /usr/lib/gnuefi/$(CRT0_NAME) \
                   /usr/lib/$(ARCH)-linux-gnu/$(CRT0_NAME) \
                   /usr/lib/$(ARCH)-linux-gnu/gnuefi/$(CRT0_NAME) \
                   gnu-efi/$(ARCH)/gnuefi/$(CRT0_NAME)))
GNU_EFI_LIB ?= $(dir $(CRT0))

VERS = $(SRC_DIR)/efi.vers

EFI_LDFLAGS = -nostdlib -znocombreloc -z notext -T $(LDS) -shared \
              -Bsymbolic -Wl,--version-script=$(VERS) -L $(GNU_EFI_LIB) \
              $(CRT0) -lefi -lgnuefi

SRCS = main.c efi_helpers.c gui.c config.c linux_boot.c windows_boot.c \
       png_decoder.c gif_decoder.c font_jetbrains.c sha256.c hash_verify.c crypto.c text_menu.c \
       accent.c $(ARCH_SRC)
OBJDIR = $(BUILD_DIR)/$(ARCH)
OBJS = $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))

FONT    ?= /usr/share/fonts/TTF/JetBrainsMono-Regular.ttf
FONT_PX ?= 128

.PHONY: all clean install bakefont check-env check-reloc check-keys

all: check-env $(TARGET)

check-keys:
	@python3 tools/check_config_keys.py

check-reloc:
	@va=$$(objdump -p $(TARGET) 2>/dev/null | awk '/Virtual Address:/{print $$3; exit}'); \
	if [ -z "$$va" ]; then echo "WARN: could not read .reloc PageRVA (objdump?)"; exit 0; fi; \
	if [ $$((0x$$va)) -gt $$((0x32000 + 0x10000)) ]; then \
	  echo "ERROR: .reloc PageRVA 0x$$va is out of range - image would fail to boot."; \
	  echo "       Your binutils miswrote the relocation block; build aborted."; \
	  exit 1; \
	fi; \
	echo "reloc: PageRVA 0x$$va OK"

check-env:
	@test -n "$(GNU_EFI_INC)" || { \
	  echo "ERROR: gnu-efi headers not found (looked in /usr/include/efi)."; \
	  echo "  Install gnu-efi:  Arch: pacman -S gnu-efi | Debian/Ubuntu: apt install gnu-efi"; \
	  echo "                    Fedora: dnf install gnu-efi | openSUSE: zypper in gnu-efi-devel"; \
	  echo "  Or override:  make GNU_EFI_INC=/path/to/efi CRT0=/path/to/$(CRT0_NAME)"; \
	  exit 1; }
	@test -n "$(CRT0)" || { \
	  echo "ERROR: $(CRT0_NAME) not found. Install/cross-build gnu-efi or set CRT0=..."; exit 1; }

bakefont:
	python3 tools/bake_font.py "$(FONT)" $(FONT_PX) jetbrains $(SRC_DIR)/font_jetbrains.c

$(TARGET): $(OBJS)
	@mkdir -p $(OBJDIR)
	$(CC) $(EFI_LDFLAGS) -o $(OBJDIR)/visor.so $(OBJS)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
		   -j .dynsym  -j .dynstr -j .rel* -j .rela* -j .reloc \
		   $(EFI_OBJCOPY) $(OBJDIR)/visor.so $(TARGET)
ifeq ($(RELOC_FIXUP),1)
	@printf '\000\020\000\000\014\000\000\000\000\000\000\000' > $(OBJDIR)/reloc.bin
	$(OBJCOPY) --update-section .reloc=$(OBJDIR)/reloc.bin $(TARGET)
	@$(MAKE) --no-print-directory check-reloc ARCH=$(ARCH) TARGET=$(TARGET)
endif

$(OBJDIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(EFI_CFLAGS) \
	      -I $(GNU_EFI_INC) \
	      -I $(GNU_EFI_INC)/$(ARCH) \
	      -c $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET) visor_x64.efi visor_aa64.efi
	rm -rf $(BUILD_DIR)

install: $(TARGET)
	./install.sh
