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
    UINTN  *off;
    UINT32 *len;
    UINT32 *ms;
    UINTN   n;
    UINTN   cur;
    UINTN   W, H;
    int     ncomp;
    UINT8   comp_id[4], comp_h[4], comp_v[4], comp_tq[4];
    int     max_h, max_v;
    UINTN   cw[4], ch[4];
    UINT8  *plane[4];

    UINT16  qtab[4][64];
    UINT8   qn[4];
    UINT8   dc_bits[4][16], dc_val[4][256], dc_have[4];
    UINT8   ac_bits[4][16], ac_val[4][256], ac_have[4];
    UINT8   hs[8][256];
    UINT16  hc[8][256];
    int     ls[8][17];

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
    if (b->nbits < count && !jb_refill(b)) return 0;
    if (b->nbits < count) return 0;
    *out = (b->acc >> (b->nbits - count)) & ((1u << count) - 1);
    b->nbits -= count;
    return 1;
}

static int jb_marker(jbits_t *b) {
    b->acc = 0; b->nbits = 0;
    if (b->pos + 1 >= b->size) return -1;
    if (b->p[b->pos] != 0xFF) return -1;
    UINT8 n = b->p[b->pos + 1];
    b->pos += 2;
    return n;
}

static void jhuff_build(const UINT8 *bits,
                        UINT8 *hsize, UINT16 *hcode, int *lstart) {
    int code = 0, k = 0;
    lstart[0] = 0;
    for (int l = 1; l <= 16; l++) {
        int cnt = bits[l - 1];
        for (int c = 0; c < cnt && k < 256; c++) {
            hsize[k] = (UINT8)l;
            hcode[k] = (UINT16)code;
            k++; code++;
        }
        lstart[l] = k;
        code <<= 1;
    }
}

static int jhuff(jbits_t *b, const UINT8 *vals,
                 const UINT16 *hcode, const int *lstart) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        UINT32 bit;
        if (!jb_get(b, 1, &bit)) return -1;
        code = (code << 1) | (int)bit;
        for (int i = lstart[l - 1]; i < lstart[l]; i++) {
            if (hcode[i] == code) return vals[i];
        }
    }
    return -1;
}

static int jrecv(jbits_t *b, int s, int *out) {
    if (s == 0) { *out = 0; return 0; }
    int v = 0;
    for (int i = 0; i < s; i++) {
        UINT32 bit;
        if (!jb_get(b, 1, &bit)) return -1;
        v = (v << 1) | (int)bit;
    }
    if (v < (1 << (s - 1))) v -= (1 << s) - 1;
    *out = v;
    return 0;
}

static void jidct2(const INT32 *block, INT64 *tmp, UINT8 *out) {
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            INT64 s = 0;
            for (int k = 0; k < 8; k++)
                s += (INT64)jidct_m[i][k] * block[k * 8 + j];
            tmp[i * 8 + j] = (s + 4096) >> 13;
        }
    }
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            INT64 s = 0;
            for (int k = 0; k < 8; k++)
                s += (INT64)jidct_m[j][k] * tmp[i * 8 + k];
            s = (s + 4096) >> 13;
            int v = (int)((s + 2) >> 2) + 128;
            out[i * 8 + j] = (UINT8)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

static int jdec_block(jbits_t *b, mjpeg_t *st, int c,
                      int dc_t, int ac_t, int bx, int by) {
    INT32 block[64];
    ZeroMem(block, sizeof(block));

    int tq = st->comp_tq[c];

    int r = jhuff(b, st->dc_val[dc_t], st->hc[dc_t], st->ls[dc_t]);
    if (r < 0) return 0;
    int diff;
    if (jrecv(b, r, &diff) < 0) return 0;
    st->dcpred[c] += diff;
    block[0] = st->dcpred[c] * (INT32)st->qtab[tq][0];

    int k = 1;
    while (k < 64) {
        r = jhuff(b, st->ac_val[ac_t], st->hc[4 + ac_t], st->ls[4 + ac_t]);
        if (r < 0) return 0;
        int run = r >> 4, ssz = r & 15;
        if (ssz == 0) {
            if (run == 0) break;
            if (run == 15) { k += 16; continue; }
            return 0;
        }
        k += run;
        if (k >= 64) return 0;
        int v;
        if (jrecv(b, ssz, &v) < 0) return 0;
        block[jzig[k]] = v * (INT32)st->qtab[tq][k];
        k++;
    }

    UINT8 out[64];
    jidct2(block, st->tmp, out);

    UINT8 *plane = st->plane[c];
    UINTN cw = st->cw[c], ch = st->ch[c];
    for (UINTN y = 0; y < 8; y++) {
        UINTN py = (UINTN)by * 8 + y;
        if (py >= ch) break;
        for (UINTN x = 0; x < 8; x++) {
            UINTN px = (UINTN)bx * 8 + x;
            if (px >= cw) break;
            plane[py * cw + px] = out[y * 8 + x];
        }
    }
    return 1;
}

