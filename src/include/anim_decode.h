/* anim_decode.h - shared internals of the src/mp4_decoder.c translation units.
   Generated boundary: nothing here is part of the public API. */
#ifndef ANIM_DECODE_H
#define ANIM_DECODE_H

#include "gui.h"
#include "efi_helpers.h"
#include <efi.h>
#include <efilib.h>

#define MP4_MAX_DIM     8192
#define MP4_MAX_PIXELS  (16u * 1024u * 1024u)
typedef struct {
    int    mincode[17];
    int    maxcode[17];
    int    valptr[17];
    int    nvals;
    UINT16 lut[512];
} jhtab_t;
typedef struct {
    UINTN  *off;
    UINT32 *len;
    UINT32 *ms;
    UINTN   n;
    UINTN   cur;
    UINTN   W, H;
    UINTN   tgt_w, tgt_h;
    int     shift;
    int     ncomp;
    UINT8   comp_id[4], comp_h[4], comp_v[4], comp_tq[4];
    int     max_h, max_v;
    UINTN   cw[4], ch[4];
    UINT8  *plane[4];
    UINTN   lock_cw[4], lock_ch[4];
    int     lock_ncomp;
    int     locked;
    UINTN  *sxmap[4];

    UINT16  qtab[4][64];
    UINT8   qn[4];
    UINT8   dc_bits[4][16], dc_val[4][256], dc_have[4];
    UINT8   ac_bits[4][16], ac_val[4][256], ac_have[4];
    jhtab_t htab[8];
    UINT8   clamp[768];
    INT32   blk[64];
    UINT8   blkidx[64];

    int     scan_n;
    int     scan_comp[4];
    UINT8   scan_dc[4], scan_ac[4];
    int     ri;
    INT32   dcpred[4];

    INT64   tmp[64];
} mjpeg_t;

static inline UINT32 rd32(const UINT8 *p) {
    return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) |
           ((UINT32)p[2] << 8) | p[3];
}

static inline UINT16 rd16(const UINT8 *p) {
    return (UINT16)(((UINT16)p[0] << 8) | p[1]);
}

static inline UINT64 rd64(const UINT8 *p) {
    return ((UINT64)rd32(p) << 32) | rd32(p + 4);
}
UINTN jpeg_parse_header(mjpeg_t *st, const UINT8 *d, UINTN size);
int jpeg_decode_scan(mjpeg_t *st, const UINT8 *d, UINTN size, UINTN scan_off);
void jpeg_composite(mjpeg_t *st, UINT32 *canvas);
void jpeg_lock_geometry(mjpeg_t *st);
int jpeg_frame(mjpeg_t *st, const UINT8 *d, UINTN size, UINT32 *canvas);
void mjpeg_state_free(mjpeg_t *st);
int mjpeg_advance_n(anim_t *a, UINTN n);
void jpeg_planes_free(mjpeg_t *st);
void vbg_free(anim_t *a);
int vbg_advance(anim_t *a);

#endif
