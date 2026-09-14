/* gui_text.c - glyph rasterisation cache and text drawing */
#include "gui_internal.h"

static const font_t *g_font = &jetbrains_font;

static const unsigned char *g_glyph_cov = 0;
static const font_t        *g_cov_for   = 0;

static void font_ensure_decoded(void) {
    if (g_cov_for == g_font && g_glyph_cov) return;
    const font_t *f = g_font;
    unsigned char *out = efi_allocate_pool(f->unpacked_size);
    g_cov_for = f;
    if (!out) { g_glyph_cov = 0; return; }

    const unsigned char *in     = f->pixels;
    const unsigned char *in_end = in + f->packed_size;
    UINTN o = 0;
    while (in < in_end && o < f->unpacked_size) {
        signed char n = (signed char)*in++;
        if (n >= 0) {
            UINTN cnt = (UINTN)n + 1;
            while (cnt-- && in < in_end && o < f->unpacked_size) out[o++] = *in++;
        } else if (n != -128) {
            if (in >= in_end) break;
            UINTN cnt = (UINTN)(1 - (int)n);
            unsigned char v = *in++;
            while (cnt-- && o < f->unpacked_size) out[o++] = v;
        }
    }
    while (o < f->unpacked_size) out[o++] = 0;
    g_glyph_cov = out;
}

void gui_set_font(const char *name) {
    if (name && name[0]) {
        int is_jb = (name[0] == 'j' || name[0] == 'J');
        if (!is_jb)
            efi_log(L"WARN: font= ignored - only the built-in 'jetbrains' font is available");
    }
    g_font = &jetbrains_font;
    glyph_cache_flush();
}

#define FIXQ         16
#define FIXONE       (1 << FIXQ)
#define GLYPH_PAD    1
#define GLYPH_PHASES 4
#define GLYPH_SLOTS  512

typedef struct {
    UINT16  cp;
    UINT16  px;
    UINT8   phase;
    UINT8   valid;
    UINT16  w, h;
    INT16   ox, oy;
    UINT8  *cov;
} glyph_entry_t;

static glyph_entry_t g_glyphs[GLYPH_SLOTS];

void glyph_cache_flush(void) {
    for (UINTN i = 0; i < GLYPH_SLOTS; i++) {
        if (g_glyphs[i].cov) efi_free_pool(g_glyphs[i].cov);
        g_glyphs[i].cov = NULL;
        g_glyphs[i].valid = 0;
    }
}

static void cov_axis(const UINT8 *src, UINTN sn, UINTN sstride,
                     UINT8 *dst, UINTN dn, UINTN dstride,
                     UINTN lines, UINTN line_src, UINTN line_dst,
                     UINTN step, UINTN phase)
{
    UINTN width = step < FIXONE ? FIXONE : step;
    UINTN half  = width / 2;
    INTN  poff  = (INTN)(((UINT64)phase * step) >> FIXQ);

    for (UINTN i = 0; i < dn; i++) {
        INTN c  = ((INTN)i - GLYPH_PAD) * (INTN)step + (INTN)(step / 2) - poff;
        INTN lo = c - (INTN)half;
        INTN hi = c + (INTN)half;
        INTN k0 = lo >> FIXQ;
        INTN k1 = (hi + FIXONE - 1) >> FIXQ;
        if (k0 < 0) k0 = 0;
        if (k1 > (INTN)sn) k1 = (INTN)sn;

        for (UINTN l = 0; l < lines; l++) {
            const UINT8 *s = src + l * line_src;
            UINT64 acc = 0;
            for (INTN k = k0; k < k1; k++) {
                INTN a = (k << FIXQ), b = a + FIXONE;
                if (a < lo) a = lo;
                if (b > hi) b = hi;
                if (b <= a) continue;
                acc += (UINT64)s[(UINTN)k * sstride] * (UINT64)(b - a);
            }
            UINT64 v = (acc + (width >> 1)) / width;
            if (v > 255) v = 255;
            dst[l * line_dst + i * dstride] = (UINT8)v;
        }
    }
}

static glyph_entry_t *glyph_get(CHAR16 cp, UINTN dh, UINTN phase) {
    const font_t *f = g_font;
    if (cp < f->first || cp > f->last) cp = '?';
    UINTN slot = ((UINTN)cp * 2654435761u + dh * 97u + phase) & (GLYPH_SLOTS - 1);
    glyph_entry_t *e = &g_glyphs[slot];
    if (e->valid && e->cp == cp && e->px == dh && e->phase == phase) return e;

    const glyph_t *g = &f->glyphs[cp - f->first];
    if (e->cov) efi_free_pool(e->cov);
    e->cov = NULL;
    e->valid = 1;
    e->cp = (UINT16)cp;
    e->px = (UINT16)dh;
    e->phase = (UINT8)phase;
    e->w = e->h = 0;
    e->ox = e->oy = 0;
    if (!g->w || !g->h || !g_glyph_cov || !dh) return e;

    UINTN size = f->size;
    UINTN step = ((UINTN)size << FIXQ) / dh;
    UINTN dw = ((UINTN)g->w * dh + size - 1) / size + 2 * GLYPH_PAD;
    UINTN dhh = ((UINTN)g->h * dh + size - 1) / size + 2 * GLYPH_PAD;
    if (!dw || !dhh || dw > 4096 || dhh > 4096) return e;

    UINT8 *tmp = efi_allocate_pool(dw * (UINTN)g->h);
    UINT8 *out = efi_allocate_pool(dw * dhh);
    if (!tmp || !out) {
        if (tmp) efi_free_pool(tmp);
        if (out) efi_free_pool(out);
        return e;
    }

    const UINT8 *cov = (const UINT8 *)g_glyph_cov + g->pixel_offset;
    UINTN ph = (phase << FIXQ) / GLYPH_PHASES;

    cov_axis(cov, g->w, 1, tmp, dw, 1, g->h, g->w, dw, step, ph);
    cov_axis(tmp, g->h, dw, out, dhh, dw, dw, 1, 1, step, 0);

    efi_free_pool(tmp);
    e->cov = out;
    e->w = (UINT16)dw;
    e->h = (UINT16)dhh;
    e->ox = -GLYPH_PAD;
    e->oy = -GLYPH_PAD;
    return e;
}

