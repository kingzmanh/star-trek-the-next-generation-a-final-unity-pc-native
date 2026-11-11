/* Quarantine's sound, native.
 *
 * The game drives HMI SOS 4 (loadable HMIDRV.386 drivers) through a small
 * module of its own (44a41c..44b304): one-shot effects, the cab's engine
 * loop with speed-dependent pitch, two more looping slots and a streamed
 * VOC voice (radio messages) on a double buffer.  That module is replaced
 * here at its entry points by a software mixer on SDL audio; the game still
 * loads its VOC effects itself (44a63a: data, length, rate per sound).
 * The SOS timer services (the game's frame timer) stay recompiled. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))
#define NVOICE 24
#define OUT_RATE 44100

/* game variables */
#define G_SOUND_ON  0x4a9556    /* u16 */
#define G_NOENGINE  0x4a9558    /* u16 ("noengine" argument) */
#define G_SND_RATE  0x4a9280    /* per sound (12 bytes): rate, length, data */
#define G_SND_LEN   0x4a9284
#define G_SND_DATA  0x4a9288
#define G_ENG_RATE  0x4a9484
#define G_ENG_LEN   0x4a9488
#define G_ENG_DATA  0x4a948c
#define G_ENG_H     0x4a94d8
#define G_SPEED     0x49381c    /* s16 */
#define G_LOOPA_H   0x48c0e8    /* 20000 = not playing */
#define G_LOOPB_H   0x48c0ec
#define G_STR_STATE 0x4a95f0    /* 0 idle, 1 playing, 2 last buffer */
#define G_STR_BUSY  0x4a95ec
#define NOT_PLAYING 20000

typedef struct {
    int active, loop;
    const u8 *host;     /* host buffer (streamed voice) or NULL: guest data */
    u32 data, size;
    u64 pos, step;      /* 32.32 frames */
    u32 pitch;          /* 16.16 */
    u32 rate;
    int vol;            /* 0..256 */
} Voice;

static Voice voices[NVOICE];
static SDL_AudioDeviceID dev;
int sound_master = 100;     /* percent (settings) */
static u8 *stream_buf;
static int stream_voice = -1;

static void mix(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    s16 *out = (s16 *)stream;
    int frames = len / 4;
    static s32 acc[8192];
    if (frames > 8192) frames = 8192;
    memset(acc, 0, frames * sizeof(s32));
    int mv = getenv(ENV("HIDDEN")) ? 0 : sound_master;
    for (int v = 0; v < NVOICE; v++) {
        Voice *vc = &voices[v];
        if (!vc->active) continue;
        if (!vc->size) { vc->active = 0; continue; }
        const u8 *p = vc->host ? vc->host : MEM + vc->data;
        int g = vc->vol * mv / 100;
        for (int i = 0; i < frames; i++) {
            u32 f = (u32)(vc->pos >> 32);
            if (f >= vc->size) {
                if (vc->loop) { vc->pos -= (u64)vc->size << 32; f = (u32)(vc->pos >> 32); if (f >= vc->size) { vc->pos = 0; f = 0; } }
                else { vc->active = 0; break; }
            }
            acc[i] += ((p[f] - 128) << 8) * g / 256;
            vc->pos += vc->step;
        }
    }
    for (int i = 0; i < frames; i++) {
        s32 s = acc[i];
        s = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        out[i * 2] = out[i * 2 + 1] = (s16)s;
    }
    if (frames * 4 < len) memset(stream + frames * 4, 0, len - frames * 4);
}

static void audio_open(void)
{
    if (dev) return;
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = mix;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { rt_log("sound: cannot open audio: %s\n", SDL_GetError()); return; }
    SDL_PauseAudioDevice(dev, 0);
    rt_log("sound: SDL audio %d Hz\n", have.freq);
}

static void lock(void) { if (dev) SDL_LockAudioDevice(dev); }
static void unlock(void) { if (dev) SDL_UnlockAudioDevice(dev); }

static void set_step(Voice *v)
{
    v->step = (u64)((double)v->rate * v->pitch / 65536.0 / OUT_RATE * 4294967296.0);
}

