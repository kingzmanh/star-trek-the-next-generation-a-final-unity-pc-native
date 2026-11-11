/* Red Book CD audio for MSCDEX: the game's music tracks are played from
 * <root>\cdaudio\trackNN.wav (44.1 kHz 16-bit stereo, ripped from the CD by
 * the installer) on their own SDL audio stream.  <root>\cdaudio\toc.txt keeps
 * the disc's real table of contents ("track start_lba sectors", "leadout
 * lba"), so games that address music by absolute sector find their tracks.
 *
 * Driven from cdrom.c: PLAY AUDIO (132), STOP (133), RESUME (136) and the
 * IOCTL inputs 4/10/11/12/15 (channel info, disk info, track info, Q channel,
 * audio status); the request header's busy bit is set while a track plays. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define MAXTRK 100
#define FRAME_BYTES 2352          /* one CD sector of audio = 588 stereo frames */

static struct { u32 lba, nsec; } toc[MAXTRK];
static int first_trk = 1, last_trk = 1, toc_loaded;
static u32 leadout = 0;

static SDL_AudioDeviceID dev;
static volatile int playing, paused;
static u32 play_start, play_end;        /* absolute LBAs, end exclusive */
static volatile u32 play_pos;           /* absolute LBA being played */
static u32 pos_byte;                    /* byte offset inside the current sector */
static FILE *cur_f;
static int cur_trk = -1;
static int cd_volume = 255;             /* IOCTL output 3 (channel volume, 0..255) */
int cdaudio_master = 100;               /* settings: music volume in percent */

static void load_toc(void)
{
    if (toc_loaded) return;
    toc_loaded = 1;
    char p[700];
    snprintf(p, sizeof p, "%s\\cdaudio\\toc.txt", rt_root);
    FILE *f = fopen(p, "r");
    if (!f) { rt_log("cdaudio: no %s - CD music off\n", p); toc[1].lba = 0; toc[1].nsec = 1000; leadout = 1150; return; }
    char line[200];
    first_trk = 99; last_trk = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned t, a, n;
        if (line[0] == '#') continue;
        if (sscanf(line, "leadout %u", &a) == 1) { leadout = a; continue; }
        if (sscanf(line, "%u %u %u", &t, &a, &n) == 3 && t < MAXTRK) {
            toc[t].lba = a; toc[t].nsec = n;
            if ((int)t < first_trk) first_trk = t;
            if ((int)t > last_trk) last_trk = t;
        }
    }
    fclose(f);
    if (!leadout) leadout = toc[last_trk].lba + toc[last_trk].nsec;
    rt_log("cdaudio: tracks %d-%d, leadout %u\n", first_trk, last_trk, leadout);
}

static int track_of(u32 lba)
{
    for (int t = last_trk; t >= first_trk; t--) if (lba >= toc[t].lba) return t;
    return first_trk;
}

static void lba_to_msf(u32 lba, u8 *m, u8 *s, u8 *f)
{
    *m = (u8)(lba / (60 * 75)); *s = (u8)((lba / 75) % 60); *f = (u8)(lba % 75);
}

/* Red Book address as MSCDEX stores it: frame, second, minute, 0 (lba + 150) */
static u32 redbook(u32 lba)
{
    u8 m, s, f;
    lba_to_msf(lba + 150, &m, &s, &f);
    return (u32)f | ((u32)s << 8) | ((u32)m << 16);
}

static int open_track(int t)
{
    if (cur_trk == t && cur_f) return 1;
    if (cur_f) fclose(cur_f);
    cur_f = NULL; cur_trk = -1;
    char p[700];
    snprintf(p, sizeof p, "%s\\cdaudio\\track%02d.wav", rt_root, t);
    cur_f = fopen(p, "rb");
    if (!cur_f) return 0;
    cur_trk = t;
    return 1;
}

