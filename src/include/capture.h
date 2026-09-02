#ifndef VISOR_CAPTURE_H
#define VISOR_CAPTURE_H

#include <efi.h>

/*
 * Capture support: PNG (stored, uncompressed) screenshots and animated GIF
 * recording. All functions are usable both in firmware and in host test
 * harnesses (via tools/efi_stub.h).
 */

/* Encode a 32bpp XRGB (0x00RRGGBB) backbuffer into a written PNG.
 * Allocates *out through efi_allocate_pool; caller frees it. */
EFI_STATUS cap_png_encode(const UINT32 *pixels, UINTN w, UINTN h,
                          UINT8 **out, UINTN *out_size);

/* ---- animated GIF encoder ---- */
typedef struct cap_gif cap_gif;

#define CAP_GIF_MAX_FRAMES 64

/* Create an encoder for a w x h image stream. Returns NULL on failure. */
cap_gif *cap_gif_new(UINTN w, UINTN h);

/* Push one frame (0x00RRGGBB rows). delay_cs = frame delay in hundredths of a
 * second. Returns 1 on success, 0 on failure / when frame budget is reached. */
int cap_gif_frame(cap_gif *g, const UINT32 *pixels, UINTN delay_cs);

/* Number of frames captured so far. */
UINTN cap_gif_count(const cap_gif *g);

/* Finish the GIF, writing *out (efi_allocate_pool'd). Frees g on success.
 * On failure returns an error and frees g. */
EFI_STATUS cap_gif_close(cap_gif *g, UINT8 **out, UINTN *out_size);

/* Abort and free without producing output. */
void cap_gif_free(cap_gif *g);

/* ---- ESP file helpers ---- */

/* Ensure directory exists on the boot volume root, creating it if needed. */
EFI_STATUS cap_ensure_dir(const CHAR16 *path);

/* Write data to a file (created/overwritten) on the boot volume root. */
EFI_STATUS cap_save_file(const CHAR16 *path, const UINT8 *data, UINTN size);

/* Build filename like <base>_HHMMSS<ext> into out (cap chars incl NUL local).
 * Uses the UEFI runtime clock when available. */
void cap_timestamp_name(CHAR16 *out, UINTN cap, const CHAR16 *base,
                        const CHAR16 *ext);

#endif /* VISOR_CAPTURE_H */
