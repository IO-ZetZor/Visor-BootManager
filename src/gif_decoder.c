#include "gui.h"
#include "efi_helpers.h"
#include <efi.h>
#include <efilib.h>


#define GIF_MAX_DIM    8192
#define GIF_MAX_PIXELS (16u * 1024u * 1024u)
#define GIF_MAX_FRAMES 4096

#define LZW_MAX_CODES 4096

static int gif_mul_overflow(UINTN a, UINTN b, UINTN *out) {
    if (a && b > ~(UINTN)0 / a) return 1;
    *out = a * b;
    return 0;
}

static UINT16 gif_rd16(const UINT8 *p) {
    return (UINT16)(p[0] | ((UINT16)p[1] << 8));
}

static void gif_skip_subblocks(const UINT8 *data, UINTN size, UINTN *off) {
    UINTN p = *off;
    while (p < size) {
        UINT8 n = data[p];
        if (n == 0) { p++; break; }
        if (p + 1 + n > size) { p = size; break; }
        p += 1 + n;
    }
    *off = p;
}

typedef struct {
    const UINT8 *data;
    UINTN  size;
    UINTN  pos;
    UINTN  left;
    UINT32 acc;
    int    nbits;
} gif_bits_t;

static void bits_init(gif_bits_t *b, const UINT8 *data, UINTN size, UINTN off) {
    b->data = data; b->size = size; b->pos = off;
    b->left = 0; b->acc = 0; b->nbits = 0;
}

static void bits_refill(gif_bits_t *b) {
    while (b->nbits <= 24) {
        if (b->left == 0) {
            if (b->pos >= b->size) return;
            UINT8 n = b->data[b->pos];
            if (n == 0) return;
            if (b->pos + 1 + n > b->size) return;
            b->pos++;
            b->left = n;
        }
        b->acc |= (UINT32)b->data[b->pos] << b->nbits;
        b->pos++;
        b->left--;
        b->nbits += 8;
    }
}

static int bits_get(gif_bits_t *b, int count, UINT32 *out) {
    if (b->nbits < count) bits_refill(b);
    if (b->nbits < count) return 0;
    *out = b->acc & ((1u << count) - 1);
    b->acc >>= count;
    b->nbits -= count;
    return 1;
}

typedef struct {
    UINT8 *out;
    UINTN  w, h;
    UINTN  total;
    UINTN  written;
    int    interlace;
    UINTN  row, col;
    int    pass;
} idx_sink_t;

static const UINTN gif_pass_start[4] = {0, 4, 2, 1};
static const UINTN gif_pass_step[4]  = {8, 8, 4, 2};

static void sink_put(idx_sink_t *s, UINT8 v) {
    if (s->written >= s->total) return;

    if (!s->interlace) {
        s->out[s->written++] = v;
        return;
    }

    if (s->row >= s->h) { s->written++; return; }
    s->out[s->row * s->w + s->col] = v;
    s->written++;
    if (++s->col >= s->w) {
        s->col = 0;
        s->row += gif_pass_step[s->pass];
        while (s->row >= s->h && s->pass < 3) {
            s->pass++;
            s->row = gif_pass_start[s->pass];
        }
    }
}

