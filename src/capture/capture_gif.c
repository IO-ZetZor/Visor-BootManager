/* capture_gif.c - GIF recorder: quantiser and LZW encoder (feature: record) */
#include "capture_internal.h"

#define CAP_GIF_CLEAR 256
#define CAP_GIF_EOI   257
#define CAP_GIF_MAXCODE 4096

#define CAP_GIF_TRANSPARENT 255
#define CAP_GIF_COLORS      255

#define CAP_GIF_DEFAULT_BUDGET (24u * 1024u * 1024u)
#define CAP_GIF_MIN_DELAY_CS   2
#define CAP_GIF_MAX_DELAY_CS   500

typedef struct {
    UINT8  *data;
    UINTN   len;
    UINT16  x, y, w, h;
    UINT16  delay_cs;
    UINT8   min_code_size;
    UINT8   transparent;
} cap_gif_fr;

#define CAP_BUCKETS 32768

typedef struct {
    UINT32 *n;
    UINT32 *sr, *sg, *sb;
    UINTN   passes;
} cap_hist;

#define CAP_HIST_SAMPLES     200000u
#define CAP_HIST_MAX_PASSES  8u

struct cap_gif {
    UINTN   src_w, src_h;
    UINTN   w, h;
    UINTN   max_frames;
    UINTN   budget;
    UINTN   bytes;
    UINTN   nominal_delay_cs;
    int     full;

    UINTN   nframes;
    cap_gif_fr frames[CAP_GIF_MAX_FRAMES];

    UINT8   palette[256 * 3];
    int     palette_ready;
    cap_hist hist;
    UINT8  *bucket_idx;

    UINT8  *prev;
    UINT8  *cur;
    UINT32 *scaled;

    UINT64  last_ms;
    int     have_last_ms;
};

#define CAP_BUCKET_MID(v) (((v) << 3) | 4)

static void cap_hist_color(const cap_hist *h, UINTN b,
                           INTN *r, INTN *g, INTN *bl) {
    UINT32 c = h->n[b];
    if (c) {
        *r  = (INTN)(h->sr[b] / c);
        *g  = (INTN)(h->sg[b] / c);
        *bl = (INTN)(h->sb[b] / c);
    } else {
        *r  = (INTN)CAP_BUCKET_MID(b >> 10);
        *g  = (INTN)CAP_BUCKET_MID((b >> 5) & 31);
        *bl = (INTN)CAP_BUCKET_MID(b & 31);
    }
}

static int cap_box_tighten(const cap_hist *h, INTN *lo, INTN *hi) {
    INTN nlo[3] = { 32, 32, 32 }, nhi[3] = { -1, -1, -1 };
    for (INTN r = lo[0]; r <= hi[0]; r++) {
        for (INTN gg = lo[1]; gg <= hi[1]; gg++) {
            UINTN base = ((UINTN)r << 10) | ((UINTN)gg << 5);
            for (INTN bb = lo[2]; bb <= hi[2]; bb++) {
                if (!h->n[base | (UINTN)bb]) continue;
                if (r  < nlo[0]) nlo[0] = r;
                if (r  > nhi[0]) nhi[0] = r;
                if (gg < nlo[1]) nlo[1] = gg;
                if (gg > nhi[1]) nhi[1] = gg;
                if (bb < nlo[2]) nlo[2] = bb;
                if (bb > nhi[2]) nhi[2] = bb;
            }
        }
    }
    if (nhi[0] < nlo[0]) return 0;
    for (INTN a = 0; a < 3; a++) { lo[a] = nlo[a]; hi[a] = nhi[a]; }
    return 1;
}

