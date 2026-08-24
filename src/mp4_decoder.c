#include "gui.h"
#include "efi_helpers.h"
#include <efi.h>
#include <efilib.h>

#define MP4_MAX_DIM     8192
#define MP4_MAX_PIXELS  (16u * 1024u * 1024u)
#define MP4_MAX_SAMPLES 16384
#define MP4_MAX_FILE    (512u * 1024u * 1024u)

static const UINT8 jzig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63 };

static const INT32 jidct_m[8][8] = {
    {  5793,  8035,  7568,  6811,  5793,  4551,  3135,  1598 },
    {  5793,  6811,  3135, -1598, -5793, -8035, -7568, -4551 },
    {  5793,  4551, -3135, -8035, -5793,  1598,  7568,  6811 },
    {  5793,  1598, -7568, -4551,  5793,  6811, -3135, -8035 },
    {  5793, -1598, -7568,  4551,  5793, -6811, -3135,  8035 },
    {  5793, -4551, -3135,  8035, -5793, -1598,  7568, -6811 },
    {  5793, -6811,  3135,  1598, -5793,  8035, -7568,  4551 },
    {  5793, -8035,  7568, -6811,  5793, -4551,  3135, -1598 },
};

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

static UINT32 rd32(const UINT8 *p) {
    return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) |
           ((UINT32)p[2] << 8) | p[3];
}

static UINT16 rd16(const UINT8 *p) {
    return (UINT16)(((UINT16)p[0] << 8) | p[1]);
}

static UINT64 rd64(const UINT8 *p) {
    return ((UINT64)rd32(p) << 32) | rd32(p + 4);
}

typedef struct {
    const UINT8 *p;
    UINTN size, pos;
    UINT32 acc;
    int    nbits;
    int    marker;
} jbits_t;

static void jb_reset(jbits_t *b, const UINT8 *p, UINTN size, UINTN pos) {
    b->p = p; b->size = size; b->pos = pos;
    b->acc = 0; b->nbits = 0; b->marker = 0;
}

static int jb_refill(jbits_t *b) {
    while (b->nbits <= 24) {
        if (b->pos >= b->size) return 0;
        UINT8 c = b->p[b->pos++];
        if (c == 0xFF) {
            if (b->pos >= b->size) return 0;
            UINT8 n = b->p[b->pos];
            if (n == 0) {
                b->acc = (b->acc << 8) | 0xFF;
                b->nbits += 8;
                b->pos++;
                continue;
            }
            b->marker = n;
            b->pos -= 1;
            return 0;
        }
        b->acc = (b->acc << 8) | c;
        b->nbits += 8;
    }
    return 1;
}

static int jb_get(jbits_t *b, int count, UINT32 *out) {
    if (b->nbits < count) jb_refill(b);
    if (b->nbits < count) return 0;
    *out = (b->acc >> (b->nbits - count)) & ((1u << count) - 1);
    b->nbits -= count;
    return 1;
}

#define JLUT_BITS 9
#define JLUT_SIZE (1 << JLUT_BITS)

static int jhuff_build(const UINT8 *bits, const UINT8 *vals, jhtab_t *t) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        int cnt = bits[l - 1];
        if (code + cnt > (1 << l)) return 0;
        t->valptr[l] = k;
        t->mincode[l] = code;
        k += cnt;
        code += cnt;
        t->maxcode[l] = cnt ? code - 1 : -1;
        code <<= 1;
    }
    t->nvals = k;

    for (int i = 0; i < JLUT_SIZE; i++) t->lut[i] = 0;
    code = 0; k = 0;
    for (int l = 1; l <= 16; l++) {
        int cnt = bits[l - 1];
        for (int c = 0; c < cnt; c++, k++, code++) {
            if (l <= JLUT_BITS) {
                int shift = JLUT_BITS - l;
                int lo = code << shift;
                for (int i = 0; i < (1 << shift); i++)
                    t->lut[lo + i] = (UINT16)((l << 8) | vals[k]);
            }
        }
        code <<= 1;
    }
    return 1;
}

static int jhuff(jbits_t *b, const UINT8 *vals, const jhtab_t *t) {
    if (b->nbits < JLUT_BITS) jb_refill(b);
    if (b->nbits >= JLUT_BITS) {
        UINT32 peek = (b->acc >> (b->nbits - JLUT_BITS)) & (JLUT_SIZE - 1);
        UINT16 e = t->lut[peek];
        if (e) {
            b->nbits -= (int)(e >> 8);
            return (int)(e & 0xFF);
        }
    }
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        UINT32 bit;
        if (!jb_get(b, 1, &bit)) return -1;
        code = (code << 1) | (int)bit;
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l]) {
            int i = t->valptr[l] + code - t->mincode[l];
            if (i < 0 || i >= t->nvals) return -1;
            return vals[i];
        }
    }
    return -1;
}

static int jrecv(jbits_t *b, int s, int *out) {
    if (s == 0) { *out = 0; return 0; }
    UINT32 raw;
    if (!jb_get(b, s, &raw)) return -1;
    int v = (int)raw;
    if (v < (1 << (s - 1))) v -= (1 << s) - 1;
    *out = v;
    return 0;
}

