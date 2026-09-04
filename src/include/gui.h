#ifndef GUI_H
#define GUI_H

#include <efi.h>
#include <efilib.h>
#include "gpt.h"

#define GUI_ACCENT_ROLES 14

#define ICON_SIZE 64
#define ICON_SPACING 20
#define PADDING 30

#define FONT_SMALL 8
#define FONT_LARGE 16

typedef struct {
    UINT8 r, g, b;
} color_t;

#define SPEC_UNSET  0
#define SPEC_OFF    1
#define SPEC_ON     2
#define SPEC_ROLE   3
#define SPEC_COLOR  4

typedef struct {
    int     mode;
    int     role;
    color_t color;
} accent_spec_t;

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
    UINTN scaled_size;
    UINT32 *scaled;
} icon_t;

#include "anim.h"
#include "filebrowse.h"

typedef struct cap_gif cap_gif;

#define VISOR_ACTION_BOOT      0
#define VISOR_ACTION_SHUTDOWN  1
#define VISOR_ACTION_REBOOT    2
#define VISOR_ACTION_FIRMWARE  3
#define VISOR_ACTION_RETRY     4
#define VISOR_ACTION_MENU      5
#define VISOR_ACTION_RESCUE    6

#define LOGO_MODE_OFF    0
#define LOGO_MODE_TITLE  1
#define LOGO_MODE_ONLY   2
#define LOGO_MODE_ABOVE  3

#define POWER_POS_BOTTOMRIGHT  0
#define POWER_POS_BOTTOMLEFT   1
#define POWER_POS_TOPRIGHT     2
#define POWER_POS_TOPLEFT      3

#define CLOCK_POS_TOPRIGHT     0
#define CLOCK_POS_TOPLEFT      1
#define CLOCK_POS_TOPCENTER    2
#define CLOCK_POS_BOTTOMRIGHT  3
#define CLOCK_POS_BOTTOMLEFT   4
#define CLOCK_POS_BOTTOMCENTER 5
#define CLOCK_POS_CENTER       6

#define CLOCK_DATE_OFF   0
#define CLOCK_DATE_LONG  1
#define CLOCK_DATE_ISO   2
#define CLOCK_DATE_DMY   3
#define CLOCK_DATE_MDY   4

#define FOCUS_ENTRIES  0
#define FOCUS_POWER    1

#define DEPLOY_CURRENT  0
#define DEPLOY_ROLLBACK 1
#define DEPLOY_OLDER    2
#define DEPLOY_PINNED   3

typedef struct deployment {
    CHAR16 *version;
    CHAR16 *kernel;
    CHAR16 *initrd;
    CHAR16 *cmdline;
    CHAR16 *bls_path;
    int     role;
    int     tries_left;
    int     tries_done;
} deployment_t;

typedef struct snapshot {
    CHAR16 *id;
    CHAR16 *date;
    CHAR16 *desc;
    CHAR16 *kernel;
    CHAR16 *initrd;
    CHAR16 *cmdline;
} snapshot_t;

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
    int     color_role;
    UINT8   sha256[32];
    int     has_sha256;
    int     encrypted;
    int     initrd_encrypted;
    int     luks;
    int     luks_confirm;
    int     luks_verbose;
    CHAR16 *luks_key_path;
    CHAR16 *luks_cmdline;
    CHAR16 *luks_preset;
    CHAR16 *decrypt_password;

    deployment_t *deployments;
    UINTN   deploy_count;
    UINTN   deploy_default;
    UINTN   deploy_sel;

    snapshot_t *snapshots;
    UINTN   snap_count;
    UINTN   snap_sel;

    EFI_HANDLE hp_volume;
} boot_entry_t;