/* returns the voice number, or -1 */
static int play(u32 data, const u8 *host, u32 size, u32 rate, int loop)
{
    if (!size || (!data && !host)) return -1;
    audio_open();
    lock();
    int h = -1;
    for (int i = 0; i < NVOICE; i++) if (!voices[i].active) { h = i; break; }
    if (h < 0) h = 0;   /* all busy: take the first */
    Voice *v = &voices[h];
    memset(v, 0, sizeof *v);
    v->data = data; v->host = host; v->size = size;
    v->rate = rate >= 2000 && rate <= 48000 ? rate : 11000;
    v->pitch = 0x10000;
    v->loop = loop;
    v->vol = 200;
    set_step(v);
    v->active = 1;
    unlock();
    { static int n; if (n++ < 12) rt_log("sound: play %s%08x len %u rate %u%s -> %d\n", host ? "host " : "", data, size, rate, loop ? " loop" : "", h); }
    return h;
}

static void stop(int h)
{
    if (h < 0 || h >= NVOICE) return;
    lock(); voices[h].active = 0; unlock();
}

void sound_reset(void)
{
    lock();
    memset(voices, 0, sizeof voices);
    free(stream_buf); stream_buf = NULL; stream_voice = -1;
    unlock();
}

void sound_poll(void) {}

/* 44a41c (wrapped): sound system start.  The original also starts the SOS
   timer services (the game's frame timer) whatever the sound card; with
   SOUND.DAT = 0 (no card - what the PC install writes) that is all it does.
   Then the PC's mixer takes the card's place. */
void N_snd_init(void)
{
    rt_wrap_orig("N_snd_init", NULL)();
    u32 r = cpu.r[R_EAX];
    audio_open();
    WR16(G_SOUND_ON, 1);
    cpu.r[R_EAX] = r;
}

/* 44a9f3 (wrapped): shutdown - silence, then the original removes its timer
   events (it skips the SOS card driver with the sound flag off) */
void N_snd_uninit(void)
{
    sound_reset();
    WR16(G_SOUND_ON, 0);
    rt_wrap_orig("N_snd_uninit", NULL)();
}

void play_effect(int idx)
{
    if (!RD16(G_SOUND_ON) || idx < 0 || idx >= 0x2c) return;
    u32 d = RD32(G_SND_DATA + idx * 12);
    if (!d) return;
    play(d, NULL, RD32(G_SND_LEN + idx * 12), RD32(G_SND_RATE + idx * 12), 0);
}

void N_snd_play(void)  { play_effect((int)cpu.r[R_EAX]); RET(0); }          /* 44a865 */
void N_snd_play2(void) { play_effect((s16)cpu.r[R_EAX]); RET(0); }          /* 44a91f */

/* 44abc4: the cab's engine loop */
void N_eng_start(void)
{
    if (!RD16(G_NOENGINE) && RD16(G_SOUND_ON) && RD32(G_ENG_DATA)) {
        int h = play(RD32(G_ENG_DATA), NULL, RD32(G_ENG_LEN), RD32(G_ENG_RATE), 1);
        WR32(G_ENG_H, (u32)h);
    }
    RET(0);
}

/* 44aca6: engine pitch follows the speed (0.55 .. 1.0) */
void N_eng_pitch(void)
{
    if (!RD16(G_NOENGINE) && RD16(G_SOUND_ON)) {
        int s = (s16)RD16(G_SPEED);
        if (s < 0) s = -s;
        if (s > 350) s = 350;
        u32 pitch = (u32)(s * 0x745d / 350 + 0x8ba2);
        int h = (int)RD32(G_ENG_H);
        if (h >= 0 && h < NVOICE) { lock(); voices[h].pitch = pitch; set_step(&voices[h]); unlock(); }
    }
    RET(0);
}

void N_eng_stop(void)          /* 44ad1d */
{
    if (!RD16(G_NOENGINE) && RD16(G_SOUND_ON)) stop((int)RD32(G_ENG_H));
    RET(0);
}

static void loop_start(u32 slot)
{
    int idx = (s16)cpu.r[R_EAX];
    if (RD16(G_SOUND_ON) && idx >= 0 && idx < 0x2c && RD32(G_SND_DATA + idx * 12) && RD32(slot) == NOT_PLAYING) {
        int h = play(RD32(G_SND_DATA + idx * 12), NULL, RD32(G_SND_LEN + idx * 12), RD32(G_SND_RATE + idx * 12), 1);
        WR32(slot, h < 0 ? NOT_PLAYING : (u32)h);
    }
    RET(0);
}

