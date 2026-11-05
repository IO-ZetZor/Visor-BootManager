/* main.c - entry point: EFI app init, boot menu and boot path dispatch */
#include <efi.h>
#include <efilib.h>
#include "gui.h"
#include "config.h"
#include "crypto.h"
#include "efi_helpers.h"
#include "linux_boot.h"
#include "windows_boot.h"
#include "text_menu.h"
#include "tcg2.h"
#include "loader_iface.h"
#include "efi_selfheal.h"
#include "rbd.h"
#include "menu_sound.h"
#include "capture_rec.h"
#include "intro.h"

EFI_HANDLE IH;

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table);

typedef void (*visor_init_fn)(void);
extern visor_init_fn __init_array_start[], __init_array_end[];
extern visor_init_fn __CTOR_LIST__[], __CTOR_END__[];
extern visor_init_fn __fini_array_start[], __fini_array_end[];
extern visor_init_fn __DTOR_LIST__[], __DTOR_END__[];

/* gnu-efi's stock entry point runs constructors before efi_main() and
 * destructors afterwards.  Replacing _entry() means we must keep that
 * contract; otherwise any vendor constructor/destructor is silently skipped. */
static void visor_run_ctors(void) {
    UINTN n = (UINTN)(__init_array_end - __init_array_start);
    for (UINTN i = 0; i < n; i++)
        if (__init_array_start[i]) __init_array_start[i]();
    n = (UINTN)(__CTOR_END__ - __CTOR_LIST__);
    for (UINTN i = 0; i < n; i++) {
        visor_init_fn fn = __CTOR_LIST__[n - i - 1];
        if (fn) fn();
    }
}

static void visor_run_dtors(void) {
    UINTN n = (UINTN)(__DTOR_END__ - __DTOR_LIST__);
    for (UINTN i = 0; i < n; i++)
        if (__DTOR_LIST__[i]) __DTOR_LIST__[i]();
    n = (UINTN)(__fini_array_end - __fini_array_start);
    for (UINTN i = 0; i < n; i++) {
        visor_init_fn fn = __fini_array_start[n - i - 1];
        if (fn) fn();
    }
}

/**
 * _entry - PE entry point, called by the firmware before gnu-efi is set up.
 *
 * Runs constructors, then efi_main.  gnu-efi's stock _entry() calls
 * InitializeLib() first; we keep our own so the constructor/destructor
 * contract stays explicit and the service pointers are installed by us.
 *
 * Deliberately no console output here.  ConOut->OutputString is a privileged
 * protocol call: on firmware where Visor is the first PE image to run, it can
 * bind console device drivers before GOP is up, and that is a known freeze on
 * some consumer boards.  Production builds stay silent until efi_main();
 * diagnostic builds get a stage-0 checkpoint from VISOR_EARLY_DIAG in
 * early_start_x86_64.S, which runs even earlier than this function.
 *
 * @param image_handle this image's handle
 * @param system_table firmware system table
 * @return efi_main's status
 */
EFI_STATUS _entry(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    /*
     * Passing a NULL image handle deliberately skips gnu-efi's early
     * HandleProtocol/GetVariable/LocateHandle probing.  Those calls used to
     * run before efi_main() could print anything, so a firmware bug there
     * looked exactly like a PE-loader freeze.  Visor installs IH and the
     * service pointers itself, so it does not need the loaded-image lookup
     * or the Unicode collation table during startup.
     */
    InitializeLib(NULL, system_table);
    IH = image_handle;
    visor_run_ctors();
    EFI_STATUS status = efi_main(image_handle, system_table);
    visor_run_dtors();
    return status;
}

static boot_entry_t* main_entry_at(gui_state_t *state, UINTN idx) {
    boot_entry_t *e = state ? state->entries : NULL;
    for (UINTN i = 0; i < idx && e; i++) e = e->next;
    return e;
}

static int autoboot_key_matches(const config_t *config,
                                const EFI_INPUT_KEY *key) {
    if (!config->autoboot_key_unicode && !config->autoboot_key_scancode)
        return 1;

    if (config->autoboot_key_scancode)
        return key->ScanCode == config->autoboot_key_scancode;

    CHAR16 expected = (CHAR16)config->autoboot_key_unicode;
    CHAR16 actual = key->UnicodeChar;
    if (expected >= L'A' && expected <= L'Z') expected += L'a' - L'A';
    if (actual >= L'A' && actual <= L'Z') actual += L'a' - L'A';
    return actual == expected;
}

static int autoboot_menu_requested(const config_t *config) {
    EFI_INPUT_KEY key;
    while (!EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key))) {
        if (autoboot_key_matches(config, &key)) return 1;
    }
    return 0;
}

/* Bring the GUI up after the fast path decided it was not needed.  Used when
 * the user interrupts autoboot: the menu has to appear in the mode the
 * configuration asks for, not in the text fallback the fast path selected. */
static void main_clamp_gui_sizes(gui_state_t *gui);

/* Wire the parsed configuration into the GUI state.  gui_init() resets most of
 * the struct - entries, entry count, timeout, colours, callbacks - so this has
 * to run after every gui_init(), including the late one on the fast path,
 * where the menu would otherwise come up with no entries at all. */
