/* stub_anim.c - stub when animation is compiled out */

#include "gui.h"
#include "anim_decode.h"

int anim_advance_n(anim_t *a, UINTN n) {
    (void)a; (void)n;
    return 0;
}

void anim_free(anim_t *a) {
    (void)a;
}
