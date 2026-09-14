/* stub_anim_mp4.c - stub when MP4 animation is compiled out */

#include "anim_decode.h"

int mjpeg_advance_n(anim_t *a, UINTN n) {
    (void)a; (void)n;
    return 0;
}

void mjpeg_state_free(mjpeg_t *st) {
    (void)st;
}

anim_t * mp4_load(UINT8 *data, UINTN size, UINTN tgt_w, UINTN tgt_h) {
    (void)data; (void)size; (void)tgt_w; (void)tgt_h;
    return 0;
}

int vbg_advance(anim_t *a) {
    (void)a;
    return 0;
}

void vbg_free(anim_t *a) {
    (void)a;
}

anim_t * vbg_load(UINT8 *data, UINTN size) {
    (void)data; (void)size;
    return 0;
}
