#include <efi.h>
#include <efilib.h>
#include "gui.h"
#include "config.h"
#include "efi_helpers.h"
#include "linux_boot.h"
#include "windows_boot.h"
#include "text_menu.h"

EFI_HANDLE IH;

static boot_entry_t* main_entry_at(gui_state_t *state, UINTN idx) {
    boot_entry_t *e = state ? state->entries : NULL;
    for (UINTN i = 0; i < idx && e; i++) e = e->next;
    return e;
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

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    InitializeLib(image_handle, system_table);

    ST = system_table;
    BS = system_table->BootServices;
    RT = system_table->RuntimeServices;
    IH = image_handle;

    efi_print(L"Visor loading...\r\n");

    efi_log_set_file(0);
    int early_file_log = config_early_file_log_enabled();
    efi_log_set_file(early_file_log);
    if (early_file_log) efi_log_begin();
    else efi_print(L"Visor: boot logging disabled by config\r\n");
    efi_log(L"main: efi_main entered, services initialised");

    ST->ConOut->ClearScreen(ST->ConOut);
    ST->ConOut->EnableCursor(ST->ConOut, FALSE);

    efi_log(L"main: initialising GUI (locating GOP, allocating back buffer)");
    gui_state_t gui = {0};
    int text_mode = 0;
    EFI_STATUS status = gui_init(&gui);
    if (EFI_ERROR(status)) {
        efi_log(L"WARN: gui_init failed (no GOP or out of memory) - falling back to text mode");
        efi_print(L"Graphics unavailable - using text menu\r\n");
        text_mode = 1;
        gui.selected = 0;
        gui.timeout_active = 1;
    } else {
        efi_log(L"main: GUI ready");
    }

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
        efi_log(L"WARN: no boot entries found in config or auto-detect");

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
        gui.hotplug_poll = hotplug_poll_cb;
        gui.hotplug_ctx  = &config;
        efi_log(L"main: hotplug volume watch armed");
    }
    gui.entries         = config.entries;
    gui.entry_count     = config.entry_count;
    gui.per_page        = config.entries_per_page ? config.entries_per_page : 3;
    gui.timeout         = config.timeout;
    gui.bg_color        = config.bg_color;
    gui.fg_color        = config.fg_color;
    gui.highlight_color = config.highlight_color;
    gui.title           = config.title;
    gui.show_title      = !config.no_title;
    gui.logo_mode       = config.no_logo ? LOGO_MODE_OFF : config.logo_mode;
    gui.logo_size       = config.logo_size;
    gui.logo_gap        = config.logo_gap;
    gui.accent_logo     = config.has_accent_logo ? config.accent_logo
                                                 : config.accent_text;
    gui.sp_logo         = config.sp_logo;
    gui.sp_underline    = config.sp_underline;
    gui.sp_highlight    = config.sp_highlight;
    gui.sp_title        = config.sp_title;
    gui.sp_name         = config.sp_name;
    gui.sp_info         = config.sp_info;
    gui.sp_shutdown     = config.sp_shutdown;
    gui.sp_reboot       = config.sp_reboot;
    gui.sp_firmware     = config.sp_firmware;
    gui.sp_os_icons     = config.sp_os_icons;
    gui.sp_blur         = config.sp_blur;
    gui.sp_bg           = config.sp_bg;
    gui.sp_g_text       = config.sp_g_text;
    gui.sp_g_icons      = config.sp_g_icons;
    gui.sp_g_underline  = config.sp_g_underline;
    gui.sp_all          = config.sp_all;
    gui.show_names      = config.show_names;
    gui.center_info     = config.center_info;
    gui.box_radius      = config.box_radius;
    gui.title_color     = config.title_color;
    gui.name_color      = config.name_color;
    gui.title_size      = config.title_size;
    gui.name_size       = config.name_size;
    gui.icon_size       = config.icon_size;
    gui.icon_spacing    = config.icon_spacing;
    gui.icon_y          = config.icon_y;
    gui.underline_thickness = config.underline_thickness;
    gui.underline_length    = config.underline_length;
    gui.underline_color = config.has_underline_color ? config.underline_color
                                                     : config.highlight_color;
    gui.power_position  = config.power_position;
    gui.shutdown_color  = config.has_shutdown_color ? config.shutdown_color
                                                    : config.highlight_color;
    gui.reboot_color    = config.has_reboot_color   ? config.reboot_color
                                                    : config.highlight_color;
    gui.firmware_color  = config.has_firmware_color ? config.firmware_color
                                                    : config.highlight_color;

    gui.power_icons     = config.power_icons;
    gui.power_icon_size = config.power_icon_size;
    gui.blur            = config.blur;
    gui.blur_title      = config.blur_title;
    gui.blur_color      = config.has_blur_color ? config.blur_color : COLOR_WHITE;
    gui.blur_color_set  = config.has_blur_color;
    gui.accent_enabled   = config.accent_enabled;
    gui.accent_icons     = config.accent_icons;
    gui.accent_underline = config.accent_underline;
    gui.accent_text      = config.accent_text;
    gui.accent_os_icons  = config.accent_os_icons;
    gui.accent_variant   = config.accent_variant;
    gui.animation       = config.animation;
    gui.anim_speed      = config.anim_speed;
    gui.fade_speed      = config.fade_speed;
    {
        int sp = config.anim_speed;
        if (sp < 1) sp = 8;
        if (sp > 10) sp = 10;
        gui.anim_frames = gui.animation ? 26 - sp * 2 : 1;
        if (gui.animation && gui.anim_frames < 5) gui.anim_frames = 5;
    }
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

    gui.editor_enabled = config.editor;
    gui.mouse_enabled  = config.mouse;
    gui.pointer_speed  = config.pointer_speed;

    if (config.remember_last) {
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

    boot_entry_t *selected = NULL;
    int action = VISOR_ACTION_BOOT;
    int autobooted = 0;
    int gui_closed = 0;
    int force_menu = 0;
    int retry_selected = 0;
    boot_entry_t *bls_decremented = NULL;
    CHAR16 *saved_cmdline = NULL;
    CHAR16 *saved_kernel_path = NULL;
    CHAR16 *saved_initrd_path = NULL;
    CHAR16 *override_cmdline = NULL;
    CHAR16 *override_kernel_path = NULL;
    CHAR16 *override_initrd_path = NULL;
    int cmdline_overridden = 0;
    int kernel_overridden = 0;
    int initrd_overridden = 0;
    EFI_STATUS cleanup_status = EFI_SUCCESS;

select_entry:
    selected = NULL;
    action = VISOR_ACTION_BOOT;
    autobooted = 0;

    if (!force_menu && config.autoboot && gui.entry_count >= 1) {
        EFI_INPUT_KEY abk;
        if (EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &abk))) {
            UINTN pick = (gui.entry_count == 1) ? 0
                       : (gui.selected < gui.entry_count ? gui.selected : 0);
            selected = gui.entries;
            for (UINTN i = 0; i < pick && selected; i++) selected = selected->next;
            autobooted = 1;
            efi_log(L"main: autoboot - skipping menu, booting directly");
        } else {
            efi_log(L"main: autoboot armed but a key was pressed - showing menu");
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
        selected = (text_mode || gui_closed) ? text_menu_run(&gui) : gui_run(&gui);
        action = gui.action;
        force_menu = 0;
        efi_log(L"main: menu closed");
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
        efi_log(L"action: shutdown requested - ResetSystem(Shutdown)");
        RT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }
    if (action == VISOR_ACTION_REBOOT) {
        if (!text_mode) {
            gui_fade_out(&gui);
            gui_shutdown(&gui);
        }
        efi_log(L"action: reboot requested - ResetSystem(Cold)");
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
    override_cmdline = NULL;
    override_kernel_path = NULL;
    override_initrd_path = NULL;
    cmdline_overridden = 0;
    kernel_overridden = 0;
    initrd_overridden = 0;
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

    if (selected->encrypted || selected->initrd_encrypted || selected->luks) {
        CHAR16 *pw = NULL;
        int splash_hides_prompt =
            selected->luks && !selected->luks_verbose &&
            (visor_cmdline_has_word(selected->cmdline, L"quiet") ||
             visor_cmdline_has_word(selected->cmdline, L"splash"));
        CHAR16 *prompt = selected->luks
            ? L"LUKS Password   (Enter = boot, Esc = cancel)"
            : L"Password   (Enter = boot, Esc = cancel)";
        CHAR16 *hint = splash_hides_prompt
            ? L"F2 shows what you typed. Visor cannot check the passphrase - if it "
              L"is wrong, splash hides the initramfs retry prompt (luks_verbose=1)"
            : L"F2 shows what you typed - use it to check your keyboard layout";
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
                if (EFI_ERROR(ps)) {
                    efi_log(L"main: encrypted/LUKS boot cancelled at password prompt");
                    efi_print(L"Encrypted boot cancelled\r\n");
                    goto boot_cleanup_return;
                }
                if (confirm) {
                    ps = gui_prompt_password(&gui,
                            L"Repeat passphrase   (Enter = boot, Esc = cancel)",
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

            if (pw && again && efi_strcmp(pw, again) == 0) {
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

    if (!text_mode && !gui_closed) {
        gui_fade_out(&gui);
        gui_shutdown(&gui);
        gui_closed = 1;
        text_mode = 1;
    }

    visor_quiet = config.quiet;
    if (visor_quiet) {
        efi_log(L"main: quiet mode - suppressing console output");
        ST->ConOut->ClearScreen(ST->ConOut);
    }
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

    efi_print(L"Booting: ");
    efi_print(selected->name);
    efi_print(L"\r\n");

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

    efi_log(L"ERROR: boot returned (control should have transferred to the OS)");
    efi_log_set_console(0);

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