static void main_apply_gui_config(gui_state_t *gui, config_t *config) {
    gui->entries         = config->entries;
    gui->entry_count     = config->entry_count;
    gui->per_page        = config->entries_per_page ? config->entries_per_page : 3;
    gui->timeout         = config->timeout;
    gui->bg_color        = config->bg_color;
    gui->fg_color        = config->fg_color;
    gui->highlight_color = config->highlight_color;
    gui->title           = config->title;
    gui->show_title      = !config->no_title;
    gui->logo_mode       = config->no_logo ? LOGO_MODE_OFF : config->logo_mode;
    gui->logo_size       = config->logo_size;
    gui->logo_gap        = config->logo_gap;
    gui->accent_logo     = config->has_accent_logo ? config->accent_logo
                                                 : config->accent_text;
    gui->sp_logo         = config->sp_logo;
    gui->sp_underline    = config->sp_underline;
    gui->sp_highlight    = config->sp_highlight;
    gui->sp_title        = config->sp_title;
    gui->sp_name         = config->sp_name;
    gui->sp_info         = config->sp_info;
    gui->sp_shutdown     = config->sp_shutdown;
    gui->sp_reboot       = config->sp_reboot;
    gui->sp_firmware     = config->sp_firmware;
    gui->sp_os_icons     = config->sp_os_icons;
    gui->sp_blur         = config->sp_blur;
    gui->sp_bg           = config->sp_bg;
    gui->sp_g_text       = config->sp_g_text;
    gui->sp_g_icons      = config->sp_g_icons;
    gui->sp_g_underline  = config->sp_g_underline;
    gui->sp_all          = config->sp_all;
    gui->show_names      = config->show_names;
    gui->center_info     = config->center_info;
    gui->box_radius      = config->box_radius;
    gui->title_color     = config->title_color;
    gui->name_color      = config->name_color;
    gui->title_size      = config->title_size;
    gui->name_size       = config->name_size;
    gui->icon_size       = config->icon_size;
    gui->icon_spacing    = config->icon_spacing;
    gui->icon_y          = config->icon_y;
    gui->underline_thickness = config->underline_thickness;
    gui->underline_length    = config->underline_length;
    gui->underline_color = config->has_underline_color ? config->underline_color
                                                     : config->highlight_color;
    gui->power_position  = config->power_position;
    gui->shutdown_color  = config->has_shutdown_color ? config->shutdown_color
                                                    : config->highlight_color;
    gui->reboot_color    = config->has_reboot_color   ? config->reboot_color
                                                    : config->highlight_color;
    gui->firmware_color  = config->has_firmware_color ? config->firmware_color
                                                    : config->highlight_color;

    gui->power_icons     = config->power_icons;
    gui->power_icon_size = config->power_icon_size;

    main_clamp_gui_sizes(gui);

    gui->blur            = config->blur;
    gui->blur_title      = config->blur_title;
    gui->blur_color      = config->has_blur_color ? config->blur_color : COLOR_WHITE;
    gui->blur_color_set  = config->has_blur_color;
    gui->accent_enabled   = config->accent_enabled;
    gui->accent_icons     = config->accent_icons;
    gui->accent_underline = config->accent_underline;
    gui->accent_text      = config->accent_text;
    gui->accent_os_icons  = config->accent_os_icons;
    gui->accent_variant   = config->accent_variant;
    gui->show_clock       = config->show_clock;
    gui->accent_clock     = config->accent_clock;
    gui->clock_color      = config->has_clock_color ? config->clock_color
                                                   : config->highlight_color;
    gui->clock_size       = config->clock_size;
    gui->sp_clock         = config->sp_clock;
    gui->clock_24h        = config->clock_24h;
    gui->clock_seconds    = config->clock_seconds;
    gui->clock_position   = config->clock_position;
    gui->clock_date       = config->clock_date;
    gui->clock_date_format = config->clock_date_format;
    gui->clock_blur       = config->clock_blur;
    gui->clock_shadow     = config->clock_shadow;
    gui->clock_sync       = config->clock_sync;
    gui->screensaver      = config->screensaver;
    gui->ss_delay_ms      = config->screensaver_delay * 1000;
    gui->ss_blank_ms      = config->screensaver_blank * 1000;
    gui->ss_keep_clock    = config->screensaver_clock;
    gui->record_seconds   = config->record_seconds;
    gui->animation       = config->animation;
    gui->anim_speed      = config->anim_speed;
    gui->fade_speed      = config->fade_speed;
    {
        int sp = config->anim_speed;
        if (sp < 1) sp = 8;
        if (sp > 10) sp = 10;
        gui->anim_frames = gui->animation ? 26 - sp * 2 : 1;
        if (gui->animation && gui->anim_frames < 5) gui->anim_frames = 5;
    }
    if (config->default_entry < gui->entry_count)
        gui->selected = config->default_entry;
}

/* boot.conf is user input; cap display sizes so extreme values cannot push the
 * menu off-screen or overflow layout arithmetic.  The caps depend on the screen
 * height, so this has to run after the mode is chosen - in the fast path the
 * GUI comes up late, and clamping against a zero height would pin every size to
 * its floor. */
static void main_clamp_gui_sizes(gui_state_t *gui) {
    UINTN title_cap = gui->screen_height / 6;
    if (title_cap < 48) title_cap = 48;
    if (gui->title_size > title_cap) gui->title_size = title_cap;

    UINTN name_cap = gui->screen_height / 10;
    if (name_cap < 18) name_cap = 18;
    if (gui->name_size > name_cap) gui->name_size = name_cap;

    UINTN icon_cap = gui->screen_height * 2 / 5;
    if (icon_cap < 32) icon_cap = 32;
    if (icon_cap > 512) icon_cap = 512;
    if (gui->icon_size > icon_cap) gui->icon_size = icon_cap;
    if (gui->power_icon_size > icon_cap) gui->power_icon_size = icon_cap;
}

/**
 * Bring the configured display mode up and do whatever the menu needs a
 * framebuffer for.
 *
 * Runs on the normal path right after the GUI is initialised, and again from
 * main_gui_ensure() when a user interrupts a fastboot and asks for the menu.
 * The configured title, name and icon sizes are restored first, because the
 * fast path wired them while no mode was known, then re-capped against the
 * height that is actually in use.
 *
 * @param[in,out] gui    GUI state returned by gui_init().
 * @param[in]     config Parsed configuration.
 */
static void main_gui_late_start(gui_state_t *gui, config_t *config) {
    if (config->res_max || (config->res_w && config->res_h))
        gui_set_mode(gui, config->res_w, config->res_h, config->res_max);

    /* The fast path wired the sizes before a mode existed; put the configured
     * values back and cap them against the height that is actually in use. */
    gui->title_size = config->title_size;
    gui->name_size  = config->name_size;
    gui->icon_size  = config->icon_size;
    gui->power_icon_size = config->power_icon_size;
    main_clamp_gui_sizes(gui);

    if (config->power_icons) {
        if (config->shutdown_icon) gui->shutdown_icon = gui_load_icon(config->shutdown_icon);
        if (config->reboot_icon)   gui->reboot_icon   = gui_load_icon(config->reboot_icon);
        if (config->firmware_icon) gui->firmware_icon = gui_load_icon(config->firmware_icon);
        if ((config->shutdown_icon && !gui->shutdown_icon) ||
            (config->reboot_icon   && !gui->reboot_icon)   ||
            (config->firmware_icon && !gui->firmware_icon))
            efi_log(L"WARN: a power icon failed to load - falling back to text for it");
    }

    if (config->font) {
        char name[32]; UINTN i = 0;
        for (; config->font[i] && i < sizeof(name) - 1; i++) name[i] = (char)config->font[i];
        name[i] = '\0';
        gui_set_font(name);
    }

    cap_session_arm();
    int rbd_played = rbd_check_and_play(gui);
    if (!rbd_played &&
        menu_sound_prepare(config->menu_sound_on, config->menu_sound,
                           config->menu_sound_volume)) {
        gui->sound_start = menu_sound_start;
        gui->sound_poll  = menu_sound_poll;
    }

    if (config->mouse)
        gui_set_cursor(gui, config->cursor, config->cursor_size,
                       config->cursor_hot_x, config->cursor_hot_y);
}

