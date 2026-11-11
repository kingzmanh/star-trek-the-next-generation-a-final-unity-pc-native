/* The intro / closing videos (Duck TrueMotion AVIs) play through the Duck
 * player's own recompiled decoder (DUKDOSNA.EXE - the variant without sound
 * card drivers).  The PC side here: frames are paced at the file's rate
 * (the DOS player just ran as fast as a 486 could decode), the AVI's PCM
 * soundtrack is played on SDL audio, and Esc / Enter / Space skip. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))

static u8 *pcm;
static u32 pcm_len, pcm_rate = 22050, pcm_pos;   /* pos in 16.16 frames would be overkill: rate is fixed */
static SDL_AudioDeviceID adev;
static u64 t0, frame_us = 66667;
static u32 frame_n;
static int skipping;
extern int sound_master;

static void amix(void *ud, Uint8 *s, int len)
{
    (void)ud;
    s16 *o = (s16 *)s;
    int n = len / 2;
    int vol = getenv(ENV("HIDDEN")) ? 0 : sound_master;
    for (int i = 0; i < n; i++) {
        int v = 0;
        if (pcm && pcm_pos < pcm_len) v = (pcm[pcm_pos++] - 128) << 8;
        o[i] = (s16)(v * vol / 100);
    }
}

static u32 rd32le(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* walk the RIFF tree: frame rate from avih, audio from the '01wb' chunks */
static void load_avi(const char *host)
{
    FILE *f = fopen(host, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n);
    if (!d || fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return; }
    fclose(f);
    pcm = malloc(n / 8 + 65536);
    pcm_len = 0;
    u32 cap = (u32)(n / 8 + 65536);
    long p = 12;
    long stack_end[8]; int sp = 0;
    long end = n;
    while (p + 8 <= end) {
        u32 id = rd32le(d + p), len = rd32le(d + p + 4);
        if (id == 0x5453494c /* LIST */) {
            if (sp < 8) stack_end[sp++] = end;
            end = p + 8 + len;
            p += 12;
            continue;
        }
        if (id == 0x68697661 /* avih */ && len >= 4) {
            u32 us = rd32le(d + p + 8);
            if (us >= 10000 && us <= 200000) frame_us = us;
        } else if (id == 0x66727473 /* strf */ && len >= 16 && rd32le(d + p + 8) == 0x00010001) {
            pcm_rate = rd32le(d + p + 12);
        } else if ((id & 0xffff0000u) == 0x62770000u /* ??wb */) {
            u32 c = len;
            if (pcm_len + c > cap) c = cap - pcm_len;
            memcpy(pcm + pcm_len, d + p + 8, c);
            pcm_len += c;
        }
        p += 8 + ((len + 1) & ~1u);
        while (p >= end && sp > 0) { p = end; end = stack_end[--sp]; }
    }
    free(d);
    rt_log("movie: %s: %u us/frame, audio %u bytes at %u Hz\n", host, (u32)frame_us, pcm_len, pcm_rate);
}

void movie_start(const char *avi_guest)
{
    extern int dos_resolve(const char *guest, char *host);
    char host[700];
    frame_n = 0; t0 = 0; skipping = 0; pcm_pos = 0;
    free(pcm); pcm = NULL; pcm_len = 0;
    if (dos_resolve(avi_guest, host)) load_avi(host);
    if (!adev) {
        SDL_AudioSpec want = {0}, have;
        want.freq = (int)pcm_rate; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 1024; want.callback = amix;
        adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    }
}

void movie_stop(void)
{
    if (adev) { SDL_CloseAudioDevice(adev); adev = 0; }
    free(pcm); pcm = NULL; pcm_len = 0;
}

int movie_skip_requested(void) { return skipping; }

/* after each decoded frame: show it, keep the pace, start the sound with the
   first frame */
static void frame_done(void)
{
    extern void video_present(int);
    extern int bios_key_avail(void);
    extern int bios_key_get(u16 *, int);
    video_present(1);
    u64 now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    if (!t0) { t0 = now; if (adev) SDL_PauseAudioDevice(adev, 0); }
    frame_n++;
    u64 due = t0 + (u64)((double)frame_n * frame_us * f / 1e6);
    while ((now = SDL_GetPerformanceCounter()) < due) {
        u64 ms = (due - now) * 1000 / f;
        if (ms > 1) SDL_Delay((Uint32)(ms - 1));
        irq_poll();
    }
    while (bios_key_avail()) {
        u16 k;
        bios_key_get(&k, 0);
        u8 sc = k >> 8;
        if (sc == 0x01 || sc == 0x1c || sc == 0x39) skipping = 1;
    }
}

/* 4121d0 / 412550: the player's frame output (decode + 2x blit to VESA) */
void N_movie_frame1(void) { if (skipping) { RET(0); return; } rt_wrap_orig("N_movie_frame1", NULL)(); frame_done(); }
void N_movie_frame2(void) { if (skipping) { RET(0); return; } rt_wrap_orig("N_movie_frame2", NULL)(); frame_done(); }