static UINTN cap_median_cut(const cap_hist *h, UINT8 *palette, UINTN maxc) {
    INTN lo[256][3], hi[256][3];
    UINT32 pop[256];
    INTN nb = 1;

    for (INTN a = 0; a < 3; a++) { lo[0][a] = 0; hi[0][a] = 31; }
    pop[0] = 0;
    for (UINTN b = 0; b < CAP_BUCKETS; b++) pop[0] += h->n[b];
    if (!pop[0]) {
        palette[0] = palette[1] = palette[2] = 0;
        return 1;
    }

    while (nb < (INTN)maxc) {
        INTN pick = -1, pickaxis = 0;
        UINT64 pickmax = 0;
        for (INTN i = 0; i < nb; i++) {
            if (!pop[i]) continue;
            INTN range = 0, rax = 0;
            for (INTN a = 0; a < 3; a++) {
                INTN rr = hi[i][a] - lo[i][a];
                if (rr > range) { range = rr; rax = a; }
            }
            if (range == 0) continue;
            UINT64 score = (UINT64)range * pop[i];
            if (score > pickmax) { pickmax = score; pick = i; pickaxis = rax; }
        }
        if (pick < 0) break;

        if (!cap_box_tighten(h, lo[pick], hi[pick])) { pop[pick] = 0; continue; }
        {
            INTN range = 0;
            for (INTN a = 0; a < 3; a++) {
                INTN rr = hi[pick][a] - lo[pick][a];
                if (rr > range) { range = rr; pickaxis = a; }
            }
            if (range == 0) continue;
        }

        UINT32 sweep[32];
        memset(sweep, 0, sizeof(sweep));
        for (INTN r = lo[pick][0]; r <= hi[pick][0]; r++)
            for (INTN gg = lo[pick][1]; gg <= hi[pick][1]; gg++) {
                UINTN base = ((UINTN)r << 10) | ((UINTN)gg << 5);
                for (INTN bb = lo[pick][2]; bb <= hi[pick][2]; bb++) {
                    INTN v = (pickaxis == 0) ? r : (pickaxis == 1 ? gg : bb);
                    sweep[v] += h->n[base | (UINTN)bb];
                }
            }

        UINT32 half = pop[pick] / 2, acc = 0;
        INTN split = lo[pick][pickaxis];
        for (INTN v = lo[pick][pickaxis]; v < hi[pick][pickaxis]; v++) {
            acc += sweep[v];
            split = v;
            if (acc >= half) break;
        }

        UINT32 lopop = 0;
        for (INTN v = lo[pick][pickaxis]; v <= split; v++) lopop += sweep[v];
        if (!lopop || lopop >= pop[pick]) { pop[pick] = 0; continue; }

        for (INTN a = 0; a < 3; a++) {
            lo[nb][a] = lo[pick][a];
            hi[nb][a] = hi[pick][a];
        }
        lo[nb][pickaxis] = split + 1;
        hi[pick][pickaxis] = split;
        pop[nb] = pop[pick] - lopop;
        pop[pick] = lopop;
        nb++;
    }

    for (INTN i = 0; i < nb; i++) {
        UINT64 sr = 0, sg = 0, sb = 0;
        UINT32 cnt = 0;
        for (INTN r = lo[i][0]; r <= hi[i][0]; r++)
            for (INTN gg = lo[i][1]; gg <= hi[i][1]; gg++) {
                UINTN base = ((UINTN)r << 10) | ((UINTN)gg << 5);
                for (INTN bb = lo[i][2]; bb <= hi[i][2]; bb++) {
                    UINTN b = base | (UINTN)bb;
                    UINT32 c = h->n[b];
                    if (!c) continue;
                    sr += h->sr[b];
                    sg += h->sg[b];
                    sb += h->sb[b];
                    cnt += c;
                }
            }
        if (cnt) {
            palette[i * 3 + 0] = (UINT8)(sr / cnt);
            palette[i * 3 + 1] = (UINT8)(sg / cnt);
            palette[i * 3 + 2] = (UINT8)(sb / cnt);
        } else {
            palette[i * 3 + 0] = (UINT8)CAP_BUCKET_MID(lo[i][0]);
            palette[i * 3 + 1] = (UINT8)CAP_BUCKET_MID(lo[i][1]);
            palette[i * 3 + 2] = (UINT8)CAP_BUCKET_MID(lo[i][2]);
        }
    }
    return (UINTN)nb;
}

static void cap_hist_add(cap_hist *h, const UINT32 *pixels,
                         UINTN w, UINTN h_px, UINTN stride) {
    if (h->passes >= CAP_HIST_MAX_PASSES) return;
    h->passes++;
    UINTN step = 1;
    while ((w / step) * (h_px / step) > CAP_HIST_SAMPLES) step++;
    for (UINTN y = 0; y < h_px; y += step) {
        const UINT32 *row = pixels + y * stride;
        for (UINTN x = 0; x < w; x += step) {
            UINT32 p = row[x];
            UINTN b = cap_bucket_of(p);
            h->n[b]++;
            h->sr[b] += (p >> 16) & 0xFF;
            h->sg[b] += (p >> 8) & 0xFF;
            h->sb[b] += p & 0xFF;
        }
    }
}

