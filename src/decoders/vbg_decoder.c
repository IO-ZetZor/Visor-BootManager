/* vbg_decoder.c - Visor background codec (feature: vbg) */
#include "anim_decode.h"

#define VBG_MAX_FRAMES   16384
#define VBG_TILE_LOG2_MIN 2
#define VBG_TILE_LOG2_MAX 6
#define VBG_FLAG_MC       1u

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

void vbg_free(anim_t *a) {
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

int vbg_advance(anim_t *a) {
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
