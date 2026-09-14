/* stub_rbd.c - stub when the easter-egg feature is compiled out */

#include "rbd.h"

void rbd_arm_on_reboot(int enabled) {
    (void)enabled;
}

int rbd_check_and_play(gui_state_t *gui) {
    (void)gui;
    return 0;
}
