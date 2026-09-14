/* capture_internal.h - shared internals of the src/capture/capture.c translation units. */
#ifndef CAPTURE_INTERNAL_H
#define CAPTURE_INTERNAL_H

#include "capture.h"
#include "efi_helpers.h"

#include <efilib.h>
#include <string.h>

UINT32 cap_crc32(UINT32 crc, const UINT8 *data, UINTN len);
UINT32 cap_adler32(const UINT8 *data, UINTN len);
typedef struct {
    UINT8  *buf;
    UINTN   len;
    UINTN   cap;
} cap_buf;
void cap_buf_free(cap_buf *b);
int cap_buf_reserve(cap_buf *b, UINTN need);
int cap_buf_put(cap_buf *b, const UINT8 *p, UINTN n);
int cap_buf_u32be(cap_buf *b, UINT32 v);
int cap_buf_u16le(cap_buf *b, UINT16 v);

static inline UINTN cap_bucket_of(UINT32 p) {
    return ((((p >> 16) & 0xF8) << 7) | (((p >> 8) & 0xF8) << 2)
            | ((p & 0xF8) >> 3));
}

#endif
