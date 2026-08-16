#ifndef FILEBROWSE_H
#define FILEBROWSE_H

#include <efi.h>
#include <efilib.h>
#include "efi_helpers.h"

#define FB_PATH_MAX    1024
#define FB_NAME_MAX    256
#define FB_MAX_ENTRIES 4096

typedef struct {
    CHAR16 *name;
    UINT64 size;
    int is_dir;
} fb_entry_t;

typedef struct {
    EFI_HANDLE handle;
    CHAR16 *uuid;
    CHAR16 *label;
} fb_volume_t;

typedef struct {
    fb_volume_t *vols;
    UINTN vol_count;
    UINTN vol_cur;
    CHAR16 path[FB_PATH_MAX];
    fb_entry_t *entries;
    UINTN entry_count;
    UINTN cursor;
    UINTN scroll;
} fb_t;

struct gui_state;

int fb_init(fb_t *s);
void fb_free(fb_t *s);
int fb_set_path(fb_t *s, CHAR16 *path);
int fb_list(fb_t *s);
int fb_enter(fb_t *s);
int fb_up(fb_t *s);
void fb_switch_volume(fb_t *s, int dir);
void fb_move(fb_t *s, int delta);
fb_entry_t* fb_cursor(fb_t *s);

int fb_is_efi(const CHAR16 *name);
int fb_is_initrd(const CHAR16 *name);
int fb_is_kernel(const CHAR16 *name);
void fb_format_size(UINT64 size, CHAR16 *buf, UINTN cap);

int fb_boot_apply(fb_t *s, struct gui_state *gui);

#endif