static void cap_hist_free(cap_hist *h) {
    if (h->n)  { efi_free_pool(h->n);  h->n = NULL; }
    if (h->sr) { efi_free_pool(h->sr); h->sr = NULL; }
    if (h->sg) { efi_free_pool(h->sg); h->sg = NULL; }
    if (h->sb) { efi_free_pool(h->sb); h->sb = NULL; }
}

static void cap_palette_build(cap_gif *g) {
    UINTN paln = cap_median_cut(&g->hist, g->palette, CAP_GIF_COLORS);
    if (paln == 0) paln = 1;
    while (paln < 256) {
        g->palette[paln * 3 + 0] = g->palette[paln * 3 + 1] =
        g->palette[paln * 3 + 2] = 0;
        paln++;
    }
    g->palette[CAP_GIF_TRANSPARENT * 3 + 0] = 0;
    g->palette[CAP_GIF_TRANSPARENT * 3 + 1] = 0;
    g->palette[CAP_GIF_TRANSPARENT * 3 + 2] = 0;

    for (UINTN b = 0; b < CAP_BUCKETS; b++) {
        INTN r, gg, bl;
        cap_hist_color(&g->hist, b, &r, &gg, &bl);
        UINT32 bestd = 0xFFFFFFFFu; UINT8 besti = 0;
        for (UINTN pi = 0; pi < CAP_GIF_COLORS; pi++) {
            INTN dr = r - g->palette[pi * 3 + 0];
            INTN dg = gg - g->palette[pi * 3 + 1];
            INTN db = bl - g->palette[pi * 3 + 2];
            UINT32 d = (UINT32)(dr * dr + dg * dg + db * db);
            if (d < bestd) { bestd = d; besti = (UINT8)pi; }
        }
        g->bucket_idx[b] = besti;
    }

    cap_hist_free(&g->hist);
    g->palette_ready = 1;
}

static void cap_downscale(cap_gif *g, const UINT32 *src, UINT32 *dst) {
    for (UINTN j = 0; j < g->h; j++) {
        UINTN sy0 = j * g->src_h / g->h;
        UINTN sy1 = (j + 1) * g->src_h / g->h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > g->src_h) sy1 = g->src_h;
        for (UINTN i = 0; i < g->w; i++) {
            UINTN sx0 = i * g->src_w / g->w;
            UINTN sx1 = (i + 1) * g->src_w / g->w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > g->src_w) sx1 = g->src_w;

            UINT32 ar = 0, ag = 0, ab = 0, n = 0;
            for (UINTN sy = sy0; sy < sy1; sy++) {
                const UINT32 *row = src + sy * g->src_w;
                for (UINTN sx = sx0; sx < sx1; sx++) {
                    UINT32 p = row[sx];
                    ar += (p >> 16) & 0xFF;
                    ag += (p >> 8) & 0xFF;
                    ab += p & 0xFF;
                    n++;
                }
            }
            if (!n) n = 1;
            dst[j * g->w + i] = ((ar / n) << 16) | ((ag / n) << 8) | (ab / n);
        }
    }
}

typedef struct {
    cap_buf  out;
    UINTN    bitbuf, bitcnt;
    UINT8    sub[256];
    UINTN    sublen;
    int      err;
} lzw_writer;

static void lzw_byte(lzw_writer *w, UINT8 b) {
    if (w->err) return;
    w->sub[w->sublen++] = b;
    if (w->sublen == 255) {
        UINT8 hdr = 255;
        if (!cap_buf_put(&w->out, &hdr, 1) ||
            !cap_buf_put(&w->out, w->sub, w->sublen)) { w->err = 1; return; }
        w->sublen = 0;
    }
}

static void lzw_flush_subs(lzw_writer *w) {
    if (w->err) return;
    UINT8 hdr = (UINT8)w->sublen;
    UINT8 term = 0x00;
    if (!cap_buf_put(&w->out, &hdr, 1) ||
        !cap_buf_put(&w->out, w->sub, w->sublen)) { w->err = 1; return; }
    w->sublen = 0;
    if (!cap_buf_put(&w->out, &term, 1)) w->err = 1;
}

