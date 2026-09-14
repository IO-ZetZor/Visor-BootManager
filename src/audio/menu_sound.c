/* menu_sound.c - boot sound playback and the PCM helpers (feature: audio) */
#include "menu_sound.h"
#include <efilib.h>
#include "efi_helpers.h"
#include "hda.h"

#define MENU_WAV_MAX       (4u * 1024u * 1024u)
#define MENU_PAD_MARGIN_MS 2000

#define MENU_SOUND_MAX_SEC 15

static void  *g_menu_audio;
static INT16 *g_menu_pcm;
static int    g_menu_started;

static INT16 clamp16(INT32 v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (INT16)v;
}

void pcm_resample_stereo(const INT16 *src, UINTN in_frames,
                         UINT32 in_rate, INT16 *out, UINTN out_frames,
                         UINT32 out_rate) {
    UINT64 step = ((UINT64)in_rate << 16) / out_rate;
    UINT64 cur = 0;
    for (UINTN i = 0; i < out_frames; i++) {
        UINTN idx = (UINTN)(cur >> 16);
        UINT32 frac = (UINT32)(cur & 0xFFFF);
        UINTN nxt = idx + 1 < in_frames ? idx + 1 : idx;
        for (UINTN c = 0; c < 2; c++) {
            INT32 a = src[idx * 2 + c];
            INT32 b = src[nxt * 2 + c];
            out[i * 2 + c] = clamp16(a + (INT32)(((INT64)(b - a) * frac) >> 16));
        }
        cur += step;
    }
}

static UINT32 rd32(const UINT8 *p) {
    return (UINT32)p[0] | ((UINT32)p[1] << 8) | ((UINT32)p[2] << 16) |
           ((UINT32)p[3] << 24);
}

static UINT16 rd16(const UINT8 *p) {
    return (UINT16)((UINT32)p[0] | ((UINT32)p[1] << 8));
}

static INT16 *pcm_parse_wav(const UINT8 *d, UINTN size, UINTN *frames_out) {
    *frames_out = 0;

    if (!d || size < 44 || size > MENU_WAV_MAX) return NULL;
    if (d[0] != 'R' || d[1] != 'I' || d[2] != 'F' || d[3] != 'F') return NULL;
    if (d[8] != 'W' || d[9] != 'A' || d[10] != 'V' || d[11] != 'E') return NULL;

    UINT16 fmt = 0, channels = 0, bits = 0;
    UINT32 rate = 0;
    const UINT8 *data = NULL;
    UINTN data_size = 0;

    for (UINTN off = 12; off + 8 <= size; ) {
        const UINT8 *id = d + off;
        UINT32 clen = rd32(d + off + 4);
        UINTN body = off + 8;
        if (clen > size - body) clen = (UINT32)(size - body);

        if (id[0] == 'f' && id[1] == 'm' && id[2] == 't' && id[3] == ' ') {
            if (clen >= 16) {
                fmt      = rd16(d + body + 0);
                channels = rd16(d + body + 2);
                rate     = rd32(d + body + 4);
                bits     = rd16(d + body + 14);
            }
        } else if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a') {
            data = d + body;
            data_size = clen;
        }

        off = body + clen + (clen & 1);
    }

    if (fmt != 1 && fmt != 0xFFFE) {
        efi_log(L"sound: not uncompressed PCM");
        return NULL;
    }
    if (bits != 16 || (channels != 1 && channels != 2) || !rate || !data ||
        !data_size) {
        efi_log(L"sound: not 16-bit PCM, mono or stereo");
        return NULL;
    }

    UINTN in_frames = data_size / (channels * 2u);
    if (!in_frames) return NULL;

    UINTN max_frames = (UINTN)HDA_SAMPLE_RATE * MENU_SOUND_MAX_SEC;
    UINTN out_frames = (UINTN)(((UINT64)in_frames * HDA_SAMPLE_RATE) / rate);
    if (!out_frames) return NULL;
    if (out_frames > max_frames) {
        CHAR16 msg[160];
        SPrint(msg, sizeof(msg),
               L"WARN: sound: %u s long - playing only the first %u s",
               (unsigned int)(out_frames / HDA_SAMPLE_RATE),
               (unsigned int)MENU_SOUND_MAX_SEC);
        efi_log(msg);
        out_frames = max_frames;
    }

    INT16 *out = efi_allocate_pool(out_frames * 2 * sizeof(INT16));
    if (!out) return NULL;

    const INT16 *src = (const INT16*)data;
    INT16 *mono = NULL;
    if (channels == 1) {
        mono = efi_allocate_pool(in_frames * 2 * sizeof(INT16));
        if (!mono) {
            efi_free_pool(out);
            return NULL;
        }
        for (UINTN i = 0; i < in_frames; i++) mono[i * 2] = mono[i * 2 + 1] = src[i];
        src = mono;
    }

    pcm_resample_stereo(src, in_frames, rate, out, out_frames, HDA_SAMPLE_RATE);
    if (mono) efi_free_pool(mono);

    *frames_out = out_frames;
    return out;
}

