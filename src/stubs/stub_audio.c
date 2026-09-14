/* stub_audio.c - stub when audio is compiled out */

#include "menu_sound.h"

int menu_sound_prepare(int enabled, CHAR16 *path) {
    (void)enabled; (void)path;
    return 0;
}

void menu_sound_start(void)  { }
void menu_sound_poll(void)   { }
void menu_sound_stop(void)   { }
void menu_sound_finish(void) { }
