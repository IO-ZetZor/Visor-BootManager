/* capture_png.c - PNG screenshot encoder (feature: screenshot) */
#include "capture_internal.h"

#define CAP_PNG_CHUNK_IHDR 0x52484449u
#define CAP_PNG_CHUNK_IDAT 0x54414449u
#define CAP_PNG_CHUNK_IEND 0x444E4549u

EFI_STATUS cap_png_encode(const UINT32 *pixels, UINTN w, UINTN h,
                          UINT8 **out, UINTN *out_size) {
    if (!pixels || !w || !h || !out || !out_size) return EFI_INVALID_PARAMETER;
    *out = NULL;
    *out_size = 0;

    cap_buf raw;
    raw.buf = NULL; raw.len = 0; raw.cap = 0;
    cap_buf zl;
    zl.buf = NULL; zl.len = 0; zl.cap = 0;
    cap_buf png;
    png.buf = NULL; png.len = 0; png.cap = 0;

    UINTN rsize = h * (1 + w * 3);
    if (!cap_buf_reserve(&raw, rsize)) goto fail;

    for (UINTN y = 0; y < h; y++) {
        raw.buf[raw.len++] = 0x00;
        const UINT32 *row = pixels + y * w;
        for (UINTN x = 0; x < w; x++) {
            UINT32 p = row[x];
            raw.buf[raw.len++] = (UINT8)(p >> 16);
            raw.buf[raw.len++] = (UINT8)(p >> 8);
            raw.buf[raw.len++] = (UINT8)p;
        }
    }

    {
        UINT8 hdr[2] = { 0x78, 0x01 };
        if (!cap_buf_put(&zl, hdr, 2)) goto fail;
    }

    {
        const UINTN STORED_MAX = 65535;
        UINTN off = 0;
        int final = 0;
        while (!final) {
            UINTN n = raw.len - off;
            if (n > STORED_MAX) n = STORED_MAX;
            final = (off + n >= raw.len);
            UINT8 bhdr = (UINT8)(final ? 1 : 0);
            UINT8 p16[4];
            p16[0] = (UINT8)n; p16[1] = (UINT8)(n >> 8);
            UINTN nlen = ((UINTN)0xFFFF) ^ n;
            p16[2] = (UINT8)nlen; p16[3] = (UINT8)(nlen >> 8);
            if (!cap_buf_put(&zl, &bhdr, 1)) goto fail;
            if (!cap_buf_put(&zl, p16, 4)) goto fail;
            if (!cap_buf_put(&zl, raw.buf + off, n)) goto fail;
            off += n;
        }
    }

    {
        UINT32 a = cap_adler32(raw.buf, raw.len);
        if (!cap_buf_u32be(&zl, a)) goto fail;
    }
    cap_buf_free(&raw);

    {
        static const UINT8 sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (!cap_buf_put(&png, sig, 8)) goto fail;
    }
    {
        UINT8 ihdr[13];
        ihdr[0] = (UINT8)(w >> 24); ihdr[1] = (UINT8)(w >> 16);
        ihdr[2] = (UINT8)(w >> 8);  ihdr[3] = (UINT8)w;
        ihdr[4] = (UINT8)(h >> 24); ihdr[5] = (UINT8)(h >> 16);
        ihdr[6] = (UINT8)(h >> 8);  ihdr[7] = (UINT8)h;
        ihdr[8] = 8;
        ihdr[9] = 2;
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        UINT8 type[4] = { 'I', 'H', 'D', 'R' };
        if (!cap_buf_u32be(&png, 13)) goto fail;
        if (!cap_buf_put(&png, type, 4)) goto fail;
        if (!cap_buf_put(&png, ihdr, 13)) goto fail;
        UINT32 crc = cap_crc32(0, type, 4);
        crc = cap_crc32(crc, ihdr, 13);
        if (!cap_buf_u32be(&png, crc)) goto fail;
    }

    {
        UINT8 type[4] = { 'I', 'D', 'A', 'T' };
        UINT32 idat_crc = cap_crc32(0, type, 4);
        idat_crc = cap_crc32(idat_crc, zl.buf, zl.len);
        if (!cap_buf_u32be(&png, (UINT32)zl.len)) goto fail;
        if (!cap_buf_put(&png, type, 4)) goto fail;
        if (!cap_buf_put(&png, zl.buf, zl.len)) goto fail;
        if (!cap_buf_u32be(&png, idat_crc)) goto fail;
    }
    cap_buf_free(&zl);

    {
        UINT8 type[4] = { 'I', 'E', 'N', 'D' };
        UINT32 crc = cap_crc32(0, type, 4);
        if (!cap_buf_u32be(&png, 0)) goto fail;
        if (!cap_buf_put(&png, type, 4)) goto fail;
        if (!cap_buf_u32be(&png, crc)) goto fail;
    }

    *out = png.buf;
    *out_size = png.len;
    return EFI_SUCCESS;

fail:
    cap_buf_free(&raw);
    cap_buf_free(&zl);
    cap_buf_free(&png);
    return EFI_OUT_OF_RESOURCES;
}
