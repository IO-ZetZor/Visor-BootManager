/* stub_anim_gif.c - stub when GIF animation is compiled out */

#include "anim_decode.h"

int gif_advance(anim_t *a) {
    (void)a;
    return 0;
}

void gif_free(anim_t *a) {
    (void)a;
}

anim_t * gif_load(UINT8 *data, UINTN size) {
    (void)data; (void)size;
    return 0;
}