INT16 *pcm_load_wav(CHAR16 *path, UINTN *frames_out) {
    efi_file_buffer_t *fb = efi_load_file(path);
    if (!fb) return NULL;

    INT16 *out = pcm_parse_wav((const UINT8*)fb->data, fb->size, frames_out);
    if (fb->data) efi_free_pool(fb->data);
    efi_free_pool(fb);
    return out;
}

INT16 *pcm_pad_silence(INT16 *pcm, UINTN *frames, UINTN pad_frames) {
    if (*frames >= pad_frames) return pcm;

    INT16 *pad = efi_allocate_pool(pad_frames * 2 * sizeof(INT16));
    if (!pad) return pcm;

    UINTN i;
    for (i = 0; i < *frames; i++) {
        pad[i * 2]     = pcm[i * 2];
        pad[i * 2 + 1] = pcm[i * 2 + 1];
    }
    for (i = *frames * 2; i < pad_frames * 2; i++) pad[i] = 0;
    efi_free_pool(pcm);
    *frames = pad_frames;
    return pad;
}

int menu_sound_prepare(int enabled, CHAR16 *path) {
    if (!enabled || !path) return 0;
    if (g_menu_audio) return 1;

    UINTN frames = 0;
    INT16 *pcm = pcm_load_wav(path, &frames);
    if (!pcm) {
        efi_log(L"sound: boot sound could not be loaded - silent");
        return 0;
    }

    UINTN active = frames;
    pcm = pcm_pad_silence(pcm, &frames,
                          frames + (UINTN)MENU_PAD_MARGIN_MS *
                                   (UINTN)HDA_SAMPLE_RATE / 1000);
    if (!pcm || !frames) {
        if (pcm) efi_free_pool(pcm);
        return 0;
    }

    int st = HDA_HW_ERROR;
    void *audio = hda_play_prepare(pcm, frames, active, &st);
    if (!audio) {
        CHAR16 msg[128];
        SPrint(msg, sizeof(msg), L"sound: no playback - %s", hda_status_str(st));
        efi_log(msg);
        efi_free_pool(pcm);
        return 0;
    }

    g_menu_pcm = pcm;
    g_menu_audio = audio;
    g_menu_started = 0;
    return 1;
}

void menu_sound_start(void) {
    if (!g_menu_audio || g_menu_started) return;
    g_menu_started = 1;
    hda_play_start(g_menu_audio);
}

void menu_sound_poll(void) {
    if (!g_menu_audio || !g_menu_started) return;
    if (hda_play_done(g_menu_audio)) menu_sound_stop();
}

static void menu_sound_release(int wait) {
    if (!g_menu_audio) return;

    void *audio = g_menu_audio;
    g_menu_audio = NULL;
    g_menu_started = 0;
    if (wait) hda_play_end(audio);
    else      hda_play_cut(audio);

    if (g_menu_pcm) {
        efi_free_pool(g_menu_pcm);
        g_menu_pcm = NULL;
    }
}

void menu_sound_stop(void) {
    menu_sound_release(0);
}

void menu_sound_finish(void) {
    menu_sound_start();
    menu_sound_release(1);
}