static void lzw_code(lzw_writer *w, UINTN code, UINTN codesize) {
    w->bitbuf |= (UINTN)code << w->bitcnt;
    w->bitcnt += codesize;
    while (w->bitcnt >= 8) {
        lzw_byte(w, (UINT8)(w->bitbuf & 0xFF));
        w->bitbuf >>= 8;
        w->bitcnt -= 8;
    }
}

#define LZW_HASH_SIZE 8191
typedef struct {
    UINT32 key;
    UINT16 code;
} lzw_slot;

typedef struct {
    lzw_slot *slot;
    UINT16   *pref;
    UINT8    *ch;
} lzw_dict;

static void lzw_dict_clear(lzw_dict *d) {
    for (UINTN i = 0; i < LZW_HASH_SIZE; i++) d->slot[i].key = 0xFFFFFFFF;
}

static void lzw_dict_insert(lzw_dict *d, UINTN px, UINTN c, UINTN code) {
    UINT32 k = ((UINT32)px << 8) | (UINT32)c;
    UINTN s = (UINTN)((k ^ (k >> 8) ^ (k >> 16)) % LZW_HASH_SIZE);
    while (d->slot[s].key != 0xFFFFFFFF) s = (s + 1) % LZW_HASH_SIZE;
    d->slot[s].key = k;
    d->slot[s].code = (UINT16)code;
    d->pref[code] = (UINT16)px;
    d->ch[code] = (UINT8)c;
}

static int lzw_dict_find(lzw_dict *d, UINTN px, UINTN c) {
    UINT32 k = ((UINT32)px << 8) | (UINT32)c;
    UINTN s = (UINTN)((k ^ (k >> 8) ^ (k >> 16)) % LZW_HASH_SIZE);
    for (UINTN p = 0; p < LZW_HASH_SIZE; p++) {
        if (d->slot[s].key == 0xFFFFFFFF) return -1;
        if (d->slot[s].key == k) return (int)d->slot[s].code;
        s = (s + 1) % LZW_HASH_SIZE;
    }
    return -1;
}

static EFI_STATUS cap_gif_lzw_encode(const UINT8 *idx, UINTN stride,
                                     UINTN rx, UINTN ry, UINTN rw, UINTN rh,
                                     UINT8 **data, UINTN *datalen,
                                     UINTN *min_code_size) {
    UINTN minsize = 8;
    UINTN n = rw * rh;
    *min_code_size = minsize;

    lzw_writer w;
    memset(&w, 0, sizeof(w));
    if (!cap_buf_reserve(&w.out, n / 4 + 256)) return EFI_OUT_OF_RESOURCES;

    lzw_dict d;
    d.slot = efi_allocate_pool(sizeof(lzw_slot) * LZW_HASH_SIZE);
    d.pref = efi_allocate_pool(sizeof(UINT16) * CAP_GIF_MAXCODE);
    d.ch   = efi_allocate_pool(CAP_GIF_MAXCODE);
    if (!d.slot || !d.pref || !d.ch) {
        if (d.slot) efi_free_pool(d.slot);
        if (d.pref) efi_free_pool(d.pref);
        if (d.ch)   efi_free_pool(d.ch);
        cap_buf_free(&w.out);
        return EFI_OUT_OF_RESOURCES;
    }

    for (UINTN c = 0; c < 256; c++) { d.pref[c] = 0; d.ch[c] = (UINT8)c; }
    lzw_dict_clear(&d);

    UINTN codesize = minsize + 1;
    UINTN next = CAP_GIF_EOI + 1;
    UINTN prefix = 0;
    int started = 0;

    lzw_code(&w, CAP_GIF_CLEAR, codesize);

    for (UINTN y = 0; y < rh; y++) {
        const UINT8 *row = idx + (ry + y) * stride + rx;
        for (UINTN x = 0; x < rw; x++) {
            UINTN c = (UINTN)row[x];
            if (!started) { prefix = c; started = 1; continue; }
            int f = lzw_dict_find(&d, prefix, c);
            if (f >= 0) {
                prefix = (UINTN)f;
                continue;
            }
            lzw_code(&w, prefix, codesize);
            if (next >= (1u << codesize) && codesize < 12) codesize++;
            if (next < CAP_GIF_MAXCODE) {
                lzw_dict_insert(&d, prefix, c, next);
                next++;
            } else {
                lzw_code(&w, CAP_GIF_CLEAR, codesize);
                lzw_dict_clear(&d);
                next = CAP_GIF_EOI + 1;
                codesize = minsize + 1;
            }
            prefix = c;
        }
    }
    if (started) lzw_code(&w, prefix, codesize);

    lzw_code(&w, CAP_GIF_EOI, codesize);

    if (w.bitcnt > 0) {
        lzw_byte(&w, (UINT8)(w.bitbuf & 0xFF));
        w.bitbuf = 0; w.bitcnt = 0;
    }
    lzw_flush_subs(&w);

    efi_free_pool(d.slot);
    efi_free_pool(d.pref);
    efi_free_pool(d.ch);

    if (w.err) {
        cap_buf_free(&w.out);
        return EFI_OUT_OF_RESOURCES;
    }

    *data = w.out.buf;
    *datalen = w.out.len;
    return EFI_SUCCESS;
}

