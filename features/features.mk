# features.mk - the build half of the Visor feature registry.
#
# This file is the single source of truth for what can be compiled out and
# what each feature costs.  It is plain make syntax so `make` consumes it
# natively with no Python involved, and it is regular enough that
# tools/check_features.py and install.sh can parse it with a regex.
#
# For each feature <f>:
#   FEAT_<f>_SRC      sources compiled when <f> is on
#   FEAT_<f>_SRC_GUI  extra sources compiled only when <f> AND gui are on
#                     (the gui_*.c companion that draws the feature's panel)
#   FEAT_<f>_DEPS     features that must also be on for <f> to mean anything
#   FEAT_<f>_STUB     the no-op unit compiled in <f>'s place when it is off
#   FEAT_<f>_ARCH     architectures the feature is meaningful on; unset means
#                     all of them.  On any other arch the feature is forced
#                     off, and anything depending on it goes off with it.
#   FEAT_<f>_PROFILES profiles the feature may appear in; unset means any.
#                     Outside those profiles it is forced off the same way an
#                     unavailable arch forces it off - including when it is
#                     asked for by name in FEATURES.  This is for things that
#                     are not meant to be assembled a la carte; it is not a
#                     security boundary, the source is right there.
#
# A feature with no _STUB defines nothing the rest of the image references;
# dropping it needs no replacement.  Everything not listed here is core and
# is always compiled - see CORE_SRC at the bottom.

FEAT_ALL := gui fade screensaver blur clock pointer editor accent \
            anim anim_gif anim_mp4 capture filebrowse audio rbd \
            recovery gptrepair snapshots bls discover hotplug luks \
            verify crypto tpm selfheal loader_iface rawboot

# ---------------------------------------------------------------- graphics --

FEAT_gui_SRC          := gui/gui.c gui/gui_draw.c gui/gui_text.c \
                         gui/gui_card.c gui/gui_menu.c gui/gui_panels.c \
                         gui/gui_image.c gui/gui_background.c gui/gui_run.c \
                         gui/font_jetbrains.c decoders/png_decoder.c
FEAT_gui_DEPS         :=
FEAT_gui_STUB         := stub_gui.c

FEAT_fade_SRC         := gui/gui_fade.c
FEAT_fade_DEPS        := gui
FEAT_fade_STUB        := stub_fade.c

FEAT_screensaver_SRC  := gui/gui_screensaver.c
FEAT_screensaver_DEPS := gui
FEAT_screensaver_STUB := stub_screensaver.c

FEAT_blur_SRC         := gui/gui_blur.c
FEAT_blur_DEPS        := gui
FEAT_blur_STUB        := stub_blur.c

FEAT_clock_SRC        := gui/gui_clock.c
FEAT_clock_DEPS       := gui
FEAT_clock_STUB       := stub_clock.c

FEAT_pointer_SRC      := gui/gui_pointer.c
FEAT_pointer_DEPS     := gui
FEAT_pointer_STUB     := stub_pointer.c

FEAT_editor_SRC       := gui/gui_editor.c
FEAT_editor_DEPS      := gui
FEAT_editor_STUB      := stub_editor.c

FEAT_accent_SRC       := gui/accent.c
FEAT_accent_SRC_GUI   := gui/gui_accent.c
FEAT_accent_DEPS      := gui
FEAT_accent_STUB      := stub_accent.c

# ------------------------------------------------------------- animation ----

FEAT_anim_SRC         := decoders/anim.c
FEAT_anim_DEPS        := gui
FEAT_anim_STUB        := stub_anim.c

FEAT_anim_gif_SRC     := decoders/gif_decoder.c
FEAT_anim_gif_DEPS    := anim
FEAT_anim_gif_STUB    := stub_anim_gif.c

FEAT_anim_mp4_SRC     := decoders/mp4_decoder.c decoders/mjpeg_decoder.c \
                          decoders/vbg_decoder.c
FEAT_anim_mp4_DEPS    := anim
FEAT_anim_mp4_STUB    := stub_anim_mp4.c

# ------------------------------------------------------------ interaction ---

FEAT_capture_SRC      := capture/capture.c capture/capture_png.c \
                         capture/capture_gif.c capture/capture_file.c
FEAT_capture_SRC_GUI  := gui/gui_capture.c
FEAT_capture_DEPS     := gui
FEAT_capture_STUB     := stub_capture.c

# filebrowse works in text mode too, so it does not require gui.
FEAT_filebrowse_SRC     := browse/filebrowse.c browse/text_browse.c
FEAT_filebrowse_SRC_GUI := gui/gui_browse.c
FEAT_filebrowse_DEPS    :=
FEAT_filebrowse_STUB    := stub_filebrowse.c

# menu_sound.c owns the boot sound and the PCM helpers rbd.c borrows, so a
# build can have menu sounds without the easter egg's baked audio.
# x86-only: hda.c is a fail-closed stub elsewhere, because the HDA controller
# is x86 chipset hardware.  On aarch64 this feature would compile to nothing
# but still cost menu_sound.c, so refuse it rather than ship a silent knob.
FEAT_audio_SRC        := audio/hda.c audio/menu_sound.c
FEAT_audio_DEPS       :=
FEAT_audio_ARCH       := x86_64
FEAT_audio_STUB       := stub_audio.c