static void mix(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    memset(stream, 0, len);
    if (!playing || paused) return;
    int done = 0;
    int vol = cd_volume * cdaudio_master / 100;    /* 0..255 */
    if (getenv(ENV("HIDDEN"))) vol = 0;            /* background test runs stay silent */
    while (done < len && playing) {
        if (play_pos >= play_end) { playing = 0; break; }
        int t = track_of(play_pos);
        if (toc[t].nsec == 0 || t == 1 && first_trk == 1 && toc[1].lba == 0 && play_pos < toc[1].nsec) {
            /* data track: silence, skip */
            play_pos++; pos_byte = 0; continue;
        }
        if (!open_track(t)) { play_pos++; pos_byte = 0; continue; }
        u32 off = 44 + (play_pos - toc[t].lba) * FRAME_BYTES + pos_byte;
        fseek(cur_f, (long)off, SEEK_SET);
        int want = FRAME_BYTES - (int)pos_byte;
        if (want > len - done) want = len - done;
        int got = (int)fread(stream + done, 1, want, cur_f);
        if (got <= 0) { play_pos++; pos_byte = 0; continue; }
        s16 *s = (s16 *)(stream + done);
        for (int i = 0; i < got / 2; i++) s[i] = (s16)(s[i] * vol / 255);
        done += got;
        pos_byte += got;
        if (pos_byte >= FRAME_BYTES) { pos_byte = 0; play_pos++; }
    }
}

static void audio_open(void)
{
    if (dev) return;
    SDL_AudioSpec want = {0}, have;
    want.freq = 44100;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 2048;
    want.callback = mix;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { rt_log("cdaudio: cannot open audio: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(dev, 0);
}

int cdaudio_busy(void) { return playing && !paused; }

void cdaudio_play(u32 start, u32 nsec)
{
    load_toc();
    audio_open();
    if (dev) SDL_LockAudioDevice(dev);
    play_start = start;
    play_end = start + nsec;
    if (play_end > leadout) play_end = leadout;
    play_pos = start; pos_byte = 0;
    playing = 1; paused = 0;
    if (dev) SDL_UnlockAudioDevice(dev);
    rt_log("cdaudio: play lba %u..%u (track %d)\n", start, play_end, track_of(start));
}

void cdaudio_stop(void)
{
    if (dev) SDL_LockAudioDevice(dev);
    if (playing && !paused) paused = 1;          /* first STOP pauses (MSCDEX) */
    else { playing = 0; paused = 0; }            /* second STOP resets */
    if (dev) SDL_UnlockAudioDevice(dev);
}

void cdaudio_resume(void)
{
    if (dev) SDL_LockAudioDevice(dev);
    if (playing) paused = 0;
    if (dev) SDL_UnlockAudioDevice(dev);
}

void cdaudio_reset(void)
{
    if (dev) SDL_LockAudioDevice(dev);
    playing = paused = 0;
    if (dev) SDL_UnlockAudioDevice(dev);
}

void cdaudio_set_volume(int v) { cd_volume = v < 0 ? 0 : v > 255 ? 255 : v; }

/* IOCTL input codes the audio side answers; returns 0 when not ours */
int cdaudio_ioctl_in(u32 cb, u8 code)
{
    load_toc();
    switch (code) {
    case 4:     /* audio channel info */
        for (int i = 0; i < 4; i++) { WR8(cb + 1 + i * 2, (u8)i); WR8(cb + 2 + i * 2, (u8)(i < 2 ? cd_volume : 0)); }
        return 1;
    case 10:    /* audio disk info */
        WR8(cb + 1, (u8)first_trk);
        WR8(cb + 2, (u8)last_trk);
        WR32(cb + 3, redbook(leadout));
        return 1;
    case 11: {  /* audio track info */
        int t = RD8(cb + 1);
        if (t < first_trk || t > last_trk) t = first_trk;
        WR32(cb + 2, redbook(toc[t].lba));
        WR8(cb + 6, (u8)(t == 1 ? 0x41 : 0x01));   /* control/ADR: data track flag on track 1 */
        return 1;
    }
    case 12: {  /* Q channel info */
        u32 at = playing ? play_pos : 0;
        int t = track_of(at);
        u32 rel = at >= toc[t].lba ? at - toc[t].lba : 0;
        u8 m, s, f;
        WR8(cb + 1, (u8)(t == 1 ? 0x41 : 0x01));
        WR8(cb + 2, (u8)(((t / 10) << 4) | (t % 10)));
        WR8(cb + 3, 1);
        lba_to_msf(rel, &m, &s, &f);
        WR8(cb + 4, m); WR8(cb + 5, s); WR8(cb + 6, f); WR8(cb + 7, 0);
        lba_to_msf(at + 150, &m, &s, &f);
        WR8(cb + 8, m); WR8(cb + 9, s); WR8(cb + 10, f);
        return 1;
    }
    case 15:    /* audio status */
        WR16(cb + 1, (u16)(paused ? 1 : 0));
        WR32(cb + 3, redbook(play_start));
        WR32(cb + 7, redbook(play_end));
        return 1;
    }
    return 0;
}