cap_gif *cap_gif_new(UINTN src_w, UINTN src_h, UINTN max_width,
                     UINTN max_frames, UINTN budget_bytes,
                     UINTN nominal_delay_cs) {
    if (!src_w || !src_h || src_w > 0x7FFF || src_h > 0x7FFF) return NULL;

    UINTN w = src_w, h = src_h;
    if (max_width && w > max_width) {
        h = (src_h * max_width + src_w / 2) / src_w;
        if (!h) h = 1;
        w = max_width;
    }

    cap_gif *g = efi_allocate_pool(sizeof(cap_gif));
    if (!g) return NULL;
    memset(g, 0, sizeof(*g));

    g->src_w = src_w; g->src_h = src_h;
    g->w = w; g->h = h;
    g->max_frames = max_frames ? max_frames : CAP_GIF_MAX_FRAMES;
    if (g->max_frames > CAP_GIF_MAX_FRAMES) g->max_frames = CAP_GIF_MAX_FRAMES;
    g->budget = budget_bytes ? budget_bytes : CAP_GIF_DEFAULT_BUDGET;
    g->nominal_delay_cs = nominal_delay_cs ? nominal_delay_cs
                                           : CAP_GIF_MIN_DELAY_CS;

    g->hist.n     = efi_allocate_pool(sizeof(UINT32) * CAP_BUCKETS);
    g->hist.sr    = efi_allocate_pool(sizeof(UINT32) * CAP_BUCKETS);
    g->hist.sg    = efi_allocate_pool(sizeof(UINT32) * CAP_BUCKETS);
    g->hist.sb    = efi_allocate_pool(sizeof(UINT32) * CAP_BUCKETS);
    g->bucket_idx = efi_allocate_pool(CAP_BUCKETS);
    g->prev       = efi_allocate_pool(w * h);
    g->cur        = efi_allocate_pool(w * h);
    if (w != src_w || h != src_h)
        g->scaled = efi_allocate_pool(w * h * sizeof(UINT32));

    if (!g->hist.n || !g->hist.sr || !g->hist.sg || !g->hist.sb ||
        !g->bucket_idx || !g->prev || !g->cur ||
        ((w != src_w || h != src_h) && !g->scaled)) {
        cap_gif_free(g);
        return NULL;
    }
    memset(g->hist.n,  0, sizeof(UINT32) * CAP_BUCKETS);
    memset(g->hist.sr, 0, sizeof(UINT32) * CAP_BUCKETS);
    memset(g->hist.sg, 0, sizeof(UINT32) * CAP_BUCKETS);
    memset(g->hist.sb, 0, sizeof(UINT32) * CAP_BUCKETS);
    return g;
}

void cap_gif_free(cap_gif *g) {
    if (!g) return;
    for (UINTN i = 0; i < g->nframes; i++)
        if (g->frames[i].data) efi_free_pool(g->frames[i].data);
    cap_hist_free(&g->hist);
    if (g->bucket_idx) efi_free_pool(g->bucket_idx);
    if (g->prev)       efi_free_pool(g->prev);
    if (g->cur)        efi_free_pool(g->cur);
    if (g->scaled)     efi_free_pool(g->scaled);
    efi_free_pool(g);
}

