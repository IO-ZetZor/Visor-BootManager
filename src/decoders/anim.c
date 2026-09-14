/* anim.c - codec-agnostic animation advance/free dispatch */
#include "anim_decode.h"

int anim_advance(anim_t *a) {
    if (!a) return 0;
    if (a->codec == 1) return mjpeg_advance_n(a, 1);
    if (a->codec == 2) return vbg_advance(a);
    return gif_advance(a);
}

int anim_advance_n(anim_t *a, UINTN n) {
    if (!a || n == 0) return 0;
    if (a->codec == 1) return mjpeg_advance_n(a, n);
    if (a->codec == 2) {
        UINTN done = 0;
        for (UINTN i = 0; i < n; i++) {
            if (!vbg_advance(a)) break;
            done++;
        }
        return done > 0;
    }
    UINTN done = 0;
    for (UINTN i = 0; i < n; i++) {
        if (!gif_advance(a)) break;
        done++;
    }
    return done > 0;
}

void anim_free(anim_t *a) {
    if (!a) return;
    if (a->codec == 1) {
        mjpeg_state_free(a->mj);
        a->mj = NULL;
    } else if (a->codec == 2) {
        vbg_free(a);
    }
    gif_free(a);
}
