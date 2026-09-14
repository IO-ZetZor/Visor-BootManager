ARCH ?= x86_64
BUILD_DIR = build
SRC_DIR = src

VISOR_VERSION ?= $(shell sed -n 's/^pkgver=//p' PKGBUILD 2>/dev/null | head -1)
VISOR_VERSION := $(if $(VISOR_VERSION),$(VISOR_VERSION),1.4)

ifeq ($(ARCH),x86_64)
  TARGET       ?= visor_x64.efi
  CC_CANDIDATES = x86_64-linux-gnu-gcc gcc
  ARCH_CFLAGS   = -mno-red-zone -DGNU_EFI_USE_MS_ABI
  LDS           = $(SRC_DIR)/visor_x86_64.lds
  ARCH_SRC      = arch/arch_x86_64.c
  EFI_OBJCOPY   = -O pei-x86-64 --subsystem=10
  OBJCOPY_DEF   = objcopy
  CRT0_NAME     = crt0-efi-x86_64.o
  RELOC_FIXUP   = 1
else ifeq ($(ARCH),aarch64)
  TARGET       ?= visor_aa64.efi
  CC_CANDIDATES = aarch64-linux-gnu-gcc
  ARCH_CFLAGS   = -mstrict-align
  LDS           = $(GNU_EFI_LIB)elf_aarch64_efi.lds
  ARCH_SRC      = arch/arch_aarch64.c
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
             -DVISOR_VERSION='L"$(VISOR_VERSION)"' \
             -Wall -Wextra -O2 -I $(SRC_DIR)/include -MMD -MP \
             -I $(GEN_DIR) -include visor_features.h

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

include features/features.mk

PROFILE  ?= full
FEATURES ?=

ifeq ($(filter $(PROFILE),$(PROFILE_ALL)),)
  $(error unknown PROFILE=$(PROFILE) - use one of: $(PROFILE_ALL))
endif

visor_comma := ,
visor_toks  := $(subst $(visor_comma), ,$(FEATURES))
visor_add   := $(patsubst +%,%,$(filter +%,$(visor_toks)))
visor_del   := $(patsubst -%,%,$(filter -%,$(visor_toks)))
visor_bare  := $(filter-out +% -%,$(visor_toks))

visor_asprof := $(filter $(visor_bare),$(PROFILE_ALL))
ifneq ($(visor_asprof),)
  $(error FEATURES names the profile '$(firstword $(visor_asprof))' - did you mean PROFILE=$(firstword $(visor_asprof))?)
endif

visor_unknown := $(filter-out $(FEAT_ALL),$(visor_bare) $(visor_add) $(visor_del))
ifneq ($(visor_unknown),)
  $(error unknown feature(s): $(visor_unknown) - run `make list-features`)
endif

ifeq ($(strip $(visor_bare)),)
  visor_base := $(PROFILE_$(PROFILE))
else
  visor_base := $(visor_bare)
endif

ifeq ($(strip $(visor_base) $(visor_add)),)
  $(error PROFILE=$(PROFILE) has no fixed feature set - pass FEATURES=name,name,...)
endif

visor_want := $(filter-out $(visor_del),$(sort $(visor_base) $(visor_add)))

visor_profdrop := $(strip $(foreach f,$(visor_want),\
                    $(if $(FEAT_$(f)_PROFILES),$(if $(filter $(PROFILE),$(FEAT_$(f)_PROFILES)),,$(f)))))
visor_want := $(filter-out $(visor_profdrop),$(visor_want))

ifeq ($(strip $(visor_want)),)
  $(error every feature asked for is unavailable in profile $(PROFILE): $(visor_profdrop))
endif

visor_closure = $(sort $1 $(foreach _f,$1,$(if $(FEAT_$(_f)_DEPS),$(call visor_closure,$(FEAT_$(_f)_DEPS)))))

visor_conflict := $(filter $(visor_del),$(call visor_closure,$(visor_add)))
ifneq ($(visor_conflict),)
  $(error FEATURES adds $(visor_add) but removes $(visor_conflict), which it depends on)
endif

visor_closed := $(call visor_closure,$(visor_want))
visor_pulled := $(strip $(filter-out $(visor_want),$(visor_closed)))

visor_archbad := $(strip $(foreach f,$(visor_closed),\
                   $(if $(FEAT_$(f)_ARCH),$(if $(filter $(ARCH),$(FEAT_$(f)_ARCH)),,$(f)))))