UINTN cap_gif_count(const cap_gif *g)  { return g ? g->nframes : 0; }
UINTN cap_gif_bytes(const cap_gif *g)  { return g ? g->bytes : 0; }
int   cap_gif_is_full(const cap_gif *g) { return g ? g->full : 1; }
UINTN cap_gif_width(const cap_gif *g)  { return g ? g->w : 0; }
UINTN cap_gif_height(const cap_gif *g) { return g ? g->h : 0; }

void cap_gif_sample(cap_gif *g, const UINT32 *pixels) {
    if (!g || !pixels || g->palette_ready || !g->hist.n) return;
    cap_hist_add(&g->hist, pixels, g->src_w, g->src_h, g->src_w);
}

static void cap_gif_map(cap_gif *g, const UINT32 *pixels) {
    const UINT32 *src = pixels;
    if (g->scaled) {
        cap_downscale(g, pixels, g->scaled);
        src = g->scaled;
    }
    UINTN n = g->w * g->h;
    for (UINTN i = 0; i < n; i++)
        g->cur[i] = g->bucket_idx[cap_bucket_of(src[i])];
}

int cap_gif_frame(cap_gif *g, const UINT32 *pixels, UINT64 now_ms) {
    if (!g || !pixels) return CAP_FRAME_ERROR;

    if (g->nframes && g->have_last_ms) {
        UINT64 dt = now_ms - g->last_ms;
        UINTN cs = (UINTN)((dt + 5) / 10);
        if (cs < CAP_GIF_MIN_DELAY_CS) cs = CAP_GIF_MIN_DELAY_CS;
        if (cs > CAP_GIF_MAX_DELAY_CS) cs = CAP_GIF_MAX_DELAY_CS;
        g->frames[g->nframes - 1].delay_cs = (UINT16)cs;
    }

    if (g->full || g->nframes >= g->max_frames) {
        g->full = 1;
        return g->nframes ? CAP_FRAME_FULL : CAP_FRAME_ERROR;
    }

    if (!g->palette_ready) {
        cap_hist_add(&g->hist, pixels, g->src_w, g->src_h, g->src_w);
        cap_palette_build(g);
    }

    cap_gif_map(g, pixels);

    UINTN rx = 0, ry = 0, rw = g->w, rh = g->h;
    int transparent = 0;

    if (g->nframes) {
        UINTN x0 = g->w, y0 = g->h, x1 = 0, y1 = 0;
        for (UINTN y = 0; y < g->h; y++) {
            const UINT8 *cr = g->cur + y * g->w;
            const UINT8 *pr = g->prev + y * g->w;
            for (UINTN x = 0; x < g->w; x++) {
                if (cr[x] == pr[x]) continue;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
        if (x0 > x1 || y0 > y1) {
            g->last_ms = now_ms;
            g->have_last_ms = 1;
            return CAP_FRAME_OK;
        }
        rx = x0; ry = y0; rw = x1 - x0 + 1; rh = y1 - y0 + 1;

        for (UINTN y = 0; y < rh; y++) {
            UINT8 *cr = g->cur + (ry + y) * g->w + rx;
            const UINT8 *pr = g->prev + (ry + y) * g->w + rx;
            for (UINTN x = 0; x < rw; x++) {
                if (cr[x] == pr[x]) { cr[x] = CAP_GIF_TRANSPARENT; transparent = 1; }
            }
        }
    }

    UINT8 *data = NULL; UINTN len = 0, minsize = 0;
    EFI_STATUS st = cap_gif_lzw_encode(g->cur, g->w, rx, ry, rw, rh,
                                       &data, &len, &minsize);
    if (EFI_ERROR(st) || !data) {
        if (data) efi_free_pool(data);
        g->full = 1;
        return g->nframes ? CAP_FRAME_FULL : CAP_FRAME_ERROR;
    }

    if (g->bytes + len > g->budget) {
        efi_free_pool(data);
        g->full = 1;
        return g->nframes ? CAP_FRAME_FULL : CAP_FRAME_ERROR;
    }

    cap_gif_fr *fr = &g->frames[g->nframes];
    fr->data = data;
    fr->len = len;
    fr->x = (UINT16)rx; fr->y = (UINT16)ry;
    fr->w = (UINT16)rw; fr->h = (UINT16)rh;
    fr->min_code_size = (UINT8)minsize;
    fr->transparent = (UINT8)transparent;
    fr->delay_cs = (UINT16)g->nominal_delay_cs;
    g->bytes += len;
    g->nframes++;

    if (transparent) {
        for (UINTN y = 0; y < rh; y++) {
            UINT8 *cr = g->cur + (ry + y) * g->w + rx;
            const UINT8 *pr = g->prev + (ry + y) * g->w + rx;
            for (UINTN x = 0; x < rw; x++)
                if (cr[x] == CAP_GIF_TRANSPARENT) cr[x] = pr[x];
        }
    }
    memcpy(g->prev, g->cur, g->w * g->h);

    g->last_ms = now_ms;
    g->have_last_ms = 1;

    if (g->nframes >= g->max_frames || g->bytes >= g->budget) g->full = 1;
    return g->full ? CAP_FRAME_FULL : CAP_FRAME_OK;
}

static EFI_STATUS cap_gif_build(cap_gif *g, UINT8 **out, UINTN *out_size) {
    cap_buf outb;
    outb.buf = NULL; outb.len = 0; outb.cap = 0;
    if (!cap_buf_reserve(&outb, g->bytes + g->nframes * 24 + 1024)) goto oom;

    if (!cap_buf_put(&outb, (const UINT8*)"GIF89a", 6)) goto oom;
    if (!cap_buf_u16le(&outb, (UINT16)g->w)) goto oom;
    if (!cap_buf_u16le(&outb, (UINT16)g->h)) goto oom;
    {
        UINT8 lsd[3] = { 0xF7, 0x00, 0x00 };
        if (!cap_buf_put(&outb, lsd, 3)) goto oom;
    }
    if (!cap_buf_put(&outb, g->palette, 256 * 3)) goto oom;

    {
        static const UINT8 loop[19] = {
            0x21, 0xFF, 0x0B, 'N','E','T','S','C','A','P','E','2','.','0',
            0x03, 0x01, 0x00, 0x00, 0x00
        };
        if (!cap_buf_put(&outb, loop, sizeof(loop))) goto oom;
    }

    for (UINTN f = 0; f < g->nframes; f++) {
        cap_gif_fr *fr = &g->frames[f];
        if (!fr->data) goto oom;

        UINT8 packed = fr->transparent ? 0x05 : 0x04;
        UINT8 gce[8] = { 0x21, 0xF9, 0x04, packed,
                         (UINT8)fr->delay_cs, (UINT8)(fr->delay_cs >> 8),
                         CAP_GIF_TRANSPARENT, 0x00 };
        if (!cap_buf_put(&outb, gce, 8)) goto oom;

        UINT8 id[10] = { 0x2C,
                         (UINT8)fr->x, (UINT8)(fr->x >> 8),
                         (UINT8)fr->y, (UINT8)(fr->y >> 8),
                         (UINT8)fr->w, (UINT8)(fr->w >> 8),
                         (UINT8)fr->h, (UINT8)(fr->h >> 8),
                         0x00 };
        if (!cap_buf_put(&outb, id, 10)) goto oom;

        if (!cap_buf_put(&outb, &fr->min_code_size, 1)) goto oom;
        if (!cap_buf_put(&outb, fr->data, fr->len)) goto oom;
    }

    UINT8 trailer = 0x3B;
    if (!cap_buf_put(&outb, &trailer, 1)) goto oom;

    *out = outb.buf;
    *out_size = outb.len;
    return EFI_SUCCESS;
oom:
    cap_buf_free(&outb);
    return EFI_OUT_OF_RESOURCES;
}

EFI_STATUS cap_gif_close(cap_gif *g, UINT8 **out, UINTN *out_size) {
    if (!g || !out || !out_size) return EFI_INVALID_PARAMETER;
    if (g->nframes == 0) { cap_gif_free(g); return EFI_NOT_READY; }
    if (g->frames[g->nframes - 1].delay_cs < CAP_GIF_MIN_DELAY_CS)
        g->frames[g->nframes - 1].delay_cs = (UINT16)g->nominal_delay_cs;
    if (EFI_ERROR(cap_gif_build(g, out, out_size))) {
        cap_gif_free(g);
        return EFI_OUT_OF_RESOURCES;
    }
    cap_gif_free(g);
    return EFI_SUCCESS;
}
