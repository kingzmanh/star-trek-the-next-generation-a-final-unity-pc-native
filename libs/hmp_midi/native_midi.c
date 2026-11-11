/* Native music: replaces the game's MIDI module (which drove the HMI SOS
 * MIDI drivers) with an HMP player on the Windows MIDI synthesizer.
 *
 * HMP ("HMIMIDIP013195"): track count at 0x30, ticks per second at 0x38,
 * tracks from 0x388, each = { u32 number, u32 length (incl. header), u32 id }
 * followed by MIDI events.  Delta times are HMI variable-length numbers:
 * 7 bits per byte, least significant first, last byte has bit 7 set.
 *
 * Game-visible state kept like the original: 4d9c68 device id, 4d9d24 MIDI
 * up, 4d9d30 song loaded, 4d9d2c song handle, 4d9d28 volume (0..127). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))
#define G_DEVICE  0x4d9c68u
#define G_MIDI_UP 0x4d9d24u
#define G_VOLUME  0x4d9d28u
#define G_HANDLE  0x4d9d2cu
#define G_LOADED  0x4d9d30u
#define G_NAME    0x619ab0u
#define MLOG(n) rt_log("music: " n " eax=%d vol=%d\n", (int)cpu.r[R_EAX], (int)RD32(G_VOLUME))

extern int dos_resolve(const char *guest, char *host);

typedef struct { u32 tick; u8 len, b[3]; } Ev;

static HMIDIOUT mout;
static CRITICAL_SECTION lock;
static HANDLE thread;
static Ev *evs;
static int nev, pos, playing, paused, looping, stop_thread;
static u32 tps = 120;
static double t0, pause_at;          /* seconds */
static int volume = 127, fade_to = -1;
static double fade_start;
static int fade_from;
static u8 chan_vol[16];

static double now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static void out3(u8 a, u8 b, u8 c) { if (mout) midiOutShortMsg(mout, a | (b << 8) | (c << 16)); }

static void send_volumes(void)
{
    for (int ch = 0; ch < 16; ch++) out3(0xb0 | ch, 7, (u8)(chan_vol[ch] * volume / 127));
}

static void all_off(void)
{
    for (int ch = 0; ch < 16; ch++) { out3(0xb0 | ch, 123, 0); out3(0xb0 | ch, 120, 0); }
}

static void reset_channels(void)
{
    for (int ch = 0; ch < 16; ch++) {
        chan_vol[ch] = 100;
        out3(0xb0 | ch, 121, 0);        /* reset controllers */
        out3(0xe0 | ch, 0, 0x40);       /* centre pitch bend */
    }
    send_volumes();
}

static void send_event(const Ev *e)
{
    u8 st = e->b[0];
    if ((st & 0xf0) == 0xb0) {
        u8 cc = e->b[1];
        if (cc >= 0x6e && cc <= 0x77) return;   /* HMI private controllers */
        if (cc == 7) { chan_vol[st & 15] = e->b[2]; out3(st, 7, (u8)(e->b[2] * volume / 127)); return; }
    }
    if (e->len == 3) out3(st, e->b[1], e->b[2]);
    else out3(st, e->b[1], 0);
}

static DWORD WINAPI player(LPVOID p)
{
    (void)p;
    while (!stop_thread) {
        EnterCriticalSection(&lock);
        if (playing && !paused && evs) {
            double t = now_s();
            if (fade_to >= 0) {
                double k = (t - fade_start) / 0.5;
                int v = k >= 1 ? fade_to : (int)(fade_from + (fade_to - fade_from) * k);
                if (v != volume) { volume = v; send_volumes(); }
                if (k >= 1) {
                    if (fade_to == 0) { all_off(); playing = 0; }
                    fade_to = -1;
                }
            }
            u32 tick = (u32)((t - t0) * tps);
            while (playing && pos < nev && evs[pos].tick <= tick) send_event(&evs[pos++]);
            if (playing && pos >= nev) {
                if (looping) { all_off(); pos = 0; t0 = t; }
                else { all_off(); playing = 0; }
            }
        }
        LeaveCriticalSection(&lock);
        Sleep(2);
    }
    return 0;
}

