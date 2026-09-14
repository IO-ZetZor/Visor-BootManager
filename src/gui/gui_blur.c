/* gui_blur.c - box blur cache and frosted panels (feature: blur) */
#include "gui_internal.h"

#define FROST_RADIUS 16

#define BLUR_RADIUS 16

static UINTN blur_pick_shift(UINTN w) {
    UINTN s = 2;
    while ((w >> s) > 640 && s < 5) s++;
    return s;
}

void blur_free(gui_state_t *state) {
    if (state->blur_cache) { efi_free_pool(state->blur_cache); state->blur_cache = NULL; }
    if (state->blur_line)  { efi_free_pool(state->blur_line);  state->blur_line  = NULL; }
    state->blur_valid = 0;
    state->blur_w = state->blur_h = 0;
}

static void box_blur_pass(UINT32 *src, UINT32 *dst, UINTN W, UINTN H, INTN rad) {
    INTN win = 2 * rad + 1;
    UINT32 recip = (UINT32)(((1u << 16) + (UINT32)win - 1) / (UINT32)win);
    for (UINTN y = 0; y < H; y++) {
        UINT32 *s = src + y * W;
        UINT32 *d = dst + y * W;
        UINT32 sr = 0, sg = 0, sb = 0;
        for (INTN i = -rad; i <= rad; i++) {
            UINTN xi = (i < 0) ? 0 : ((UINTN)i >= W ? W - 1 : (UINTN)i);
            UINT32 p = s[xi];
            sr += (p >> 16) & 0xFF; sg += (p >> 8) & 0xFF; sb += p & 0xFF;
        }
        for (UINTN x = 0; x < W; x++) {
            d[x] = (0xFFu << 24) | ((((sr * recip) >> 16) & 0xFF) << 16)
                 | ((((sg * recip) >> 16) & 0xFF) << 8) | (((sb * recip) >> 16) & 0xFF);
            UINTN xa = x + (UINTN)rad + 1; if (xa >= W) xa = W - 1;
            INTN xrm = (INTN)x - rad; UINTN xr = (xrm < 0) ? 0 : (UINTN)xrm;
            UINT32 pa = s[xa], pr = s[xr];
            sr += ((pa >> 16) & 0xFF) - ((pr >> 16) & 0xFF);
            sg += ((pa >> 8) & 0xFF) - ((pr >> 8) & 0xFF);
            sb += (pa & 0xFF) - (pr & 0xFF);
        }
    }
}

static void box_blur_vpass(UINT32 *src, UINT32 *dst, UINTN W, UINTN H, INTN rad) {
    INTN win = 2 * rad + 1;
    UINT32 recip = (UINT32)(((1u << 16) + (UINT32)win - 1) / (UINT32)win);
    UINTN strip = 64;
    UINT32 sr[64], sg[64], sb[64];

    for (UINTN x0 = 0; x0 < W; x0 += strip) {
        UINTN nx = W - x0 < strip ? W - x0 : strip;
        for (UINTN i = 0; i < nx; i++) { sr[i] = 0; sg[i] = 0; sb[i] = 0; }
        for (INTN i = -rad; i <= rad; i++) {
            UINTN yi = (i < 0) ? 0 : ((UINTN)i >= H ? H - 1 : (UINTN)i);
            const UINT32 *s = src + yi * W + x0;
            for (UINTN k = 0; k < nx; k++) {
                UINT32 p = s[k];
                sr[k] += (p >> 16) & 0xFF; sg[k] += (p >> 8) & 0xFF; sb[k] += p & 0xFF;
            }
        }
        for (UINTN y = 0; y < H; y++) {
            UINT32 *d = dst + y * W + x0;
            for (UINTN k = 0; k < nx; k++)
                d[k] = (0xFFu << 24) | ((((sr[k] * recip) >> 16) & 0xFF) << 16)
                     | ((((sg[k] * recip) >> 16) & 0xFF) << 8)
                     | (((sb[k] * recip) >> 16) & 0xFF);
            UINTN ya = y + (UINTN)rad + 1; if (ya >= H) ya = H - 1;
            INTN yrm = (INTN)y - rad; UINTN yr = (yrm < 0) ? 0 : (UINTN)yrm;
            const UINT32 *pa = src + ya * W + x0;
            const UINT32 *pr = src + yr * W + x0;
            for (UINTN k = 0; k < nx; k++) {
                UINT32 A = pa[k], R = pr[k];
                sr[k] += ((A >> 16) & 0xFF) - ((R >> 16) & 0xFF);
                sg[k] += ((A >> 8) & 0xFF) - ((R >> 8) & 0xFF);
                sb[k] += (A & 0xFF) - (R & 0xFF);
            }
        }
    }
}