static void blend_glyph(gui_state_t *state, const glyph_entry_t *e, UINT32 rgb,
                        INTN dx, INTN dyTop, INTN master) {
    if (!e->cov || !e->w || !e->h) return;
    UINT8 fr = (rgb >> 16) & 0xFF, fg = (rgb >> 8) & 0xFF, fb = rgb & 0xFF;
    for (UINTN j = 0; j < e->h; j++) {
        INTN py = dyTop + (INTN)j;
        if (py < 0 || py >= (INTN)state->screen_height) continue;
        const UINT8 *row = e->cov + j * e->w;
        for (UINTN i = 0; i < e->w; i++) {
            UINT8 a = row[i];
            if (!a) continue;
            if (master < 255) a = (UINT8)((UINTN)a * (UINTN)master / 255);
            if (!a) continue;
            INTN px = dx + (INTN)i;
            if (px < 0 || px >= (INTN)state->screen_width) continue;
            UINT32 *p = get_pixel(state, (UINTN)px, (UINTN)py);
            if (!p) continue;
            UINT8 br = (*p >> 16) & 0xFF, bg = (*p >> 8) & 0xFF, bb = *p & 0xFF;
            UINT8 r = (fr * a + br * (255 - a)) / 255;
            UINT8 gg = (fg * a + bg * (255 - a)) / 255;
            UINT8 b = (fb * a + bb * (255 - a)) / 255;
            *p = (0xFFu << 24) | (r << 16) | (gg << 8) | b;
        }
    }
}

UINTN text_width_px(CHAR16 *text, UINTN dh) {
    if (!text) return 0;
    UINTN pen = 0;
    UINTN size = g_font->size;
    while (*text) {
        CHAR16 c = *text++;
        if (c < g_font->first || c > g_font->last) c = '?';
        const glyph_t *g = &g_font->glyphs[c - g_font->first];
        pen += (UINTN)g->advance_q6 * dh / size;
    }
    return (pen + 32) / 64;
}

void draw_text_px_a(gui_state_t *state, CHAR16 *text, INTN x, INTN y,
                           color_t color, UINTN dh, INTN master) {
    if (!text || master <= 0 || !dh) return;
    if (master > 255) master = 255;
    font_ensure_decoded();
    UINT32 rgb = color_to_u32(color) & 0x00FFFFFF;
    UINTN size = g_font->size;
    INTN baseline = y + scale_metric((INTN)g_font->ascent, dh, size);
    INTN pen64 = x * 64;
    while (*text) {
        CHAR16 c = *text++;
        if (c < g_font->first || c > g_font->last) c = '?';
        const glyph_t *g = &g_font->glyphs[c - g_font->first];

        INTN gpen = pen64 + ((INTN)g->left * (INTN)dh * 64) / (INTN)size;
        INTN gx    = gpen >> 6;
        INTN frac  = gpen - (gx << 6);
        UINTN phase = (UINTN)((frac * GLYPH_PHASES) >> 6);
        if (phase >= GLYPH_PHASES) phase = GLYPH_PHASES - 1;

        INTN gyTop = baseline - scale_metric((INTN)g->top, dh, size);
        glyph_entry_t *e = glyph_get(c, dh, phase);
        if (e) blend_glyph(state, e, rgb, gx + e->ox, gyTop + e->oy, master);

        pen64 += ((INTN)g->advance_q6 * (INTN)dh) / (INTN)size;
    }
}

void draw_text_px(gui_state_t *state, CHAR16 *text, INTN x, INTN y,
                         color_t color, UINTN dh) {
    draw_text_px_a(state, text, x, y, color, dh, 255);
}

void draw_text_centered_px(gui_state_t *state, CHAR16 *text, INTN x, UINTN w,
                                  INTN y, color_t color, UINTN dh) {
    UINTN tw = text_width_px(text, dh);
    INTN tx = (tw < w) ? x + (INTN)(w - tw) / 2 : x;
    draw_text_px(state, text, tx, y, color, dh);
}

void chop_to_width(CHAR16 *s, UINTN px, UINTN maxw) {
    if (text_width_px(s, px) <= maxw) return;
    UINTN len = 0; while (s[len]) len++;
    while (len > 3 && text_width_px(s, px) > maxw) {
        s[len - 3] = '.'; s[len - 2] = '.'; s[len - 1] = 0;
        len--;
    }
}