static u32 hmi_vlq(const u8 *p, const u8 *end, int *n)
{
    u32 v = 0;
    int sh = 0, i = 0;
    while (p + i < end && !(p[i] & 0x80)) { v |= (u32)p[i] << sh; sh += 7; i++; }
    if (p + i < end) { v |= (u32)(p[i] & 0x7f) << sh; i++; }
    *n = i;
    return v;
}

static u32 midi_vlq(const u8 *p, const u8 *end, int *n)
{
    u32 v = 0;
    int i = 0;
    while (p + i < end) { v = (v << 7) | (p[i] & 0x7f); if (!(p[i++] & 0x80)) break; }
    *n = i;
    return v;
}

typedef struct { Ev e; int i; } Tagged;
static int cmp_tag(const void *a, const void *b)
{
    const Tagged *x = a, *y = b;
    if (x->e.tick != y->e.tick) return x->e.tick < y->e.tick ? -1 : 1;
    return x->i - y->i;
}

static int load_hmp(const u8 *d, u32 size)
{
    if (size < 0x388 || memcmp(d, "HMIMIDIP", 8)) return 0;
    u32 ntr = *(const u32 *)(d + 0x30);
    tps = *(const u32 *)(d + 0x38);
    if (!tps) tps = 120;
    int cap = 4096;
    Ev *list = malloc(cap * sizeof(Ev));
    int n = 0;
    u32 off = 0x388;
    for (u32 t = 0; t < ntr && off + 12 <= size; t++) {
        u32 len = *(const u32 *)(d + off + 4);
        if (len < 12 || off + len > size) break;
        const u8 *p = d + off + 12, *end = d + off + len;
        u32 tick = 0;
        u8 run = 0;
        while (p < end) {
            int k;
            tick += hmi_vlq(p, end, &k);
            p += k;
            if (p >= end) break;
            u8 st = *p;
            if (st & 0x80) { p++; if (st < 0xf0) run = st; } else st = run;
            if (st == 0xff) {
                if (p >= end) break;
                u8 type = *p++;
                u32 ml = midi_vlq(p, end, &k);
                p += k + ml;
                if (type == 0x2f) break;
                continue;
            }
            if (st == 0xf0 || st == 0xf7) { u32 ml = midi_vlq(p, end, &k); p += k + ml; continue; }
            if (!(st & 0x80)) break;
            int nd = ((st & 0xe0) == 0xc0) ? 1 : 2;
            if (p + nd > end) break;
            if (n == cap) { cap *= 2; list = realloc(list, cap * sizeof(Ev)); }
            Ev *e = &list[n++];
            e->tick = tick;
            e->len = (u8)(1 + nd);
            e->b[0] = st;
            e->b[1] = p[0] & 0x7f;
            e->b[2] = nd == 2 ? (p[1] & 0x7f) : 0;
            p += nd;
        }
        off += len;
    }
    /* merge the tracks: stable order by time (tracks are already sorted) */
    Tagged *tg = malloc((n ? n : 1) * sizeof(Tagged));
    for (int i = 0; i < n; i++) { tg[i].e = list[i]; tg[i].i = i; }
    qsort(tg, n, sizeof(Tagged), cmp_tag);
    for (int i = 0; i < n; i++) list[i] = tg[i].e;
    free(tg);
    free(evs);
    evs = list;
    nev = n;
    pos = 0;
    return 1;
}

static void midi_open(void)
{
    if (mout) return;
    if (midiOutOpen(&mout, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        rt_log("music: no Windows MIDI output\n");
        mout = NULL;
        return;
    }
    thread = CreateThread(NULL, 0, player, NULL, 0, NULL);
    SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL);
    reset_channels();
    rt_log("music: Windows MIDI output open\n");
}

/* called from the native sound init (device id from ALIENS.CFG) */
void midi_system_init(void)
{
    u32 dev = RD32(G_DEVICE);
    /* the Windows synth is General MIDI: report an MPU-401 GM device so the
       game picks its GM arrangements (FM ids 0xa002/0xa009 get the x* files) */
    (void)dev; dev = 0xa000;
    WR32(G_DEVICE, dev);
    static int lock_ready;
    if (!lock_ready) { InitializeCriticalSection(&lock); lock_ready = 1; }
    if (getenv("ALIENS_HIDDEN")) { WR32(G_MIDI_UP, 1); return; }  /* silent test runs */
    midi_open();
    WR32(G_MIDI_UP, 1);
    WR32(G_HANDLE, 0xffffffff);
    WR32(G_LOADED, 0);
}

