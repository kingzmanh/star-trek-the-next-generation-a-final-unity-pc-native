/* Native digital sound: replaces the HMI SOS digital driver API the game was
 * built against (sosDIGIStartSample & co.) and Cryo's sound-system init with
 * a software mixer on SDL audio.  Sample data is played straight out of the
 * game's memory, as the original driver did through DMA.
 *
 * Calling convention (Watcom registers): eax = driver handle, edx = sample
 * handle, ebx = value; StartSample takes a far pointer to an SOS
 * _SOS_START_SAMPLE record in ebx:ecx. */
#include <string.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))
#define NVOICE 32
#define OUT_RATE 44100

/* _SOS_START_SAMPLE (32-bit SOS 4) */
#define SS_PTR      0x00
#define SS_SIZE     0x08
#define SS_LOOPS    0x0c
#define SS_CHANNEL  0x10
#define SS_VOLUME   0x14
#define SS_FLAGS    0x28
#define SS_PITCH    0x38
#define F_LOOPING   0x4000
#define F_8TO16     0x0020
#define F_16TO8     0x0080

typedef struct {
    int active;
    u32 data, size;     /* guest address / bytes */
    int bits16, stereo;
    u64 pos;            /* 32.32 fixed, in frames */
    u64 step;
    int loops_left;     /* -1 = forever */
    int vol;            /* 0..32767 */
    int pan;            /* 0..65535, 32768 = centre */
} Voice;

static Voice voices[NVOICE];
static SDL_AudioDeviceID dev;
static u32 driver_rate = 22050;
static int master_vol = 32767;

