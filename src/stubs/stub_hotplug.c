/* stub_hotplug.c - stub when hotplug rescanning is compiled out */

#include "config.h"

void config_hotplug_arm(config_t *config) {
    (void)config;
}

int config_hotplug_poll(config_t *config, UINTN *first_new) {
    (void)config; (void)first_new;
    return 0;
}
