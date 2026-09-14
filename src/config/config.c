/* config.c - boot.conf parsing entry point */
#include "config_internal.h"

int config_early_file_log_enabled(void) {
    int previous = efi_log_file_enabled();
    efi_log_set_file(0);
    CHAR16 *buf = read_text_file(CONFIG_FILE);
    efi_log_set_file(previous);
    if (!buf) return 1;

    int enabled = 1;
    int in_entry = 0;
    CHAR16 *start = buf;
    while (*start) {
        CHAR16 *end = start;
        while (*end && *end != '\n') end++;
        int had_nl = (*end == '\n');
        if (had_nl) *end = '\0';

        CHAR16 *cr = efi_strchr(start, '\r');
        if (cr) *cr = '\0';
        CHAR16 *line = trim(start);

        if (in_entry) {
            if (line[0] == '}') in_entry = 0;
        } else if (is_header(line, L"entry") ||
                   is_header(line, L"linux") ||
                   is_header(line, L"windows")) {
            in_entry = 1;
        } else if (line[0] != '#' && line[0] != '\0') {
            CHAR16 *eq = efi_strchr(line, '=');
            if (eq) {
                *eq = '\0';
                CHAR16 *key = trim(line);
                strip_inline_comment(eq + 1);
                CHAR16 *value = trim(eq + 1);
                int truthy = (*value == '1' || *value == 't' || *value == 'y');
                if (efi_strcmp(key, L"log") == 0 ||
                    efi_strcmp(key, L"boot_log") == 0 ||
                    efi_strcmp(key, L"file_log") == 0)
                    enabled = truthy;
                else if (efi_strcmp(key, L"no_log") == 0)
                    enabled = !truthy;
            }
        }

        if (!had_nl) break;
        start = end + 1;
    }

    efi_free_pool(buf);
    return enabled;
}