/* Take the screen over for the code that is about to draw.  Until this runs
 * the firmware's own screen is still up, which is what a boot that shows no
 * menu and no splash wants to hand to the kernel. */
static void console_takeover(void) {
    ST->ConOut->ClearScreen(ST->ConOut);
    ST->ConOut->EnableCursor(ST->ConOut, FALSE);
    /* ClearScreen just erased the warning efi_log() printed when the first
     * write failed.  Say it again, otherwise the rest of the boot looks as if
     * the log is simply empty.  Logging stays off: no retry here. */
    if (efi_log_write_failed())
        efi_print(L"WARN: boot.log could not be written - this boot is console only\r\n");
}

/* Make sure the menu can actually be drawn: initialise the GUI if the fast
 * path skipped it, and fall back to text mode when that is impossible. */
static void main_gui_ensure(gui_state_t *gui, config_t *config,
                            int *text_mode, int *gui_started,
                            int *keep_firmware_screen) {
    /* From here on something is drawn over the firmware screen, so take it
     * over first: the text menu needs a cleared console, and the GUI path
     * needs the cursor off and the log warning back.  Leaving the flag set
     * would make the handover suppress the console again after a menu. */
    if (*keep_firmware_screen) {
        console_takeover();
        *keep_firmware_screen = 0;
    }
    if (config->text_menu) { *text_mode = 1; return; }
    if (*gui_started) return;

    EFI_STATUS status = gui_init(gui);
    if (EFI_ERROR(status)) {
        efi_log(L"WARN: gui_init failed - showing the text menu instead");
        efi_print(L"Graphics unavailable - using text menu\r\n");
        *text_mode = 1;
        return;
    }
    *gui_started = 1;
    *text_mode = 0;
    /* gui_init() just reset the struct: re-apply the configuration (entries,
     * timeout, colours, hotplug callback) before anything draws a menu, then
     * do the mode/font/cursor work that needs a framebuffer. */
    main_apply_gui_config(gui, config);
    main_gui_late_start(gui, config);
}

static int hotplug_poll_cb(void *ctx, boot_entry_t **head, UINTN *count,
                           UINTN *first_new) {
    config_t *cfg = (config_t*)ctx;
    int mask = config_hotplug_poll(cfg, first_new);
    if (!mask) return 0;
    *head  = cfg->entries;
    *count = cfg->entry_count;
    return mask;
}

static void wipe_password(CHAR16 **pw) {
    if (!pw || !*pw) return;
    volatile CHAR16 *p = (volatile CHAR16*)*pw;
    while (*p) *p++ = 0;
    efi_free_pool(*pw);
    *pw = NULL;
}

