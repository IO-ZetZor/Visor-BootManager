/* mjpeg_decoder.c - baseline JPEG / MJPEG frame decoder (feature: mjpeg) */
#include "anim_decode.h"

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

UINTN jpeg_parse_header(mjpeg_t *st, const UINT8 *d, UINTN size) {
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
                if (nf != 1 && nf != 3) return 0;
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

int jpeg_decode_scan(mjpeg_t *st, const UINT8 *d, UINTN size, UINTN scan_off) {
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

void jpeg_composite(mjpeg_t *st, UINT32 *canvas) {
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

void jpeg_lock_geometry(mjpeg_t *st) {
    st->lock_ncomp = st->ncomp;
    for (int i = 0; i < 4; i++) {
        st->lock_cw[i] = st->cw[i];
        st->lock_ch[i] = st->ch[i];
    }
    st->locked = 1;
}

int jpeg_frame(mjpeg_t *st, const UINT8 *d, UINTN size, UINT32 *canvas) {
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

void mjpeg_state_free(mjpeg_t *st) {
    if (!st) return;
    if (st->off) efi_free_pool(st->off);
    if (st->len) efi_free_pool(st->len);
    if (st->ms)  efi_free_pool(st->ms);
    if (st->plane[0]) efi_free_pool(st->plane[0]);
    for (int i = 0; i < 4; i++)
        if (st->sxmap[i]) efi_free_pool(st->sxmap[i]);
    efi_free_pool(st);
}

int mjpeg_advance_n(anim_t *a, UINTN n) {
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

void jpeg_planes_free(mjpeg_t *st) {
    if (st->plane[0]) efi_free_pool(st->plane[0]);
    st->plane[0] = NULL;
    for (int i = 0; i < 4; i++) {
        if (st->sxmap[i]) efi_free_pool(st->sxmap[i]);
        st->sxmap[i] = NULL;
    }
}