EFI_STATUS config_parse(config_t *config) {

    config->timeout = 5;
    config->timeout_set = 0;
    config->default_entry = 0;
    config->quiet = 0;
    config->text_menu = 0;
    config->res_w = 0;
    config->res_h = 0;
    config->res_max = 0;
    config->def_cmdline = NULL;
    config->show_names = 1;
    config->center_info = 0;
    config->box_radius = 0;
    config->remember_last = 0;
    config->selfheal = 1;
    config->selfheal_order = NVSH_ORDER_ENSURE;
    config->restore_fallback = 1;
    config->recovery_entries = 0;
    config->snapshots_mode = 1;
    config->autoboot = 0;
    config->mouse = 1;
    config->pointer_speed = 4;
    config->file_log = 1;
    config->scan_existing = -1;
    config->scan_mode = SCAN_MODE_DEEP;
    config->entries_from_config = 0;
    config->editor = 1;
    config->theme = NULL;
    config->title = NULL;
    config->no_title = 0;
    config->logo = NULL;
    config->no_logo = 0;
    config->logo_mode = LOGO_MODE_TITLE;
    config->logo_size = 0;
    config->logo_gap = 0;
    config->accent_logo = 0;
    config->has_accent_logo = 0;
    config->sp_logo.mode = SPEC_UNSET;
    config->sp_underline.mode = SPEC_UNSET;
    config->sp_highlight.mode = SPEC_UNSET;
    config->sp_title.mode = SPEC_UNSET;
    config->sp_name.mode = SPEC_UNSET;
    config->sp_info.mode = SPEC_UNSET;
    config->sp_shutdown.mode = SPEC_UNSET;
    config->sp_reboot.mode = SPEC_UNSET;
    config->sp_firmware.mode = SPEC_UNSET;
    config->sp_os_icons.mode = SPEC_UNSET;
    config->sp_blur.mode = SPEC_UNSET;
    config->sp_bg.mode = SPEC_UNSET;
    config->sp_g_text.mode = SPEC_UNSET;
    config->sp_g_icons.mode = SPEC_UNSET;
    config->sp_g_underline.mode = SPEC_UNSET;
    config->sp_all.mode = SPEC_UNSET;
    config->font = NULL;
    config->background = NULL;
    config->bg_color = (color_t){0x1a, 0x1a, 0x2e};
    config->fg_color = COLOR_WHITE;
    config->highlight_color = COLOR_BLUE;
    config->title_color = COLOR_WHITE;
    config->name_color = COLOR_WHITE;
    config->title_size = 0;
    config->name_size = 0;
    config->icon_size = 0;
    config->icon_spacing = 0;
    config->icon_y = 0;
    config->underline_color = COLOR_BLUE;
    config->has_underline_color = 0;
    config->underline_thickness = 0;
    config->underline_length = 0;
    config->power_position = POWER_POS_BOTTOMRIGHT;
    config->shutdown_color = COLOR_BLUE;
    config->reboot_color = COLOR_BLUE;
    config->firmware_color = COLOR_BLUE;
    config->has_shutdown_color = 0;
    config->has_reboot_color = 0;
    config->has_firmware_color = 0;
    config->blur = 0;
    config->blur_title = 0;
    config->blur_color = COLOR_WHITE;
    config->has_blur_color = 0;
    config->accent_enabled = 0;
    config->accent_icons = 1;
    config->accent_underline = 1;
    config->accent_text = 0;
    config->accent_os_icons = 0;
    config->accent_variant = 0;
    config->show_clock = 0;
    config->accent_clock = 1;
    config->clock_color = COLOR_WHITE;
    config->has_clock_color = 0;
    config->clock_size = 0;
    config->clock_24h = 1;
    config->clock_seconds = 0;
    config->clock_position = CLOCK_POS_TOPRIGHT;
    config->clock_date = 0;
    config->clock_date_format = CLOCK_DATE_LONG;
    config->clock_blur = 0;
    config->clock_shadow = 1;
    config->screensaver = 0;
    config->screensaver_delay = 60;
    config->screensaver_blank = 600;
    config->screensaver_clock = 1;
    config->record_seconds = 3;
    config->menu_sound_on = 1;
    config->menu_sound = NULL;
    config->tpm = 1;
    config->tpm_pcr_config = TPM_PCR_CONFIG_DEFAULT;
    config->tpm_pcr_cmdline = TPM_PCR_CMDLINE_DEFAULT;
    config->loader_vars = 1;
    config->animation = 1;
    config->anim_speed = 0;
    config->fade_speed = 0;
    config->entries_per_page = 0;
    config->hotplug = 1;
    config->power_icons = 0;
    config->power_icon_size = 0;
    config->shutdown_icon = NULL;
    config->reboot_icon = NULL;
    config->firmware_icon = NULL;
    config->entries = NULL;
    config->tail = NULL;
    config->entry_count = 0;

    CHAR16 *buf = read_text_file(CONFIG_FILE);
    if (!buf) {
        efi_log(L"config: boot.conf not found, auto-detecting boot entries");
        EFI_STATUS st = detect_entries(config);
        snapshots_apply(config);
        return st;
    }

    UINTN max_lines = 1;
    for (CHAR16 *p = buf; *p; p++)
        if (*p == '\n') max_lines++;

    CHAR16 **lines = efi_allocate_pool(max_lines * sizeof(CHAR16 *));
    if (!lines) {
        efi_free_pool(buf);
        efi_log(L"ERROR: out of memory splitting config - auto-detecting");
        EFI_STATUS st = detect_entries(config);
        snapshots_apply(config);
        return st;
    }

    UINTN line_count = 0;
    CHAR16 *start = buf;

    while (*start && line_count < max_lines) {
        CHAR16 *end = start;
        while (*end && *end != '\n') end++;
        int had_nl = (*end == '\n');
        if (had_nl) *end = '\0';

        CHAR16 *cr = efi_strchr(start, '\r');
        if (cr) *cr = '\0';

        lines[line_count++] = start;
        if (!had_nl) break;
        start = end + 1;
    }

    UINTN entry_count_before_parse = config->entry_count;

    for (UINTN i = 0; i < line_count; i++) {
        CHAR16 *line = trim(lines[i]);

        if (line[0] == '#' || line[0] == '\0') continue;

        CHAR16 *eq = efi_strchr(line, '=');
        if (eq) {
            *eq = '\0';
            CHAR16 *key = trim(line);
            strip_inline_comment(eq + 1);
            CHAR16 *value = trim(eq + 1);
            apply_global(config, key, value);
            continue;
        }

        if (line[0] == 'e' || line[0] == 'l' || line[0] == 'w') {
            int win_hint = is_header(line, L"windows");
            if (is_header(line, L"entry") ||
                is_header(line, L"linux") ||
                win_hint) {
                efi_log(L"config: calling parse_entry");
                parse_entry(config, lines, &i, line_count, win_hint);
            }
        }
    }

    if (config->theme) {
        if (efi_strcmp(config->theme, L"random") == 0 ||
            efi_strcmp(config->theme, L"cycle") == 0) {
            int cycle = (efi_strcmp(config->theme, L"cycle") == 0);
            CHAR16 *chosen = resolve_rotating_theme(cycle);
            efi_free_pool(config->theme);
            config->theme = chosen;
        }
        if (config->theme) apply_theme(config, config->theme);
    }

    efi_free_pool(lines);
    efi_free_pool(buf);

    if (config->entry_count == entry_count_before_parse) {
        detect_entries(config);
    } else {
        config->entries_from_config = 1;
        efi_log(L"config: boot.conf supplied the entries - skipping volume scan");
    }

    snapshots_apply(config);

    if (config->recovery_entries) add_recovery_entries(config);

    return EFI_SUCCESS;
}