static CHAR16* text_prompt_password(CHAR16 *label) {
    CHAR16 buf[512];
    UINTN len = 0;
    int reveal = 0;
    EFI_INPUT_KEY key;

    for (UINTN i = 0; i < 512; i++) buf[i] = 0;
    efi_print(label);
    efi_print(L"   (F2 shows what you typed)\r\n");
    efi_print(label);

    for (;;) {
        EFI_STATUS ks = ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
        if (EFI_ERROR(ks)) { BS->Stall(30000); continue; }

        if (key.UnicodeChar == 0x0D) break;
        if (key.UnicodeChar == 0x1B) {
            for (UINTN i = 0; i < 512; i++) buf[i] = 0;
            return NULL;
        }
        if (key.UnicodeChar == 0x00 && key.ScanCode == 0x0C) {
            reveal = !reveal;
        } else if (key.UnicodeChar == 0x08) {
            if (len) len--;
        } else if (key.UnicodeChar >= 0x20 && len < 511) {
            buf[len++] = key.UnicodeChar;
        } else {
                continue;
            }

            buf[len] = 0;
            CHAR16 echo[520];
            UINTN k = 0;
            echo[k++] = L'\r';
            for (UINTN i = 0; label[i] && k < 500; i++) echo[k++] = label[i];
            for (UINTN i = 0; i < len && k < 515; i++) echo[k++] = reveal ? buf[i] : L'*';
            echo[k++] = L' ';
            echo[k] = 0;
            efi_print(echo);
        }

        buf[len] = 0;
        efi_print(L"\r\n");
        CHAR16 *out = efi_strdup(buf);
        for (UINTN i = 0; i < 512; i++) buf[i] = 0;
        if (!out) efi_log(L"ERROR: out of memory copying encrypted boot password");
        return out;
    }

    /**
     * efi_main - bring up logging, the GUI and the menu, then boot an entry.
     *
     * Order matters for diagnosis: the console checkpoints and the log header
     * are written before the first RT->GetTime call, so a hang in the firmware
     * clock leaves a named checkpoint on screen and a non-empty boot.log
     * instead of a silent freeze.
     *
     * @param image_handle this image's handle
     * @param system_table firmware system table
     * @return EFI_SUCCESS when an entry was started, an error status otherwise
     */
    EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
        /* Our _entry() already ran InitializeLib(NULL, ...), so this call returns
         * immediately.  Keep it for stock-entry builds and set the globals again
         * explicitly: the project uses IH rather than gnu-efi's LibImageHandle. */
        InitializeLib(image_handle, system_table);

        ST = system_table;
        BS = system_table->BootServices;
        RT = system_table->RuntimeServices;
        IH = image_handle;

        /* boot.conf decides whether these checkpoints print at all, so it has
         * to be read before the first line.  That read is the one path the
         * checkpoints no longer cover: on a machine that hangs while reading
         * the config, nothing appears on screen - which is why an unreadable
         * config leaves checkpoints on instead of off. */
        int early_file_log = 1, early_checkpoints = 1;
        config_early_flags(&early_file_log, &early_checkpoints);

        /* Checkpoint: this line is the last thing printed before any firmware
         * call that is not a plain protocol lookup.  If it appears and the
         * machine still freezes, the fault is in the file/log or GUI setup
         * below; if it never appears, it is in the entry path, the safe
         * library initialization above, or the constructor run.  A diagnostic
         * build also prints a stage-0 line from the PE entry point. */
        if (early_checkpoints)
            efi_print(L"Visor loading...\r\n");

        /* Every step below up to the GUI touches the ESP through the FAT
         * driver, and that driver may itself ask RT->GetTime for file
         * timestamps.  These console checkpoints cost nothing and split the
         * file-system path into named steps without using the file system. */
        if (early_checkpoints)
            efi_print(L"Visor: reading boot.conf\r\n");
        efi_log_set_file(0);
        efi_log_set_file(early_file_log);
        if (early_file_log) {
            if (early_checkpoints)
                efi_print(L"Visor: opening boot.log\r\n");
            efi_log_begin();
        } else if (early_checkpoints) {
            efi_print(L"Visor: boot logging disabled by config\r\n");
        }
        efi_log(L"main: efi_main entered, services initialised");

        /*
         * Stamp the boot time only after the log file is open and the first
         * line is on disk.  loader_mark_init() calls RT->GetTime(), a firmware
         * RuntimeServices call that hangs on some boards; running it first made
         * that hang look like a silent freeze with an empty boot.log.
         *
         * The log file alone is not a trustworthy checkpoint for that call:
         * EnhancedFatDxe asks RT->GetTime for the file timestamp itself, so on
         * the very firmware this guards against, opening or writing boot.log
         * can enter the same hang.  The console line below is the checkpoint
         * that does not need the FAT driver, so the screen shows whether the
         * machine got past the log setup even when nothing reaches the ESP.
         */
        if (early_checkpoints)
            efi_print(L"Visor: log ready, reading the firmware clock\r\n");
        loader_mark_init();

        /* The console is deliberately left alone here.  A boot that shows no
         * menu or splash never touches the video, so the firmware's own screen
         * stays up until the kernel takes over; whichever branch below does
         * draw calls console_takeover() first. */

        gui_state_t gui = {0};
        int text_mode = 0;
        int gui_started = 0;
        int force_menu = 0;
        int keep_firmware_screen = 0;
        UINTN loader_pick = (UINTN)-1;
        EFI_STATUS status;

        efi_log(L"main: loading filesystem drivers from \\EFI\\visor\\drivers");
        efi_load_fs_drivers();

        efi_log(L"main: parsing config \\EFI\\visor\\boot.conf");
        config_t config;

        efi_fs_drivers_set_lazy(0);
        status = config_parse(&config);
        efi_log_set_file(config.file_log);
        efi_log_rotate();
        if (EFI_ERROR(status)) {
            efi_log(L"WARN: config_parse returned error - using auto-detected entries");
            efi_print(L"Warning: Config parse failed, using auto-detect\r\n");
        }
        { CHAR16 d[80]; SPrint(d, sizeof(d), L"main: config parsed, %d entr%s",
            (int)config.entry_count, config.entry_count == 1 ? L"y" : L"ies"); efi_log(d); }
        if (config.entry_count == 0)
            efi_log(L"WARN: no boot entries found - starting from zero");

        /* A platform can advertise a TPM through EFI_TCG2_PROTOCOL while firmware
         * setup has it disabled; the probe then blocks inside firmware and the
         * reset that follows is the only way out.  Skip the probe when the marker
         * says an earlier boot died in it, or when it already disabled it. */
        if (config.tpm && tpm_probe_skip()) {
            efi_log(L"main: skipping measured boot this boot");
            config.tpm = 0;
        }

        if (config.tpm) {
            EFI_STATUS stage = tpm_stage_begin();
            if (EFI_ERROR(stage)) {
                /* Without a marker a hang here is unrecoverable, so not probing is
                 * the safer failure: the machine still boots, unmeasured. */
                CHAR16 d[128];
                SPrint(d, sizeof(d),
                       L"tpm: cannot store the boot-loop marker (status=%r) - skipping the probe",
                       stage);
                efi_log(d);
            } else {
            tpm_set_pcrs(config.tpm_pcr_config, config.tpm_pcr_cmdline);
            if (tpm_init()) {

                efi_file_buffer_t *cfg_raw = efi_load_file(CONFIG_FILE);
                if (cfg_raw) {
                    if (cfg_raw->data && cfg_raw->size)
                        tpm_measure_config(cfg_raw->data, cfg_raw->size, L"boot.conf");
                    if (cfg_raw->data) efi_free_pool(cfg_raw->data);
                    efi_free_pool(cfg_raw);
                } else {
                    efi_log(L"tpm: no boot.conf on disk - nothing to measure into the config PCR");
                }
            }
            tpm_stage_end();
        }
    } else {
        efi_log(L"main: tpm=0 in config - skipping measured boot");
    }

    if (config.loader_vars) {
        loader_export_common(&config);
        loader_export_entries(config.entries);
        loader_pick = loader_apply_overrides(&config, config.entries,
                                             config.entry_count, &config.timeout);
        if (loader_pick != (UINTN)-1) config.default_entry = loader_pick;
    } else {
        efi_log(L"main: loader_vars=0 in config - not exporting Loader* variables");
    }

    {
        extern const char visor_build_manifest[];
        efi_set_loader_var_raw(L"VisorBuild",
                               (void *)visor_build_manifest,
                               efi_strlen8(visor_build_manifest), 0);
    }

    if (config.selfheal) {
        efi_log(L"main: running boot self-heal (NVRAM boot entries + fallback copy)");
        nvsh_policy_t pol;
        nvsh_policy_defaults(&pol);
        pol.order_mode       = config.selfheal_order;
        pol.restore_fallback = config.restore_fallback;
        nvram_self_heal(&pol);
    } else {
        efi_log(L"main: selfheal=0 in config - boot self-heal disabled");
    }

    /* --- Framebuffer ------------------------------------------------------
     * A fast path that shows nothing (autoboot with no splash, or splash
     * drawing turned off with splash_gui=0) does not need a framebuffer at
     * all: skip gui_init so the firmware's own screen stays up until the
     * kernel takes over.  The menu is only built when it will be shown, and
     * if the user interrupts autoboot the GUI is started late, in the mode
     * the configuration asks for. */
    int fastboot   = !force_menu && config.autoboot && config.entry_count >= 1;
    int splash_on  = config.splash && config.splash_gui;

    if (!config.text_menu && (!fastboot || splash_on)) {
        console_takeover();
        efi_log(L"main: initialising GUI (locating GOP, allocating back buffer)");
        status = gui_init(&gui);
        if (EFI_ERROR(status)) {
            efi_log(L"WARN: gui_init failed (no GOP or out of memory) - falling back to text mode");
            efi_print(L"Graphics unavailable - using text menu\r\n");
            text_mode = 1;
            gui.selected = 0;
            gui.timeout_active = 1;
        } else {
            gui_started = 1;
            efi_log(L"main: GUI ready");
        }
    } else if (config.text_menu) {
        console_takeover();
        efi_log(L"main: text_menu=1 - no framebuffer needed");
        text_mode = 1;
        gui.selected = 0;
        gui.timeout_active = 1;
    } else {
        efi_log(L"main: fastboot - no framebuffer needed, leaving the firmware screen up");
        keep_firmware_screen = 1;
        text_mode = 1;
        gui.selected = 0;
        gui.timeout_active = 1;
    }

    if (config.text_menu && !text_mode) {
        efi_log(L"main: text_menu=1 in config - using text mode despite available graphics");
        text_mode = 1;
        if (gui.backbuffer)  { efi_free_pool(gui.backbuffer);  gui.backbuffer  = NULL; }
        if (gui.scene_cache) { efi_free_pool(gui.scene_cache); gui.scene_cache = NULL; }
        if (gui.blur_cache)  { efi_free_pool(gui.blur_cache);  gui.blur_cache  = NULL; }
        gui.selected = 0;
        gui.timeout_active = 1;
    }

    if (!text_mode && (config.res_max || (config.res_w && config.res_h))) {
        CHAR16 d[80];
        if (config.res_max) SPrint(d, sizeof(d), L"main: applying resolution=max");
        else SPrint(d, sizeof(d), L"main: applying resolution %dx%d",
                    (int)config.res_w, (int)config.res_h);
        efi_log(d);
        gui_set_mode(&gui, config.res_w, config.res_h, config.res_max);
    }

    if (config.hotplug) {
        config_hotplug_arm(&config);
        efi_log(L"main: hotplug volume watch armed");
    }
    main_apply_gui_config(&gui, &config);
    if (!text_mode && config.power_icons) {
        if (config.shutdown_icon) gui.shutdown_icon = gui_load_icon(config.shutdown_icon);
        if (config.reboot_icon)   gui.reboot_icon   = gui_load_icon(config.reboot_icon);
        if (config.firmware_icon) gui.firmware_icon = gui_load_icon(config.firmware_icon);
        if ((config.shutdown_icon && !gui.shutdown_icon) ||
            (config.reboot_icon   && !gui.reboot_icon)   ||
            (config.firmware_icon && !gui.firmware_icon))
            efi_log(L"WARN: a power icon failed to load - falling back to text for it");
    }

    if (!text_mode && config.font) {
        char name[32]; UINTN i = 0;
        for (; config.font[i] && i < sizeof(name) - 1; i++) name[i] = (char)config.font[i];
        name[i] = '\0';
        gui_set_font(name);
    }

    if (config.default_entry < gui.entry_count) {
        gui.selected = config.default_entry;
    }

    if (!text_mode) {
        /* Before the first pixel: if the ESP is armed, everything drawn from
         * here on - easter egg, fade-in, menu, fade-out - is recorded. */
        cap_session_arm();

        int rbd_played = rbd_check_and_play(&gui);
        if (!rbd_played &&
            menu_sound_prepare(config.menu_sound_on, config.menu_sound,
                               config.menu_sound_volume)) {
            gui.sound_start = menu_sound_start;
            gui.sound_poll  = menu_sound_poll;
        }
    }

    gui.editor_enabled = config.editor;
    gui.mouse_enabled  = config.mouse;
    gui.pointer_speed  = config.pointer_speed;
    if (!text_mode && config.mouse)
        gui_set_cursor(&gui, config.cursor, config.cursor_size,
                       config.cursor_hot_x, config.cursor_hot_y);

    if (config.remember_last && loader_pick == (UINTN)-1) {
        CHAR16 *last = efi_get_var_str(L"VisorLastEntry");
        if (last) {
            UINTN i = 0;
            for (boot_entry_t *e = gui.entries; e; e = e->next, i++) {
                if (efi_strcmp(e->name, last) == 0) { gui.selected = i; break; }
            }
            efi_free_pool(last);
            efi_log(L"main: remember_last - preselected previously booted entry");
        }
    }

    if (config.hotplug) {
        gui.hotplug_poll = hotplug_poll_cb;
        gui.hotplug_ctx  = &config;
    }

    boot_entry_t *selected = NULL;
    int action = VISOR_ACTION_BOOT;
    int autobooted = 0;
    int gui_closed = 0;
    int retry_selected = 0;
    int intro_seen = 0;
    int intro_boot = 0;
    boot_entry_t *bls_decremented = NULL;
    CHAR16 *saved_cmdline = NULL;
    CHAR16 *saved_kernel_path = NULL;
    CHAR16 *saved_initrd_path = NULL;
    CHAR16 *saved_uuid = NULL;
    EFI_HANDLE saved_hp_volume = NULL;
    CHAR16 *override_cmdline = NULL;
    CHAR16 *override_kernel_path = NULL;
    CHAR16 *override_initrd_path = NULL;
    CHAR16 *override_uuid = NULL;
    int cmdline_overridden = 0;
    int kernel_overridden = 0;
    int initrd_overridden = 0;
    int uuid_overridden = 0;
    int hp_overridden = 0;
    EFI_STATUS cleanup_status = EFI_SUCCESS;
    int splash_shown = 0;
    int splash_covered = 0;

    /* The still splash goes up before the menu, the countdown and any
     * autoboot decision, so an enabled fast boot never shows the firmware's
     * text console - or a black screen - between Visor and the kernel.  It
     * reuses the wallpaper, logo and title the menu would draw, so the frame
     * the kernel paints over already looks like the boot splash. */
    if (fastboot && splash_on && !text_mode && !gui_closed) {
        if (config.splash_image && !gui.splash_img) {
            efi_log(L"main: splash - loading splash_image");
            gui_set_splash(&gui, config.splash_image);
        }
        if (!gui.splash_img && config.background && !gui.background) {
            efi_log(L"main: splash - loading background image");
            gui_set_background(&gui, config.background);
            if (!gui.background)
                efi_log(L"WARN: splash background failed to load/decode - using solid colour");
        }
        if (!gui.splash_img && gui.logo_mode != LOGO_MODE_OFF && !gui.logo)
            gui_set_logo(&gui, config.logo);
        gui_apply_accent(&gui);
        splash_shown = gui_draw_splash(&gui);
        if (splash_shown)
            efi_log(L"main: splash painted");
        else
            efi_log(L"WARN: splash requested but no framebuffer to paint on");
    }