static void mix(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    s16 *out = (s16 *)stream;
    int frames = len / 4;
    static s32 acc[8192 * 2];
    if (frames > 8192) frames = 8192;
    memset(acc, 0, frames * 2 * sizeof(s32));
    for (int v = 0; v < NVOICE; v++) {
        Voice *vc = &voices[v];
        if (!vc->active) continue;
        u32 fsize = vc->size / ((vc->bits16 ? 2 : 1) * (vc->stereo ? 2 : 1));
        if (!fsize) { vc->active = 0; continue; }
        int gl = (int)((s64)vc->vol * master_vol / 32767);
        int pl = vc->pan > 32768 ? (65535 - vc->pan) * 2 : 65535;
        int pr = vc->pan < 32768 ? vc->pan * 2 : 65535;
        for (int i = 0; i < frames; i++) {
            u32 f = (u32)(vc->pos >> 32);
            if (f >= fsize) {
                if (vc->loops_left != 0) {
                    if (vc->loops_left > 0) vc->loops_left--;
                    vc->pos -= (u64)fsize << 32;
                    f = (u32)(vc->pos >> 32);
                    if (f >= fsize) { vc->pos = 0; f = 0; }
                } else { vc->active = 0; break; }
            }
            s32 l, r;
            const u8 *p = MEM + vc->data;
            if (vc->bits16) {
                const s16 *s = (const s16 *)p;
                if (vc->stereo) { l = s[f * 2]; r = s[f * 2 + 1]; } else l = r = s[f];
            } else {
                if (vc->stereo) { l = (p[f * 2] - 128) << 8; r = (p[f * 2 + 1] - 128) << 8; }
                else l = r = (p[f] - 128) << 8;
            }
            acc[i * 2] += (s32)((s64)l * gl / 32767 * pl / 65535);
            acc[i * 2 + 1] += (s32)((s64)r * gl / 32767 * pr / 65535);
            vc->pos += vc->step;
        }
    }
    for (int i = 0; i < frames * 2; i++) {
        s32 s = acc[i];
        out[i] = (s16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
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
    if (getenv("ALIENS_HIDDEN")) master_vol = 0;   /* background test runs stay silent */
    SDL_PauseAudioDevice(dev, 0);
    rt_log("sound: SDL audio %d Hz\n", have.freq);
}

static u64 step_for(u32 pitch)
{
    if (!pitch) pitch = 0x10000;
    return (u64)((double)driver_rate * pitch / 65536.0 / OUT_RATE * 4294967296.0);
}

/* 45d390: Cryo's sound system init (eax = mixing rate, edx = verbose) */
void N_snd_init(void)
{
    u32 rate = cpu.r[R_EAX];
    if (rate >= 8000 && rate <= 48000) driver_rate = rate;
    audio_open();
    /* 16-bit stereo output */
    WR32(0x4d7eec, 1); WR32(0x4d7ef0, 0); WR32(0x4d7ef4, 0); WR32(0x4d7ef8, 0);
    WR32(0x4d7ee4, NVOICE);
    WR32(0x4d9c64, NVOICE);
    for (int i = 0; i < NVOICE; i++) {
        u32 s = 0x4d7f4c + i * 0x74;
        WR32(s, 0); WR16(s + 4, 0); WR32(s + 0x1c, 0); WR16(s + 0x20, 0);
        WR32(s + SS_PITCH, 0x10000);
        WR32(s + SS_FLAGS, 0x500);
        WR32(0x619680 + i * 4, 0);
        WR32(0x619780 + i * 8, 0xffffffff);
        WR32(0x619784 + i * 8, 0);
    }
    WR32(0x619954, 1);                 /* driver handle */
    { extern void midi_system_init(void); midi_system_init(); }   /* native music (native_midi.c) */
    WR32(0x4dd0f0, 1);                 /* sound system up */
    rt_log("sound: native init, mixing rate %u\n", driver_rate);
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 45d314 / 45d320: sound system shutdown */
void N_snd_uninit(void)
{
    if (dev) SDL_LockAudioDevice(dev);
    memset(voices, 0, sizeof voices);
    if (dev) SDL_UnlockAudioDevice(dev);
    WR32(0x4dd0f0, 0);
    RET(0);
}

/* 4683e3: sosDIGIStartSample(hDriver, far _SOS_START_SAMPLE *) -> handle */
void N_sos_start_sample(void)
{
    u32 ss = cpu.r[R_EBX] + sel_base((u16)cpu.r[R_ECX]);
    u32 data = RD32(ss + SS_PTR) + sel_base(RD16(ss + SS_PTR + 4));
    u32 size = RD32(ss + SS_SIZE);
    u32 flags = RD32(ss + SS_FLAGS);
    u32 chan = RD32(ss + SS_CHANNEL);
    int h = -1;
    {
        static int n;
        if (n++ < 30) {
            rt_log("sos start: data=%08x size=%u loops=%u chan=%u vol=%x flags=%x pitch=%x |", data, size,
                   RD32(ss + SS_LOOPS), chan, RD32(ss + SS_VOLUME), flags, RD32(ss + SS_PITCH));
            for (int i = 0; i < 0x50; i += 4) rt_log(" %x", RD32(ss + i));
            rt_log("\n");
        }
    }
    if (dev) SDL_LockAudioDevice(dev);
    for (int i = 0; i < NVOICE; i++) if (!voices[i].active) { h = i; break; }
    if (h >= 0) {
        Voice *v = &voices[h];
        memset(v, 0, sizeof *v);
        v->data = data;
        v->size = size;
        v->bits16 = !(flags & F_8TO16);
        v->stereo = chan == 3;
        v->step = step_for(RD32(ss + SS_PITCH));
        { u32 lc = RD32(ss + SS_LOOPS); v->loops_left = (flags & F_LOOPING) ? -1 : (lc == 0xffffffffu || lc > 1000) ? 0 : (int)lc; }
        u32 vol = RD32(ss + SS_VOLUME) & 0xffff;
        if (vol < 0x100) vol <<= 7;   /* the game also passes 0..255 levels unshifted (Mindscape logo: 0x3c) */
        v->vol = vol > 32767 ? 32767 : (int)vol;
        v->pan = 32768;
        v->active = 1;
    }
    if (dev) SDL_UnlockAudioDevice(dev);
    cpu.r[R_EAX] = (u32)h;
    RET(0);
}

static Voice *voice_arg(void)
{
    u32 h = cpu.r[R_EDX];
    return h < NVOICE ? &voices[h] : NULL;
}

/* 4689fc: sosDIGIStopSample(hDriver, handle) */
void N_sos_stop_sample(void)
{
    Voice *v = voice_arg();
    if (dev) SDL_LockAudioDevice(dev);
    if (v) v->active = 0;
    if (dev) SDL_UnlockAudioDevice(dev);
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 468e77: sosDIGISetSampleVolume(hDriver, handle, volume) */
void N_sos_set_volume(void)
{
    Voice *v = voice_arg();
    u32 vol = cpu.r[R_EBX] & 0xffff;
    { static int n; if (n++ < 40) rt_log("sos volume: handle %u ebx=%08x\n", cpu.r[R_EDX], cpu.r[R_EBX]); }
    if (v) v->vol = vol > 32767 ? 32767 : (int)vol;
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 46911a: sosDIGISetPanLocation(hDriver, handle, pan) */
void N_sos_set_pan(void)
{
    Voice *v = voice_arg();
    u32 pan = cpu.r[R_EBX] & 0xffff;
    if (v) v->pan = (int)pan;
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 46904d: sosDIGISetPitch(hDriver, handle, pitch 16.16) */
void N_sos_set_pitch(void)
{
    Voice *v = voice_arg();
    if (dev) SDL_LockAudioDevice(dev);
    if (v) v->step = step_for(cpu.r[R_EBX]);
    if (dev) SDL_UnlockAudioDevice(dev);
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 46946f: sosDIGISetMasterVolume(hDriver, volume) */
void N_sos_master_volume(void)
{
    u32 vol = cpu.r[R_EDX] & 0xffff;
    if (!getenv("ALIENS_HIDDEN")) master_vol = vol > 32767 ? 32767 : (int)vol;
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 4694cf: sosDIGISampleDone(hDriver, handle) -> nonzero when finished */
void N_sos_sample_done(void)
{
    Voice *v = voice_arg();
    cpu.r[R_EAX] = (!v || !v->active) ? 1 : 0;
    RET(0);
}
