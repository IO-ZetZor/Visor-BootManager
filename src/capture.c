#include "capture.h"
#include "efi_helpers.h"

#include <efilib.h>
#include <string.h>

/* CRC32 (PNG / ISO 3309, reflected) */

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

static UINT32 cap_crc32(UINT32 crc, const UINT8 *data, UINTN len) {
    cap_crc32_init_table();
    crc = ~crc;
    for (UINTN i = 0; i < len; i++)
        crc = cap_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ADLER32 */

static UINT32 cap_adler32(const UINT8 *data, UINTN len) {
    UINT32 a = 1, b = 0;
    for (UINTN i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

/* Small growable byte buffer */

typedef struct {
    UINT8  *buf;
    UINTN   len;
    UINTN   cap;
} cap_buf;

static void cap_buf_free(cap_buf *b) {
    if (b && b->buf) { efi_free_pool(b->buf); b->buf = NULL; }
    b->len = b->cap = 0;
}

static int cap_buf_reserve(cap_buf *b, UINTN need) {
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

static int cap_buf_put(cap_buf *b, const UINT8 *p, UINTN n) {
    if (!cap_buf_reserve(b, b->len + n)) return 0;
    CopyMem(b->buf + b->len, (void*)p, n);
    b->len += n;
    return 1;
}

static int cap_buf_u32be(cap_buf *b, UINT32 v) {
    UINT8 t[4] = { (UINT8)(v >> 24), (UINT8)(v >> 16), (UINT8)(v >> 8), (UINT8)v };
    return cap_buf_put(b, t, 4);
}

static int cap_buf_u16le(cap_buf *b, UINT16 v) {
    UINT8 t[2] = { (UINT8)v, (UINT8)(v >> 8) };
    return cap_buf_put(b, t, 2);
}

/* PNG encoder (stored / uncompressed DEFLATE) */

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

    /* Build the filtered raw stream. */
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

/* GIF encoder */

#define CAP_GIF_CLEAR 256
#define CAP_GIF_EOI   257
#define CAP_GIF_MAXCODE 4096

typedef struct {
    UINT8  *idx;
    UINT8   palette[256 * 3];
} cap_gif_fr;

struct cap_gif {
    UINTN       w, h;
    UINTN       nframes;
    UINT32      delays[CAP_GIF_MAX_FRAMES];
    cap_gif_fr *frames[CAP_GIF_MAX_FRAMES];
};

#define CAP_BUCKETS 32768

/* Median-cut quantizer over the 5-bit-per-channel histogram.
 * Splits the RGB cube (bucket index space, 0..31) into at most maxc boxes
 * and writes each box's weighted-mean colour into palette. Returns the
 * number of boxes produced (1..maxc). */
static UINTN cap_median_cut(const UINT32 *hist, UINT8 *palette, UINTN maxc) {
    INTN lo[256][3], hi[256][3];
    INTN nb = 1;
    for (INTN a = 0; a < 3; a++) { lo[0][a] = 0; hi[0][a] = 31; }

    while (nb < (INTN)maxc) {
        INTN pick = -1, pickaxis = 0;
        UINT32 pickmax = 0, pickpop = 0;
        for (INTN i = 0; i < nb; i++) {
            INTN range = 0, rax = 0;
            for (INTN a = 0; a < 3; a++) {
                INTN rr = hi[i][a] - lo[i][a];
                if (rr > range) { range = rr; rax = a; }
            }
            if (range == 0) continue;
            UINT32 pop = 0;
            for (UINTN b = 0; b < CAP_BUCKETS; b++) {
                UINTN r = b >> 10, gg = (b >> 5) & 31, bb = b & 31;
                if (r >= (UINTN)lo[i][0] && r <= (UINTN)hi[i][0] &&
                    gg >= (UINTN)lo[i][1] && gg <= (UINTN)hi[i][1] &&
                    bb >= (UINTN)lo[i][2] && bb <= (UINTN)hi[i][2])
                    pop += hist[b];
            }
            UINT32 score = (UINT32)((UINT64)range * pop);
            if (score > pickmax) { pickmax = score; pick = i; pickaxis = rax; pickpop = pop; }
        }
        if (pick < 0 || pickpop == 0) break;

        /* Sweep counts along the chosen axis to find the weighted median. */
        UINT32 sweep[32];
        memset(sweep, 0, sizeof(sweep));
        for (UINTN b = 0; b < CAP_BUCKETS; b++) {
            UINTN r = b >> 10, gg = (b >> 5) & 31, bb = b & 31;
            if (r >= (UINTN)lo[pick][0] && r <= (UINTN)hi[pick][0] &&
                gg >= (UINTN)lo[pick][1] && gg <= (UINTN)hi[pick][1] &&
                bb >= (UINTN)lo[pick][2] && bb <= (UINTN)hi[pick][2]) {
                UINTN v = (pickaxis == 0) ? r : (pickaxis == 1 ? gg : bb);
                sweep[v] += hist[b];
            }
        }
        UINT32 acc = 0, half = pickpop / 2;
        INTN split = hi[pick][pickaxis];
        for (INTN v = lo[pick][pickaxis]; v <= hi[pick][pickaxis]; v++) {
            acc += sweep[v];
            if (acc >= half) { split = v; break; }
        }
        if (split >= hi[pick][pickaxis]) split = hi[pick][pickaxis] - 1;
        if (split < lo[pick][pickaxis]) break;

        for (INTN a = 0; a < 3; a++) {
            lo[nb][a] = lo[pick][a];
            hi[nb][a] = hi[pick][a];
        }
        lo[nb][pickaxis] = split + 1;
        hi[pick][pickaxis] = split;
        nb++;
    }

    for (INTN i = 0; i < nb; i++) {
        UINT64 sr = 0, sg = 0, sb = 0;
        UINT32 cnt = 0;
        for (UINTN b = 0; b < CAP_BUCKETS; b++) {
            UINTN r = b >> 10, gg = (b >> 5) & 31, bb = b & 31;
            if (r >= (UINTN)lo[i][0] && r <= (UINTN)hi[i][0] &&
                gg >= (UINTN)lo[i][1] && gg <= (UINTN)hi[i][1] &&
                bb >= (UINTN)lo[i][2] && bb <= (UINTN)hi[i][2]) {
                UINT32 c = hist[b];
                if (c) {
                    sr += (UINT64)(r * 255 / 31) * c;
                    sg += (UINT64)(gg * 255 / 31) * c;
                    sb += (UINT64)(bb * 255 / 31) * c;
                    cnt += c;
                }
            }
        }
        if (cnt) {
            palette[i * 3 + 0] = (UINT8)(sr / cnt);
            palette[i * 3 + 1] = (UINT8)(sg / cnt);
            palette[i * 3 + 2] = (UINT8)(sb / cnt);
        } else {
            palette[i * 3 + 0] = palette[i * 3 + 1] =
            palette[i * 3 + 2] = 0;
        }
    }
    return (UINTN)nb;
}

static int cap_gif_quantize(const UINT32 *pixels, UINTN n,
                            UINT8 *palette, UINT8 *idx) {
    UINT32 *hist = efi_allocate_pool(sizeof(UINT32) * CAP_BUCKETS);
    UINT8  *bucket_idx = efi_allocate_pool(CAP_BUCKETS);
    if (!hist || !bucket_idx) {
        if (hist) efi_free_pool(hist);
        if (bucket_idx) efi_free_pool(bucket_idx);
        return 0;
    }
    memset(hist, 0, sizeof(UINT32) * CAP_BUCKETS);

    for (UINTN i = 0; i < n; i++) {
        UINT32 p = pixels[i];
        UINT32 bucket = ((((p >> 16) & 0xF8) << 7) | (((p >> 8) & 0xF8) << 2)
                         | ((p & 0xF8) >> 3));
        hist[bucket]++;
    }

    UINTN paln = cap_median_cut(hist, palette, 256);
    while (paln < 256) {
        palette[paln * 3 + 0] = palette[paln * 3 + 1] =
        palette[paln * 3 + 2] = 0;
        paln++;
    }

    for (UINTN b = 0; b < CAP_BUCKETS; b++) {
        UINTN rb = b >> 10, gb = (b >> 5) & 31, bb = b & 31;
        INTN  r = (INTN)((rb * 255) / 31), gg = (INTN)((gb * 255) / 31),
              bl = (INTN)((bb * 255) / 31);
        UINT32 bestd = 0xFFFFFFFFu; UINT8 besti = 0;
        for (UINTN pi = 0; pi < paln; pi++) {
            INTN dr = r - palette[pi * 3 + 0];
            INTN dg = gg - palette[pi * 3 + 1];
            INTN db = bl - palette[pi * 3 + 2];
            UINT32 d = (UINT32)(dr * dr + dg * dg + db * db);
            if (d < bestd) { bestd = d; besti = (UINT8)pi; }
        }
        bucket_idx[b] = besti;
    }

    for (UINTN i = 0; i < n; i++) {
        UINT32 p = pixels[i];
        UINTN bucket = ((((p >> 16) & 0xF8) << 7) | (((p >> 8) & 0xF8) << 2)
                        | ((p & 0xF8) >> 3));
        idx[i] = bucket_idx[bucket];
    }

    efi_free_pool(hist);
    efi_free_pool(bucket_idx);
    return 1;
}

cap_gif *cap_gif_new(UINTN w, UINTN h) {
    if (!w || !h || w > 0x7FFF || h > 0x7FFF) return NULL;
    cap_gif *g = efi_allocate_pool(sizeof(cap_gif));
    if (!g) return NULL;
    g->w = w; g->h = h; g->nframes = 0;
    for (UINTN i = 0; i < CAP_GIF_MAX_FRAMES; i++) g->frames[i] = NULL;
    return g;
}

int cap_gif_frame(cap_gif *g, const UINT32 *pixels, UINTN delay_cs) {
    if (!g || !pixels) return 0;
    if (g->nframes >= CAP_GIF_MAX_FRAMES) return 0;
    UINTN n = g->w * g->h;
    cap_gif_fr *fr = efi_allocate_pool(sizeof(cap_gif_fr));
    if (!fr) return 0;
    fr->idx = efi_allocate_pool(n);
    if (!fr->idx) { efi_free_pool(fr); return 0; }
    if (!cap_gif_quantize(pixels, n, fr->palette, fr->idx)) {
        efi_free_pool(fr->idx);
        efi_free_pool(fr);
        return 0;
    }
    g->frames[g->nframes] = fr;
    g->delays[g->nframes] = (UINT32)delay_cs;
    g->nframes++;
    return 1;
}

UINTN cap_gif_count(const cap_gif *g) {
    return g ? g->nframes : 0;
}

void cap_gif_free(cap_gif *g) {
    if (!g) return;
    for (UINTN i = 0; i < g->nframes; i++) {
        if (g->frames[i]) {
            if (g->frames[i]->idx) efi_free_pool(g->frames[i]->idx);
            efi_free_pool(g->frames[i]);
        }
    }
    efi_free_pool(g);
}

/* LZW sub-block writer */

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

/* Hash table mapping (prefix,char) -> code. Open addressing; 8191 slots. */
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

/* Returns code or -1 if (px,c) not in the dictionary. */
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

static EFI_STATUS cap_gif_lzw_encode(cap_gif *g, const UINT8 *idx, UINTN n,
                                     UINT8 **data, UINTN *datalen,
                                     UINTN *min_code_size) {
    (void)g;
    UINTN minsize = 8;
    *min_code_size = minsize;

    lzw_writer w;
    memset(&w, 0, sizeof(w));
    if (!cap_buf_reserve(&w.out, n + n / 2 + 64)) return EFI_OUT_OF_RESOURCES;

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

    /* Single-symbol codes 0..255 are implicit. */
    for (UINTN c = 0; c < 256; c++) { d.pref[c] = 0; d.ch[c] = (UINT8)c; }
    lzw_dict_clear(&d);

    UINTN codesize = minsize + 1;
    UINTN next = CAP_GIF_EOI + 1;
    UINTN prefix;

    lzw_code(&w, CAP_GIF_CLEAR, codesize);

    if (n > 0) {
        prefix = (UINTN)idx[0];
        for (UINTN i = 1; i < n; i++) {
            UINTN c = (UINTN)idx[i];
            int f = lzw_dict_find(&d, prefix, c);
            if (f >= 0) {
                prefix = (UINTN)f;
            } else {
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
        lzw_code(&w, prefix, codesize);
    }

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

static EFI_STATUS cap_gif_build(cap_gif *g, UINT8 **out, UINTN *out_size) {
    cap_buf outb;
    outb.buf = NULL; outb.len = 0; outb.cap = 0;
    if (!cap_buf_reserve(&outb, 4096 + g->nframes * (g->w * g->h / 2))) goto oom;

    /* Header - no global colour table; each frame carries its own palette. */
    if (!cap_buf_put(&outb, (const UINT8*)"GIF89a", 6)) goto oom;
    if (!cap_buf_u16le(&outb, (UINT16)g->w)) goto oom;
    if (!cap_buf_u16le(&outb, (UINT16)g->h)) goto oom;
    outb.buf[outb.len++] = 0x00;
    outb.buf[outb.len++] = 0x00;
    outb.buf[outb.len++] = 0x00;

    for (UINTN f = 0; f < g->nframes; f++) {
        cap_gif_fr *fr = g->frames[f];
        if (!fr || !fr->idx) goto oom;

        /* Graphic Control Extension: dispose=0, no transparency; delay */
        UINT8 gce[8] = { 0x21, 0xF9, 0x04, 0x00,
                         (UINT8)g->delays[f], (UINT8)(g->delays[f] >> 8),
                         0x00, 0x00 };
        if (!cap_buf_put(&outb, gce, 8)) goto oom;

        /* Image Descriptor with a 256-entry local colour table */
        UINT8 id[10] = { 0x2C,
                         0, 0, 0, 0,
                         (UINT8)g->w, (UINT8)(g->w >> 8),
                         (UINT8)g->h, (UINT8)(g->h >> 8),
                         0x87 };
        if (!cap_buf_put(&outb, id, 10)) goto oom;
        if (!cap_buf_put(&outb, fr->palette, 256 * 3)) goto oom;

        /* LZW data */
        UINT8 *fdata = NULL; UINTN fdatalen = 0; UINTN minsize = 0;
        EFI_STATUS s = cap_gif_lzw_encode(g, fr->idx, g->w * g->h,
                                          &fdata, &fdatalen, &minsize);
        if (EFI_ERROR(s)) { if (fdata) efi_free_pool(fdata); goto oom; }
        UINT8 ms = (UINT8)minsize;
        if (!cap_buf_put(&outb, &ms, 1)) { efi_free_pool(fdata); goto oom; }
        if (!cap_buf_put(&outb, fdata, fdatalen)) { efi_free_pool(fdata); goto oom; }
        efi_free_pool(fdata);
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
    if (EFI_ERROR(cap_gif_build(g, out, out_size))) {
        cap_gif_free(g);
        return EFI_OUT_OF_RESOURCES;
    }
    cap_gif_free(g);
    return EFI_SUCCESS;
}

/* ESP file helpers */

#ifndef CAP_NO_IO

EFI_STATUS cap_ensure_dir(const CHAR16 *path) {
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return EFI_DEVICE_ERROR;
    EFI_FILE_PROTOCOL *d = NULL;
    EFI_STATUS st = root->Open(root, &d, (CHAR16*)path, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(st) || !d) {
        st = root->Open(root, &d, (CHAR16*)path,
                        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                        EFI_FILE_DIRECTORY);
    }
    if (d) d->Close(d);
    root->Close(root);
    return EFI_ERROR(st) ? st : EFI_SUCCESS;
}

EFI_STATUS cap_save_file(const CHAR16 *path, const UINT8 *data, UINTN size) {
    if (!path || (!data && size)) return EFI_INVALID_PARAMETER;
    EFI_FILE_PROTOCOL *root = efi_boot_volume_root();
    if (!root) return EFI_DEVICE_ERROR;
    EFI_FILE_PROTOCOL *f = NULL;
    EFI_STATUS st = root->Open(root, &f, (CHAR16*)path,
                               EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                               0);
    if (EFI_ERROR(st) || !f) {
        root->Close(root);
        return EFI_DEVICE_ERROR;
    }
    UINTN w = size ? size : 0;
    st = f->Write(f, &w, (void*)data);
    if (!EFI_ERROR(st)) st = f->Flush(f);
    if (EFI_ERROR(st)) st = EFI_DEVICE_ERROR;
    f->Close(f);
    root->Close(root);
    return st;
}

void cap_timestamp_name(CHAR16 *out, UINTN cap, const CHAR16 *base,
                        const CHAR16 *ext) {
    UINTN ho = 0, mi = 0, se = 0;
    EFI_TIME t;
    if (!EFI_ERROR(RT->GetTime(&t, NULL))) {
        ho = t.Hour; mi = t.Minute; se = t.Second;
    }
    if (ho > 99) ho = 0;
    if (mi > 99) mi = 0;
    if (se > 99) se = 0;
    /* base_HHMMSS.ext */
    UINTN n = 0;
    for (UINTN i = 0; base && base[i] && n + 1 < cap; i++) out[n++] = base[i];
    if (n + 1 < cap) out[n++] = L'_';
    CHAR16 digs[6];
    digs[0] = (CHAR16)(L'0' + ho / 10); digs[1] = (CHAR16)(L'0' + ho % 10);
    digs[2] = (CHAR16)(L'0' + mi / 10); digs[3] = (CHAR16)(L'0' + mi % 10);
    digs[4] = (CHAR16)(L'0' + se / 10); digs[5] = (CHAR16)(L'0' + se % 10);
    for (UINTN i = 0; i < 6 && n + 1 < cap; i++) out[n++] = digs[i];
    if (ext) for (UINTN i = 0; ext[i] && n + 1 < cap; i++) out[n++] = ext[i];
    out[n < cap ? n : cap - 1] = 0;
}

#endif
