/* config_keys.c - global and theme key application */
#include "config_internal.h"

static int clock_position_from_str(CHAR16 *s) {
    if (!s || !s[0]) return -1;

    static const struct { const CHAR16 *name; int pos; } names[] = {
        { L"topright",     CLOCK_POS_TOPRIGHT },
        { L"top_right",    CLOCK_POS_TOPRIGHT },
        { L"topleft",      CLOCK_POS_TOPLEFT },
        { L"top_left",     CLOCK_POS_TOPLEFT },
        { L"topcenter",    CLOCK_POS_TOPCENTER },
        { L"top_center",   CLOCK_POS_TOPCENTER },
        { L"topcentre",    CLOCK_POS_TOPCENTER },
        { L"top_centre",   CLOCK_POS_TOPCENTER },
        { L"top",          CLOCK_POS_TOPCENTER },
        { L"bottomright",  CLOCK_POS_BOTTOMRIGHT },
        { L"bottom_right", CLOCK_POS_BOTTOMRIGHT },
        { L"bottomleft",   CLOCK_POS_BOTTOMLEFT },
        { L"bottom_left",  CLOCK_POS_BOTTOMLEFT },
        { L"bottomcenter", CLOCK_POS_BOTTOMCENTER },
        { L"bottom_center",CLOCK_POS_BOTTOMCENTER },
        { L"bottomcentre", CLOCK_POS_BOTTOMCENTER },
        { L"bottom_centre",CLOCK_POS_BOTTOMCENTER },
        { L"bottom",       CLOCK_POS_BOTTOMCENTER },
        { L"center",       CLOCK_POS_CENTER },
        { L"centre",       CLOCK_POS_CENTER },
        { L"middle",       CLOCK_POS_CENTER },
        { NULL, 0 }
    };

    for (int i = 0; names[i].name; i++) {
        UINTN j = 0;
        while (s[j] && names[i].name[j]) {
            CHAR16 a = s[j], b = names[i].name[j];
            if (a >= L'A' && a <= L'Z') a = (CHAR16)(a - L'A' + L'a');
            if (a == L'-' || a == L' ') a = L'_';
            if (a != b) break;
            j++;
        }
        if (!s[j] && !names[i].name[j]) return names[i].pos;
    }
    return -1;
}

static int clock_date_from_str(CHAR16 *s) {
    if (!s || !s[0]) return -1;
    if (efi_strcmp(s, L"long") == 0 || efi_strcmp(s, L"full") == 0)
        return CLOCK_DATE_LONG;
    if (efi_strcmp(s, L"iso") == 0 || efi_strcmp(s, L"ymd") == 0)
        return CLOCK_DATE_ISO;
    if (efi_strcmp(s, L"dmy") == 0 || efi_strcmp(s, L"eu") == 0)
        return CLOCK_DATE_DMY;
    if (efi_strcmp(s, L"mdy") == 0 || efi_strcmp(s, L"us") == 0)
        return CLOCK_DATE_MDY;
    if (*s == '1' || *s == 't' || *s == 'y' || *s == 'T' || *s == 'Y' ||
        efi_strcmp(s, L"on") == 0)
        return CLOCK_DATE_LONG;
    if (*s == '0' || *s == 'f' || *s == 'n' || *s == 'F' || *s == 'N' ||
        efi_strcmp(s, L"off") == 0)
        return CLOCK_DATE_OFF;
    return -1;
}

static UINTN parse_pcr(CHAR16 *value, UINTN fallback) {
    if (!value || value[0] < '0' || value[0] > '9') {
        efi_log(L"WARN: PCR index must be a number 0-23 - keeping the default");
        return fallback;
    }
    UINTN v = parse_uint(value);
    if (v > 23) {
        efi_log(L"WARN: PCR index above 23 does not exist - keeping the default");
        return fallback;
    }
    return v;
}

int show_names_set;
int center_info_set;