static int gif_lzw_decode(anim_t *a, UINTN *off, UINT8 min_code_size,
                          idx_sink_t *sink) {
    UINTN chain_start = *off;

    if (min_code_size < 1 || min_code_size > 8) {
        gif_skip_subblocks(a->data, a->size, off);
        return 0;
    }

    UINT16 clear_code = (UINT16)(1u << min_code_size);
    UINT16 end_code   = (UINT16)(clear_code + 1);
    UINT16 next_code  = (UINT16)(clear_code + 2);
    int    code_size  = min_code_size + 1;
    INTN   prev       = -1;
    UINT8  prev_first = 0;

    UINT16 *prefix = a->lzw_prefix;
    UINT8  *suffix = a->lzw_suffix;
    UINT8  *stack  = a->lzw_stack;

    gif_bits_t br;
    bits_init(&br, a->data, a->size, *off);

    for (;;) {
        UINT32 code;
        if (!bits_get(&br, code_size, &code)) break;

        if (code == clear_code) {
            next_code = (UINT16)(clear_code + 2);
            code_size = min_code_size + 1;
            prev = -1;
            continue;
        }
        if (code == end_code) break;

        if (prev < 0) {
            if (code >= clear_code) break;
            sink_put(sink, (UINT8)code);
            prev = (INTN)code;
            prev_first = (UINT8)code;
            if (sink->written >= sink->total) break;
            continue;
        }

        UINTN  sp = 0;
        UINT32 cur;

        if (code < next_code) {
            cur = code;
        } else if (code == next_code) {
            stack[sp++] = prev_first;
            cur = (UINT32)prev;
        } else {
            break;
        }

        while (cur >= clear_code) {
            if (sp >= LZW_MAX_CODES || cur >= LZW_MAX_CODES) goto done;
            stack[sp++] = suffix[cur];
            cur = prefix[cur];
        }
        if (sp >= LZW_MAX_CODES) break;
        stack[sp++] = (UINT8)cur;

        UINT8 first = (UINT8)cur;

        if (next_code < LZW_MAX_CODES) {
            prefix[next_code] = (UINT16)prev;
            suffix[next_code] = first;
            next_code++;
            if (next_code >= (1u << code_size) && code_size < 12) code_size++;
        }

        while (sp > 0) sink_put(sink, stack[--sp]);

        prev = (INTN)code;
        prev_first = first;

        if (sink->written >= sink->total) break;
    }

done:
    *off = chain_start;
    gif_skip_subblocks(a->data, a->size, off);
    return 1;
}
void gif_free(anim_t *a) {
    if (!a) return;
    if (a->data)       efi_free_pool(a->data);
    if (a->canvas)     efi_free_pool(a->canvas);
    if (a->restore)    efi_free_pool(a->restore);
    if (a->plane)      efi_free_pool(a->plane);
    if (a->lzw_prefix) efi_free_pool(a->lzw_prefix);
    if (a->lzw_suffix) efi_free_pool(a->lzw_suffix);
    if (a->lzw_stack)  efi_free_pool(a->lzw_stack);
    if (a->xmap)       efi_free_pool(a->xmap);
    if (a->ymap)       efi_free_pool(a->ymap);
    efi_free_pool(a);
}