select_entry:
    selected = NULL;
    action = VISOR_ACTION_BOOT;
    autobooted = 0;
    /* Cleared before the splash runs, so re-entering through a goto cannot
     * inherit a previous pass's decision to boot straight away. */
    intro_boot = 0;

    /* The splash plays once, before the first menu, and only when there is a
     * menu to get to.  autoboot=1 and timeout=0 mean "no menu", so they
     * bypass it rather than delaying a boot that was never going to wait. */
    if (!intro_seen && !retry_selected && !text_mode && !gui_closed &&
        config.intro_media && !config.autoboot && gui.timeout != 0) {
        intro_seen = 1;
        int intro_result = gui_show_intro(&gui, config.intro_media,
                                          config.intro_duration,
                                          config.intro_loop,
                                          config.intro_auto_continue);
        if (intro_result == INTRO_SKIPPED) {
            /* The key that skipped the splash also cancels the menu
             * countdown - the user is already waiting and just said so. */
            gui.timeout_active = 0;
        } else if (intro_result == INTRO_FINISHED) {
            intro_boot = config.intro_boot;
        }
    }

    if (!force_menu && (config.autoboot || intro_boot) && gui.entry_count >= 1) {
        if (!autoboot_menu_requested(&config)) {
            UINTN pick = (gui.entry_count == 1) ? 0
                       : (gui.selected < gui.entry_count ? gui.selected : 0);
            selected = gui.entries;
            for (UINTN i = 0; i < pick && selected; i++) selected = selected->next;
            autobooted = 1;
            if (intro_boot)
                efi_log(L"main: intro_action=boot - booting default entry directly");
            else
                efi_log(L"main: autoboot - skipping menu, booting directly");
            menu_sound_finish();
        } else {
            if (intro_boot)
                efi_log(L"main: intro_action=boot cancelled by a key - showing menu");
            else
                efi_log(L"main: autoboot interrupt key pressed - showing menu");
            /* The fast path boots without ever bringing the video up.  The
             * user just asked for the menu, so start it now - in the mode the
             * configuration asks for, not the text fallback chosen above. */
            main_gui_ensure(&gui, &config, &text_mode, &gui_started, &keep_firmware_screen);
        }
    }

    if (!autobooted && !retry_selected) {

        ST->ConIn->Reset(ST->ConIn, FALSE);
        if (!text_mode && !gui_closed && config.background && !gui.background) {
            efi_log(L"main: loading background image");
            gui_set_background(&gui, config.background);
            if (!gui.background)
                efi_log(L"WARN: background failed to load/decode - using solid colour");
            else
                efi_log(L"main: background loaded");
            gui_apply_accent(&gui);
        }

        if (!text_mode && !gui_closed && gui.logo_mode != LOGO_MODE_OFF && !gui.logo) {
            efi_log(L"main: loading logo image");
            gui_set_logo(&gui, config.logo);
        }

        efi_log(text_mode || gui_closed ? L"main: entering text menu loop" : L"main: entering menu loop");
        if (config.loader_vars) loader_mark_menu();
        selected = (text_mode || gui_closed) ? text_menu_run(&gui) : gui_run(&gui);
        action = gui.action;
        force_menu = 0;
        menu_sound_stop();
        efi_log(L"main: menu closed");
        /* The menu painted over the splash frame: from here on the normal
         * fade-out is the right handover again. */
        splash_shown = 0;
    }
    retry_selected = 0;

    if (action == VISOR_ACTION_RESCUE) {
        efi_log(L"main: options/rescue console requested from the menu");
        if (!text_mode && !gui_closed) {
            gui_fade_out(&gui);
            gui_shutdown(&gui);
            gui_closed = 1;
            text_mode = 1;
        }
        efi_fs_drivers_set_lazy(1);
        action = text_recovery_run(&gui, NULL, EFI_SUCCESS, config.quiet);
        if (action == VISOR_ACTION_RETRY) {
            boot_entry_t *pick = main_entry_at(&gui, gui.selected);
            if (pick) {
                selected = pick;
                retry_selected = 1;
                goto boot_selected;
            }
            action = VISOR_ACTION_MENU;
        }
        if (action == VISOR_ACTION_MENU || action == VISOR_ACTION_BOOT) {
            force_menu = 1;
            goto select_entry;
        }
    }

    if (action == VISOR_ACTION_SHUTDOWN) {
        if (!text_mode) {
            gui_fade_out(&gui);
            gui_shutdown(&gui);
        }
        efi_log(L"action: shutdown requested - see you in the next loop");
        RT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }
    if (action == VISOR_ACTION_REBOOT) {
        if (!text_mode) {
            gui_fade_out(&gui);
            gui_shutdown(&gui);
        }
        efi_log(L"action: reboot requested - Return by Death");
        rbd_arm_on_reboot(1);
        RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }
    if (action == VISOR_ACTION_FIRMWARE) {
        if (!text_mode) {
            gui_fade_out(&gui);
            gui_shutdown(&gui);
        }
        efi_log(L"action: firmware setup requested - setting OsIndications + reboot");

        UINT64 osind = 0;
        UINTN  sz = sizeof(osind);
        UINT32 attr = 0;
        RT->GetVariable(L"OsIndications", &gEfiGlobalVariableGuid, &attr, &sz, &osind);
        osind |= 0x0000000000000001ULL;
        EFI_STATUS sv = RT->SetVariable(L"OsIndications", &gEfiGlobalVariableGuid,
                        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                        EFI_VARIABLE_RUNTIME_ACCESS,
                        sizeof(osind), &osind);
        if (EFI_ERROR(sv)) {
            efi_log(L"WARN: could not set OsIndications - firmware may not enter setup");
            efi_print(L"Could not request firmware setup (continuing reboot)\r\n");
        }
        RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }

    if (!selected) {
        efi_log(L"main: no entry selected, returning to firmware");
        efi_print(L"No selection made\r\n");
        if (!text_mode && !gui_closed) {
            gui_fade_out(&gui);
            gui_shutdown(&gui);
        }
        config_free(&config);
        return EFI_SUCCESS;
    }

