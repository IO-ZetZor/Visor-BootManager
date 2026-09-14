#ifndef RBD_H
#define RBD_H

#include <efi.h>
#include "gui.h"

void rbd_arm_on_reboot(int enabled);

int rbd_check_and_play(gui_state_t *gui);

#endif