static void blur_downsample(gui_state_t *state, UINT32 *dst) {
    UINTN W = state->screen_width, H = state->screen_height;
    UINTN S = state->blur_shift, SC = 1u << S;
    UINTN SW = state->blur_w, SH = state->blur_h;
    const UINT32 *src = state->backbuffer;

    for (UINTN sy = 0; sy < SH; sy++) {
        UINTN y0 = sy << S;
        UINTN y1 = y0 + SC; if (y1 > H) y1 = H;
        UINTN rows = y1 - y0;
        UINT32 *d = dst + sy * SW;

        for (UINTN sx = 0; sx < SW; sx++) {
            UINTN x0 = sx << S;
            UINTN x1 = x0 + SC; if (x1 > W) x1 = W;
            UINTN cols = x1 - x0;
            UINT32 ar = 0, ag = 0, ab = 0;
            for (UINTN y = y0; y < y1; y++) {
                const UINT32 *s = src + y * W + x0;
                for (UINTN k = 0; k < cols; k++) {
                    UINT32 p = s[k];
                    ar += (p >> 16) & 0xFF; ag += (p >> 8) & 0xFF; ab += p & 0xFF;
                }
            }
            UINTN n = rows * cols; if (!n) n = 1;
            d[sx] = (0xFFu << 24) | (((ar / n) & 0xFF) << 16)
                  | (((ag / n) & 0xFF) << 8) | ((ab / n) & 0xFF);
        }
    }
}

void build_blur_cache(gui_state_t *state) {
    UINTN S = blur_pick_shift(state->screen_width);
    UINTN SW = (state->screen_width  + (1u << S) - 1) >> S;
    UINTN SH = (state->screen_height + (1u << S) - 1) >> S;
    if (SW < 4) SW = 4;
    if (SH < 4) SH = 4;

    if (state->blur_cache && (state->blur_w != SW || state->blur_h != SH))
        blur_free(state);

    if (!state->blur_cache) {
        state->blur_cache = efi_allocate_pool(SW * SH * sizeof(UINT32));
        if (!state->blur_cache) return;
        state->blur_w = SW; state->blur_h = SH; state->blur_shift = S;
        state->blur_valid = 0;
    }
    if (!state->blur_line) {
        state->blur_line = efi_allocate_pool(state->screen_width * sizeof(UINT32));
        if (!state->blur_line) { blur_free(state); return; }
    }
    if (state->blur_valid && state->blur_gen == state->bg_gen) return;
    if (!state->scene_cache || !state->backbuffer) return;

    UINT32 *tmp = state->scene_cache;
    INTN rad = (INTN)((BLUR_RADIUS + (1u << S) / 2) >> S);
    if (rad < 1) rad = 1;

    blur_downsample(state, state->blur_cache);
    box_blur_pass (state->blur_cache, tmp, SW, SH, rad);
    box_blur_vpass(tmp, state->blur_cache, SW, SH, rad);
    box_blur_pass (state->blur_cache, tmp, SW, SH, rad);
    box_blur_vpass(tmp, state->blur_cache, SW, SH, rad);

    state->blur_gen = state->bg_gen;
    state->blur_valid = 1;
}