visor_profbad := $(strip $(foreach f,$(visor_closed),\
                   $(if $(FEAT_$(f)_PROFILES),$(if $(filter $(PROFILE),$(FEAT_$(f)_PROFILES)),,$(f)))))

visor_profnote := $(strip $(sort $(visor_profdrop) $(visor_profbad)))

visor_unavail := $(strip $(visor_archbad) $(visor_profbad))

VISOR_FEATURES     := $(strip $(foreach f,$(visor_closed),\
                        $(if $(filter $(visor_unavail),$(call visor_closure,$(f))),,$(f))))
VISOR_FEATURES_OFF := $(strip $(filter-out $(VISOR_FEATURES),$(FEAT_ALL)))
visor_collateral   := $(strip $(filter-out $(VISOR_FEATURES) $(visor_unavail),$(visor_closed)))

visor_archable := $(strip $(foreach f,$(FEAT_ALL),\
                   $(if $(FEAT_$(f)_ARCH),$(if $(filter $(ARCH),$(FEAT_$(f)_ARCH)),$(f)),$(f))))

visor_pickable := $(strip $(foreach f,$(visor_archable),\
                   $(if $(FEAT_$(f)_PROFILES),$(if $(filter custom,$(FEAT_$(f)_PROFILES)),$(f)),$(f))))

visor_src_on  := $(strip $(foreach f,$(VISOR_FEATURES),\
                   $(FEAT_$(f)_SRC) $(if $(filter gui,$(VISOR_FEATURES)),$(FEAT_$(f)_SRC_GUI))))
visor_src_off := $(strip $(foreach f,$(VISOR_FEATURES_OFF),$(addprefix stubs/,$(FEAT_$(f)_STUB))))

SRCS = $(sort $(CORE_SRC) $(ARCH_SRC) $(visor_src_on) $(visor_src_off))
OBJDIR = $(BUILD_DIR)/$(ARCH)
OBJS = $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))

GEN_DIR = $(OBJDIR)/gen
GEN_H   = $(GEN_DIR)/visor_features.h

GEN_C   = $(GEN_DIR)/visor_manifest.c
GEN_OBJ = $(OBJDIR)/visor_manifest.o

empty :=
space := $(empty) $(empty)

VISOR_SCHEMA := $(shell sed -n 's/.*"schema_version":[[:space:]]*"\([^"]*\)".*/\1/p' features/features.json | head -1)
VISOR_REG    := $(shell cksum features/features.mk features/features.json 2>/dev/null | md5sum | cut -c1-16)
VISOR_REG    := $(if $(VISOR_REG),$(VISOR_REG),0000000000000000)

