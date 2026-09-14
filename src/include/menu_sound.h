/* menu_sound.h - boot sound playback and shared PCM helpers (feature: audio) */
#ifndef MENU_SOUND_H
#define MENU_SOUND_H

#include <efi.h>

int  menu_sound_prepare(int enabled, CHAR16 *path);
void menu_sound_start(void);
void menu_sound_poll(void);
void menu_sound_stop(void);
void menu_sound_finish(void);

void pcm_resample_stereo(const INT16 *src, UINTN in_frames, UINT32 in_rate,
                         INT16 *out, UINTN out_frames, UINT32 out_rate);

INT16 *pcm_load_wav(CHAR16 *path, UINTN *frames_out);

INT16 *pcm_pad_silence(INT16 *pcm, UINTN *frames, UINTN pad_frames);

#endif