# x86-only by inheritance: the sting is half its audio, and audio is x86-only.
#
# full-only on purpose.  It is an easter egg, so it arrives by finding it in a
# full build rather than by being ticked off a list: it is not in any other
# profile, the custom picker does not offer it, and FEATURES=+rbd on a profile
# that is not full drops it again.  Its entry stays in features.json because a
# full build really does contain it and the manifest should say so.
FEAT_rbd_SRC          := audio/rbd.c
FEAT_rbd_SRC_GUI      := gui/gui_rbd.c
FEAT_rbd_DEPS         := gui audio capture fade
FEAT_rbd_ARCH         := x86_64
FEAT_rbd_PROFILES     := full
FEAT_rbd_STUB         := stub_rbd.c

# --------------------------------------------------------- recovery & GPT ---

FEAT_recovery_SRC     := text/text_recovery.c config/config_recovery.c
FEAT_recovery_DEPS    :=
FEAT_recovery_STUB    := stub_recovery.c

FEAT_gptrepair_SRC     := gpt/gpt.c gpt/gpt_diagnose.c gpt/gpt_text.c \
                          gpt/gpt_repair.c gpt/text_gptcmd.c
FEAT_gptrepair_SRC_GUI := gui/gui_gptwarn.c
FEAT_gptrepair_DEPS    :=
FEAT_gptrepair_STUB    := stub_gptrepair.c

FEAT_selfheal_SRC     := core/efi_selfheal.c
FEAT_selfheal_DEPS    :=
FEAT_selfheal_STUB    := stub_selfheal.c

# ------------------------------------------------------- entry discovery ----

FEAT_discover_SRC     := config/config_discover.c
FEAT_discover_DEPS    :=
FEAT_discover_STUB    := stub_discover.c

FEAT_bls_SRC          := config/config_bls.c
FEAT_bls_DEPS         := discover
FEAT_bls_STUB         := stub_bls.c

FEAT_hotplug_SRC      := config/config_hotplug.c
FEAT_hotplug_DEPS     := discover
FEAT_hotplug_STUB     := stub_hotplug.c

FEAT_snapshots_SRC    := config/config_snapshots.c
FEAT_snapshots_DEPS   := discover
FEAT_snapshots_STUB   := stub_snapshots.c

# ------------------------------------------------------------- security -----

FEAT_verify_SRC       := security/sha256.c security/hash_verify.c
FEAT_verify_DEPS      :=
FEAT_verify_STUB      := stub_verify.c

FEAT_crypto_SRC       := security/crypto.c
FEAT_crypto_DEPS      := verify
FEAT_crypto_STUB      := stub_crypto.c

FEAT_luks_SRC         := config/config_luks.c security/luks_keyfile.c
FEAT_luks_DEPS        :=
FEAT_luks_STUB        := stub_luks.c

FEAT_tpm_SRC          := security/tcg2.c
FEAT_tpm_DEPS         :=
FEAT_tpm_STUB         := stub_tpm.c

# ------------------------------------------------------------ interfaces ----

FEAT_loader_iface_SRC  := boot/loader_iface.c
FEAT_loader_iface_DEPS :=
FEAT_loader_iface_STUB := stub_loader_iface.c

# x86-only; linux_rawboot.c is bracketed by #if defined(__x86_64__) and
# compiles to nothing elsewhere.
FEAT_rawboot_SRC      := boot/linux_rawboot.c
FEAT_rawboot_DEPS     :=
FEAT_rawboot_ARCH     := x86_64
FEAT_rawboot_STUB     := stub_rawboot.c

# ------------------------------------------------------------------ core ----
#
# Never removable.  text_menu in particular is the safety net: main.c falls
# back to it when gui_init finds no GOP, so a build without it would leave
# such a machine with no menu at all.

CORE_SRC := core/main.c \
            core/efi_helpers.c core/efi_var.c core/efi_path.c core/efi_file.c \
            core/efi_fsdrv.c core/efi_log.c core/gpt_disk.c \
            config/config.c config/config_util.c config/config_entry.c \
            config/config_cmdline.c config/config_keys.c \
            boot/linux_boot.c boot/linux_initrd.c boot/linux_file.c \
            boot/windows_boot.c \
            text/text_menu.c

# ------------------------------------------------------------- profiles -----
#
# A profile is just a starting feature set; install.sh and the command line
# refine it with +feat / -feat.

PROFILE_ALL := minimal standard hardened ricer full custom

# discover is in every profile: install.sh tells users they can delete
# boot.conf and let Visor find their kernels, and that has to stay true.
PROFILE_minimal  := discover verify
PROFILE_standard := gui fade blur clock pointer editor accent \
                    discover bls hotplug luks verify crypto tpm \
                    recovery selfheal loader_iface rawboot
# hardened is about parser surface, not size: no gif/mp4/mjpeg decoders and
# no file browser reading arbitrary ESP paths.  PNG stays - a graphical menu
# without icons is not a menu.  It is a smaller target, not an airtight one.
PROFILE_hardened := gui fade blur clock pointer editor \
                    discover bls luks verify crypto tpm \
                    recovery selfheal gptrepair loader_iface rawboot
# everything except the two heavyweight non-visual extras - and not rbd, which
# is full-only (see FEAT_rbd_PROFILES); ricer keeps rbd's dependencies anyway.
PROFILE_ricer    := gui fade screensaver blur clock pointer editor accent \
                    anim anim_gif anim_mp4 capture filebrowse audio \
                    discover bls hotplug luks verify crypto tpm \
                    recovery selfheal loader_iface rawboot
PROFILE_full     := $(FEAT_ALL)