static int jpeg_dht(mjpeg_t *st, const UINT8 *p, UINTN plen) {
    UINTN off = 0;
    while (off < plen) {
        if (off + 17 > plen) return 0;
        UINT8 info = p[off++];
        int tc = info >> 4, th = info & 15;
        if (th > 3) return 0;
        UINT8 *bits = tc ? st->ac_bits[th] : st->dc_bits[th];
        UINT8 *vals = tc ? st->ac_val[th] : st->dc_val[th];
        if (tc) st->ac_have[th] = 1; else st->dc_have[th] = 1;
        int nv = 0;
        for (int i = 0; i < 16; i++) { bits[i] = p[off++]; nv += bits[i]; }
        if (nv > 256 || off + nv > plen) return 0;
        for (int i = 0; i < nv; i++) vals[i] = p[off++];
        jhuff_build(bits, st->hs[tc * 4 + th], st->hc[tc * 4 + th],
                    st->ls[tc * 4 + th]);
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
                    if (st->comp_h[i] > st->max_h) st->max_h = st->comp_h[i];
                    if (st->comp_v[i] > st->max_v) st->max_v = st->comp_v[i];
                }
                for (int i = 0; i < nf; i++) {
                    st->cw[i] = (W * st->comp_h[i] + st->max_h - 1) / st->max_h;
                    st->ch[i] = (H * st->comp_v[i] + st->max_v - 1) / st->max_v;
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
    UINTN mcu_idx = 0;
    int stopped = 0;

    for (UINTN my = 0; my < mcus_y && !stopped; my++) {
        for (UINTN mx = 0; mx < mcus_x && !stopped; mx++) {
            for (int ci = 0; ci < st->scan_n; ci++) {
                int c = st->scan_comp[ci];
                for (int v = 0; v < st->comp_v[c]; v++) {
                    for (int h = 0; h < st->comp_h[c]; h++) {
                        int bx = (int)(mx * (UINTN)st->max_h + h);
                        int by = (int)(my * (UINTN)st->max_v + v);
                        if (!jdec_block(&b, st, c, st->scan_dc[ci], st->scan_ac[ci],
                                        bx, by)) {
                            stopped = 1;
                            break;
                        }
                    }
                    if (stopped) break;
                }
                if (stopped) break;
            }
            if (stopped) break;

            mcu_idx++;
            if (st->ri > 0 && mcu_idx % (UINTN)st->ri == 0) {
                int m = jb_marker(&b);
                if (m >= 0xD0 && m <= 0xD7) {
                    for (int c = 0; c < 4; c++) st->dcpred[c] = 0;
                } else {
                    stopped = 1;
                }
            }
        }
    }
    return 1;
}

static void jpeg_composite(mjpeg_t *st, UINT32 *canvas) {
    UINTN W = st->W, H = st->H;

    if (st->ncomp == 1) {
        UINT8 *y = st->plane[0];
        for (UINTN i = 0; i < W * H; i++) {
            UINT8 g = y[i];
            canvas[i] = 0xFF000000u | ((UINT32)g << 16) | ((UINT32)g << 8) | g;
        }
        return;
    }

    UINT8 *y = st->plane[0];
    UINT8 *cb = st->plane[1];
    UINT8 *cr = st->plane[2];
    UINTN cw1 = st->cw[1], ch1 = st->ch[1];
    UINTN cw2 = st->cw[2], ch2 = st->ch[2];
    int h1 = st->comp_h[1], v1 = st->comp_v[1];
    int h2 = st->comp_h[2], v2 = st->comp_v[2];

    for (UINTN yy = 0; yy < H; yy++) {
        UINTN sy1 = yy * v1 / st->max_v;
        if (sy1 >= ch1) sy1 = ch1 - 1;
        UINTN sy2 = yy * v2 / st->max_v;
        if (sy2 >= ch2) sy2 = ch2 - 1;
        const UINT8 *crow1 = cb + sy1 * cw1;
        const UINT8 *crow2 = cr + sy2 * cw2;
        UINT32 *drow = canvas + yy * W;
        const UINT8 *yrow = y + yy * W;
        for (UINTN xx = 0; xx < W; xx++) {
            int Y = yrow[xx];
            UINTN sx1 = xx * h1 / st->max_h;
            if (sx1 >= cw1) sx1 = cw1 - 1;
            UINTN sx2 = xx * h2 / st->max_h;
            if (sx2 >= cw2) sx2 = cw2 - 1;
            int Cb = (int)crow1[sx1] - 128;
            int Cr = (int)crow2[sx2] - 128;
            int r = Y + ((1436 * Cr) >> 10);
            int g = Y - ((352 * Cb + 731 * Cr) >> 10);
            int bl = Y + ((1815 * Cb) >> 10);
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            bl = bl < 0 ? 0 : (bl > 255 ? 255 : bl);
            drow[xx] = 0xFF000000u | ((UINT32)r << 16) | ((UINT32)g << 8) | bl;
        }
    }
}

static int jpeg_frame(mjpeg_t *st, const UINT8 *d, UINTN size, UINT32 *canvas) {
    UINTN scan_off = jpeg_parse_header(st, d, size);
    if (!scan_off) return 0;
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
    efi_free_pool(st);
}

anim_t* mp4_load(UINT8 *data, UINTN size) {
    if (size < 24 || size > MP4_MAX_FILE) {
        efi_log(L"  ERROR: MP4 file too small or too large");
        return NULL;
    }
    if (rd32(data + 4) != 0x66747970u) {
        efi_log(L"  ERROR: bad MP4 signature");
        return NULL;
    }

    anim_t *a = efi_allocate_pool(sizeof(anim_t));
    if (!a) return NULL;
    ZeroMem(a, sizeof(anim_t));

    mjpeg_t *st = efi_allocate_pool(sizeof(mjpeg_t));
    if (!st) { efi_free_pool(a); return NULL; }
    ZeroMem(st, sizeof(mjpeg_t));
    a->mj = st;
    a->codec = 1;

    mp4box_t top;
    top.d = data; top.size = size; top.pos = 0; top.end = size; top.type = 0;
    mp4box_t moov;
    if (!mp4_find_child(&top, 0x6D6F6F76u, &moov)) {
        efi_log(L"  ERROR: MP4 has no moov box");
        goto fail;
    }

    UINT32 timescale = 1000;
    mp4box_t mvhd;
    if (mp4_find_child(&moov, 0x6D766864u, &mvhd) && mvhd.size >= 16) {
        UINT32 ts = rd32(mvhd.d + mvhd.pos + 12);
        if (ts) timescale = ts;
    }

    mp4box_t it = moov;
    mp4box_t trak;
    int found = 0;
    while (mp4_next(&it, &trak)) {
        if (trak.type != 0x7472616Bu) continue;
        mp4box_t mdia;
        if (!mp4_find_child(&trak, 0x6D646961u, &mdia)) continue;
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
        if (!mp4_samples(&stbl, timescale, st)) continue;
        found = 1;
        break;
    }
    if (!found || st->n == 0) {
        efi_log(L"  ERROR: MP4 has no decodable MJPEG video track");
        goto fail;
    }

    a->data = efi_allocate_pool(size);
    if (!a->data) goto fail;
    CopyMem(a->data, data, size);
    a->size = size;

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
    UINTN px = st->W * st->H;
    if (px > MP4_MAX_PIXELS || px == 0) {
        efi_log(L"  ERROR: MJPEG frame too large");
        goto fail;
    }

    a->width = st->W;
    a->height = st->H;

    a->canvas = efi_allocate_pool(px * sizeof(UINT32));
    if (!a->canvas) goto fail;

    UINTN comp_bytes = 0;
    for (int i = 0; i < st->ncomp; i++) {
        UINTN cb = st->cw[i] * st->ch[i];
        if (cb > MP4_MAX_PIXELS) goto fail;
        comp_bytes += cb;
    }
    UINT8 *comp = efi_allocate_pool(comp_bytes);
    if (!comp) goto fail;
    UINTN acc = 0;
    for (int i = 0; i < st->ncomp; i++) {
        st->plane[i] = comp + acc;
        acc += st->cw[i] * st->ch[i];
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
        SPrint(msg, sizeof(msg), L"  mp4: %dx%d, %d frames, %d KB resident",
               (int)st->W, (int)st->H, (int)st->n,
               (int)((size + px * 4 + comp_bytes) / 1024));
        efi_log(msg);
    }
    return a;

fail:
    efi_log(L"  ERROR: MJPEG video failed to load");
    anim_free(a);
    return NULL;
}

static int mjpeg_advance(anim_t *a) {
    mjpeg_t *st = a->mj;
    if (!st || st->n < 2) return 0;

    UINTN idx = st->cur + 1;
    if (idx >= st->n) {
        if (a->loops && a->loops_done + 1 >= a->loops) return 0;
        a->loops_done++;
        idx = 0;
    }
    if (idx >= st->n) return 0;
    if (st->off[idx] + st->len[idx] > a->size)
        return 0;

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
    if (a->codec == 1) return mjpeg_advance(a);
    return gif_advance(a);
}

void anim_free(anim_t *a) {
    if (!a) return;
    if (a->codec == 1) {
        mjpeg_state_free(a->mj);
        a->mj = NULL;
    }
    gif_free(a);
}
