
#include <efi.h>
#include <efilib.h>
#include "gui.h"
#include "config.h"
#include "efi_helpers.h"
#include "linux_boot.h"
#include "windows_boot.h"

EFI_HANDLE IH;

/* natsukisubaruwashere */

EFI_STATUS efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
    InitializeLib(image_handle, system_table);

    ST = system_table;
    BS = system_table->BootServices;
    RT = system_table->RuntimeServices;
    IH = image_handle;

    ST->ConOut->ClearScreen(ST->ConOut);
    ST->ConOut->EnableCursor(ST->ConOut, FALSE);

    efi_log_begin();
    efi_log(L"main: efi_main entered, services initialised");

    efi_print(L"Visor loading...\r\n");

    efi_log(L"main: initialising GUI (locating GOP, allocating back buffer)");
    gui_state_t gui;
    EFI_STATUS status = gui_init(&gui);
    if (EFI_ERROR(status)) {
        efi_log(L"ERROR: gui_init failed - no Graphics Output Protocol or out of memory");
        efi_print(L"GUI initialization failed\r\n");
        return status;
    }
    efi_log(L"main: GUI ready");

    efi_log(L"main: parsing config \\EFI\\visor\\boot.conf");
    config_t config;
    status = config_parse(&config);
    if (EFI_ERROR(status)) {
        efi_log(L"WARN: config_parse returned error - using auto-detected entries");
        efi_print(L"Warning: Config parse failed, using auto-detect\r\n");
    }
    { CHAR16 d[80]; SPrint(d, sizeof(d), L"main: config parsed, %d entr%s",
        (int)config.entry_count, config.entry_count == 1 ? L"y" : L"ies"); efi_log(d); }
    if (config.entry_count == 0)
        efi_log(L"WARN: no boot entries found in config or auto-detect");

    gui.entries         = config.entries;
    gui.entry_count     = config.entry_count;
    gui.timeout         = config.timeout;
    gui.bg_color        = config.bg_color;
    gui.fg_color        = config.fg_color;
    gui.highlight_color = config.highlight_color;
    gui.title           = config.title;
    gui.show_title      = !config.no_title;
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
    gui.anim_speed      = config.anim_speed;
    {
        int sp = config.anim_speed;
        if (sp < 1) sp = 8;
        if (sp > 10) sp = 10;
        gui.anim_frames = 26 - sp * 2;
        if (gui.anim_frames < 5) gui.anim_frames = 5;
    }
    if (config.power_icons) {
        if (config.shutdown_icon) gui.shutdown_icon = gui_load_icon(config.shutdown_icon);
        if (config.reboot_icon)   gui.reboot_icon   = gui_load_icon(config.reboot_icon);
        if (config.firmware_icon) gui.firmware_icon = gui_load_icon(config.firmware_icon);
        if ((config.shutdown_icon && !gui.shutdown_icon) ||
            (config.reboot_icon   && !gui.reboot_icon)   ||
            (config.firmware_icon && !gui.firmware_icon))
            efi_log(L"WARN: a power icon failed to load - falling back to text for it");
    }

    if (config.font) {
        char name[32]; UINTN i = 0;
        for (; config.font[i] && i < sizeof(name) - 1; i++) name[i] = (char)config.font[i];
        name[i] = '\0';
        gui_set_font(name);
    }

    if (config.default_entry < gui.entry_count) {
        gui.selected = config.default_entry;
    }

    if (config.background) {
        efi_log(L"main: loading background image");
        gui_set_background(&gui, config.background);
        if (!gui.background)
            efi_log(L"WARN: background failed to load/decode - using solid colour");
        else
            efi_log(L"main: background loaded");
    }

    efi_log(L"main: entering menu loop");
    boot_entry_t *selected = gui_run(&gui);
    int action = gui.action;
    efi_log(L"main: menu closed");
    gui_shutdown(&gui);

    if (action == VISOR_ACTION_SHUTDOWN) {
        efi_log(L"action: shutdown requested - ResetSystem(Shutdown)");
        RT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }
    if (action == VISOR_ACTION_REBOOT) {
        efi_log(L"action: reboot requested - ResetSystem(Cold)");
        RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }
    if (action == VISOR_ACTION_FIRMWARE) {
        efi_log(L"action: firmware setup requested - setting OsIndications + reboot");

        UINT64 osind = 0;
        UINTN  sz = sizeof(osind);
        UINT32 attr;
        RT->GetVariable(L"OsIndications", &gEfiGlobalVariableGuid, &attr, &sz, &osind);
        osind |= 0x0000000000000001ULL;
        RT->SetVariable(L"OsIndications", &gEfiGlobalVariableGuid,
                        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                        EFI_VARIABLE_RUNTIME_ACCESS,
                        sizeof(osind), &osind);
        RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
        return EFI_SUCCESS;
    }

    if (!selected) {
        efi_log(L"main: no entry selected, returning to firmware");
        efi_print(L"No selection made\r\n");
        config_free(&config);
        return EFI_SUCCESS;
    }

    efi_log(selected->name);
    efi_log(selected->type == 1 ? L"main: booting Windows entry"
                                : L"main: booting Linux entry");

    visor_quiet = config.quiet;
    if (visor_quiet) {
        efi_log(L"main: quiet mode - suppressing console output");
        ST->ConOut->ClearScreen(ST->ConOut);
    }

    efi_print(L"Booting: ");
    efi_print(selected->name);
    efi_print(L"\r\n");

    if (selected->type == 1) {
        status = windows_boot(selected, ST);
    } else {
        status = linux_boot(selected, ST);
    }

    efi_log(L"ERROR: boot returned (control should have transferred to the OS)");

    efi_print(L"Boot failed, resetting...\r\n");

    BS->Stall(3 * 1000 * 1000);

    RT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);

    return status;
}

void _exit(int status) {
    (void)status;
    ST->RuntimeServices->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    while (1);
}