static void loop_stop(u32 slot)
{
    if (RD16(G_SOUND_ON) && RD32(slot) != NOT_PLAYING) { stop((int)RD32(slot)); WR32(slot, NOT_PLAYING); }
    RET(0);
}

void N_loopa_start(void) { loop_start(G_LOOPA_H); }   /* 44ad56 */
void N_loopa_stop(void)  { loop_stop(G_LOOPA_H); }    /* 44ae36 */
void N_loopb_start(void) { loop_start(G_LOOPB_H); }   /* 44ae7b */
void N_loopb_stop(void)  { loop_stop(G_LOOPB_H); }    /* 44af5b */

/* ---- streamed voice (VOC file named by eax) ---- */
static u8 *load_voc(const char *guest, u32 *len, u32 *rate)
{
    extern int dos_resolve(const char *guest, char *host);
    char host[700];
    if (!dos_resolve(guest, host)) return NULL;
    FILE *f = fopen(host, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n > 0 ? n : 1);
    if (!d || fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    if (n < 26 || memcmp(d, "Creative Voice File", 19)) { free(d); return NULL; }
    u32 p = d[20] | (d[21] << 8), out = 0;
    *rate = 11000;
    u8 *pcm = malloc(n);
    while (p + 4 <= (u32)n && d[p]) {
        u32 t = d[p], bl = d[p + 1] | (d[p + 2] << 8) | (d[p + 3] << 16);
        u32 b = p + 4;
        if (b + bl > (u32)n) bl = (u32)n - b;
        if (t == 1 && bl >= 2) {
            *rate = 1000000 / (256 - d[b]);
            memcpy(pcm + out, d + b + 2, bl - 2); out += bl - 2;
        } else if (t == 2) {
            memcpy(pcm + out, d + b, bl); out += bl;
        } else if (t == 9 && bl >= 12) {
            *rate = d[b] | (d[b + 1] << 8) | (d[b + 2] << 16);
            memcpy(pcm + out, d + b + 12, bl - 12); out += bl - 12;
        }
        p = b + bl;
    }
    free(d);
    *len = out;
    return pcm;
}

/* 44b010: start the voice stream */
void N_stream_start(void)
{
    if (RD16(G_SOUND_ON)) {
        char name[128];
        u32 a = cpu.r[R_EAX];
        int i = 0;
        for (; i < 127; i++) { name[i] = (char)RD8(a + i); if (!name[i]) break; }
        name[i] = 0;
        if (stream_voice >= 0) stop(stream_voice);
        lock();
        free(stream_buf);
        stream_buf = NULL;
        unlock();
        u32 len = 0, rate = 11000;
        u8 *pcm = load_voc(name, &len, &rate);
        if (pcm) {
            stream_buf = pcm;
            stream_voice = play(0, pcm, len, rate, 0);
            if (stream_voice >= 0) voices[stream_voice].vol = 256;
            WR16(G_STR_STATE, 1);
            WR16(G_STR_BUSY, 0);
        } else rt_log("sound: voice '%s' not loaded\n", name);
    }
    RET(0);
}

static int stream_playing(void)
{
    return stream_voice >= 0 && voices[stream_voice].active && voices[stream_voice].host == stream_buf;
}

/* 44b17c: the game polls the stream; state 0 once it has finished */
void N_stream_pump(void)
{
    if (RD16(G_STR_STATE) && !stream_playing()) WR16(G_STR_STATE, 0);
    RET(0);
}

/* 44b27c: wait for the voice to finish */
void N_stream_wait(void)
{
    while (RD16(G_SOUND_ON) && stream_playing()) {
        SDL_Delay(5);
        irq_poll();
    }
    WR16(G_STR_STATE, 0);
    RET(0);
}

/* 44b2cc: stop the voice */
void N_stream_stop(void)
{
    if (stream_voice >= 0 && stream_playing()) stop(stream_voice);
    WR16(G_STR_STATE, 0);
    RET(0);
}
