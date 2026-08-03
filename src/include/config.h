#ifndef CONFIG_H
#define CONFIG_H

#include <efi.h>
#include "gui.h"

#define MAX_CMDLINE 512
#define MAX_PATH    256
#define CONFIG_DIR  L"\\EFI\\visor"
#define CONFIG_FILE L"\\EFI\\visor\\boot.conf"

#define SCAN_MODE_DEEP  0
#define SCAN_MODE_QUICK 1

typedef struct {
    INTN  timeout;
    UINTN default_entry;
    int   quiet;
    int   text_menu;
    UINTN res_w;
    UINTN res_h;
    int   res_max;
    CHAR16 *def_cmdline;
    int   show_names;
    int   center_info;
    UINTN box_radius;
    int   remember_last;
    int   recovery_entries;
    int   snapshots_mode;
    int   mouse;
    UINTN pointer_speed;
    int   file_log;
    int   editor;
    int   autoboot;
    CHAR16 *theme;
    CHAR16 *title;
    int   no_title;
    CHAR16 *logo;
    int    no_logo;
    int    logo_mode;
    UINTN  logo_size;
    UINTN  logo_gap;
    int    accent_logo;
    int    has_accent_logo;

    accent_spec_t sp_logo, sp_underline, sp_highlight;
    accent_spec_t sp_title, sp_name, sp_info;
    accent_spec_t sp_shutdown, sp_reboot, sp_firmware;
    accent_spec_t sp_os_icons, sp_blur, sp_bg;
    accent_spec_t sp_g_text, sp_g_icons, sp_g_underline, sp_all;
    CHAR16 *font;
    CHAR16 *background;
    color_t bg_color;
    color_t fg_color;
    color_t highlight_color;
    color_t title_color;
    color_t name_color;
    UINTN  title_size;
    UINTN  name_size;
    UINTN  icon_size;
    UINTN  icon_spacing;
    UINTN  icon_y;
    color_t underline_color;
    int    has_underline_color;
    UINTN  underline_thickness;
    UINTN  underline_length;
    int    power_position;
    color_t shutdown_color;
    color_t reboot_color;
    color_t firmware_color;
    int    has_shutdown_color;
    int    has_reboot_color;
    int    has_firmware_color;
    int     power_icons;
    UINTN   power_icon_size;
    CHAR16 *shutdown_icon;
    CHAR16 *reboot_icon;
    CHAR16 *firmware_icon;
    int     blur;
    int     blur_title;
    color_t blur_color;
    int     has_blur_color;
    int     accent_enabled;
    int     accent_icons;
    int     accent_underline;
    int     accent_text;
    int     accent_os_icons;
    int     accent_variant;
    int     animation;
    int     anim_speed;
    int     fade_speed;
    UINTN   entries_per_page;
    int     hotplug;
    int     scan_existing;
    int     scan_mode;
    int     entries_from_config;
    boot_entry_t *entries;
    boot_entry_t *tail;
    UINTN entry_count;
} config_t;

EFI_STATUS config_parse(config_t *config);

int config_early_file_log_enabled(void);

void config_hotplug_arm(config_t *config);

int config_hotplug_poll(config_t *config, UINTN *first_new);

void config_free(config_t *config);

void bls_decrement(boot_entry_t *e);

#endif