static void blur_fill_line(gui_state_t *state, INTN y, INTN x0, INTN n) {
    UINTN S = state->blur_shift, SC = 1u << S, mask = SC - 1;
    INTN SW = (INTN)state->blur_w, SH = (INTN)state->blur_h;
    UINT32 *out = state->blur_line;

    INTN sy = y >> (INTN)S, fy = y & (INTN)mask;
    if (sy < 0) { sy = 0; fy = 0; }
    if (sy > SH - 1) { sy = SH - 1; fy = 0; }
    const UINT32 *r0 = state->blur_cache + (UINTN)sy * (UINTN)SW;
    const UINT32 *r1 = (sy + 1 < SH) ? r0 + SW : r0;

    INTN i = 0;
    while (i < n) {
        INTN x = x0 + i;
        INTN sx = x >> (INTN)S, fx = x & (INTN)mask;
        if (sx < 0) { sx = 0; fx = 0; }
        if (sx > SW - 1) { sx = SW - 1; fx = 0; }
        INTN sx1 = (sx + 1 < SW) ? sx + 1 : sx;

        UINT32 pa = r0[sx],  pb = r1[sx];
        UINT32 pc = r0[sx1], pd = r1[sx1];
        INTN lr = (((pa >> 16) & 0xFF) * ((INTN)SC - fy) + ((pb >> 16) & 0xFF) * fy);
        INTN lg = (((pa >>  8) & 0xFF) * ((INTN)SC - fy) + ((pb >>  8) & 0xFF) * fy);
        INTN lb = (((pa      ) & 0xFF) * ((INTN)SC - fy) + ((pb      ) & 0xFF) * fy);
        INTN rr = (((pc >> 16) & 0xFF) * ((INTN)SC - fy) + ((pd >> 16) & 0xFF) * fy);
        INTN rg = (((pc >>  8) & 0xFF) * ((INTN)SC - fy) + ((pd >>  8) & 0xFF) * fy);
        INTN rb = (((pc      ) & 0xFF) * ((INTN)SC - fy) + ((pd      ) & 0xFF) * fy);

        INTN cnt = (INTN)SC - fx;
        if (cnt > n - i) cnt = n - i;
        INTN ar = lr * (INTN)SC + (rr - lr) * fx;
        INTN ag = lg * (INTN)SC + (rg - lg) * fx;
        INTN ab = lb * (INTN)SC + (rb - lb) * fx;
        INTN dr = rr - lr, dg = rg - lg, db = rb - lb;
        INTN sh = (INTN)(2 * S);
        for (INTN k = 0; k < cnt; k++) {
            out[i + k] = (0xFFu << 24)
                       | ((UINT32)((ar >> sh) & 0xFF) << 16)
                       | ((UINT32)((ag >> sh) & 0xFF) << 8)
                       |  (UINT32)((ab >> sh) & 0xFF);
            ar += dr; ag += dg; ab += db;
        }
        i += cnt;
    }
}

void draw_frost(gui_state_t *state, INTN x, INTN y, INTN w, INTN h, INTN a) {
    if (a <= 0 || w <= 0 || h <= 0) return;
    if (a > 255) a = 255;
    int clear = (state->blur == 2);
    color_t tint = state->blur_color;
    INTN r = state->box_radius ? (INTN)state->box_radius : FROST_RADIUS;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    int have_blur = (state->blur_cache && state->blur_line && state->blur_valid &&
                     w <= (INTN)state->screen_width);
    UINT32 flat = color_to_u32(state->bg_color);
    INTN base_fill = clear ? (a * 255 / 255) : (a * 220 / 255);
    INTN tint_a = clear ? 0 : 34;
    INTN lift   = clear ? 0 : 8;
    INTN feather = 10;
    for (INTN j = 0; j < h; j++) {
        INTN inset = 0;
        if (j < r)            { INTN dy = r - 1 - j; INTN q = r*r - dy*dy; inset = r - (INTN)isqrt_(q > 0 ? q : 0); }
        else if (j >= h - r)  { INTN dy = j - (h - r); INTN q = r*r - dy*dy; inset = r - (INTN)isqrt_(q > 0 ? q : 0); }
        INTN yy = y + j;
        if (yy < 0 || yy >= (INTN)state->screen_height) continue;
        if (have_blur) blur_fill_line(state, yy, x, w);
        INTN edy = (j < h - 1 - j) ? j : (h - 1 - j);
        for (INTN i = inset; i < w - inset; i++) {
            INTN xx = x + i;
            UINT32 *p = get_pixel(state, xx, yy);
            if (!p) continue;
            INTN edx = (i - inset < (w - inset - 1) - i) ? (i - inset) : ((w - inset - 1) - i);
            INTN ed = (edx < edy) ? edx : edy;
            INTN fill_a = base_fill;
            if (ed < feather) fill_a = base_fill * ed / feather;
            if (fill_a <= 0) continue;
            UINT32 src = have_blur ? state->blur_line[i] : flat;
            INTN sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
            sr += lift; sg += lift; sb += lift;
            sr = (sr * (255 - tint_a) + tint.r * tint_a) / 255;
            sg = (sg * (255 - tint_a) + tint.g * tint_a) / 255;
            sb = (sb * (255 - tint_a) + tint.b * tint_a) / 255;
            if (sr > 255) sr = 255;
            if (sg > 255) sg = 255;
            if (sb > 255) sb = 255;
            UINT8 br = (*p >> 16) & 0xFF, bgc = (*p >> 8) & 0xFF, bb = *p & 0xFF;
            UINT8 nr = (UINT8)((sr * fill_a + br * (255 - fill_a)) / 255);
            UINT8 ng = (UINT8)((sg * fill_a + bgc * (255 - fill_a)) / 255);
            UINT8 nb = (UINT8)((sb * fill_a + bb * (255 - fill_a)) / 255);
            *p = (0xFFu << 24) | (nr << 16) | (ng << 8) | nb;
        }
    }
}
