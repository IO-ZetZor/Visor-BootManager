#ifndef CONFIG_H
#define CONFIG_H

#include <efi.h>
#include "gui.h"

#define MAX_CMDLINE 512
#define MAX_PATH    256
#define CONFIG_DIR  L"\\EFI\\visor"
#define CONFIG_FILE L"\\EFI\\visor\\boot.conf"

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
    int   mouse;
    int   editor;
    int   autoboot;
    CHAR16 *theme;
    CHAR16 *title;
    int   no_title;
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
    int     anim_speed;
    UINTN   entries_per_page;
    boot_entry_t *entries;
    boot_entry_t *tail;
    UINTN entry_count;
} config_t;

EFI_STATUS config_parse(config_t *config);

void config_free(config_t *config);

boot_entry_t* config_add_entry(config_t *config,
                                CHAR16 *name,
                                CHAR16 *icon_path,
                                CHAR16 *kernel_path,
                                CHAR16 *initrd_path,
                                CHAR16 *cmdline,
                                CHAR16 *uuid,
                                int type);

EFI_STATUS config_auto_detect(config_t *config);

void bls_decrement(boot_entry_t *e);

#endif
