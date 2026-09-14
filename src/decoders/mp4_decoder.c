/* mp4_decoder.c - MP4 container parsing over the MJPEG decoder (feature: mp4) */
#include "anim_decode.h"
#define MP4_MAX_SAMPLES 16384
#define MP4_MAX_FILE    (512u * 1024u * 1024u)

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
    if (sample_size == 0 && stsz.size < 20 + (UINTN)sample_count * 4) return 0;

    const UINT8 *pt = stts.d + stts.pos;
    UINT32 stts_n = rd32(pt + 4);
    if (stts.size < 16 + (UINTN)stts_n * 8) return 0;

    const UINT8 *pc = stsc.d + stsc.pos;
    UINT32 stsc_n = rd32(pc + 4);
    if (stsc.size < 16 + (UINTN)stsc_n * 12) return 0;

    const UINT8 *po = have_stco ? (stco.d + stco.pos) : (co64.d + co64.pos);
    UINT32 chunk_n = rd32(po + 4);
    UINTN chunk_bytes = have_stco ? (UINTN)chunk_n * 4 : (UINTN)chunk_n * 8;
    if (have_stco ? (stco.size < 16 + chunk_bytes)
                  : (co64.size < 16 + chunk_bytes)) return 0;

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
