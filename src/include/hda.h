#ifndef HDA_H
#define HDA_H

#include <efi.h>

/* hda.h - HD Audio playback API */

#define HDA_SAMPLE_RATE   48000
#define HDA_CHANNELS      2

#define HDA_SETUP_BUDGET_MS  400
#define HDA_DRAIN_SLACK_MS   250

#define HDA_PLAY_BUDGET_MS   20000

#define HDA_CLEANUP_BUDGET_MS 200

#define HDA_OK             0
#define HDA_NO_CONTROLLER  1
#define HDA_NO_CODEC       2
#define HDA_NO_PATH        3
#define HDA_NO_MEMORY      4
#define HDA_HW_ERROR       5
#define HDA_TIMEOUT        6
#define HDA_UNSUPPORTED    7

int hda_play_pcm(const INT16 *pcm, UINTN frames);

void* hda_play_begin(const INT16 *pcm, UINTN frames, UINTN active_frames,
                     int *status);

void* hda_play_prepare(const INT16 *pcm, UINTN frames, UINTN active_frames,
                       int *status);
int   hda_play_start(void *handle);
int   hda_play_done(void *handle);
int   hda_play_cut(void *handle);
int   hda_play_end(void *handle);

int hda_probe(void);

const CHAR16* hda_status_str(int code);

#endif
