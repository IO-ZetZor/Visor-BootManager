#ifndef GUI_H
#define GUI_H

#include <efi.h>
#include <efilib.h>

#define ICON_SIZE 64
#define ICON_SPACING 20
#define PADDING 30

#define FONT_SMALL 8
#define FONT_LARGE 16

typedef struct {
    UINT8 r, g, b;
} color_t;

#define COLOR_BLACK     ((color_t){0x00, 0x00, 0x00})
#define COLOR_WHITE     ((color_t){0xFF, 0xFF, 0xFF})
#define COLOR_GRAY      ((color_t){0x80, 0x80, 0x80})
#define COLOR_BLUE      ((color_t){0x4A, 0x90, 0xD9})
#define COLOR_RED       ((color_t){0xD9, 0x4A, 0x4A})
#define COLOR_GREEN     ((color_t){0x4A, 0xD9, 0x6E})
#define COLOR_ORANGE    ((color_t){0xD9, 0x8A, 0x4A})
#define COLOR_DARK_BG   ((color_t){0x1a, 0x1a, 0x2e})

typedef struct {
    UINTN width;
    UINTN height;
    UINT32 *pixels;
} icon_t;

#define VISOR_ACTION_BOOT      0
#define VISOR_ACTION_SHUTDOWN  1
#define VISOR_ACTION_REBOOT    2
#define VISOR_ACTION_FIRMWARE  3

#define POWER_POS_BOTTOMRIGHT  0
#define POWER_POS_BOTTOMLEFT   1
#define POWER_POS_TOPRIGHT     2
#define POWER_POS_TOPLEFT      3

#define FOCUS_ENTRIES  0
#define FOCUS_POWER    1

typedef struct boot_entry {
    struct boot_entry *next;
    CHAR16 *name;
    CHAR16 *icon_path;
    CHAR16 *kernel_path;
    CHAR16 *initrd_path;
    CHAR16 *cmdline;
    CHAR16 *uuid;
    UINTN index;
    int type;
    icon_t *icon;
    UINTN   icon_size;
    color_t color;
    int     has_color;
    UINT8   sha256[32];
    int     has_sha256;
} boot_entry_t;

typedef struct {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    UINTN screen_width;
    UINTN screen_height;
    UINTN bpp;

    UINTN pixels_per_scanline;

    EFI_GRAPHICS_PIXEL_FORMAT pixel_format;

    UINT32 *backbuffer;

    boot_entry_t *entries;
    UINTN entry_count;

    UINTN selected;
    INTN  timeout;
    UINT64 timeout_start;
    int   timeout_active;

    int running;
    int action;

    int   focus;
    int   prev_focus;
    UINTN power_sel;

    INTN  anim_cur[9];
    INTN  anim_from[9];
    INTN  anim_to[9];
    INTN  anim_frame;
    int   anim_active;
    int   anim_init;
    int   anim_power;
    int   anim_cross;
    int   anim_frames;

    INTN  band_y[2], band_h[2];
    int   band_n;
    INTN  prev_ul_y;
    INTN  prev_box_y0, prev_box_y1;

    INTN  pwr_x[3], pwr_y[3], pwr_w[3], pwr_h[3];
    INTN  pwr_y0, pwr_y1;

    UINT32 *scene_cache;
    UINT32 *blur_cache;
    int     scene_valid;

    CHAR16 *title;
    int     show_title;
    color_t title_color;
    color_t name_color;
    UINTN   title_size;
    UINTN   name_size;

    UINTN   icon_size;
    UINTN   icon_spacing;
    UINTN   icon_y;

    color_t underline_color;
    UINTN   underline_thickness;
    UINTN   underline_length;

    int     power_position;
    color_t shutdown_color;
    color_t reboot_color;
    color_t firmware_color;

    int     power_icons;
    UINTN   power_icon_size;
    icon_t *shutdown_icon;
    icon_t *reboot_icon;
    icon_t *firmware_icon;

    color_t bg_color;
    color_t fg_color;
    color_t highlight_color;

    int     blur;
    int     blur_title;
    color_t blur_color;
    int     anim_speed;

    icon_t *background;
    CHAR16 *background_path;
} gui_state_t;

EFI_STATUS gui_init(gui_state_t *state);

icon_t* gui_load_image(CHAR16 *path);

icon_t* gui_load_icon(CHAR16 *path);

void gui_fill_rect(gui_state_t *state, UINTN x, UINTN y,
                   UINTN w, UINTN h, color_t color);

void gui_draw_image(gui_state_t *state, icon_t *img, UINTN x, UINTN y);

void gui_draw_background(gui_state_t *state);

void gui_draw_text_small(gui_state_t *state, CHAR16 *text, UINTN x, UINTN y,
                         color_t color);

void gui_draw_text_large(gui_state_t *state, CHAR16 *text, UINTN x, UINTN y,
                         color_t color);

void gui_draw_menu(gui_state_t *state, int partial);

void gui_present(gui_state_t *state);

void gui_present_band(gui_state_t *state, INTN y, INTN h);

boot_entry_t* gui_run(gui_state_t *state);

void gui_shutdown(gui_state_t *state);

void gui_set_background(gui_state_t *state, CHAR16 *path);

void gui_set_font(const char *name);

#endif