static int gif_step(anim_t *a) {
    UINTN off = a->next_off;
    UINTN W = a->width, H = a->height;
    UINTN canvas_bytes = W * H * sizeof(UINT32);

    int   disposal = 0;
    int   transp   = -1;
    UINTN delay_cs = 0;

    while (off < a->size) {
        UINT8 block = a->data[off++];

        if (block == 0x3B) break;

        if (block == 0x21) {
            if (off >= a->size) break;
            UINT8 label = a->data[off++];
            if (label == 0xF9 && off < a->size &&
                a->data[off] >= 4 && off + 1 + a->data[off] <= a->size) {
                const UINT8 *g = a->data + off + 1;
                disposal = (g[0] >> 2) & 0x07;
                transp   = (g[0] & 0x01) ? (int)g[3] : -1;
                delay_cs = gif_rd16(g + 1);
            }
            gif_skip_subblocks(a->data, a->size, &off);
            continue;
        }

        if (block != 0x2C) continue;

        if (off + 9 > a->size) break;
        UINTN fx = gif_rd16(a->data + off);
        UINTN fy = gif_rd16(a->data + off + 2);
        UINTN fw = gif_rd16(a->data + off + 4);
        UINTN fh = gif_rd16(a->data + off + 6);
        UINT8 packed = a->data[off + 8];
        off += 9;

        const UINT8 *pal   = a->gct;
        UINTN        pal_n = a->gct_n;
        if (packed & 0x80) {
            UINTN lct_n = (UINTN)1 << ((packed & 0x07) + 1);
            if (off + lct_n * 3 > a->size) break;
            pal   = a->data + off;
            pal_n = lct_n;
            off += lct_n * 3;
        }
        int interlace = (packed & 0x40) ? 1 : 0;

        if (off >= a->size) break;
        UINT8 min_code_size = a->data[off++];

        UINTN plane_px = 0;
        if (fw == 0 || fh == 0 || gif_mul_overflow(fw, fh, &plane_px) ||
            plane_px > W * H || pal_n == 0) {
            gif_skip_subblocks(a->data, a->size, &off);
            disposal = 0; transp = -1; delay_cs = 0;
            continue;
        }

        ZeroMem(a->plane, plane_px);

        idx_sink_t sink;
        ZeroMem(&sink, sizeof(sink));
        sink.out = a->plane;
        sink.w = fw; sink.h = fh; sink.total = plane_px;
        sink.interlace = interlace;

        gif_lzw_decode(a, &off, min_code_size, &sink);

        if (disposal == 3 && a->next_off == a->first_frame_off) disposal = 1;

        if (disposal == 3) {
            if (!a->restore) a->restore = efi_allocate_pool(canvas_bytes);
            if (a->restore) CopyMem(a->restore, a->canvas, canvas_bytes);
            else disposal = 1;
        }

        for (UINTN y = 0; y < fh; y++) {
            UINTN cy = fy + y;
            if (cy >= H) break;
            const UINT8 *srow = a->plane + y * fw;
            UINT32 *drow = a->canvas + cy * W;
            for (UINTN x = 0; x < fw; x++) {
                UINTN cx = fx + x;
                if (cx >= W) break;
                UINT8 idx = srow[x];
                if (transp >= 0 && idx == (UINT8)transp) continue;
                if (idx >= pal_n) continue;
                const UINT8 *c = pal + (UINTN)idx * 3;
                drow[cx] = 0xFF000000u | ((UINT32)c[0] << 16) |
                           ((UINT32)c[1] << 8) | c[2];
            }
        }

        UINTN ms = delay_cs * 10;
        if (delay_cs <= 1) ms = 100;
        if (ms > 60000) ms = 60000;
        a->cur_delay = (UINT16)ms;

        a->next_off = off;
        a->pending_disposal = disposal;
        a->pending_x = fx; a->pending_y = fy;
        a->pending_w = fw; a->pending_h = fh;
        return 1;
    }

    a->next_off = a->size;
    return 0;
}

static void gif_dispose(anim_t *a) {
    UINTN W = a->width, H = a->height;

    if (a->pending_disposal == 2) {
        for (UINTN y = 0; y < a->pending_h; y++) {
            UINTN cy = a->pending_y + y;
            if (cy >= H) break;
            UINT32 *drow = a->canvas + cy * W;
            for (UINTN x = 0; x < a->pending_w; x++) {
                UINTN cx = a->pending_x + x;
                if (cx >= W) break;
                drow[cx] = 0;
            }
        }
    } else if (a->pending_disposal == 3 && a->restore) {
        CopyMem(a->canvas, a->restore, W * H * sizeof(UINT32));
    }
    a->pending_disposal = 0;
}

int gif_advance(anim_t *a) {
    if (!a || a->frame_count < 2) return 0;

    gif_dispose(a);

    if (gif_step(a)) {
        a->cur++;
        return 1;
    }

    a->loops_done++;
    if (a->loops && a->loops_done >= a->loops) return 0;

    a->next_off = a->first_frame_off;
    a->pending_disposal = 0;
    ZeroMem(a->canvas, a->width * a->height * sizeof(UINT32));
    a->cur = 0;

    return gif_step(a) ? 1 : 0;
}
static UINTN gif_scan(const UINT8 *d, UINTN size, UINTN start, UINTN *loops_out) {
    UINTN off = start, n = 0;
    *loops_out = 0;

    while (off < size && n < GIF_MAX_FRAMES) {
        UINT8 block = d[off++];

        if (block == 0x3B) break;

        if (block == 0x21) {
            if (off >= size) break;
            UINT8 label = d[off++];
            if (label == 0xFF && off + 12 <= size && d[off] == 11 &&
                d[off + 1] == 'N' && d[off + 2] == 'E' &&
                d[off + 3] == 'T' && d[off + 4] == 'S') {
                UINTN p = off + 12;
                if (p + 3 <= size && d[p] == 3)
                    *loops_out = gif_rd16(d + p + 2);
            }
            gif_skip_subblocks(d, size, &off);
            continue;
        }

        if (block != 0x2C) continue;

        if (off + 9 > size) break;
        UINT8 packed = d[off + 8];
        off += 9;
        if (packed & 0x80) {
            UINTN lct_n = (UINTN)1 << ((packed & 0x07) + 1);
            if (off + lct_n * 3 > size) break;
            off += lct_n * 3;
        }
        if (off >= size) break;
        off++;
        gif_skip_subblocks(d, size, &off);
        n++;
    }

    return n;
}

