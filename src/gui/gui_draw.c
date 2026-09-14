/* gui_draw.c - rectangle and image drawing primitives */
#include "gui_internal.h"

void gui_fill_rect(gui_state_t *state, UINTN x, UINTN y, UINTN w, UINTN h, color_t color) {
    UINT32 pixel = color_to_u32(color);
    for (UINTN j = y; j < y + h && j < state->screen_height; j++) {
        for (UINTN i = x; i < x + w && i < state->screen_width; i++) {
            UINT32 *p = get_pixel(state, i, j);
            if (p) *p = pixel;
        }
    }
}

void fill_rect_alpha(gui_state_t *state, INTN x, INTN y, INTN w, INTN h,
                            color_t color, UINT8 alpha) {
    for (INTN j = y; j < y + h; j++) {
        if (j < 0 || j >= (INTN)state->screen_height) continue;
        for (INTN i = x; i < x + w; i++) {
            if (i < 0 || i >= (INTN)state->screen_width) continue;
            UINT32 *p = get_pixel(state, i, j);
            if (!p) continue;
            UINT8 br = (*p >> 16) & 0xFF, bg = (*p >> 8) & 0xFF, bb = *p & 0xFF;
            UINT8 r = (color.r * alpha + br * (255 - alpha)) / 255;
            UINT8 g = (color.g * alpha + bg * (255 - alpha)) / 255;
            UINT8 b = (color.b * alpha + bb * (255 - alpha)) / 255;
            *p = (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

void fill_round_rect(gui_state_t *state, INTN x, INTN y, INTN w, INTN h,
                            INTN r, color_t color, UINT8 alpha) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (INTN j = 0; j < h; j++) {
        INTN inset = 0;
        if (j < r)              { INTN dy = r - 1 - j; inset = r - (INTN)isqrt_(r*r - dy*dy); }
        else if (j >= h - r)    { INTN dy = j - (h - r); inset = r - (INTN)isqrt_(r*r - dy*dy); }
        fill_rect_alpha(state, x + inset, y + j, w - 2 * inset, 1, color, alpha);
    }
}

static UINT32* icon_build_scaled(icon_t *icon, UINTN size) {
    if (size == 0 || size > 4096) return NULL;
    if (icon->scaled && icon->scaled_size == size) return icon->scaled;
    if (icon->scaled) { efi_free_pool(icon->scaled); icon->scaled = NULL; icon->scaled_size = 0; }

    UINTN px, bytes;
    if (mul_overflow_uintn(size, size, &px) ||
        mul_overflow_uintn(px, sizeof(UINT32), &bytes))
        return NULL;
    UINT32 *out = efi_allocate_pool(bytes);
    if (!out) return NULL;

    UINTN iw = icon->width, ih = icon->height;
    for (UINTN j = 0; j < size; j++) {
        UINTN sy0 = j * ih / size;
        UINTN sy1 = (j + 1) * ih / size;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > ih) sy1 = ih;
        for (UINTN i = 0; i < size; i++) {
            UINTN sx0 = i * iw / size;
            UINTN sx1 = (i + 1) * iw / size;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > iw) sx1 = iw;

            UINT64 ar = 0, ag = 0, ab = 0, aa = 0; UINTN n = 0;
            for (UINTN sy = sy0; sy < sy1; sy++) {
                const UINT32 *row = icon->pixels + sy * iw;
                for (UINTN sx = sx0; sx < sx1; sx++) {
                    UINT32 p = row[sx];
                    UINT32 a = (p >> 24) & 0xFF;
                    ar += ((p >> 16) & 0xFF) * a;
                    ag += ((p >> 8) & 0xFF) * a;
                    ab += (p & 0xFF) * a;
                    aa += a;
                    n++;
                }
            }
            UINT8 cov = (n == 0) ? 0 : (UINT8)(aa / n);
            UINT8 sr, sg, sb;
            if (aa == 0) { sr = sg = sb = 0; }
            else { sr = (UINT8)(ar / aa); sg = (UINT8)(ag / aa); sb = (UINT8)(ab / aa); }
            out[j * size + i] = ((UINT32)cov << 24) | ((UINT32)sr << 16)
                              | ((UINT32)sg << 8) | sb;
        }
    }
    icon->scaled = out;
    icon->scaled_size = size;
    return out;
}

void draw_image_sized_a(gui_state_t *state, icon_t *icon,
                               UINTN x, UINTN y, UINTN size, INTN master) {
    if (!icon || !icon->pixels || icon->width == 0 || icon->height == 0 || size == 0)
        return;
    if (master <= 0) return;
    if (master > 255) master = 255;

    UINT32 *sc = icon_build_scaled(icon, size);
    if (!sc) return;

    for (UINTN j = 0; j < size && (y + j) < state->screen_height; j++) {
        for (UINTN i = 0; i < size && (x + i) < state->screen_width; i++) {
            UINT32 p = sc[j * size + i];
            UINTN cov = (p >> 24) & 0xFF;
            cov = cov * (UINTN)master / 255;
            if (cov == 0) continue;
            UINT8 sr = (p >> 16) & 0xFF, sg = (p >> 8) & 0xFF, sb = p & 0xFF;

            UINT32 *dest = get_pixel(state, x + i, y + j);
            if (!dest) continue;
            UINT8 br = (*dest >> 16) & 0xFF, bg = (*dest >> 8) & 0xFF, bb = *dest & 0xFF;
            UINT8 nr = (UINT8)((sr * cov + br * (255 - cov)) / 255);
            UINT8 ng = (UINT8)((sg * cov + bg * (255 - cov)) / 255);
            UINT8 nb = (UINT8)((sb * cov + bb * (255 - cov)) / 255);
            *dest = (0xFFu << 24) | (nr << 16) | (ng << 8) | nb;
        }
    }
}

void draw_image_sized(gui_state_t *state, icon_t *icon,
                             UINTN x, UINTN y, UINTN size) {
    draw_image_sized_a(state, icon, x, y, size, 255);
}

void draw_image_tinted_a(gui_state_t *state, icon_t *icon,
                                UINTN x, UINTN y, UINTN size,
                                color_t tint, INTN master) {
    if (!icon || !icon->pixels || icon->width == 0 || icon->height == 0 || size == 0)
        return;
    if (master <= 0) return;
    if (master > 255) master = 255;

    UINT32 *sc = icon_build_scaled(icon, size);
    if (!sc) return;

    for (UINTN j = 0; j < size && (y + j) < state->screen_height; j++) {
        for (UINTN i = 0; i < size && (x + i) < state->screen_width; i++) {
            UINT32 p = sc[j * size + i];
            UINTN cov = (p >> 24) & 0xFF;
            cov = cov * (UINTN)master / 255;
            if (cov == 0) continue;

            UINT32 *dest = get_pixel(state, x + i, y + j);
            if (!dest) continue;
            UINT8 br = (*dest >> 16) & 0xFF, bg = (*dest >> 8) & 0xFF, bb = *dest & 0xFF;
            UINT8 nr = (UINT8)((tint.r * cov + br * (255 - cov)) / 255);
            UINT8 ng = (UINT8)((tint.g * cov + bg * (255 - cov)) / 255);
            UINT8 nb = (UINT8)((tint.b * cov + bb * (255 - cov)) / 255);
            *dest = (0xFFu << 24) | (nr << 16) | (ng << 8) | nb;
        }
    }
}
