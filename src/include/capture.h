/* capture.h - PNG/ANSI-GIF capture API (feature: capture) */
#ifndef VISOR_CAPTURE_H
#define VISOR_CAPTURE_H

#include <efi.h>

EFI_STATUS cap_png_encode(const UINT32 *pixels, UINTN w, UINTN h,
                          UINT8 **out, UINTN *out_size);

typedef struct cap_gif cap_gif;

#define CAP_GIF_MAX_FRAMES 256

#define CAP_FRAME_ERROR 0
#define CAP_FRAME_OK    1
#define CAP_FRAME_FULL  2

cap_gif *cap_gif_new(UINTN src_w, UINTN src_h, UINTN max_width,
                     UINTN max_frames, UINTN budget_bytes,
                     UINTN nominal_delay_cs);

void cap_gif_sample(cap_gif *g, const UINT32 *pixels);

int cap_gif_frame(cap_gif *g, const UINT32 *pixels, UINT64 now_ms);

UINTN cap_gif_count(const cap_gif *g);

int cap_gif_is_full(const cap_gif *g);

UINTN cap_gif_bytes(const cap_gif *g);

UINTN cap_gif_width(const cap_gif *g);
UINTN cap_gif_height(const cap_gif *g);

EFI_STATUS cap_gif_close(cap_gif *g, UINT8 **out, UINTN *out_size);

void cap_gif_free(cap_gif *g);

EFI_STATUS cap_ensure_dir(const CHAR16 *path);

EFI_STATUS cap_save_file(const CHAR16 *path, const UINT8 *data, UINTN size);

void cap_timestamp_name(CHAR16 *out, UINTN cap, const CHAR16 *base,
                        const CHAR16 *ext);

#endif