anim_t* gif_load(UINT8 *data, UINTN size) {
    if (size < 13) { efi_log(L"  ERROR: GIF too small"); return NULL; }
    if (data[0] != 'G' || data[1] != 'I' || data[2] != 'F') {
        efi_log(L"  ERROR: bad GIF signature");
        return NULL;
    }

    UINTN W = gif_rd16(data + 6);
    UINTN H = gif_rd16(data + 8);
    UINT8 lsd_packed = data[10];

    if (W == 0 || H == 0 || W > GIF_MAX_DIM || H > GIF_MAX_DIM) {
        efi_log(L"  ERROR: unsupported GIF dimensions");
        return NULL;
    }

    UINTN canvas_px = 0, canvas_bytes = 0;
    if (gif_mul_overflow(W, H, &canvas_px) || canvas_px > GIF_MAX_PIXELS ||
        gif_mul_overflow(canvas_px, sizeof(UINT32), &canvas_bytes)) {
        efi_log(L"  ERROR: GIF canvas too large");
        return NULL;
    }

    anim_t *a = efi_allocate_pool(sizeof(anim_t));
    if (!a) return NULL;
    ZeroMem(a, sizeof(anim_t));

    a->width  = W;
    a->height = H;

    UINTN off = 13;
    if (lsd_packed & 0x80) {
        a->gct_n = (UINTN)1 << ((lsd_packed & 0x07) + 1);
        if (off + a->gct_n * 3 > size) {
            efi_log(L"  ERROR: GIF global colour table truncated");
            efi_free_pool(a);
            return NULL;
        }
        CopyMem(a->gct, data + off, a->gct_n * 3);
        off += a->gct_n * 3;
    }

    a->data       = efi_allocate_pool(size);
    a->canvas     = efi_allocate_pool(canvas_bytes);
    a->plane      = efi_allocate_pool(canvas_px);
    a->lzw_prefix = efi_allocate_pool(LZW_MAX_CODES * sizeof(UINT16));
    a->lzw_suffix = efi_allocate_pool(LZW_MAX_CODES);
    a->lzw_stack  = efi_allocate_pool(LZW_MAX_CODES);

    if (!a->data || !a->canvas || !a->plane ||
        !a->lzw_prefix || !a->lzw_suffix || !a->lzw_stack) {
        efi_log(L"  ERROR: out of memory decoding GIF");
        gif_free(a);
        return NULL;
    }

    CopyMem(a->data, data, size);
    a->size = size;
    ZeroMem(a->canvas, canvas_bytes);
    ZeroMem(a->lzw_prefix, LZW_MAX_CODES * sizeof(UINT16));
    ZeroMem(a->lzw_suffix, LZW_MAX_CODES);

    a->first_frame_off = off;
    a->next_off        = off;
    a->frame_count     = gif_scan(a->data, size, off, &a->loops);

    if (a->frame_count == 0 || !gif_step(a)) {
        efi_log(L"  ERROR: GIF has no usable frames");
        gif_free(a);
        return NULL;
    }

    { CHAR16 msg[128];
      SPrint(msg, sizeof(msg), L"  gif: %dx%d, %d frames, %d KB resident",
             (int)W, (int)H, (int)a->frame_count,
             (int)((size + canvas_bytes + canvas_px) / 1024));
      efi_log(msg); }

    return a;
}


