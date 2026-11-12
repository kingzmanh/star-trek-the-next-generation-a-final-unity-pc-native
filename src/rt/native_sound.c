/* Native digital sound for A Final Unity.
 *
 * The game drives a Sound Blaster through HMI SOS 4: the SOS library is
 * linked in (4b37ac...), the hardware driver HMIDRV.386 is loaded from disk at
 * run time.  Everything is replaced here at the SOS API: a software mixer on
 * SDL audio plays the game's sample records (_SOS_START_SAMPLE) and calls
 * the game's sample callbacks the way the driver did.
 *
 * How the game uses SOS (sounds.cpp, fvfaudio.cpp):
 *  - short effects: sosDIGIStartSample with the whole sound;
 *  - streams (music, voices, long ambience): 4 channels, each ping-ponging two
 *    64 KB buffers; the callback (4235bc) answers _SAMPLE_PROCESSED with
 *    sosDIGIContinueSample(next buffer) or a short silence buffer;
 *  - movie sound: a 0xb000 ring fed in 0x800 chunks by callback 4177bc; the
 *    movie clock is the sample's played-byte count (4b5167).
 *  - the driver's service routine is a far pointer the game calls from its
 *    72 Hz timer (42378c: call far [60ef2c]) - here that call pumps the
 *    callbacks (rt_call_far, MAGIC_SVC).
 *
 * Data handed to Start/ContinueSample is copied when it is queued, so the game
 * may refill its buffer as soon as the callback has moved on (it does).
 *
 * All SOS entry points are cdecl: [esp+4] = hDriver, ... ; far pointers are
 * offset + selector dwords. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))
#define ARG(i) RD32(cpu.r[R_ESP] + 4 + 4 * (i))

#define NVOICE 32
#define OUT_RATE 44100
#define DRV_RATE 22050
#define LOOKAHEAD_MS 160        /* ask for the next buffer this early (DOS: ~ one DMA half) */

/* _SOS_START_SAMPLE (32-bit SOS 4, 0x74 bytes) */
#define SS_PTR      0x00        /* far: offset, selector */
#define SS_SIZE     0x08
#define SS_LOOPS    0x0c
#define SS_CHANNEL  0x10        /* 0 left, 1 right, 2 centre, 3 interleaved stereo */
#define SS_VOLUME   0x14
#define SS_ID       0x18
#define SS_CB       0x1c        /* far cdecl callback(hDriver, action, hSample) */
#define SS_FLAGS    0x28
#define SS_PITCH    0x38
#define SS_PAN      0x40
#define F_LOOPING   0x4000
#define F_PANNING   0x0200
#define F_VOLUME    0x0100
#define F_16TO8     0x0080
#define F_8TO16     0x0020
#define F_PITCH     0x0400

#define A_PROCESSED 0
#define A_LOOPING   1
#define A_DONE      2

/* game globals */
#define G_HDRIVER   0x58ef10

typedef struct Buf { u8 *data; u32 frames; struct Buf *next; } Buf;

typedef struct {
    int active;
    int bits16, stereo;         /* source format */
    Buf *cur;                   /* playing; ->next = queued by ContinueSample */
    u64 pos;                    /* 32.32 frames in cur */
    u64 step;
    int looping;
    int vol;                    /* 0..32767 */
    int pan;                    /* 0..65535 */
    u32 played;                 /* source bytes consumed since StartSample */
    u32 cb, ss;                 /* callback offset, guest address of the record */
    int notified;               /* PROCESSED sent for cur (re-armed per buffer / loop) */
    int ended;                  /* mixer ran out: DONE pending */
    int cb_pending;             /* PROCESSED/LOOPING to deliver */
} Voice;

static Voice voices[NVOICE];
int sound_active_voices(void) { int c = 0; for (int i = 0; i < NVOICE; i++) if (voices[i].active) c++; return c; }
static SDL_AudioDeviceID dev;
static int master_vol = 32767;
static int sound_up;
static FILE *wav;
static int mute_out;

static void lock(void) { if (dev) SDL_LockAudioDevice(dev); }
static void unlock(void) { if (dev) SDL_UnlockAudioDevice(dev); }

static int frame_bytes(const Voice *v) { return (v->bits16 ? 2 : 1) * (v->stereo ? 2 : 1); }

static void free_bufs(Voice *v)
{
    Buf *b = v->cur;
    while (b) { Buf *n = b->next; free(b->data); free(b); b = n; }
    v->cur = NULL;
}