visor_json_feats := [$(subst $(space),$(visor_comma),$(foreach f,$(VISOR_FEATURES),\"$(f)\"))]

FONT    ?= /usr/share/fonts/TTF/JetBrainsMono-Regular.ttf
FONT_PX ?= 128

.PHONY: all clean install bakefont check-env check-reloc check-keys \
        check-features list-features list-all-features list-all-features-deps \
        FORCE

all: check-env $(TARGET)

check-keys:
	@python3 tools/check_config_keys.py
	@python3 tools/tag_schema.py --check

check-features:
	@python3 tools/check_features.py

list-features:
	@echo "profile:  $(PROFILE)$(if $(FEATURES), + FEATURES=$(FEATURES))"
	@echo "arch:     $(ARCH)"
	@echo
	@echo "on  ($(words $(VISOR_FEATURES))):"
	@for f in $(VISOR_FEATURES); do echo "    $$f"; done
	@echo "off ($(words $(VISOR_FEATURES_OFF))):"
	@for f in $(VISOR_FEATURES_OFF); do echo "    $$f"; done
ifneq ($(visor_pulled),)
	@echo
	@echo "pulled in as dependencies: $(visor_pulled)"
endif
ifneq ($(visor_archbad),)
	@echo
	@echo "unavailable on $(ARCH): $(visor_archbad)"
  ifneq ($(visor_collateral),)
	@echo "dropped because they need one of those: $(visor_collateral)"
  endif
endif
ifneq ($(visor_profnote),)
	@echo
	@echo "not part of the $(PROFILE) profile: $(visor_profnote)"
endif
	@echo
	@echo "profiles: $(PROFILE_ALL)"

list-all-features:
	@echo "features: $(visor_pickable)"

list-all-features-deps:
	@$(foreach f,$(visor_pickable),echo "$(f): $(FEAT_$(f)_DEPS)";)

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
	@missing=; for s in $(visor_src_off); do \
	  test -f "$(SRC_DIR)/$$s" || missing="$$missing $$s"; done; \
	if [ -n "$$missing" ]; then \
	  echo "ERROR: a feature is off but its stub is missing:$$missing"; \
	  echo "       every removable feature needs src/stubs/stub_<feature>.c"; \
	  exit 1; fi
	@echo "profile $(PROFILE) [$(ARCH)]: $(words $(VISOR_FEATURES)) features on, $(words $(VISOR_FEATURES_OFF)) off$(if $(visor_archbad), ($(visor_archbad) n/a here))"

$(GEN_H): FORCE
	@mkdir -p $(GEN_DIR)
	@{ echo "/* generated by make - do not edit, do not commit */"; \
	   echo "#ifndef VISOR_FEATURES_H"; \
	   echo "#define VISOR_FEATURES_H"; \
	   echo ""; \
	   echo "#define VISOR_PROFILE \"$(PROFILE)\""; \
	   echo ""; \
	   for f in $(FEAT_ALL); do \
	     u=$$(echo $$f | tr 'a-z' 'A-Z'); \
	     case " $(VISOR_FEATURES) " in *" $$f "*) v=1;; *) v=0;; esac; \
	     printf '#define VISOR_HAS_%-14s %s\n' "$$u" "$$v"; \
	   done; \
	   echo ""; \
	   echo "#endif"; } > $(GEN_H).tmp
	@cmp -s $(GEN_H).tmp $(GEN_H) 2>/dev/null && rm -f $(GEN_H).tmp \
	  || { mv -f $(GEN_H).tmp $(GEN_H); echo "gen: $(GEN_H)"; }

$(GEN_C): FORCE
	@mkdir -p $(GEN_DIR)
	@{ echo "/* generated by make - do not edit, do not commit */"; \
	   echo ""; \
	   echo "__attribute__((used))"; \
	   echo "const char visor_build_manifest[] ="; \
	   echo '  "VISORFT1{\"v\":1,\"ver\":\"$(VISOR_VERSION)\","'; \
	   echo '  "\"arch\":\"$(ARCH)\",\"profile\":\"$(PROFILE)\","'; \
	   echo '  "\"schema\":\"$(VISOR_SCHEMA)\",\"reg\":\"$(VISOR_REG)\","'; \
	   echo '  "\"feat\":$(visor_json_feats),"' ; \
	   echo '  "\"opts\":{\"font_px\":$(FONT_PX)}}"' ; \
	   echo '  "VISORFTEND";'; \
	   echo ""; } > $(GEN_C).tmp
	@cmp -s $(GEN_C).tmp $(GEN_C) 2>/dev/null && rm -f $(GEN_C).tmp \
	  || { mv -f $(GEN_C).tmp $(GEN_C); echo "gen: $(GEN_C)"; }

FORCE:

$(GEN_OBJ): $(GEN_C) $(GEN_H)
	@mkdir -p $(@D)
	$(CC) $(EFI_CFLAGS) -I $(GNU_EFI_INC) -I $(GNU_EFI_INC)/$(ARCH) \
	      -c $(GEN_C) -o $@

bakefont:
	python3 tools/bake_font.py "$(FONT)" $(FONT_PX) jetbrains $(SRC_DIR)/gui/font_jetbrains.c

$(TARGET): $(OBJS) $(GEN_OBJ)
	@mkdir -p $(OBJDIR)
	$(CC) $(EFI_LDFLAGS) -o $(OBJDIR)/visor.so $(OBJS) $(GEN_OBJ)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic \
		   -j .dynsym  -j .dynstr -j .rel* -j .rela* -j .reloc \
		   $(EFI_OBJCOPY) $(OBJDIR)/visor.so $(TARGET)
ifeq ($(RELOC_FIXUP),1)
	@printf '\000\020\000\000\014\000\000\000\000\000\000\000' > $(OBJDIR)/reloc.bin
	$(OBJCOPY) --update-section .reloc=$(OBJDIR)/reloc.bin $(TARGET)
	@$(MAKE) --no-print-directory check-reloc ARCH=$(ARCH) TARGET=$(TARGET)
endif

$(OBJS): | $(GEN_H)

$(OBJDIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
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
