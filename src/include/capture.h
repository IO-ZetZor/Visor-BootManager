#ifndef VISOR_CAPTURE_H
#define VISOR_CAPTURE_H

#include <efi.h>

/* Capture: PNG (stored, uncompressed) screenshots and animated GIF recording.
 * Usable in firmware and host test harnesses (via tools/efi_stub.h). */

/* Encode a 32bpp XRGB backbuffer into a written PNG.
 * Allocates *out via efi_allocate_pool; caller frees it. */
EFI_STATUS cap_png_encode(const UINT32 *pixels, UINTN w, UINTN h,
                          UINT8 **out, UINTN *out_size);

/* Animated GIF encoder */
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

/* Number of frames stored so far. */
UINTN cap_gif_count(const cap_gif *g);

int cap_gif_is_full(const cap_gif *g);

UINTN cap_gif_bytes(const cap_gif *g);

UINTN cap_gif_width(const cap_gif *g);
UINTN cap_gif_height(const cap_gif *g);

/* Finish the GIF, writing *out (efi_allocate_pool'd). Frees g. */
EFI_STATUS cap_gif_close(cap_gif *g, UINT8 **out, UINTN *out_size);

/* Abort and free without producing output. */
void cap_gif_free(cap_gif *g);

/* ESP file helpers */

/* Ensure directory exists on the boot volume root, creating it if needed. */
EFI_STATUS cap_ensure_dir(const CHAR16 *path);

/* Write data to a file on the boot volume root. */
EFI_STATUS cap_save_file(const CHAR16 *path, const UINT8 *data, UINTN size);

/* Build filename like <base>_HHMMSS<ext>. Uses the UEFI runtime clock. */
void cap_timestamp_name(CHAR16 *out, UINTN cap, const CHAR16 *base,
                        const CHAR16 *ext);

#endif /* VISOR_CAPTURE_H */
