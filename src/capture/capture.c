/* capture.c - CRC/Adler checksums and the growable output buffer */
#include "capture_internal.h"

static UINT32 cap_crc32_table[256];
static int    cap_crc32_ready = 0;

static void cap_crc32_init_table(void) {
    if (cap_crc32_ready) return;
    for (UINTN n = 0; n < 256; n++) {
        UINT32 c = (UINT32)n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        cap_crc32_table[n] = c;
    }
    cap_crc32_ready = 1;
}

UINT32 cap_crc32(UINT32 crc, const UINT8 *data, UINTN len) {
    cap_crc32_init_table();
    crc = ~crc;
    for (UINTN i = 0; i < len; i++)
        crc = cap_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

UINT32 cap_adler32(const UINT8 *data, UINTN len) {
    UINT32 a = 1, b = 0;
    for (UINTN i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void cap_buf_free(cap_buf *b) {
    if (b && b->buf) { efi_free_pool(b->buf); b->buf = NULL; }
    b->len = b->cap = 0;
}

int cap_buf_reserve(cap_buf *b, UINTN need) {
    if (b->cap >= need) return 1;
    UINTN ncap = b->cap ? b->cap : 4096;
    while (ncap < need) ncap *= 2;
    UINT8 *nb = efi_allocate_pool(ncap);
    if (!nb) return 0;
    if (b->buf && b->len) CopyMem(nb, b->buf, b->len);
    if (b->buf) efi_free_pool(b->buf);
    b->buf = nb;
    b->cap = ncap;
    return 1;
}

int cap_buf_put(cap_buf *b, const UINT8 *p, UINTN n) {
    if (!cap_buf_reserve(b, b->len + n)) return 0;
    CopyMem(b->buf + b->len, (void*)p, n);
    b->len += n;
    return 1;
}

int cap_buf_u32be(cap_buf *b, UINT32 v) {
    UINT8 t[4] = { (UINT8)(v >> 24), (UINT8)(v >> 16), (UINT8)(v >> 8), (UINT8)v };
    return cap_buf_put(b, t, 4);
}

int cap_buf_u16le(cap_buf *b, UINT16 v) {
    UINT8 t[2] = { (UINT8)v, (UINT8)(v >> 8) };
    return cap_buf_put(b, t, 2);
}