void apply_global(config_t *config, CHAR16 *key, CHAR16 *value) {
    if (efi_strcmp(key, L"timeout") == 0) {
        config->timeout = (*value == '-') ? -1 : (INTN)parse_uint(value);
        config->timeout_set = 1;
    } else if (efi_strcmp(key, L"default") == 0) {
        config->default_entry = (INTN)parse_uint(value);
    } else if (efi_strcmp(key, L"quiet") == 0) {
        config->quiet = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"text_menu") == 0 || efi_strcmp(key, L"text_mode") == 0) {
        config->text_menu = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"cmdline") == 0 || efi_strcmp(key, L"options") == 0) {
        if (config->def_cmdline) efi_free_pool(config->def_cmdline);
        config->def_cmdline = (value[0] == '\0') ? NULL : efi_strdup(value);
    } else if (efi_strcmp(key, L"show_names") == 0 || efi_strcmp(key, L"names") == 0) {
        config->show_names = (*value == '1' || *value == 't' || *value == 'y');
        show_names_set = 1;
    } else if (efi_strcmp(key, L"center_info") == 0 || efi_strcmp(key, L"centre_info") == 0) {
        config->center_info = (*value == '1' || *value == 't' || *value == 'y');
        center_info_set = 1;
    } else if (efi_strcmp(key, L"autoboot") == 0) {
        config->autoboot = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"remember_last") == 0 || efi_strcmp(key, L"remember") == 0) {
        config->remember_last = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"selfheal") == 0) {
        config->selfheal = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"boot_order") == 0 || efi_strcmp(key, L"selfheal_order") == 0) {
        if (efi_strcmp(value, L"off") == 0 || efi_strcmp(value, L"no") == 0 ||
            *value == '0')
            config->selfheal_order = NVSH_ORDER_OFF;
        else if (efi_strcmp(value, L"ensure") == 0 || efi_strcmp(value, L"readd") == 0 ||
                 efi_strcmp(value, L"repair") == 0)
            config->selfheal_order = NVSH_ORDER_ENSURE;
        else if (efi_strcmp(value, L"first") == 0 || efi_strcmp(value, L"visor_first") == 0 ||
                 efi_strcmp(value, L"on") == 0 || efi_strcmp(value, L"yes") == 0 ||
                 *value == '1')
            config->selfheal_order = NVSH_ORDER_FIRST;
        else {
            CHAR16 d[96];
            SPrint(d, sizeof(d), L"WARN: invalid boot_order '%s' (off/ensure/first)", value);
            efi_log(d);
        }
    } else if (efi_strcmp(key, L"restore_fallback") == 0) {
        config->restore_fallback = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"recovery_entries") == 0 || efi_strcmp(key, L"recovery") == 0) {
        config->recovery_entries = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"snapshots") == 0) {
        if (efi_strcmp(value, L"manifest") == 0) config->snapshots_mode = 2;
        else config->snapshots_mode = (*value == '1' || *value == 't' || *value == 'y') ? 1 : 0;
    } else if (efi_strcmp(key, L"hotplug") == 0) {
        config->hotplug = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"mouse") == 0 || efi_strcmp(key, L"pointer") == 0) {
        config->mouse = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"mouse_speed") == 0 ||
               efi_strcmp(key, L"pointer_speed") == 0) {
        UINTN speed = parse_uint(value);
        if (speed < 1) speed = 1;
        if (speed > 20) speed = 20;
        config->pointer_speed = speed;
    } else if (efi_strcmp(key, L"scan_existing") == 0 ||
               efi_strcmp(key, L"hotplug_scan_existing") == 0) {
        config->scan_existing = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"scan") == 0 ||
               efi_strcmp(key, L"scan_mode") == 0) {
        if (efi_strcmp(value, L"deep") == 0 || efi_strcmp(value, L"deepscan") == 0 ||
            efi_strcmp(value, L"full") == 0)
            config->scan_mode = SCAN_MODE_DEEP;
        else if (efi_strcmp(value, L"quick") == 0 || efi_strcmp(value, L"quickscan") == 0 ||
                 efi_strcmp(value, L"fast") == 0)
            config->scan_mode = SCAN_MODE_QUICK;
        else
            efi_log(L"WARN: invalid scan mode (deep/quick)");
    } else if (efi_strcmp(key, L"deep_scan") == 0 || efi_strcmp(key, L"deepscan") == 0) {
        config->scan_mode = (*value == '1' || *value == 't' || *value == 'y')
                            ? SCAN_MODE_DEEP : SCAN_MODE_QUICK;
    } else if (efi_strcmp(key, L"quick_scan") == 0 || efi_strcmp(key, L"quickscan") == 0) {
        config->scan_mode = (*value == '1' || *value == 't' || *value == 'y')
                            ? SCAN_MODE_QUICK : SCAN_MODE_DEEP;
    } else if (efi_strcmp(key, L"log") == 0 ||
               efi_strcmp(key, L"boot_log") == 0 ||
               efi_strcmp(key, L"file_log") == 0) {
        config->file_log = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"no_log") == 0) {
        config->file_log = !(*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"editor") == 0) {
        config->editor = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"box_radius") == 0 || efi_strcmp(key, L"corner_radius") == 0) {
        config->box_radius = parse_uint(value);
    } else if (efi_strcmp(key, L"resolution") == 0) {
        config->res_w = 0; config->res_h = 0; config->res_max = 0;
        if (efi_strcmp(value, L"max") == 0 || efi_strcmp(value, L"highest") == 0) {
            config->res_max = 1;
        } else if (efi_strcmp(value, L"native") == 0 || value[0] == '\0') {

        } else {
            UINTN w = 0;
            while (*value >= '0' && *value <= '9') { w = w * 10 + (*value - '0'); value++; }
            if (*value == 'x' || *value == 'X' || *value == '*') value++;
            UINTN h = 0;
            while (*value >= '0' && *value <= '9') { h = h * 10 + (*value - '0'); value++; }
            if (w && h) { config->res_w = w; config->res_h = h; }
            else efi_log(L"WARN: invalid resolution (use WxH, e.g. 1920x1080, or max/native)");
        }
    } else if (efi_strcmp(key, L"theme") == 0) {
        if (config->theme) efi_free_pool(config->theme);
        config->theme = (value[0] == '\0') ? NULL : efi_strdup(value);
    } else if (efi_strcmp(key, L"title") == 0) {
        if (config->title) efi_free_pool(config->title);
        if (efi_strcmp(value, L"none") == 0) {
            config->no_title = 1;
            config->title = NULL;
        } else if (value[0] == '\0') {
            config->no_title = 0;
            config->title = NULL;
        } else {
            config->no_title = 0;
            config->title = efi_strdup(value);
        }
    } else if (efi_strcmp(key, L"logo") == 0) {
        if (config->logo) efi_free_pool(config->logo);
        config->logo = NULL;
        if (efi_strcmp(value, L"none") == 0 || efi_strcmp(value, L"off") == 0) {
            config->no_logo = 1;
        } else {
            config->no_logo = 0;
            if (value[0] != '\0' && efi_strcmp(value, L"default") != 0)
                config->logo = dup_path(value);
        }
    } else if (efi_strcmp(key, L"logo_mode") == 0) {
        if (efi_strcmp(value, L"title") == 0 || efi_strcmp(value, L"beside") == 0)
            config->logo_mode = LOGO_MODE_TITLE;
        else if (efi_strcmp(value, L"only") == 0 || efi_strcmp(value, L"standalone") == 0)
            config->logo_mode = LOGO_MODE_ONLY;
        else if (efi_strcmp(value, L"above") == 0 || efi_strcmp(value, L"stacked") == 0)
            config->logo_mode = LOGO_MODE_ABOVE;
        else if (efi_strcmp(value, L"none") == 0 || efi_strcmp(value, L"off") == 0)
            config->logo_mode = LOGO_MODE_OFF;
        else
            efi_log(L"WARN: invalid logo_mode (title/only/above/none)");
    } else if (efi_strcmp(key, L"logo_size") == 0) {
        config->logo_size = parse_uint(value);
    } else if (efi_strcmp(key, L"logo_gap") == 0) {
        config->logo_gap = parse_uint(value);
    } else if (efi_strcmp(key, L"accent_logo") == 0 ||
               efi_strcmp(key, L"logo_color") == 0 ||
               efi_strcmp(key, L"accent_logo_color") == 0) {
        if (parse_spec(value, &config->sp_logo)) {
            config->accent_logo = (config->sp_logo.mode != SPEC_OFF);
            config->has_accent_logo = 1;
        } else {
            efi_log(L"WARN: invalid accent_logo (0/1, an accent role, or #RRGGBB)");
        }
    } else if (efi_strcmp(key, L"font") == 0) {
        if (config->font) efi_free_pool(config->font);
        config->font = (value[0] == '\0') ? NULL : efi_strdup(value);
    } else if (efi_strcmp(key, L"title_color") == 0) {
        if (!parse_spec_color(value, &config->sp_title, &config->title_color))
            efi_log(L"WARN: invalid title_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"name_color") == 0) {
        if (!parse_spec_color(value, &config->sp_name, &config->name_color))
            efi_log(L"WARN: invalid name_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"highlight_color") == 0) {
        if (!parse_spec_color(value, &config->sp_highlight, &config->highlight_color))
            efi_log(L"WARN: invalid highlight_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"title_size") == 0) {
        config->title_size = parse_uint(value);
    } else if (efi_strcmp(key, L"name_size") == 0) {
        config->name_size = parse_uint(value);
    } else if (efi_strcmp(key, L"icon_size") == 0) {
        config->icon_size = parse_uint(value);
    } else if (efi_strcmp(key, L"icon_spacing") == 0) {
        config->icon_spacing = parse_uint(value);
    } else if (efi_strcmp(key, L"icon_y") == 0) {
        config->icon_y = parse_uint(value);
    } else if (efi_strcmp(key, L"underline_color") == 0) {
        if (parse_spec_color(value, &config->sp_underline, &config->underline_color))
            config->has_underline_color = (config->sp_underline.mode == SPEC_COLOR);
        else
            efi_log(L"WARN: invalid underline_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"underline_thickness") == 0) {
        config->underline_thickness = parse_uint(value);
    } else if (efi_strcmp(key, L"underline_length") == 0) {
        config->underline_length = parse_uint(value);
    } else if (efi_strcmp(key, L"power_position") == 0) {
        if (efi_strcmp(value, L"topright") == 0)
            config->power_position = POWER_POS_TOPRIGHT;
        else if (efi_strcmp(value, L"topleft") == 0)
            config->power_position = POWER_POS_TOPLEFT;
        else if (efi_strcmp(value, L"bottomleft") == 0)
            config->power_position = POWER_POS_BOTTOMLEFT;
        else if (efi_strcmp(value, L"bottomright") == 0)
            config->power_position = POWER_POS_BOTTOMRIGHT;
        else
            efi_log(L"WARN: invalid power_position (topright/topleft/bottomleft/bottomright)");
    } else if (efi_strcmp(key, L"shutdown_color") == 0) {
        if (parse_spec_color(value, &config->sp_shutdown, &config->shutdown_color))
            config->has_shutdown_color = (config->sp_shutdown.mode == SPEC_COLOR);
        else
            efi_log(L"WARN: invalid shutdown_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"reboot_color") == 0) {
        if (parse_spec_color(value, &config->sp_reboot, &config->reboot_color))
            config->has_reboot_color = (config->sp_reboot.mode == SPEC_COLOR);
        else
            efi_log(L"WARN: invalid reboot_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"firmware_color") == 0) {
        if (parse_spec_color(value, &config->sp_firmware, &config->firmware_color))
            config->has_firmware_color = (config->sp_firmware.mode == SPEC_COLOR);
        else
            efi_log(L"WARN: invalid firmware_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"background") == 0) {
        if (config->background) efi_free_pool(config->background);
        config->background = dup_path(value);
    } else if (efi_strcmp(key, L"power_icons") == 0) {
        config->power_icons = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"power_icon_size") == 0) {
        config->power_icon_size = parse_uint(value);
    } else if (efi_strcmp(key, L"shutdown_icon") == 0) {
        if (config->shutdown_icon) efi_free_pool(config->shutdown_icon);
        config->shutdown_icon = dup_path(value);
    } else if (efi_strcmp(key, L"reboot_icon") == 0) {
        if (config->reboot_icon) efi_free_pool(config->reboot_icon);
        config->reboot_icon = dup_path(value);
    } else if (efi_strcmp(key, L"firmware_icon") == 0) {
        if (config->firmware_icon) efi_free_pool(config->firmware_icon);
        config->firmware_icon = dup_path(value);
    } else if (efi_strcmp(key, L"blur") == 0) {
        if (*value == 'c' || *value == 'C') config->blur = 2;
        else config->blur = (*value == '1' || *value == 't' || *value == 'y' || *value == 'f') ? 1 : 0;
    } else if (efi_strcmp(key, L"animation") == 0) {
        config->animation = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"anim_speed") == 0) {
        config->anim_speed = (int)parse_uint(value);
    } else if (efi_strcmp(key, L"fade_speed") == 0) {
        config->fade_speed = (int)parse_uint(value);
    } else if (efi_strcmp(key, L"entries_per_page") == 0) {
        config->entries_per_page = parse_uint(value);
    } else if (efi_strcmp(key, L"blur_title") == 0) {
        config->blur_title = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"blur_color") == 0) {
        if (parse_spec_color(value, &config->sp_blur, &config->blur_color))
            config->has_blur_color = (config->sp_blur.mode == SPEC_COLOR);
        else
            efi_log(L"WARN: invalid blur_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent") == 0) {
        if (parse_spec(value, &config->sp_all)) {
            config->accent_enabled = (config->sp_all.mode != SPEC_OFF);
        } else {
            efi_log(L"WARN: invalid accent (0/1, an accent role, or #RRGGBB)");
        }
    } else if (efi_strcmp(key, L"accent_icons") == 0) {
        if (parse_spec(value, &config->sp_g_icons))
            config->accent_icons = (config->sp_g_icons.mode != SPEC_OFF);
        else
            efi_log(L"WARN: invalid accent_icons (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent_underline") == 0) {
        if (parse_spec(value, &config->sp_g_underline))
            config->accent_underline = (config->sp_g_underline.mode != SPEC_OFF);
        else
            efi_log(L"WARN: invalid accent_underline (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent_text") == 0) {
        if (parse_spec(value, &config->sp_g_text))
            config->accent_text = (config->sp_g_text.mode != SPEC_OFF);
        else
            efi_log(L"WARN: invalid accent_text (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent_os_icons") == 0) {
        if (parse_spec(value, &config->sp_os_icons))
            config->accent_os_icons = (config->sp_os_icons.mode != SPEC_OFF);
        else
            efi_log(L"WARN: invalid accent_os_icons (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"info_color") == 0) {
        if (!parse_spec_color(value, &config->sp_info, &config->fg_color))
            efi_log(L"WARN: invalid info_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"bg_color") == 0 ||
               efi_strcmp(key, L"background_color") == 0) {
        if (!parse_spec_color(value, &config->sp_bg, &config->bg_color))
            efi_log(L"WARN: invalid bg_color (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent_variant") == 0) {
        config->accent_variant = accent_variant_from_str(value);
    } else if (efi_strcmp(key, L"clock") == 0 ||
               efi_strcmp(key, L"show_clock") == 0) {
        if (parse_spec(value, &config->sp_clock))
            config->show_clock = (config->sp_clock.mode != SPEC_OFF);
        else
            efi_log(L"WARN: invalid clock (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"accent_clock") == 0) {
        if (parse_spec(value, &config->sp_clock))
            config->accent_clock = (config->sp_clock.mode != SPEC_OFF);
        else
            efi_log(L"WARN: invalid accent_clock (0/1, an accent role, or #RRGGBB)");
    } else if (efi_strcmp(key, L"clock_color") == 0) {
        if (!parse_spec_color(value, &config->sp_clock, &config->clock_color))
            efi_log(L"WARN: invalid clock_color (0/1, an accent role, or #RRGGBB)");
        else
            config->has_clock_color = 1;
    } else if (efi_strcmp(key, L"clock_size") == 0 ||
               efi_strcmp(key, L"clock_px") == 0) {
        config->clock_size = parse_uint(value);
    } else if (efi_strcmp(key, L"clock_format") == 0) {
        if (*value == '1' && value[1] == '2')      config->clock_24h = 0;
        else if (*value == '2' && value[1] == '4') config->clock_24h = 1;
        else
            efi_log(L"WARN: invalid clock_format (12h or 24h)");
    } else if (efi_strcmp(key, L"clock_seconds") == 0) {
        config->clock_seconds = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"clock_position") == 0 ||
               efi_strcmp(key, L"clock_pos") == 0) {
        int pos = clock_position_from_str(value);
        if (pos >= 0) config->clock_position = pos;
        else
            efi_log(L"WARN: invalid clock_position (top/bottom + left/right/center, or center)");
    } else if (efi_strcmp(key, L"clock_date") == 0) {
        int fmt = clock_date_from_str(value);
        if (fmt >= 0) {
            config->clock_date_format = fmt;
            config->clock_date = (fmt != CLOCK_DATE_OFF);
        } else {
            efi_log(L"WARN: invalid clock_date (0/1, long, iso, dmy or mdy)");
        }
    } else if (efi_strcmp(key, L"clock_date_format") == 0) {
        int fmt = clock_date_from_str(value);
        if (fmt >= 0 && fmt != CLOCK_DATE_OFF) {
            config->clock_date_format = fmt;
            config->clock_date = 1;
        } else {
            efi_log(L"WARN: invalid clock_date_format (long, iso, dmy or mdy)");
        }
    } else if (efi_strcmp(key, L"clock_blur") == 0) {
        config->clock_blur = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"clock_shadow") == 0) {
        config->clock_shadow = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"screensaver") == 0) {

        if (*value >= '2' && *value <= '9') {
            config->screensaver = 1;
            config->screensaver_delay = parse_uint(value);
        } else if (*value == '1' && value[1] != '\0') {
            config->screensaver = 1;
            config->screensaver_delay = parse_uint(value);
        } else {
            config->screensaver = (*value == '1' || *value == 't' || *value == 'y');
        }
    } else if (efi_strcmp(key, L"screensaver_delay") == 0 ||
               efi_strcmp(key, L"screensaver_timeout") == 0) {
        config->screensaver_delay = parse_uint(value);
    } else if (efi_strcmp(key, L"screensaver_blank") == 0 ||
               efi_strcmp(key, L"screensaver_blank_delay") == 0) {
        config->screensaver_blank = parse_uint(value);
    } else if (efi_strcmp(key, L"screensaver_clock") == 0) {
        config->screensaver_clock = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"record_seconds") == 0) {
        UINTN s = parse_uint(value);
        if (s < 1)  s = 1;
        if (s > 12) s = 12;
        config->record_seconds = s;
    } else if (efi_strcmp(key, L"menu_sound") == 0) {
        if (*value == '0' || *value == 'n' || *value == 'f' ||
            efi_strcmp(value, L"off") == 0) {
            config->menu_sound_on = 0;
        } else {
            config->menu_sound_on = 1;
            if (config->menu_sound) efi_free_pool(config->menu_sound);
            config->menu_sound = dup_path(value);
        }
    } else if (efi_strcmp(key, L"tpm") == 0 ||
               efi_strcmp(key, L"measure") == 0) {
        config->tpm = (*value == '1' || *value == 't' || *value == 'y');
    } else if (efi_strcmp(key, L"tpm_pcr_config") == 0) {
        config->tpm_pcr_config = parse_pcr(value, TPM_PCR_CONFIG_DEFAULT);
    } else if (efi_strcmp(key, L"tpm_pcr_cmdline") == 0) {
        config->tpm_pcr_cmdline = parse_pcr(value, TPM_PCR_CMDLINE_DEFAULT);
    } else if (efi_strcmp(key, L"loader_vars") == 0 ||
               efi_strcmp(key, L"loader_interface") == 0) {
        config->loader_vars = (*value == '1' || *value == 't' || *value == 'y');
    }
}

void apply_theme(config_t *config, CHAR16 *name) {
    CHAR16 path[MAX_PATH];
    SPrint(path, sizeof(path), L"%s\\themes\\%s.conf", CONFIG_DIR, name);
    efi_log(L"config: loading theme file");
    efi_log(path);

    CHAR16 *buf = read_text_file(path);
    if (!buf) { efi_log(L"WARN: theme file not found - keeping boot.conf values"); return; }

    CHAR16 *start = buf;
    while (*start) {
        CHAR16 *end = start;
        while (*end && *end != '\n') end++;
        if (*end == '\n') *end = '\0';
        CHAR16 *cr = efi_strchr(start, '\r');
        if (cr) *cr = '\0';

        CHAR16 *line = trim(start);
        if (line[0] != '#' && line[0] != '\0') {
            CHAR16 *eq = efi_strchr(line, '=');
            if (eq) {
                *eq = '\0';
                CHAR16 *key = trim(line);
                strip_inline_comment(eq + 1);
                CHAR16 *value = trim(eq + 1);
                if (efi_strcmp(key, L"theme") != 0)
                    apply_global(config, key, value);
            }
        }
        start = end + 1;
    }
    efi_free_pool(buf);
}

#define MAX_THEMES 32

CHAR16* resolve_rotating_theme(int cycle) {
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return NULL;
    CHAR16 dir[MAX_PATH];
    SPrint(dir, sizeof(dir), L"%s\\themes", CONFIG_DIR);
    EFI_FILE_PROTOCOL *d = efi_open_dir(root, dir);
    root->Close(root);
    if (!d) return NULL;

    CHAR16 names[MAX_THEMES][64];
    int n = 0;
    CHAR16 name[128];
    int is_dir;
    while (n < MAX_THEMES && efi_read_dirent(d, name, 128, &is_dir)) {
        if (is_dir || !ends_with_ci(name, L".conf")) continue;
        UINTN k = 0;
        while (name[k] && k < 63) { names[n][k] = name[k]; k++; }
        names[n][k] = '\0';
        if (k > 5) names[n][k - 5] = '\0';
        n++;
    }
    d->Close(d);
    if (n == 0) return NULL;

    for (int a = 1; a < n; a++) {
        CHAR16 tmp[64];
        UINTN t = 0; while (names[a][t]) { tmp[t] = names[a][t]; t++; } tmp[t] = '\0';
        int b = a - 1;
        while (b >= 0 && efi_strcmp(names[b], tmp) > 0) {
            UINTN c = 0; while (names[b][c]) { names[b + 1][c] = names[b][c]; c++; } names[b + 1][c] = '\0';
            b--;
        }
        UINTN c = 0; while (tmp[c]) { names[b + 1][c] = tmp[c]; c++; } names[b + 1][c] = '\0';
    }

    UINT32 idx;
    if (cycle) {
        UINT32 saved = 0;
        efi_get_var_u32(L"VisorThemeIndex", &saved);
        idx = saved % (UINT32)n;
        efi_set_var_u32(L"VisorThemeIndex", (idx + 1) % (UINT32)n);
    } else {
        idx = efi_rand() % (UINT32)n;
    }
    efi_log(L"config: rotated theme selected");
    efi_log(names[idx]);
    return efi_strdup(names[idx]);
}