typedef struct gui_state {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    UINTN screen_width;
    UINTN screen_height;
    UINTN bpp;

    UINTN pixels_per_scanline;

    EFI_GRAPHICS_PIXEL_FORMAT pixel_format;

    int fb_fast;

    UINT32 *backbuffer;

    boot_entry_t *entries;
    UINTN entry_count;

    UINTN selected;

    UINTN per_page;
    UINTN prev_page;
    UINTN prev_selected;
    int   page_anim;
    int   page_frame;
    UINTN page_old;
    UINTN page_old_sel;

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
    int   anim_cross;
    int   anim_frames;

    INTN  band_y[4], band_h[4];
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
    icon_t *logo;
    int     logo_mode;
    UINTN   logo_size;
    UINTN   logo_gap;
    int     accent_logo;
    int     show_names;
    int     center_info;
    UINTN   box_radius;
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
    int     blur_color_set;
    int     animation;
    int     anim_speed;
    int     fade_speed;

    int     accent_enabled;
    int     accent_icons;
    int     accent_underline;
    int     accent_text;
    int     accent_os_icons;
    int     accent_variant;
    color_t accent_roles[GUI_ACCENT_ROLES];
    color_t accent_primary;
    color_t accent_secondary;
    color_t accent_tertiary;
    int     accent_valid;

    accent_spec_t sp_logo, sp_underline, sp_highlight;
    accent_spec_t sp_title, sp_name, sp_info;
    accent_spec_t sp_shutdown, sp_reboot, sp_firmware;
    accent_spec_t sp_os_icons, sp_blur, sp_bg;
    accent_spec_t sp_g_text, sp_g_icons, sp_g_underline, sp_all;

    color_t logo_tint;      int logo_tint_on;
    color_t os_icon_tint;   int os_icon_tint_on;
    int     pwr_tint_on[3];

    int     show_clock;
    int     accent_clock;
    color_t clock_color;
    UINTN   clock_size;
    accent_spec_t sp_clock;
    int     clock_24h;
    int     clock_seconds;
    int     clock_position;
    int     clock_date;
    int     clock_date_format;
    int     clock_blur;
    int     clock_shadow;

    INTN    clock_last_key;

    INTN    clock_x, clock_y, clock_w, clock_h;
    int     clock_drawn;
    int     clock_dirty;

    int     screensaver;
    UINTN   ss_delay_ms;
    UINTN   ss_blank_ms;
    int     ss_keep_clock;

    int     ss_level;
    UINT64  ss_last_input_ms;

    icon_t *background;
    CHAR16 *background_path;
    anim_t *bg_anim;

    int     version_mode;
    int     ver_fading;
    int     ver_frame;
    int     ver_dir;
    int     ver_what;
    int     ver_next;
    int     snap_mode;
    UINTN   snap_scroll;

    int     editor_enabled;
    int     editing;
    int     edit_secret;
    int     edit_reveal;
    CHAR16 *edit_title;
    CHAR16 *edit_hint;
    CHAR16  edit_buf[512];
    UINTN   edit_len;
    UINTN   edit_cursor;
    CHAR16 *override_cmdline;
    CHAR16 *override_kernel_path;
    CHAR16 *override_initrd_path;
    CHAR16 *override_uuid;
    EFI_HANDLE override_volume;
    int     override_initrd_set;

    fb_t   *browse;

    int   (*hotplug_poll)(void *ctx, boot_entry_t **head, UINTN *count,
                          UINTN *first_new);
    void   *hotplug_ctx;
    UINT64  hp_last_ms;
    int     hp_scanning;
    int     hp_anim;
    int     hp_frame;
    UINTN   hp_first;
    INTN    hp_shift;
    int     hp_removal;

    int     mouse_enabled;
    UINTN   pointer_speed;
    void   *spp;
    void   *app;
    int     has_pointer;
    int     cursor_active;
    INTN    cursor_x, cursor_y;
    INTN    cur_prev_x, cur_prev_y;
    int     cursor_saved;
    UINT32  cursor_save[18 * 24];
    INTN    hit_x[32], hit_y[32], hit_w[32], hit_h[32];
    UINTN   hit_idx[32];
    int     hit_n;

    /* capture (F6 screenshot / F10 GIF record) state */
    int     cap_mode;
    UINT64  cap_start_ms;
    UINTN   cap_frames;
    INTN    cap_status_ms;
    UINT64  cap_last_ms;
    CHAR16  cap_status[96];
    CHAR16  cap_detail[128];
    int     cap_status_err;
    UINTN   cap_sec_prev;
    UINT64  cap_next_due_ms;
    int     cap_truncated;
    UINTN   record_seconds;
    cap_gif *cap_gif;

    /* GPT corruption warning modal (gptw_state: 0=hidden, 2=overview,
     * 3=details, 6=confirm, 4=working, 5=done) */
    int      gptw_state;
    int      gptw_found;
    int      gptw_suppressed;
    CHAR16   gptw_disk[40];
    gpt_dev_t    gptw_dev;
    int          gptw_have_dev;
    gpt_diag_t   gptw_diag;
    gpt_plan_t   gptw_plan;
    gpt_result_t gptw_res;
    CHAR16   gptw_confirm[8];
} gui_state_t;

EFI_STATUS gui_init(gui_state_t *state);

EFI_STATUS gui_set_mode(gui_state_t *state, UINTN want_w, UINTN want_h, int want_max);

icon_t* gui_load_image(CHAR16 *path);

icon_t* gui_load_icon(CHAR16 *path);

void gui_draw_menu(gui_state_t *state, int partial);

void gui_present(gui_state_t *state);

void gui_present_band(gui_state_t *state, INTN y, INTN h);

void gui_fade_out(gui_state_t *state);

boot_entry_t* gui_run(gui_state_t *state);

EFI_STATUS gui_prompt_password(gui_state_t *state, CHAR16 *title, CHAR16 *hint,
                               CHAR16 **out);

void gui_shutdown(gui_state_t *state);

void gui_set_background(gui_state_t *state, CHAR16 *path);

void gui_set_logo(gui_state_t *state, CHAR16 *path);

void gui_apply_accent(gui_state_t *state);

void gui_set_font(const char *name);

anim_t* gif_load(UINT8 *data, UINTN size);

int gif_advance(anim_t *a);

void gif_free(anim_t *a);

EFI_STATUS png_decompress(UINT8 *input, UINTN input_size,
                           UINT8 *output, UINTN *output_size);

anim_t* vbg_load(UINT8 *data, UINTN size);

anim_t* mp4_load(UINT8 *data, UINTN size, UINTN tgt_w, UINTN tgt_h);

int anim_advance(anim_t *a);

int anim_advance_n(anim_t *a, UINTN n);

void anim_free(anim_t *a);

#endif