static void jidct2(const INT32 *block, INT64 *tmp, UINT8 *out,
                   int nrows, int ncols) {
    for (int i = 0; i < 8; i++) {
        const INT32 *mi = jidct_m[i];
        INT64 *tr = tmp + i * 8;
        for (int j = 0; j < ncols; j++) {
            INT64 s = 0;
            for (int k = 0; k < nrows; k++)
                s += (INT64)mi[k] * block[k * 8 + j];
            tr[j] = (s + 4096) >> 13;
        }
    }
    for (int i = 0; i < 8; i++) {
        const INT64 *tr = tmp + i * 8;
        UINT8 *orow = out + i * 8;
        for (int j = 0; j < 8; j++) {
            const INT32 *mj = jidct_m[j];
            INT64 s = 0;
            for (int k = 0; k < ncols; k++)
                s += (INT64)mj[k] * tr[k];
            s = (s + 4096) >> 13;
            int v = (int)((s + 2) >> 2) + 128;
            orow[j] = (UINT8)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

static UINT8 jidct_flat(INT32 dc) {
    INT64 t = ((INT64)5793 * dc + 4096) >> 13;
    INT64 s = ((INT64)5793 * t + 4096) >> 13;
    int v = (int)((s + 2) >> 2) + 128;
    return (UINT8)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void jblock_store(mjpeg_t *st, int c, int bx, int by, const UINT8 *out) {
    UINT8 *plane = st->plane[c];
    UINTN cw = st->cw[c], ch = st->ch[c];
    int sh = st->shift;

    if (sh == 0) {
        UINTN py0 = (UINTN)by * 8;
        UINTN px0 = (UINTN)bx * 8;
        UINTN ny = ch - py0 < 8 ? ch - py0 : 8;
        UINTN nx = cw - px0 < 8 ? cw - px0 : 8;
        if (py0 >= ch || px0 >= cw) return;
        for (UINTN y = 0; y < ny; y++) {
            UINT8 *d = plane + (py0 + y) * cw + px0;
            const UINT8 *s = out + y * 8;
            for (UINTN x = 0; x < nx; x++) d[x] = s[x];
        }
        return;
    }

    UINTN bs = (UINTN)8 >> sh;
    UINTN step = (UINTN)1 << sh;
    UINTN norm = step * step;
    UINTN py0 = (UINTN)by * bs;
    UINTN px0 = (UINTN)bx * bs;
    if (py0 >= ch || px0 >= cw) return;
    UINTN ny = ch - py0 < bs ? ch - py0 : bs;
    UINTN nx = cw - px0 < bs ? cw - px0 : bs;
    for (UINTN y = 0; y < ny; y++) {
        UINT8 *d = plane + (py0 + y) * cw + px0;
        for (UINTN x = 0; x < nx; x++) {
            UINTN sum = 0;
            for (UINTN sy = 0; sy < step; sy++) {
                const UINT8 *s = out + (y * step + sy) * 8 + x * step;
                for (UINTN sx = 0; sx < step; sx++) sum += s[sx];
            }
            d[x] = (UINT8)((sum + norm / 2) / norm);
        }
    }
}

static void jblock_fill(mjpeg_t *st, int c, int bx, int by, UINT8 u) {
    UINT8 *plane = st->plane[c];
    UINTN cw = st->cw[c], ch = st->ch[c];
    UINTN bs = (UINTN)8 >> st->shift;
    UINTN py0 = (UINTN)by * bs;
    UINTN px0 = (UINTN)bx * bs;
    if (py0 >= ch || px0 >= cw) return;
    UINTN ny = ch - py0 < bs ? ch - py0 : bs;
    UINTN nx = cw - px0 < bs ? cw - px0 : bs;
    for (UINTN y = 0; y < ny; y++) {
        UINT8 *d = plane + (py0 + y) * cw + px0;
        for (UINTN x = 0; x < nx; x++) d[x] = u;
    }
}

static int jdec_block(jbits_t *b, mjpeg_t *st, int c,
                      int dc_t, int ac_t, int bx, int by) {
    INT32 *block = st->blk;
    UINT8 *widx = st->blkidx;
    int nw = 0;
    int tq = st->comp_tq[c];
    const UINT16 *q = st->qtab[tq];

    int r = jhuff(b, st->dc_val[dc_t], &st->htab[dc_t]);
    if (r < 0) return 0;
    int diff;
    if (jrecv(b, r, &diff) < 0) return 0;
    st->dcpred[c] += diff;

    INT32 dc = st->dcpred[c] * (INT32)q[0];
    int maxrow = 0, maxcol = 0;

    int k = 1;
    while (k < 64) {
        r = jhuff(b, st->ac_val[ac_t], &st->htab[4 + ac_t]);
        if (r < 0) { for (int i = 0; i < nw; i++) block[widx[i]] = 0; return 0; }
        int run = r >> 4, ssz = r & 15;
        if (ssz == 0) {
            if (run == 0) break;
            if (run == 15) { k += 16; continue; }
            for (int i = 0; i < nw; i++) block[widx[i]] = 0;
            return 0;
        }
        k += run;
        if (k >= 64) { for (int i = 0; i < nw; i++) block[widx[i]] = 0; return 0; }
        int v;
        if (jrecv(b, ssz, &v) < 0) {
            for (int i = 0; i < nw; i++) block[widx[i]] = 0;
            return 0;
        }
        int zz = jzig[k];
        block[zz] = v * (INT32)q[k];
        widx[nw++] = (UINT8)zz;
        int rr = zz >> 3, cc = zz & 7;
        if (rr > maxrow) maxrow = rr;
        if (cc > maxcol) maxcol = cc;
        k++;
    }

    if (nw == 0) {
        jblock_fill(st, c, bx, by, jidct_flat(dc));
        return 1;
    }

    block[0] = dc;
    UINT8 out[64];
    jidct2(block, st->tmp, out, maxrow + 1, maxcol + 1);
    jblock_store(st, c, bx, by, out);

    block[0] = 0;
    for (int i = 0; i < nw; i++) block[widx[i]] = 0;
    return 1;
}

static int jpeg_dht(mjpeg_t *st, const UINT8 *p, UINTN plen) {
    UINTN off = 0;
    while (off < plen) {
        if (off + 17 > plen) return 0;
        UINT8 info = p[off++];
        int tc = info >> 4, th = info & 15;
        if (th > 3) return 0;

        if (tc > 1) return 0;
        UINT8 *bits = tc ? st->ac_bits[th] : st->dc_bits[th];
        UINT8 *vals = tc ? st->ac_val[th] : st->dc_val[th];
        if (tc) st->ac_have[th] = 1; else st->dc_have[th] = 1;
        int nv = 0;
        for (int i = 0; i < 16; i++) { bits[i] = p[off++]; nv += bits[i]; }
        if (nv > 256 || off + nv > plen) return 0;
        for (int i = 0; i < nv; i++) vals[i] = p[off++];
        if (!jhuff_build(bits, vals, &st->htab[tc * 4 + th])) return 0;
    }
    return 1;
}

static int jpeg_dqt(mjpeg_t *st, const UINT8 *p, UINTN plen) {
    UINTN off = 0;
    while (off < plen) {
        if (off + 1 > plen) return 0;
        UINT8 info = p[off++];
        int pq = info >> 4, tq = info & 15;
        if (tq > 3) return 0;
        int n = pq ? 128 : 64;
        if (off + n > plen) return 0;
        if (pq == 0) {
            for (int i = 0; i < 64; i++) st->qtab[tq][i] = p[off + i];
        } else {
            for (int i = 0; i < 64; i++)
                st->qtab[tq][i] = (UINT16)((p[off + 2 * i] << 8) | p[off + 2 * i + 1]);
        }
        off += n;
        st->qn[tq] = 1;
    }
    return 1;
}

static UINTN jpeg_parse_sos(mjpeg_t *st, const UINT8 *d, UINTN size, UINTN pos) {
    if (pos + 2 > size) return 0;
    UINTN len = ((UINTN)d[pos] << 8) | d[pos + 1];
    if (len < 6 || pos + len > size) return 0;
    UINTN p = pos + 2;
    int ns = d[p];
    if (ns < 1 || ns > st->ncomp) return 0;
    if (len < (UINTN)(6 + 2 * ns)) return 0;
    st->scan_n = ns;
    for (int i = 0; i < ns; i++) {
        UINT8 cid = d[p + 1 + 2 * i];
        UINT8 sel = d[p + 1 + 2 * i + 1];
        int c = -1;
        for (int j = 0; j < st->ncomp; j++)
            if (st->comp_id[j] == cid) { c = j; break; }
        if (c < 0) return 0;
        st->scan_comp[i] = c;
        st->scan_dc[i] = sel >> 4;
        st->scan_ac[i] = sel & 15;
        if (st->scan_dc[i] > 3 || st->scan_ac[i] > 3) return 0;
    }
    return p + 1 + 2 * ns + 3;
}

static int jb_sync_restart(jbits_t *b) {
    b->acc = 0; b->nbits = 0; b->marker = 0;
    while (b->pos + 1 < b->size) {
        if (b->p[b->pos] == 0xFF) {
            UINT8 n = b->p[b->pos + 1];
            if (n >= 0xD0 && n <= 0xD7) { b->pos += 2; return 1; }
            if (n != 0x00) return 0;
        }
        b->pos++;
    }
    return 0;
}

static UINTN jpeg_parse_header(mjpeg_t *st, const UINT8 *d, UINTN size) {
    UINTN pos = 0;
    while (pos + 1 < size && !(d[pos] == 0xFF && d[pos + 1] == 0xD8)) pos++;
    if (pos + 1 >= size) return 0;
    pos += 2;

    st->ri = 0;
    st->ncomp = 0;
    for (int i = 0; i < 4; i++) {
        st->qn[i] = 0;
        st->dc_have[i] = 0;
        st->ac_have[i] = 0;
    }

    for (;;) {
        while (pos + 1 < size && d[pos] == 0xFF && d[pos + 1] == 0xFF) pos++;
        if (pos + 1 >= size || d[pos] != 0xFF || d[pos + 1] == 0x00) return 0;
        UINT8 m = d[pos + 1];
        pos += 2;

        if (m == 0xD9) return 0;
        if (m == 0xDA) return jpeg_parse_sos(st, d, size, pos);
        if ((m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xD8) continue;

        if (pos + 2 > size) return 0;
        UINTN len = ((UINTN)d[pos] << 8) | d[pos + 1];
        if (len < 2 || pos + len > size) return 0;
        const UINT8 *p = d + pos + 2;
        UINTN plen = len - 2;

        switch (m) {
        case 0xC0:
        case 0xC1:
            if (plen < 6 || p[0] != 8) return 0;
            {
                UINTN H = rd16(p + 1), W = rd16(p + 3);
                if (W == 0 || H == 0 || W > MP4_MAX_DIM || H > MP4_MAX_DIM) return 0;
                int nf = p[5];
                if (nf < 1 || nf > 3) return 0;
                if (plen < (UINTN)(6 + 3 * nf)) return 0;
                st->W = W;
                st->H = H;
                st->ncomp = nf;
                st->max_h = st->max_v = 0;
                for (int i = 0; i < nf; i++) {
                    const UINT8 *cp = p + 6 + 3 * i;
                    st->comp_id[i] = cp[0];
                    st->comp_h[i] = cp[1] >> 4;
                    st->comp_v[i] = cp[1] & 15;
                    st->comp_tq[i] = cp[2];
                    if (st->comp_h[i] == 0 || st->comp_h[i] > 4 ||
                        st->comp_v[i] == 0 || st->comp_v[i] > 4) return 0;

                    if (st->comp_tq[i] > 3) return 0;
                    if (st->comp_h[i] > st->max_h) st->max_h = st->comp_h[i];
                    if (st->comp_v[i] > st->max_v) st->max_v = st->comp_v[i];
                }
                for (int i = 0; i < nf; i++) {
                    UINTN s = (UINTN)1 << st->shift;
                    UINTN fw = (W * st->comp_h[i] + st->max_h - 1) / st->max_h;
                    UINTN fh = (H * st->comp_v[i] + st->max_v - 1) / st->max_v;
                    st->cw[i] = (fw + s - 1) / s;
                    st->ch[i] = (fh + s - 1) / s;
                }
                {
                    UINTN s = (UINTN)1 << st->shift;
                    st->tgt_w = (W + s - 1) / s;
                    st->tgt_h = (H + s - 1) / s;
                }
            }
            break;
        case 0xC2:
        case 0xC6:
        case 0xCA:
        case 0xCB:
            return 0;
        case 0xC4:
            if (!jpeg_dht(st, p, plen)) return 0;
            break;
        case 0xDB:
            if (!jpeg_dqt(st, p, plen)) return 0;
            break;
        case 0xDD:
            if (plen < 2) return 0;
            st->ri = rd16(p);
            break;
        default:
            break;
        }
        pos += len;
    }
}

static int jpeg_decode_scan(mjpeg_t *st, const UINT8 *d, UINTN size, UINTN scan_off) {
    jbits_t b;
    jb_reset(&b, d, size, scan_off);

    for (int c = 0; c < 4; c++) st->dcpred[c] = 0;

    UINTN mw = (UINTN)st->max_h * 8;
    UINTN mh = (UINTN)st->max_v * 8;
    UINTN mcus_x = (st->W + mw - 1) / mw;
    UINTN mcus_y = (st->H + mh - 1) / mh;
    int done = 0;
    UINTN mcu_idx = 0;

    for (UINTN my = 0; my < mcus_y && !done; my++) {
        for (UINTN mx = 0; mx < mcus_x && !done; mx++) {
            if (st->ri > 0 && mcu_idx > 0 && mcu_idx % (UINTN)st->ri == 0) {
                if (!jb_sync_restart(&b)) { done = 1; break; }
                for (int c = 0; c < 4; c++) st->dcpred[c] = 0;
            }
            for (int ci = 0; ci < st->scan_n; ci++) {
                int c = st->scan_comp[ci];
                for (int v = 0; v < st->comp_v[c] && !done; v++) {
                    for (int h = 0; h < st->comp_h[c] && !done; h++) {
                        int bx = (int)(mx * (UINTN)st->comp_h[c] + h);
                        int by = (int)(my * (UINTN)st->comp_v[c] + v);
                        if (!jdec_block(&b, st, c, st->scan_dc[ci], st->scan_ac[ci],
                                        bx, by)) {
                            done = 1;
                        }
                    }
                }
                if (done) break;
            }
            mcu_idx++;
        }
    }

    return 1;
}

static void jpeg_composite(mjpeg_t *st, UINT32 *canvas) {
    UINTN W = st->tgt_w, H = st->tgt_h;

    if (st->ncomp == 1) {
        UINT8 *y = st->plane[0];
        for (UINTN i = 0; i < W * H; i++) {
            UINT8 g = y[i];
            canvas[i] = 0xFF000000u | ((UINT32)g << 16) | ((UINT32)g << 8) | g;
        }
        return;
    }

    const UINT8 *y = st->plane[0];
    const UINT8 *cb = st->plane[1];
    const UINT8 *cr = st->plane[2];
    const UINT8 *cl = st->clamp + 256;
    UINTN cw1 = st->cw[1], ch1 = st->ch[1];
    UINTN cw2 = st->cw[2], ch2 = st->ch[2];
    int h1 = st->comp_h[1], v1 = st->comp_v[1];
    int h2 = st->comp_h[2], v2 = st->comp_v[2];
    int grouped = (h1 == h2 && st->max_h % h1 == 0);
    UINTN g = grouped ? (UINTN)(st->max_h / h1) : 1;

    for (UINTN yy = 0; yy < H; yy++) {
        UINTN sy1 = yy * (UINTN)v1 / (UINTN)st->max_v;
        if (sy1 >= ch1) sy1 = ch1 - 1;
        UINTN sy2 = yy * (UINTN)v2 / (UINTN)st->max_v;
        if (sy2 >= ch2) sy2 = ch2 - 1;
        const UINT8 *crow1 = cb + sy1 * cw1;
        const UINT8 *crow2 = cr + sy2 * cw2;
        UINT32 *drow = canvas + yy * W;
        const UINT8 *yrow = y + yy * W;

        if (grouped) {
            UINTN xx = 0, sx = 0;
            while (xx < W) {
                UINTN s1 = sx < cw1 ? sx : cw1 - 1;
                UINTN s2 = sx < cw2 ? sx : cw2 - 1;
                int Cb = (int)crow1[s1] - 128;
                int Cr = (int)crow2[s2] - 128;
                int ro = (1436 * Cr) >> 10;
                int go = -((352 * Cb + 731 * Cr) >> 10);
                int bo = (1815 * Cb) >> 10;
                UINTN end = xx + g;
                if (end > W) end = W;
                for (; xx < end; xx++) {
                    int Y = yrow[xx];
                    drow[xx] = 0xFF000000u | ((UINT32)cl[Y + ro] << 16) |
                               ((UINT32)cl[Y + go] << 8) | cl[Y + bo];
                }
                sx++;
            }
        } else {
            const UINTN *m1 = st->sxmap[1], *m2 = st->sxmap[2];
            for (UINTN xx = 0; xx < W; xx++) {
                int Y = yrow[xx];
                int Cb = (int)crow1[m1[xx]] - 128;
                int Cr = (int)crow2[m2[xx]] - 128;
                drow[xx] = 0xFF000000u |
                    ((UINT32)cl[Y + ((1436 * Cr) >> 10)] << 16) |
                    ((UINT32)cl[Y - ((352 * Cb + 731 * Cr) >> 10)] << 8) |
                    cl[Y + ((1815 * Cb) >> 10)];
            }
        }
    }
}

static void jpeg_lock_geometry(mjpeg_t *st) {
    st->lock_ncomp = st->ncomp;
    for (int i = 0; i < 4; i++) {
        st->lock_cw[i] = st->cw[i];
        st->lock_ch[i] = st->ch[i];
    }
    st->locked = 1;
}

static int jpeg_frame(mjpeg_t *st, const UINT8 *d, UINTN size, UINT32 *canvas) {
    UINTN scan_off = jpeg_parse_header(st, d, size);
    if (!scan_off) return 0;
    if (st->locked) {
        if (st->ncomp != st->lock_ncomp) return 0;
        for (int i = 0; i < st->ncomp; i++)
            if (st->cw[i] != st->lock_cw[i] || st->ch[i] != st->lock_ch[i])
                return 0;
    }
    if (!jpeg_decode_scan(st, d, size, scan_off)) return 0;
    jpeg_composite(st, canvas);
    return 1;
}

typedef struct {
    const UINT8 *d;
    UINTN size;
    UINTN pos;
    UINTN end;
    UINT32 type;
} mp4box_t;

static int mp4_next(mp4box_t *it, mp4box_t *out) {
    if (it->pos + 8 > it->end) return 0;
    const UINT8 *p = it->d + it->pos;
    UINT64 sz = rd32(p);
    UINT32 type = rd32(p + 4);
    UINTN hdr = 8;
    if (sz == 1) {
        if (it->pos + 16 > it->end) return 0;
        sz = rd64(p + 8);
        hdr = 16;
    } else if (sz == 0) {
        sz = it->end - it->pos;
    }
    if (sz < hdr || it->pos + sz > it->end) return 0;
    out->d = it->d;
    out->size = (UINTN)sz;
    out->pos = it->pos + hdr;
    out->end = it->pos + (UINTN)sz;
    out->type = type;
    it->pos += (UINTN)sz;
    return 1;
}

static int mp4_find_child(const mp4box_t *parent, UINT32 want, mp4box_t *out) {
    mp4box_t it = *parent;
    mp4box_t b;
    while (mp4_next(&it, &b))
        if (b.type == want) { *out = b; return 1; }
    return 0;
}

static int mp4_samples(mp4box_t *stbl, UINT32 timescale, mjpeg_t *st) {
    mp4box_t stsz = {0}, stts = {0}, stsc = {0}, stco = {0}, co64 = {0};
    if (!mp4_find_child(stbl, 0x7374737Au, &stsz) || stsz.size < 12) return 0;
    if (!mp4_find_child(stbl, 0x73747473u, &stts)) return 0;
    if (!mp4_find_child(stbl, 0x73747363u, &stsc)) return 0;
    int have_stco = mp4_find_child(stbl, 0x7374636Fu, &stco);
    int have_co64 = mp4_find_child(stbl, 0x636F3634u, &co64);
    if (!have_stco && !have_co64) return 0;

    const UINT8 *ps = stsz.d + stsz.pos;
    UINT32 sample_size = rd32(ps + 4);
    UINT32 sample_count = rd32(ps + 8);
    if (sample_count == 0 || sample_count > MP4_MAX_SAMPLES) return 0;
    const UINT8 *sztab = (sample_size == 0) ? ps + 12 : NULL;
    if (sample_size == 0 && stsz.size < 12 + (UINTN)sample_count * 4) return 0;

    const UINT8 *pt = stts.d + stts.pos;
    UINT32 stts_n = rd32(pt + 4);
    if (stts.size < 8 + (UINTN)stts_n * 8) return 0;

    const UINT8 *pc = stsc.d + stsc.pos;
    UINT32 stsc_n = rd32(pc + 4);
    if (stsc.size < 8 + (UINTN)stsc_n * 12) return 0;

    const UINT8 *po = have_stco ? (stco.d + stco.pos) : (co64.d + co64.pos);
    UINT32 chunk_n = rd32(po + 4);
    UINTN chunk_bytes = have_stco ? (UINTN)chunk_n * 4 : (UINTN)chunk_n * 8;
    if (have_stco ? (stco.size < 8 + chunk_bytes)
                  : (co64.size < 8 + chunk_bytes)) return 0;

    st->n = sample_count;
    st->off = efi_allocate_pool(sample_count * sizeof(UINTN));
    st->len = efi_allocate_pool(sample_count * sizeof(UINT32));
    st->ms = efi_allocate_pool(sample_count * sizeof(UINT32));
    if (!st->off || !st->len || !st->ms) return 0;

    UINTN idx = 0;
    UINT32 chunk = 1;
    UINT32 stts_i = 0, stts_left = 0, stts_delta = 0;

    for (UINT32 e = 0; e < stsc_n && idx < sample_count; e++) {
        const UINT8 *se = pc + 8 + e * 12;
        UINT32 first_chunk = rd32(se);
        UINT32 spc = rd32(se + 4);
        UINT32 next_first = (e + 1 < stsc_n) ? rd32(se + 12) : (chunk_n + 1);
        if (spc == 0) continue;
        if (first_chunk < chunk) first_chunk = chunk;

        for (UINT32 c = first_chunk; c < next_first && idx < sample_count; c++) {
            if (c > chunk_n) break;
            UINT64 base = have_stco ? rd32(po + 8 + (UINTN)(c - 1) * 4)
                                    : rd64(po + 8 + (UINTN)(c - 1) * 8);
            for (UINT32 s = 0; s < spc && idx < sample_count; s++) {
                UINT32 len = sample_size ? sample_size : rd32(sztab + idx * 4);
                st->off[idx] = (UINTN)base;
                st->len[idx] = len;

                while (stts_left == 0) {
                    if (stts_i >= stts_n) { stts_delta = 0; break; }
                    const UINT8 *te = pt + 8 + stts_i * 8;
                    stts_left = rd32(te);
                    stts_delta = rd32(te + 4);
                    stts_i++;
                }
                UINT32 ms;
                if (stts_left > 0) {
                    ms = (UINT32)(((UINT64)stts_delta * 1000 + timescale / 2) / timescale);
                    stts_left--;
                } else {
                    ms = 100;
                }
                if (ms < 1) ms = 1;
                if (ms > 60000) ms = 60000;
                st->ms[idx] = ms;

                base += len;
                idx++;
            }
            chunk++;
        }
    }

    return idx == sample_count;
}

static int mp4_codec_ok(mp4box_t *stbl) {
    mp4box_t stsd;
    if (!mp4_find_child(stbl, 0x73747364u, &stsd) || stsd.size < 16) return 0;
    const UINT8 *p = stsd.d + stsd.pos;
    UINT32 n = rd32(p + 4);
    if (n == 0) return 0;
    UINT32 codec = rd32(p + 12);
    if (codec == 0x6A706567u || codec == 0x6D6A7061u || codec == 0x6D6A7062u ||
        codec == 0x6D703476u)
        return 1;
    if (codec == 0x61766331u || codec == 0x68657631u || codec == 0x68766331u)
        efi_log(L"  ERROR: MP4 holds H.26x video - convert to MJPEG first (ffmpeg -c:v mjpeg)");
    return 0;
}

static void mjpeg_state_free(mjpeg_t *st) {
    if (!st) return;
    if (st->off) efi_free_pool(st->off);
    if (st->len) efi_free_pool(st->len);
    if (st->ms)  efi_free_pool(st->ms);
    if (st->plane[0]) efi_free_pool(st->plane[0]);
    for (int i = 0; i < 4; i++)
        if (st->sxmap[i]) efi_free_pool(st->sxmap[i]);
    efi_free_pool(st);
}

anim_t* mp4_load(UINT8 *data, UINTN size, UINTN tgt_w, UINTN tgt_h) {
    if (size < 24 || size > MP4_MAX_FILE) {
        efi_log(L"  ERROR: MP4 file too small or too large");
        efi_free_pool(data);
        return NULL;
    }
    if (rd32(data + 4) != 0x66747970u) {
        efi_log(L"  ERROR: bad MP4 signature");
        efi_free_pool(data);
        return NULL;
    }

    anim_t *a = efi_allocate_pool(sizeof(anim_t));
    if (!a) { efi_free_pool(data); return NULL; }
    ZeroMem(a, sizeof(anim_t));

    mjpeg_t *st = efi_allocate_pool(sizeof(mjpeg_t));
    if (!st) { efi_free_pool(data); efi_free_pool(a); return NULL; }
    ZeroMem(st, sizeof(mjpeg_t));
    a->mj = st;
    a->codec = 1;
    a->data = data;
    a->size = size;

    for (int i = 0; i < 768; i++) {
        int v = i - 256;
        st->clamp[i] = (UINT8)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    mp4box_t top;
    top.d = data; top.size = size; top.pos = 0; top.end = size; top.type = 0;
    mp4box_t moov;
    if (!mp4_find_child(&top, 0x6D6F6F76u, &moov)) {
        efi_log(L"  ERROR: MP4 has no moov box");
        goto fail;
    }

    UINT32 timescale = 1000;
    mp4box_t mvhd;
    if (mp4_find_child(&moov, 0x6D766864u, &mvhd)) {
        const UINT8 *vp = mvhd.d + mvhd.pos;
        UINTN avail = mvhd.end - mvhd.pos;
        UINT32 ts = 0;
        if (avail >= 16 && vp[0] != 1) ts = rd32(vp + 12);
        else if (avail >= 24) ts = rd32(vp + 20);
        if (ts) timescale = ts;
    }

    mp4box_t it = moov;
    mp4box_t trak;
    int found = 0;
    while (mp4_next(&it, &trak)) {
        if (trak.type != 0x7472616Bu) continue;
        mp4box_t mdia;
        if (!mp4_find_child(&trak, 0x6D646961u, &mdia)) continue;
        UINT32 track_ts = timescale;
        mp4box_t mdhd;
        if (mp4_find_child(&mdia, 0x6D646864u, &mdhd)) {
            const UINT8 *mp = mdhd.d + mdhd.pos;
            UINTN avail = mdhd.end - mdhd.pos;
            UINT32 ts = 0;
            if (avail >= 16 && mp[0] != 1) ts = rd32(mp + 12);
            else if (avail >= 24) ts = rd32(mp + 20);
            if (ts) track_ts = ts;
        }
        mp4box_t hdlr;
        int is_video = 0;
        if (mp4_find_child(&mdia, 0x68646C72u, &hdlr) && hdlr.size >= 12)
            is_video = (rd32(hdlr.d + hdlr.pos + 8) == 0x76696465u);
        mp4box_t minf;
        if (!mp4_find_child(&mdia, 0x6D696E66u, &minf)) continue;
        mp4box_t stbl;
        if (!mp4_find_child(&minf, 0x7374626Cu, &stbl)) continue;
        if (!mp4_codec_ok(&stbl)) continue;
        if (!is_video) {
            mp4box_t chk = moov;
            mp4box_t t2;
            int traks = 0;
            while (mp4_next(&chk, &t2))
                if (t2.type == 0x7472616Bu) traks++;
            if (traks > 1) continue;
        }
        if (!mp4_samples(&stbl, track_ts, st)) continue;
        found = 1;
        break;
    }
    if (!found || st->n == 0) {
        efi_log(L"  ERROR: MP4 has no decodable MJPEG video track");
        goto fail;
    }

    for (UINTN i = 0; i < st->n; i++) {
        if (st->len[i] == 0 || st->off[i] + st->len[i] > size) {
            efi_log(L"  ERROR: MP4 sample table out of range");
            goto fail;
        }
    }

    {
        UINTN scan_off = jpeg_parse_header(st, a->data + st->off[0], st->len[0]);
        if (!scan_off) {
            efi_log(L"  ERROR: first MJPEG frame unreadable");
            goto fail;
        }
    }

    if (st->W == 0 || st->H == 0 ||
        st->W > MP4_MAX_DIM || st->H > MP4_MAX_DIM) {
        efi_log(L"  ERROR: bad MJPEG frame dimensions");
        goto fail;
    }

    st->shift = 0;
    if (tgt_w && tgt_h) {
        while (st->shift < 3) {
            UINTN s = (UINTN)2 << st->shift;
            if ((st->W + s - 1) / s < tgt_w) break;
            if ((st->H + s - 1) / s < tgt_h) break;
            st->shift++;
        }
    }
    {
        UINTN s = (UINTN)1 << st->shift;
        st->tgt_w = (st->W + s - 1) / s;
        st->tgt_h = (st->H + s - 1) / s;
        for (int i = 0; i < st->ncomp; i++) {
            UINTN fw = (st->W * st->comp_h[i] + st->max_h - 1) / st->max_h;
            UINTN fh = (st->H * st->comp_v[i] + st->max_v - 1) / st->max_v;
            st->cw[i] = (fw + s - 1) / s;
            st->ch[i] = (fh + s - 1) / s;
        }
    }

    UINTN px = st->tgt_w * st->tgt_h;
    if (px > MP4_MAX_PIXELS || px == 0) {
        efi_log(L"  ERROR: MJPEG frame too large");
        goto fail;
    }

    a->width = st->tgt_w;
    a->height = st->tgt_h;

    a->canvas = efi_allocate_pool(px * sizeof(UINT32));
    if (!a->canvas) goto fail;
    ZeroMem(a->canvas, px * sizeof(UINT32));

    UINTN comp_bytes = 0;
    for (int i = 0; i < st->ncomp; i++) {
        UINTN cb = st->cw[i] * st->ch[i];
        if (cb > MP4_MAX_PIXELS) goto fail;
        comp_bytes += cb;
    }
    UINT8 *comp = efi_allocate_pool(comp_bytes);
    if (!comp) goto fail;
    ZeroMem(comp, comp_bytes);
    UINTN acc = 0;
    for (int i = 0; i < st->ncomp; i++) {
        st->plane[i] = comp + acc;
        acc += st->cw[i] * st->ch[i];
    }
    jpeg_lock_geometry(st);

    for (int i = 1; i < st->ncomp && i < 3; i++) {
        st->sxmap[i] = efi_allocate_pool(st->tgt_w * sizeof(UINTN));
        if (!st->sxmap[i]) goto fail;
        for (UINTN x = 0; x < st->tgt_w; x++) {
            UINTN v = x * (UINTN)st->comp_h[i] / (UINTN)st->max_h;
            if (v >= st->cw[i]) v = st->cw[i] - 1;
            st->sxmap[i][x] = v;
        }
    }

    {
        UINTN scan_off = jpeg_parse_header(st, a->data + st->off[0], st->len[0]);
        if (!scan_off) goto fail;
        if (!jpeg_decode_scan(st, a->data + st->off[0], st->len[0], scan_off))
            goto fail;
        jpeg_composite(st, a->canvas);
    }

    a->frame_count = st->n;
    a->loops = 0;
    a->cur = 0;
    st->cur = 0;
    a->cur_delay = st->ms[0];

    {
        CHAR16 msg[160];
        SPrint(msg, sizeof(msg), L"  mp4: %dx%d (video %dx%d, 1/%d), %d frames, %d ms, %d KB resident",
               (int)st->tgt_w, (int)st->tgt_h, (int)st->W, (int)st->H,
               (int)(1 << st->shift), (int)st->n, (int)st->ms[0],
               (int)((size + px * 4 + comp_bytes) / 1024));
        efi_log(msg);
    }
    return a;

fail:
    efi_log(L"  ERROR: MJPEG video failed to load");
    anim_free(a);
    return NULL;
}

static int vbg_advance(anim_t *a);

static int mjpeg_advance_n(anim_t *a, UINTN n) {
    mjpeg_t *st = a->mj;
    if (!st || st->n < 2) return 0;

    UINTN idx = st->cur;
    for (UINTN i = 0; i < n; i++) {
        idx++;
        if (idx >= st->n) {
            if (a->loops && a->loops_done + 1 >= a->loops) return 0;
            a->loops_done++;
            idx = 0;
        }
    }
    if (idx >= st->n) return 0;
    if (st->off[idx] + st->len[idx] > a->size) return 0;

    if (!jpeg_frame(st, a->data + st->off[idx], st->len[idx], a->canvas)) {
        efi_log(L"  WARN: MJPEG frame decode failed, holding last frame");
    }

    st->cur = idx;
    a->cur = idx;
    a->cur_delay = st->ms[idx];
    return 1;
}

int anim_advance(anim_t *a) {
    if (!a) return 0;
    if (a->codec == 1) return mjpeg_advance_n(a, 1);
    if (a->codec == 2) return vbg_advance(a);
    return gif_advance(a);
}

int anim_advance_n(anim_t *a, UINTN n) {
    if (!a || n == 0) return 0;
    if (a->codec == 1) return mjpeg_advance_n(a, n);
    if (a->codec == 2) {
        UINTN done = 0;
        for (UINTN i = 0; i < n; i++) {
            if (!vbg_advance(a)) break;
            done++;
        }
        return done > 0;
    }
    UINTN done = 0;
    for (UINTN i = 0; i < n; i++) {
        if (!gif_advance(a)) break;
        done++;
    }
    return done > 0;
}

#define VBG_MAX_FRAMES   16384
#define VBG_TILE_LOG2_MIN 2
#define VBG_TILE_LOG2_MAX 6
#define VBG_FLAG_MC       1u

static void jpeg_planes_free(mjpeg_t *st) {
    if (st->plane[0]) efi_free_pool(st->plane[0]);
    st->plane[0] = NULL;
    for (int i = 0; i < 4; i++) {
        if (st->sxmap[i]) efi_free_pool(st->sxmap[i]);
        st->sxmap[i] = NULL;
    }
}

typedef struct {
    mjpeg_t j;
    UINT8  *scratch;
    UINTN   scratch_sz;
    UINT32 *ref;
    UINTN   cur, n;
    UINTN   next_off, first_off;
    UINT32  delay_ms;
    UINT32  flags;
} vbg_t;

static int vbg_read_frame(const UINT8 *data, UINTN size,
                          UINTN off, UINT8 *type, UINT32 *fsize,
                          const UINT8 **payload) {
    if (off + 5 > size) return 0;
    *type = data[off];
    UINT32 sz = (UINT32)data[off + 1] | ((UINT32)data[off + 2] << 8) |
                ((UINT32)data[off + 3] << 16) | ((UINT32)data[off + 4] << 24);
    if (off + 5 + sz > size) return 0;
    *fsize = sz;
    *payload = data + off + 5;
    return 1;
}

static void vbg_free(anim_t *a) {
    vbg_t *v = a->mj;
    if (!v) return;
    if (v->scratch) efi_free_pool(v->scratch);
    if (v->ref) efi_free_pool(v->ref);
    jpeg_planes_free(&v->j);
    efi_free_pool(v);
    a->mj = NULL;
}

static void vbg_apply_delta(anim_t *a, const UINT8 *res) {
    UINTN px = a->width * a->height;
    UINT32 *c = a->canvas;
    for (UINTN i = 0; i < px; i++) {
        INTN r = (INTN)((c[i] >> 16) & 0xFF) + (INTN)(INT8)res[3 * i + 0];
        INTN g = (INTN)((c[i] >> 8) & 0xFF) + (INTN)(INT8)res[3 * i + 1];
        INTN b = (INTN)(c[i] & 0xFF) + (INTN)(INT8)res[3 * i + 2];
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;
        c[i] = 0xFF000000u | ((UINT32)r << 16) | ((UINT32)g << 8) | (UINT32)b;
    }
}

static int vbg_apply_tiled(anim_t *a, const UINT8 *payload, UINT32 fsize,
                           int with_mv) {
    vbg_t *v = a->mj;
    if (fsize < 6) return 0;

    UINT8  qshift = payload[0];
    UINT8  tlog   = payload[1];
    UINTN  ntx    = (UINTN)payload[2] | ((UINTN)payload[3] << 8);
    UINTN  nty    = (UINTN)payload[4] | ((UINTN)payload[5] << 8);

    if (qshift > 7) return 0;
    if (tlog < VBG_TILE_LOG2_MIN || tlog > VBG_TILE_LOG2_MAX) return 0;
    if (with_mv && !v->ref) return 0;

    UINTN W = a->width, H = a->height;
    UINTN t = (UINTN)1 << tlog;
    if (ntx != (W + t - 1) / t || nty != (H + t - 1) / t) return 0;
    if (ntx == 0 || nty == 0) return 0;

    UINTN ntiles  = ntx * nty;
    UINTN bmbytes = (ntiles + 7) / 8;
    if ((UINTN)fsize < 6 + bmbytes) return 0;

    const UINT8 *bm      = payload + 6;
    const UINT8 *comp    = payload + 6 + bmbytes;
    UINTN        comp_sz = (UINTN)fsize - 6 - bmbytes;

    UINTN tile_bytes = t * t * 3;
    UINTN nchanged   = 0;
    for (UINTN i = 0; i < ntiles; i++)
        if (bm[i >> 3] & (1u << (i & 7))) nchanged++;

    if (nchanged == 0) return 1;

    UINTN mv_bytes = with_mv ? nchanged * 2 : 0;
    UINTN want     = mv_bytes + nchanged * tile_bytes;
    if (want > v->scratch_sz) return 0;
    UINTN got = want;
    if (EFI_ERROR(png_decompress((UINT8 *)comp, comp_sz, v->scratch, &got)) ||
        got != want)
        return 0;

    const UINT8 *mvp = v->scratch;
    const UINT8 *s   = v->scratch + mv_bytes;
    UINT32      *can = a->canvas;
    UINT32      *src = can;

    if (with_mv) {
        UINTN bx0 = W, by0 = H, bx1 = 0, by1 = 0;
        UINTN k = 0;
        for (UINTN ty = 0; ty < nty; ty++) {
            UINTN y0 = ty * t;
            UINTN th = (y0 + t <= H) ? t : H - y0;
            for (UINTN tx = 0; tx < ntx; tx++) {
                UINTN i = ty * ntx + tx;
                if (!(bm[i >> 3] & (1u << (i & 7)))) continue;
                INTN  mvy = (INTN)(INT8)mvp[2 * k + 0];
                INTN  mvx = (INTN)(INT8)mvp[2 * k + 1];
                k++;
                UINTN x0 = tx * t;
                UINTN tw = (x0 + t <= W) ? t : W - x0;
                INTN  sy = (INTN)y0 + mvy;
                INTN  sx = (INTN)x0 + mvx;
                if (sy < 0 || sx < 0 ||
                    (UINTN)sy + th > H || (UINTN)sx + tw > W)
                    return 0;
                if ((UINTN)sy < by0) by0 = (UINTN)sy;
                if ((UINTN)sx < bx0) bx0 = (UINTN)sx;
                if ((UINTN)sy + th > by1) by1 = (UINTN)sy + th;
                if ((UINTN)sx + tw > bx1) bx1 = (UINTN)sx + tw;
            }
        }
        for (UINTN y = by0; y < by1; y++)
            CopyMem(v->ref + y * W + bx0, can + y * W + bx0,
                    (bx1 - bx0) * sizeof(UINT32));
        src = v->ref;
    }

    INTN  q = (INTN)1 << qshift;
    UINTN k = 0;

    for (UINTN ty = 0; ty < nty; ty++) {
        UINTN y0 = ty * t;
        UINTN th = (y0 + t <= H) ? t : H - y0;
        for (UINTN tx = 0; tx < ntx; tx++) {
            UINTN i = ty * ntx + tx;
            if (!(bm[i >> 3] & (1u << (i & 7)))) continue;

            UINTN x0 = tx * t;
            UINTN tw = (x0 + t <= W) ? t : W - x0;
            INTN  mvy = 0, mvx = 0;
            if (with_mv) {
                mvy = (INTN)(INT8)mvp[2 * k + 0];
                mvx = (INTN)(INT8)mvp[2 * k + 1];
            }
            k++;

            for (UINTN yy = 0; yy < th; yy++) {
                UINT32       *row = can + (y0 + yy) * W + x0;
                const UINT32 *pre = src + ((UINTN)((INTN)(y0 + yy) + mvy)) * W +
                                    (UINTN)((INTN)x0 + mvx);
                const UINT8  *rs  = s + yy * t * 3;
                for (UINTN xx = 0; xx < tw; xx++) {
                    UINT32 p = pre[xx];
                    INTN r = (INTN)((p >> 16) & 0xFF) + (INTN)(INT8)rs[3 * xx + 0] * q;
                    INTN g = (INTN)((p >> 8) & 0xFF)  + (INTN)(INT8)rs[3 * xx + 1] * q;
                    INTN b = (INTN)(p & 0xFF)         + (INTN)(INT8)rs[3 * xx + 2] * q;
                    if (r < 0) r = 0; else if (r > 255) r = 255;
                    if (g < 0) g = 0; else if (g > 255) g = 255;
                    if (b < 0) b = 0; else if (b > 255) b = 255;
                    row[xx] = 0xFF000000u | ((UINT32)r << 16) |
                              ((UINT32)g << 8) | (UINT32)b;
                }
            }
            s += tile_bytes;
        }
    }
    return 1;
}

static int vbg_advance(anim_t *a) {
    vbg_t *v = a->mj;
    if (!v || v->n < 2) return 0;

    UINT8 type; UINT32 fsize; const UINT8 *payload;
    if (!vbg_read_frame(a->data, a->size, v->next_off, &type, &fsize, &payload)) {
        v->next_off = v->first_off;
        if (!vbg_read_frame(a->data, a->size, v->next_off, &type, &fsize, &payload))
            return 0;
        if (type != 0) return 0;
        if (!jpeg_frame(&v->j, payload, fsize, a->canvas)) return 0;
        v->cur = 0;
    } else {

        int ok;
        if (type == 0) {
            ok = jpeg_frame(&v->j, payload, fsize, a->canvas);
        } else if (type == 1) {
            UINTN want = a->width * a->height * 3;
            UINTN got  = want;
            ok = (want <= v->scratch_sz) &&
                 !EFI_ERROR(png_decompress((UINT8 *)payload, fsize,
                                           v->scratch, &got)) &&
                 got == want;
            if (ok) vbg_apply_delta(a, v->scratch);
        } else if (type == 2) {
            ok = vbg_apply_tiled(a, payload, fsize, 0);
        } else if (type == 3) {
            ok = vbg_apply_tiled(a, payload, fsize, 1);
        } else {
            ok = 0;
        }
        if (!ok) efi_log(L"  WARN: VBG frame decode failed, holding last frame");
        v->cur++;
    }

    v->next_off += 5 + fsize;
    a->cur = v->cur;
    a->cur_delay = v->delay_ms;
    return 1;
}

anim_t* vbg_load(UINT8 *data, UINTN size) {
    if (!data || size < 32) { efi_free_pool(data); return NULL; }
    static const UINT8 magic[8] = {'V','I','S','O','R','V','B','G'};
    for (int i = 0; i < 8; i++)
        if (data[i] != magic[i]) { efi_free_pool(data); return NULL; }
    UINT32 version = (UINT32)data[8] | ((UINT32)data[9] << 8) |
                     ((UINT32)data[10] << 16) | ((UINT32)data[11] << 24);
    UINT32 w = (UINT32)data[12] | ((UINT32)data[13] << 8) |
               ((UINT32)data[14] << 16) | ((UINT32)data[15] << 24);
    UINT32 h = (UINT32)data[16] | ((UINT32)data[17] << 8) |
               ((UINT32)data[18] << 16) | ((UINT32)data[19] << 24);
    UINT32 n = (UINT32)data[20] | ((UINT32)data[21] << 8) |
               ((UINT32)data[22] << 16) | ((UINT32)data[23] << 24);
    UINT32 delay = (UINT32)data[24] | ((UINT32)data[25] << 8) |
                   ((UINT32)data[26] << 16) | ((UINT32)data[27] << 24);
    if ((version != 1 && version != 2 && version != 3) ||
        w == 0 || h == 0 || n == 0 ||
        n > VBG_MAX_FRAMES ||
        w > MP4_MAX_DIM || h > MP4_MAX_DIM ||
        (UINT64)w * h > MP4_MAX_PIXELS || delay == 0 || delay > 60000) {
        efi_free_pool(data);
        return NULL;
    }

    UINT32 flags = 0, max_delta = 0;
    if (version >= 3) {
        if (size < 40) { efi_free_pool(data); return NULL; }
        flags = (UINT32)data[28] | ((UINT32)data[29] << 8) |
                ((UINT32)data[30] << 16) | ((UINT32)data[31] << 24);
        max_delta = (UINT32)data[32] | ((UINT32)data[33] << 8) |
                    ((UINT32)data[34] << 16) | ((UINT32)data[35] << 24);
        if (flags & ~VBG_FLAG_MC) { efi_free_pool(data); return NULL; }
    }

    anim_t *a = efi_allocate_pool(sizeof(anim_t));
    if (!a) { efi_free_pool(data); return NULL; }
    ZeroMem(a, sizeof(anim_t));
    a->codec = 2;
    a->data = data;
    a->size = size;

    vbg_t *v = efi_allocate_pool(sizeof(vbg_t));
    if (!v) { anim_free(a); return NULL; }
    ZeroMem(v, sizeof(vbg_t));
    a->mj = v;

    for (int i = 0; i < 768; i++) {
        int val = i - 256;
        v->j.clamp[i] = (UINT8)(val < 0 ? 0 : (val > 255 ? 255 : val));
    }

    v->first_off = (version >= 3) ? 40 : 32;
    v->next_off = v->first_off;
    v->n = n;
    v->delay_ms = delay;
    v->flags = flags;

    UINT8 type; UINT32 fsize; const UINT8 *payload;
    if (!vbg_read_frame(data, size, v->first_off, &type, &fsize, &payload) ||
        type != 0) {
        efi_log(L"  ERROR: VBG must start with a JPEG keyframe");
        goto fail;
    }

    mjpeg_t *st = &v->j;
    if (!jpeg_parse_header(st, payload, fsize)) {
        efi_log(L"  ERROR: VBG first keyframe unreadable");
        goto fail;
    }
    if (st->W != w || st->H != h || st->ncomp == 0 ||
        st->W > MP4_MAX_DIM || st->H > MP4_MAX_DIM) {
        efi_log(L"  ERROR: VBG keyframe dimensions mismatch");
        goto fail;
    }

    a->width = w;
    a->height = h;
    a->canvas = efi_allocate_pool((UINTN)w * h * sizeof(UINT32));
    if (!a->canvas) goto fail;
    ZeroMem(a->canvas, (UINTN)w * h * sizeof(UINT32));

    {
        UINTN tmax  = (UINTN)1 << VBG_TILE_LOG2_MAX;
        UINTN pw    = ((UINTN)w + tmax - 1) / tmax * tmax;
        UINTN ph    = ((UINTN)h + tmax - 1) / tmax * tmax;
        UINTN bound = pw * ph * 3;

        bound += (pw / (1u << VBG_TILE_LOG2_MIN)) *
                 (ph / (1u << VBG_TILE_LOG2_MIN)) * 2;
        if (version >= 3) {
            if (max_delta > bound) {
                efi_log(L"  ERROR: VBG max_delta exceeds the format bound");
                goto fail;
            }
            v->scratch_sz = max_delta;
        } else {
            v->scratch_sz = bound;
        }
    }
    if (v->scratch_sz) {
        v->scratch = efi_allocate_pool(v->scratch_sz);
        if (!v->scratch) goto fail;
    }

    if (flags & VBG_FLAG_MC) {
        v->ref = efi_allocate_pool((UINTN)w * h * sizeof(UINT32));
        if (!v->ref) goto fail;
        ZeroMem(v->ref, (UINTN)w * h * sizeof(UINT32));
    }

    UINTN comp_bytes = 0;
    for (int i = 0; i < st->ncomp; i++) {
        UINTN cb = st->cw[i] * st->ch[i];
        if (cb > MP4_MAX_PIXELS) goto fail;
        comp_bytes += cb;
    }
    UINT8 *comp = efi_allocate_pool(comp_bytes);
    if (!comp) goto fail;
    ZeroMem(comp, comp_bytes);
    UINTN acc = 0;
    for (int i = 0; i < st->ncomp; i++) {
        st->plane[i] = comp + acc;
        acc += st->cw[i] * st->ch[i];
    }
    jpeg_lock_geometry(st);

    if (!jpeg_frame(st, payload, fsize, a->canvas)) {
        efi_log(L"  ERROR: VBG first keyframe decode failed");
        goto fail;
    }
    v->next_off = v->first_off + 5 + fsize;

    a->frame_count = n;
    a->loops = 0;
    a->cur = 0;
    v->cur = 0;
    a->cur_delay = delay;

    {
        CHAR16 msg[192];
        SPrint(msg, sizeof(msg),
               L"  vbg: %dx%d, %d frames, %d ms, v%d%s, %d KB resident",
               (int)w, (int)h, (int)n, (int)delay, (int)version,
               (flags & VBG_FLAG_MC) ? L" +mc" : L"",
               (int)((size + (UINTN)w * h * 4 + comp_bytes + v->scratch_sz +
                      (v->ref ? (UINTN)w * h * 4 : 0)) / 1024));
        efi_log(msg);
    }
    return a;

fail:
    anim_free(a);
    return NULL;
}

void anim_free(anim_t *a) {
    if (!a) return;
    if (a->codec == 1) {
        mjpeg_state_free(a->mj);
        a->mj = NULL;
    } else if (a->codec == 2) {
        vbg_free(a);
    }
    gif_free(a);
}