/* 459b80: InitMidi(device) */
void N_midi_init(void)
{
    WR32(G_DEVICE, cpu.r[R_EAX]);
    midi_system_init();
    cpu.r[R_EAX] = 0;
    RET(0);
}

/* 459b50: shut music down */
void N_midi_uninit(void)
{
    if (mout) {
        EnterCriticalSection(&lock);
        playing = 0;
        all_off();
        LeaveCriticalSection(&lock);
    }
    RET(0);
}

/* 459e90: PlaySong(char *name, int loop) */
void N_midi_play(void)
{
    char g[260];
    u32 a = cpu.r[R_EAX];
    int i = 0;
    for (; i < 259 && RD8(a + i); i++) g[i] = (char)RD8(a + i);
    g[i] = 0;
    memcpy(MEM + G_NAME, g, i + 1);
    int loop = cpu.r[R_EDX] != 0;
    char h[800];
    if (!dos_resolve(g, h)) { rt_log("music: '%s' not found\n", g); RET(0); return; }
    FILE *f = fopen(h, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        u8 *d = malloc(n);
        if (fread(d, 1, n, f) == (size_t)n) {
            EnterCriticalSection(&lock);
            all_off();
            if (load_hmp(d, (u32)n)) {
                int v = (int)RD32(G_VOLUME);
                if (v > 0x7e) v = 0x7e;
                if (v < 0) v = 0;
                WR32(G_VOLUME, v);
                volume = v;
                reset_channels();
                looping = loop;
                paused = 0;
                fade_to = -1;
                t0 = now_s();
                playing = mout != NULL;
                rt_log("music: %d events, %u ticks/s\n", nev, tps);
            }
            LeaveCriticalSection(&lock);
        }
        free(d);
        fclose(f);
    }
    rt_log("music: play '%s'%s\n", g, loop ? " (loop)" : "");
    WR32(G_LOADED, 1);
    WR32(G_HANDLE, 0);
    RET(0);
}

/* 459d50: StopSong (the original fades out first) */
void N_midi_stop(void)
{
    MLOG("N_midi_stop");
    if (mout) {
        EnterCriticalSection(&lock);
        if (playing) { fade_from = volume; fade_to = 0; fade_start = now_s(); }
        LeaveCriticalSection(&lock);
    }
    WR32(G_LOADED, 0);
    WR32(G_HANDLE, 0xffffffff);
    RET(0);
}

/* 459e30: fade out (used by StopSong) */
void N_midi_fade(void)
{
    MLOG("N_midi_fade");
    if (mout) {
        EnterCriticalSection(&lock);
        if (playing) { fade_from = volume; fade_to = 0; fade_start = now_s(); }
        LeaveCriticalSection(&lock);
    }
    RET(0);
}

/* 459e00: SetMusicVolume(v 0..127) */
void N_midi_volume(void)
{
    MLOG("N_midi_volume");
    int v = (int)cpu.r[R_EAX];
    WR32(G_VOLUME, (u32)v);
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    if (mout) {
        EnterCriticalSection(&lock);
        volume = v;
        fade_to = -1;
        send_volumes();
        LeaveCriticalSection(&lock);
    }
    RET(0);
}

/* 459cb0: pause music */
void N_midi_pause(void)
{
    MLOG("N_midi_pause");
    if (mout) {
        EnterCriticalSection(&lock);
        if (playing && !paused) { paused = 1; pause_at = now_s(); all_off(); }
        LeaveCriticalSection(&lock);
    }
    RET(0);
}

/* 459d00: resume music */
void N_midi_resume(void)
{
    MLOG("N_midi_resume");
    if (mout) {
        EnterCriticalSection(&lock);
        if (paused) { paused = 0; t0 += now_s() - pause_at; }
        LeaveCriticalSection(&lock);
    }
    RET(0);
}