boot_selected:

    efi_fs_drivers_set_lazy(1);
    saved_cmdline = NULL;
    saved_kernel_path = NULL;
    saved_initrd_path = NULL;
    saved_uuid = NULL;
    saved_hp_volume = NULL;
    override_cmdline = NULL;
    override_kernel_path = NULL;
    override_initrd_path = NULL;
    override_uuid = NULL;
    cmdline_overridden = 0;
    kernel_overridden = 0;
    initrd_overridden = 0;
    uuid_overridden = 0;
    hp_overridden = 0;
    cleanup_status = EFI_SUCCESS;

    efi_log(selected->name);
    efi_log(L"main: booting entry (auto-detecting boot method)");

    if (gui.override_cmdline) {
        saved_cmdline = selected->cmdline;
        override_cmdline = gui.override_cmdline;
        selected->cmdline = gui.override_cmdline;
        gui.override_cmdline = NULL;
        cmdline_overridden = 1;
        efi_log(L"main: using cmdline edited at the menu (one-shot)");
    }
    if (gui.override_kernel_path) {
        saved_kernel_path = selected->kernel_path;
        override_kernel_path = gui.override_kernel_path;
        selected->kernel_path = gui.override_kernel_path;
        gui.override_kernel_path = NULL;
        kernel_overridden = 1;
        efi_log(L"main: using recovery kernel path override (one-shot)");
    }
    if (gui.override_initrd_set) {
        saved_initrd_path = selected->initrd_path;
        override_initrd_path = gui.override_initrd_path;
        selected->initrd_path = gui.override_initrd_path;
        gui.override_initrd_path = NULL;
        gui.override_initrd_set = 0;
        initrd_overridden = 1;
        efi_log(L"main: using recovery initrd path override (one-shot)");
    }
    if (gui.override_uuid) {
        saved_uuid = selected->uuid;
        override_uuid = gui.override_uuid;
        selected->uuid = gui.override_uuid;
        gui.override_uuid = NULL;
        uuid_overridden = 1;
        efi_log(L"main: using browse volume override (one-shot)");
    }
    if (gui.override_volume) {
        saved_hp_volume = selected->hp_volume;
        selected->hp_volume = gui.override_volume;
        gui.override_volume = NULL;
        hp_overridden = 1;
        efi_log(L"main: pinning boot to the browsed volume (one-shot)");
    }

    if (selected->encrypted || selected->initrd_encrypted || selected->luks) {
        CHAR16 *pw = NULL;
        int splash_hides_prompt =
            selected->luks && !selected->luks_verbose &&
            (visor_cmdline_has_word(selected->cmdline, L"quiet") ||
             visor_cmdline_has_word(selected->cmdline, L"splash"));
        CHAR16 *prompt = selected->luks
            ? L"LUKS passphrase"
            : L"Password";
        CHAR16 *hint = splash_hides_prompt
            ? L"Visor cannot check the passphrase - if it is wrong, splash hides "
              L"the initramfs retry prompt (luks_verbose=1)"
            : L"Checked by the initramfs, not by Visor - F2 to verify your layout";
        if (splash_hides_prompt)
            efi_log(L"WARN: luks=1 entry uses quiet/splash - see luks_verbose in boot.conf");

        if (selected->decrypt_password) {
            volatile CHAR16 *p = (volatile CHAR16*)selected->decrypt_password;
            while (*p) *p++ = 0;
            efi_free_pool(selected->decrypt_password);
            selected->decrypt_password = NULL;
        }

        int confirm = selected->luks_confirm;
        for (int attempt = 0; attempt < 3; attempt++) {
            CHAR16 *again = NULL;

            if (!text_mode && !gui_closed) {
                EFI_STATUS ps = gui_prompt_password(&gui, prompt, hint, &pw);
                splash_covered = 1;
                if (EFI_ERROR(ps)) {
                    efi_log(L"main: encrypted/LUKS boot cancelled at password prompt");
                    efi_print(L"Encrypted boot cancelled\r\n");
                    goto boot_cleanup_return;
                }
                if (confirm) {
                    ps = gui_prompt_password(&gui,
                            L"Repeat passphrase",
                            L"Typed twice because Visor cannot verify it before handing over",
                            &again);
                    if (EFI_ERROR(ps)) {
                        wipe_password(&pw);
                        efi_log(L"main: encrypted/LUKS boot cancelled at confirmation prompt");
                        efi_print(L"Encrypted boot cancelled\r\n");
                        goto boot_cleanup_return;
                    }
                }
            } else {
                /* A prompt is drawn: the firmware screen has to go first, or
                 * the password is typed over the vendor logo with the cursor
                 * still hidden and no cleared line to type on. */
                if (keep_firmware_screen) {
                    console_takeover();
                    keep_firmware_screen = 0;
                }
                pw = text_prompt_password(L"Password: ");
                if (!pw) {
                    efi_print(L"\r\nEncrypted boot cancelled\r\n");
                    efi_log(L"main: encrypted/LUKS boot cancelled at password prompt");
                    visor_quiet = 0;
                    efi_log_set_console(0);
                    goto boot_cleanup_return;
                }
                if (confirm) {
                    again = text_prompt_password(L"Repeat passphrase: ");
                    if (!again) {
                        wipe_password(&pw);
                        efi_print(L"\r\nEncrypted boot cancelled\r\n");
                        efi_log(L"main: encrypted/LUKS boot cancelled at confirmation prompt");
                        visor_quiet = 0;
                        efi_log_set_console(0);
                        goto boot_cleanup_return;
                    }
                }
            }

            if (pw && !pw[0]) {
                wipe_password(&pw);
                wipe_password(&again);
                efi_log(L"WARN: empty passphrase entered - reprompting");
                prompt = selected->luks
                    ? L"Passphrase was empty - LUKS Password   (Esc = cancel)"
                    : L"Passphrase was empty - Password   (Esc = cancel)";
                continue;
            }

            if (!confirm) break;

            if (pw && again && mem_equal(pw, again, (efi_strlen16(pw) + 1) * sizeof(CHAR16))) {
                wipe_password(&again);
                break;
            }

            wipe_password(&again);
            wipe_password(&pw);
            efi_log(L"WARN: passphrase confirmation did not match - reprompting");
            prompt = selected->luks
                ? L"Passphrases did not match - LUKS Password   (Esc = cancel)"
                : L"Passphrases did not match - Password   (Esc = cancel)";
        }

        if (!pw) {
            efi_log(L"ERROR: no usable passphrase captured after 3 attempts");
            efi_print(L"Passphrase confirmation failed\r\n");
            goto boot_cleanup_return;
        }

        selected->decrypt_password = pw;
        efi_log(L"main: password captured for encrypted/LUKS boot entry");
    }

    /* Last chance to catch the autoboot key: the splash and the self-heal
     * work above gave the user a window after the first poll. */
    if (!gui_closed && autobooted && autoboot_menu_requested(&config)) {
        efi_log(L"main: autoboot key pressed during boot - opening the menu");
        main_gui_ensure(&gui, &config, &text_mode, &gui_started, &keep_firmware_screen);
        force_menu = 1;
        autobooted = 0;
        splash_shown = 0;
        /* The entry that was about to boot may already hold a captured
         * passphrase.  The menu is about to pick a different entry, and the
         * cleanup path only visits the one that is finally booted, so wipe it
         * here instead of leaving the secret in memory. */
        if (selected) wipe_password(&selected->decrypt_password);
        goto select_entry;
    }

    if (!text_mode && !gui_closed && !splash_shown) {
        gui_fade_out(&gui);
        gui_shutdown(&gui);
        gui_closed = 1;
        text_mode = 1;
    }

    /* With the splash up, the fade-out, the mode restore and the console
     * clear are exactly the black flash that makes one boot look like two.
     * Leave the frame alone: the kernel paints over it, and the distro's own
     * splash continues from an image that already matches it.  The console
     * stays quiet too - text over a splash is worse than no text. */
    if (splash_shown && !gui_closed) {
        /* A passphrase panel may have been drawn over the splash; repaint so
         * the frame handed to the kernel is the splash again. */
        if (splash_covered) {
            if (gui_draw_splash(&gui))
                efi_log(L"main: splash repainted after the passphrase prompt");
            splash_covered = 0;
        }
        efi_log(L"main: splash retained - handing the framebuffer over as-is");
        gui_closed = 1;
    }

    /* Nothing was drawn, so there is nothing to undo and nothing that should
     * paint over the firmware's own screen: clear it and the user sees black
     * where the vendor logo used to be. */
    if (keep_firmware_screen && !gui_closed) {
        efi_log(L"main: fastboot - leaving the firmware screen in place");
        gui_closed = 1;
    }

    visor_quiet = config.quiet;
    if (visor_quiet) {
        efi_log(L"main: quiet mode - suppressing console output");
        if (!splash_shown && !keep_firmware_screen) ST->ConOut->ClearScreen(ST->ConOut);
    }
    if (splash_shown || keep_firmware_screen) visor_quiet = 1;
    efi_log_set_console(!visor_quiet);
    efi_log(L"main: boot log streaming to console is active");
    efi_log(selected->name);
    efi_log(L"main: final boot handoff sequence started");

    if (bls_decremented != selected) {
        bls_decrement(selected);
        bls_decremented = selected;
    }

    if (config.remember_last)
        efi_set_var_str(L"VisorLastEntry", selected->name);

    if (config.loader_vars)
        loader_mark_selected(selected);

    if (!splash_shown && !keep_firmware_screen) {
        efi_print(L"Booting: ");
        efi_print(selected->name);
        efi_print(L"\r\n");
    }

    /* Last chance: visor_boot() may never come back, and a recording that is
     * still open past ExitBootServices is a truncated file. Idempotent, so the
     * usual close in gui_shutdown() stays the normal path. */
    cap_session_finish();

    status = visor_boot(selected, ST);

    if (!visor_boot_services_active) {
        RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        while (1);
    }

    if (cmdline_overridden) {
        selected->cmdline = saved_cmdline;
        if (override_cmdline) efi_free_pool(override_cmdline);
        saved_cmdline = NULL;
        override_cmdline = NULL;
        cmdline_overridden = 0;
    }
    if (kernel_overridden) {
        selected->kernel_path = saved_kernel_path;
        if (override_kernel_path) efi_free_pool(override_kernel_path);
        saved_kernel_path = NULL;
        override_kernel_path = NULL;
        kernel_overridden = 0;
    }
    if (initrd_overridden) {
        selected->initrd_path = saved_initrd_path;
        if (override_initrd_path) efi_free_pool(override_initrd_path);
        saved_initrd_path = NULL;
        override_initrd_path = NULL;
        initrd_overridden = 0;
    }
    if (uuid_overridden) {
        selected->uuid = saved_uuid;
        if (override_uuid) efi_free_pool(override_uuid);
        saved_uuid = NULL;
        override_uuid = NULL;
        uuid_overridden = 0;
    }
    if (hp_overridden) {
        selected->hp_volume = saved_hp_volume;
        saved_hp_volume = NULL;
        hp_overridden = 0;
    }

    efi_log(L"ERROR: boot returned (control should have transferred to the OS)");
    efi_log_set_console(0);
    visor_quiet = 0;

    efi_print(L"Boot failed - entering recovery\r\n");
    efi_fs_drivers_set_lazy(1);

    action = text_recovery_run(&gui, selected, status, config.quiet);

    if (action == VISOR_ACTION_RETRY) {
        efi_log(L"recovery: retry requested");
        boot_entry_t *retry_entry = main_entry_at(&gui, gui.selected);
        if (retry_entry) selected = retry_entry;
        retry_selected = 1;
        goto boot_selected;
    }
    if (action == VISOR_ACTION_MENU) {
        efi_log(L"recovery: returning to boot menu");
        force_menu = 1;
        retry_selected = 0;
        goto select_entry;
    }
    if (action == VISOR_ACTION_SHUTDOWN) {
        efi_log(L"recovery: shutdown requested");
        config_free(&config);
        RT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }
    if (action == VISOR_ACTION_FIRMWARE) {
        efi_log(L"recovery: firmware setup requested");
        UINT64 osind = 0;
        UINTN  sz = sizeof(osind);
        UINT32 attr = 0;
        RT->GetVariable(L"OsIndications", &gEfiGlobalVariableGuid, &attr, &sz, &osind);
        osind |= 0x0000000000000001ULL;
        RT->SetVariable(L"OsIndications", &gEfiGlobalVariableGuid,
                        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                        EFI_VARIABLE_RUNTIME_ACCESS,
                        sizeof(osind), &osind);
    }

    efi_log(L"recovery: reboot requested");
    config_free(&config);
    RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);

    return status;

boot_cleanup_return:
    if (cmdline_overridden) {
        selected->cmdline = saved_cmdline;
        if (override_cmdline) efi_free_pool(override_cmdline);
    }
    if (kernel_overridden) {
        selected->kernel_path = saved_kernel_path;
        if (override_kernel_path) efi_free_pool(override_kernel_path);
    }
    if (initrd_overridden) {
        selected->initrd_path = saved_initrd_path;
        if (override_initrd_path) efi_free_pool(override_initrd_path);
    }
    if (!text_mode && !gui_closed) {
        gui_fade_out(&gui);
        gui_shutdown(&gui);
    }
    config_free(&config);
    return cleanup_status;
}

void _exit(int status) {
    (void)status;
    ST->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    while (1);
}