static void mix(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    s16 *out = (s16 *)stream;
    int frames = len / 4;
    static s32 acc[8192 * 2];
    if (frames > 8192) frames = 8192;
    memset(acc, 0, frames * 2 * sizeof(s32));
    for (int vi = 0; vi < NVOICE; vi++) {
        Voice *v = &voices[vi];
        if (!v->active || v->ended || !v->cur) continue;
        int gl = (int)((s64)v->vol * master_vol / 32767);
        int pl = v->pan > 32768 ? (65535 - v->pan) * 2 : 65535;
        int pr = v->pan < 32768 ? v->pan * 2 : 65535;
        if (pl > 65535) pl = 65535;
        if (pr > 65535) pr = 65535;
        int fb = frame_bytes(v);
        for (int i = 0; i < frames; i++) {
            u32 f = (u32)(v->pos >> 32);
            int stall = 0;
            while (f >= v->cur->frames) {
                Buf *b = v->cur;
                if (b->next) {                       /* continue with the queued buffer */
                    v->cur = b->next;
                    v->pos -= (u64)b->frames << 32;
                    free(b->data); free(b);
                    v->notified = 0;
                } else if (v->looping && b->frames) {
                    v->pos -= (u64)b->frames << 32;
                    v->notified = 0;
                    v->cb_pending = 1;              /* _SAMPLE_LOOPING */
                } else if (!v->notified) {
                    /* out of data before the game was asked for more (an empty
                       first buffer): wait for its answer to _SAMPLE_PROCESSED */
                    v->pos = (u64)b->frames << 32;
                    stall = 1;
                    break;
                } else {
                    v->ended = 1;
                    break;
                }
                f = (u32)(v->pos >> 32);
            }
            if (v->ended || stall) break;
            const u8 *p = v->cur->data + (size_t)f * fb;
            s32 l, r;
            if (v->bits16) {
                const s16 *s = (const s16 *)p;
                if (v->stereo) { l = s[0]; r = s[1]; } else l = r = s[0];
            } else {
                if (v->stereo) { l = (p[0] - 128) << 8; r = (p[1] - 128) << 8; }
                else l = r = (p[0] - 128) << 8;
            }
            acc[i * 2] += (s32)((s64)l * gl / 32767 * pl / 65535);
            acc[i * 2 + 1] += (s32)((s64)r * gl / 32767 * pr / 65535);
            u64 np = v->pos + v->step;
            v->played += (u32)((np >> 32) - (v->pos >> 32)) * fb;
            v->pos = np;
        }
    }
    for (int i = 0; i < frames * 2; i++) {
        s32 s = acc[i];
        out[i] = (s16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    if (wav) { fwrite(out, 4, frames, wav); fflush(wav); }   /* <ENV>WAVOUT=1: capture the mix */
    if (mute_out) memset(out, 0, frames * 4);               /* hidden test runs stay silent */
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
    mute_out = getenv(ENV("HIDDEN")) != NULL;
    if (getenv(ENV("WAVOUT")) && !wav) {
        wav = fopen("sound_out.wav", "wb");
        if (wav) {   /* header with open length: players read to the end */
            static const u8 h[44] = {'R','I','F','F',0xff,0xff,0xff,0x7f,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                0x44,0xac,0,0,0x10,0xb1,2,0,4,0,16,0,'d','a','t','a',0xff,0xff,0xff,0x7f};
            fwrite(h, 1, 44, wav);
        }
    }
    SDL_PauseAudioDevice(dev, 0);
    rt_log("sound: SDL audio %d Hz\n", have.freq);
}

void sound_reset(void)
{
    lock();
    for (int i = 0; i < NVOICE; i++) { free_bufs(&voices[i]); memset(&voices[i], 0, sizeof voices[i]); }
    unlock();
    sound_up = 0;
}

/* copy a record's data into a new buffer (format of the voice) */
static Buf *take(Voice *v, u32 ss)
{
    u32 data = RD32(ss + SS_PTR) + sel_base(RD16(ss + SS_PTR + 4));
    u32 size = RD32(ss + SS_SIZE);
    Buf *b = calloc(1, sizeof *b);
    int fb = frame_bytes(v);
    if (size > 64u << 20) size = 0;
    b->frames = size / fb;
    b->data = malloc(b->frames * fb + 4);
    if (b->frames) memcpy(b->data, MEM + data, (size_t)b->frames * fb);
    return b;
}

static void apply_params(Voice *v, u32 ss)
{
    u32 flags = RD32(ss + SS_FLAGS);
    u32 vol = RD32(ss + SS_VOLUME);
    if (vol > 0xffff) vol = ((vol >> 16) + (vol & 0xffff)) / 2;   /* packed L/R */
    v->vol = vol > 32767 ? 32767 : (int)vol;
    v->pan = (flags & F_PANNING) ? (int)(RD32(ss + SS_PAN) & 0xffff) : 32768;
    u32 pitch = (flags & F_PITCH) ? RD32(ss + SS_PITCH) : 0x10000;
    if (!pitch) pitch = 0x10000;
    v->step = (u64)((double)DRV_RATE * pitch / 65536.0 / OUT_RATE * 4294967296.0);
}

/* 4b37ac: sosDIGIStartSample(hDriver, far _SOS_START_SAMPLE *) -> handle */
void N_sos_start_sample(void)
{
    u32 ss = ARG(1) + sel_base((u16)ARG(2));
    u32 flags = RD32(ss + SS_FLAGS);
    int h = -1;
    lock();
    for (int i = 0; i < NVOICE; i++) if (!voices[i].active) { h = i; break; }
    if (h >= 0) {
        Voice *v = &voices[h];
        free_bufs(v);
        memset(v, 0, sizeof *v);
        v->bits16 = (flags & F_8TO16) ? 0 : 1;      /* the game writes the device format: 16-bit */
        if (flags & F_16TO8) v->bits16 = 1;
        v->stereo = RD32(ss + SS_CHANNEL) == 3;
        v->looping = (flags & F_LOOPING) != 0;
        v->cb = RD32(ss + SS_CB);
        v->ss = ss;
        apply_params(v, ss);
        v->cur = take(v, ss);
        v->active = 1;
    }
    unlock();
    {
        static int n;
        if (n++ < 40 || getenv(ENV("SOSLOG"))) rt_log("sos start: h=%d size=%u chan=%u vol=%x flags=%x cb=%08x\n", h, RD32(ss + SS_SIZE),
                             RD32(ss + SS_CHANNEL), RD32(ss + SS_VOLUME), flags, RD32(ss + SS_CB));
    }
    cpu.r[R_EAX] = (u32)h;
    RET(0);
}

static Voice *voice(u32 h) { return h < NVOICE && voices[h].active ? &voices[h] : NULL; }

/* 4b3e6d: sosDIGIContinueSample(hDriver, hSample, far _SOS_START_SAMPLE *) */
void N_sos_continue_sample(void)
{
    u32 ss = ARG(2) + sel_base((u16)ARG(3));
    lock();
    Voice *v = voice(ARG(1));
    if (v) {
        Buf *b = take(v, ss);
        Buf **t = &v->cur;
        while (*t) t = &(*t)->next;
        *t = b;
        if (v->ended) {                     /* ran dry meanwhile: resume with the new data */
            Buf *o = v->cur;
            v->cur = b;
            o->next = NULL;
            if (o != b) { free(o->data); free(o); }
            v->pos = 0;
            v->ended = 0;
        }
        v->cb = RD32(ss + SS_CB) ? RD32(ss + SS_CB) : v->cb;
        v->ss = ss;
    }
    unlock();
    cpu.r[R_EAX] = v ? 0 : 10;
    RET(0);
}

/* 4b3dc0: sosDIGIStopSample(hDriver, hSample) */
void N_sos_stop_sample(void)
{
    lock();
    Voice *v = voice(ARG(1));
    if (v) { free_bufs(v); v->active = 0; }
    unlock();
    if (getenv(ENV("SOSLOG"))) rt_log("sos stop: h=%u\n", ARG(1));
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 4b6c6f: sosDIGISampleDone(hDriver, hSample): 1 = not playing */
void N_sos_sample_done(void)
{
    u32 h = ARG(1);
    cpu.r[R_EAX] = h >= NVOICE ? 10 : voices[h].active ? 0 : 1;
    RET(0);
}

/* 4b5167: bytes played since StartSample (the movie clock) */
void N_sos_sample_pos(void)
{
    Voice *v = voice(ARG(1));
    cpu.r[R_EAX] = v ? v->played : 0;
    RET(0);
}

/* 4b5231: sosDIGISetSampleVolume(hDriver, hSample, volume) -> old */
void N_sos_set_volume(void)
{
    Voice *v = voice(ARG(1));
    u32 vol = ARG(2) & 0xffff;
    cpu.r[R_EAX] = v ? (u32)v->vol : 0;
    if (v) v->vol = vol > 32767 ? 32767 : (int)vol;
    RET(0);
}

/* 4b5577: sosDIGISetMasterVolume(hDriver, volume) */
void N_sos_master_volume(void)
{
    u32 vol = ARG(1) & 0xffff;
    master_vol = vol > 32767 ? 32767 : (int)vol;
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 4b5d81: sosDIGIInitSystem(far path, flags) */
void N_sos_init_system(void) { cpu.r[R_EAX] = 0; RET(0); }
/* 4b5ded: sosDIGIUnInitSystem() */
void N_sos_uninit_system(void) { cpu.r[R_EAX] = 0; RET(0); }

/* 4b5e19: sosDIGIInitDriver(id, far hw settings, far _SOS_INIT_DRIVER, far *hDriver) */
void N_sos_init_driver(void)
{
    u32 init = ARG(3) + sel_base((u16)ARG(4));
    u32 ph = ARG(5) + sel_base((u16)ARG(6));
    audio_open();
    master_vol = 32767;
    /* the driver's service routine, called far from the game's timer */
    WR32(init + 0x1c, MAGIC_SVC);
    WR16(init + 0x20, SEL_CODE);
    WR32(ph, 0);
    sound_up = 1;
    rt_log("sound: native SOS driver (id %x, buffer %x, rate %u)\n", ARG(0), RD32(init), RD32(init + 0x10));
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 4b66d2: sosDIGIUnInitDriver(hDriver, close, unload) */
void N_sos_uninit_driver(void)
{
    sound_reset();
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 4b6b9f: sosDIGIGetDeviceCaps(hDriver, far _SOS_CAPABILITIES *) */
void N_sos_get_caps(void)
{
    u32 c = ARG(1) + sel_base((u16)ARG(2));
    for (int i = 0; i < 0x60; i += 4) WR32(c + i, 0);
    memcpy(MEM + c, "PC native mixer", 16);
    WR32(c + 0x20, 0x400);      /* version */
    WR32(c + 0x24, 16);         /* bits per sample */
    WR32(c + 0x28, 2);          /* channels */
    WR32(c + 0x2c, 4000);
    WR32(c + 0x30, 44100);
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* far cdecl callback(hDriver, action, hSample) - returns with retf */
static void call_cb(u32 fn, u32 action, u32 h)
{
    void (*f)(void) = rt_lookup(fn);
    if (!f) { rt_log("sound: callback to unknown code %08x\n", fn); return; }
    CPU save = cpu;
    cpu.r[R_ESP] = (cpu.r[R_ESP] - 64) & ~3u;
    cpu.r[R_ESP] -= 12;
    WR32(cpu.r[R_ESP] + 8, h);
    WR32(cpu.r[R_ESP] + 4, action);
    WR32(cpu.r[R_ESP], 0);              /* hDriver */
    cpu.r[R_ESP] -= 8;
    WR32(cpu.r[R_ESP] + 4, SEL_CODE);
    WR32(cpu.r[R_ESP], MAGIC_IRET + 0xfc);
    f();
    memcpy(cpu.r, save.r, sizeof save.r);
    cpu.cf = save.cf; cpu.zf = save.zf; cpu.sf = save.sf; cpu.of = save.of; cpu.pf = save.pf; cpu.af = save.af;
    cpu.df = save.df;
    for (int i = 0; i < 6; i++) { cpu.sreg[i] = save.sreg[i]; cpu.sbase[i] = save.sbase[i]; }
}

/* the driver service (72 Hz from the game's timer): deliver callbacks */
void sound_service(void)
{
    static int busy;
    if (!sound_up || busy) return;
    busy = 1;
    for (int h = 0; h < NVOICE; h++) {
        u32 cb = 0, action = 0;
        int fire = 0, done = 0;
        lock();
        Voice *v = &voices[h];
        if (v->active) {
            cb = v->cb;
            if (v->ended) {
                done = 1;
                free_bufs(v);
                v->active = 0;
            } else if (v->cb_pending) {
                v->cb_pending = 0;
                fire = 1; action = A_LOOPING;
            } else if (v->cur && !v->cur->next && !v->notified) {
                u32 f = (u32)(v->pos >> 32);
                u64 left = v->cur->frames > f ? v->cur->frames - f : 0;
                /* frames are at the driver rate scaled by the step */
                u64 left_out = v->step ? (left << 32) / v->step : left;
                if (left_out * 1000 < (u64)LOOKAHEAD_MS * OUT_RATE) {
                    v->notified = 1;
                    fire = 1; action = A_PROCESSED;
                }
            }
        }
        unlock();
        if ((fire || done) && getenv(ENV("SOSLOG"))) rt_log("sos cb: h=%d action=%u cb=%08x\n", h, done ? A_DONE : action, cb);
        if (cb && (fire || done)) call_cb(cb, done ? A_DONE : action, (u32)h);
    }
    busy = 0;
}

void sound_poll(void) {}
