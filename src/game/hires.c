/* Real higher internal resolution for A Final Unity's 3D (space: viewscreen,
 * tactical, combat).
 *
 * The 3D library (4b8000-4d0000) draws through one parameter block: buffer
 * base 6114ac, pitch 6114c0, size 6114c4/6114c8, window and clip 611f10..,
 * centre 611f20/611ee4, projection scale 611e74/611e7c (doubles), z-buffer
 * 636f0c (pitch 611ed4 pixels).  The space loop (4827c4) per frame: clear the
 * window, stars (485356), 4bcd34 (clears the span-row table), camera, objects,
 * effects, then the HUD (486fac...) and the screen flush.
 *
 * So the 3D part of a frame is rendered once, at N x: at 4bcd34 the block
 * is switched to an N x buffer in guest scratch memory (seeded with the window
 * as it is - background, stars - upscaled) and an N x z-buffer; at 486fac it
 * is switched back and the N x picture is merged into the game's 640x480
 * frame (a pixel the 3D pass touched gets the N x block's centre sample).
 * The presenter shows the N x pixels over the window wherever the presented
 * frame still has the merged pixel (the HUD drawn later wins).
 *
 * The span-row table at 636a14 (1024 rows) is swapped for a bigger one
 * during the pass (tools/subst_sttng.txt).  The low-detail modes (56cad0 != 0, half-size + doubling) stay at 1x. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define HR_COL  0x30000000u     /* guest scratch: N x colour buffer */
#define HR_Z    0x31000000u     /* guest scratch: N x z-buffer (32-bit) */

/* viewport fields saved/restored around the pass.  NOT the camera, which the
   frame updates during the pass: matrix 611e94..611ec0, angles 611f1c/611f24/
   611f28, position 611f2c.. (restoring those rolled the camera back a frame:
   the view shook while it turned, e.g. tracking a ship) */
static const u32 vp_rng[][2] = {
    {0x611e60u, 0x611e94u},     /* projection doubles: frustum tangents, x/y scale */
    {0x611ec4u, 0x611f1cu},     /* window, clip, centre y, z pitch, offsets */
    {0x611f20u, 0x611f24u},     /* centre x */
    {0x6114a8u, 0x6114ccu},     /* buffer base, pitch, size */
};
#define VP_N 4
#define VP_BYTES 0x200
#define G_ZBUF  0x636f0cu
#define G_BASE  0x6114acu
#define G_PITCH 0x6114c0u
#define G_W     0x6114c4u
#define G_H     0x6114c8u
#define G_MODE  0x56cad0u
#define G_BACK  0x50534cu       /* -> the 640x480 back buffer */
#define HR_ROW  0x34000000u     /* guest scratch: span-row table for N x heights (tools/subst_sttng.txt) */
#define ROWTAB_MAX 4096

u32 rt_hr_n = 1, rt_hr_k = 0, rt_hr_rowtab = 0;   /* recomp.py substitutions */

static u8 save_vp[VP_BYTES], set_vp[VP_BYTES];   /* 1x values / what the pass set */
static u32 save_z;

static void vp_copy(u8 *dst, int to_mem)
{
    int o = 0;
    for (int i = 0; i < VP_N; i++) {
        u32 n = vp_rng[i][1] - vp_rng[i][0];
        if (to_mem) memcpy(MEM + vp_rng[i][0], dst + o, n); else memcpy(dst + o, MEM + vp_rng[i][0], n);
        o += (int)n;
    }
}

/* back to 1x: a field the game changed during the pass is left as the game set it */
static void vp_restore(void)
{
    int o = 0;
    for (int i = 0; i < VP_N; i++) {
        for (u32 a = vp_rng[i][0]; a < vp_rng[i][1]; a += 4, o += 4) {
            u32 cur = RD32(a), set, old;
            memcpy(&set, set_vp + o, 4); memcpy(&old, save_vp + o, 4);
            if (cur == set) WR32(a, old);
            else { static int n; if (n++ < 20) rt_log("hires: %08x changed during the pass (%08x -> %08x)\n", a, set, cur); }
        }
    }
}
static int active, n_cur, win_x, win_y, win_w, win_h;
static u8 *lo_begin, *touched, *lo_end;      /* per 1x window pixel */
static u8 *hr_pix, *hr_mask;                 /* full screen N x layer */
static int hr_n, hr_w, hr_h;
static DWORD frame_time;
static u32 frames;

static int want_scale(void)
{
    extern int opt_scale;
    return opt_scale < 1 ? 1 : opt_scale > 4 ? 4 : opt_scale;
}

int hires3d_scales(void) { return (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4); }   /* menu: 1x-4x */
void hires3d_base(int *w, int *h) { *w = 640; *h = 480; }

void hires3d_reset(void) { active = 0; frame_time = 0; }

static void call_guest(u32 addr)
{
    CPU save = cpu;
    rt_call_guest(addr);
    memcpy(cpu.r, save.r, sizeof save.r);
}

/* 4bcd34: start of a frame's 3D - switch to N x */
void N_hr_begin(void)
{
    void (*orig)(void) = rt_wrap_orig("N_hr_begin", NULL);
    int n = want_scale();
    u32 ra = RD32(cpu.r[R_ESP]);
    if (ra < 0x4827c4 || ra >= 0x482ca1) n = 1;     /* only the space loop ends its 3D at 486fac */
    u32 base = RD32(G_BASE), pitch = RD32(G_PITCH);
    int w = (int)RD32(G_W), h = (int)RD32(G_H);
    while (n > 1 && h * n > ROWTAB_MAX) n--;
    if (active || n < 2 || RD32(G_MODE) != 0 || w <= 0 || h <= 0 || w > 640 || h > 480 || pitch != 640) {
        orig();
        return;
    }
    u32 back = RD32(G_BACK);
    u32 off = base - back;
    win_x = (int)(off % 640); win_y = (int)(off / 640);
    win_w = w; win_h = h; n_cur = n;
    {
        static int lw, lh, ln;
        if (lw != w || lh != h || ln != n) { lw = w; lh = h; ln = n; rt_log("hires: 3D window %d,%d %dx%d at %dx\n", win_x, win_y, w, h, n); }
    }
    vp_copy(save_vp, 0);
    save_z = RD32(G_ZBUF);
    int W = w * n;
    lo_begin = realloc(lo_begin, (size_t)w * h);
    for (int y = 0; y < h; y++) {
        const u8 *s = MEM + base + (u32)y * pitch;
        memcpy(lo_begin + y * w, s, w);
        for (int yy = 0; yy < n; yy++) {
            u8 *d = MEM + HR_COL + (u32)((y * n + yy) * W);
            for (int x = 0; x < w; x++) memset(d + x * n, s[x], n);
        }
    }
    /* the viewport at N x: the game's own derivation (4b9504) from the window */
    WR32(G_W, (u32)W); WR32(G_H, (u32)(h * n)); WR32(G_PITCH, (u32)W); WR32(G_BASE, HR_COL);
    s32 x0 = (s32)RD32(0x611f10), x1 = (s32)RD32(0x611f0c), y0 = (s32)RD32(0x611f08), y1 = (s32)RD32(0x611ef8);
    s32 cx = (s32)RD32(0x611f20), cy = (s32)RD32(0x611ee4);
    WR32(0x611f10, (u32)(x0 * n)); WR32(0x611f0c, (u32)((x1 + 1) * n - 1));
    WR32(0x611f08, (u32)(y0 * n)); WR32(0x611ef8, (u32)((y1 + 1) * n - 1));
    WR32(0x611f20, (u32)(cx * n + (n - 1) / 2)); WR32(0x611ee4, (u32)(cy * n + (n - 1) / 2));
    call_guest(0x4b9504);
    {
        double sx, sy;
        memcpy(&sx, MEM + 0x611e74, 8); memcpy(&sy, MEM + 0x611e7c, 8);
        sx *= n; sy *= n;
        memcpy(MEM + 0x611e74, &sx, 8); memcpy(MEM + 0x611e7c, &sy, 8);
    }
    WR32(G_ZBUF, HR_Z);
    vp_copy(set_vp, 0);
    rt_hr_rowtab = HR_ROW;
    active = 1;
    orig();
}

static void hr_finish(void);

/* 486fac: the HUD starts - back to 1x, merge the N x picture */
void N_hr_end(void)
{
    void (*orig)(void) = rt_wrap_orig("N_hr_end", NULL);
    hr_finish();
    orig();
}

/* Frame pacing.  The game times its frames in ticks of its 72 Hz timer
 * (559554, +4 per tick): the space loop (4827c4) takes the ticks since the
 * last frame and clamps 0 to 1.  A modern PC draws many frames per tick, so
 * the steps went 1,1,1,4,1,1,4...: uneven, faster than real time, and the
 * ship's steering (Delegate / AI) overshot - the view shook in combat.  On a
 * 1995 PC every frame took at least one tick.  So the flush waits until the
 * timer has moved on since the last frame: steady steps of 4 (72 fps max).
 * sttng_pc.ini [game] SyncToGameTimer=0 switches it off. */
#define G_TIMER 0x559554u
static void pace_frame(void)
{
    extern int settings_int(const char *sec, const char *key, int def, int lo, int hi);
    extern void irq_poll(void);
    static u32 last;
    static int on = -1;
    if (on < 0) on = settings_int("game", "SyncToGameTimer", 1, 0, 1);
    if (!on) return;
    DWORD t0 = GetTickCount();
    static int ticks = -1;
    if (ticks < 0) { const char *e = getenv(ENV("PACE_TICKS")); ticks = e ? atoi(e) : 1; if (ticks < 1) ticks = 1; }
    while ((s32)(RD32(G_TIMER) - last) < 4 * ticks && GetTickCount() - t0 < 200) {
        irq_poll();
        if ((s32)(RD32(G_TIMER) - last) < 4 * ticks) Sleep(1);
    }
    last = RD32(G_TIMER);
}

/* 42769f: screen flush - frame pacing; never leave the viewport switched (safety) */
void N_hr_flush(void)
{
    u32 ra = RD32(cpu.r[R_ESP]);
    if (active) { rt_log("hires: pass not closed before the flush\n"); hr_finish(); }
    if (ra >= 0x4827c4 && ra < 0x482ca1) pace_frame();     /* the space loop's own flush only */
    rt_wrap_orig("N_hr_flush", NULL)();
}

static void hr_finish(void)
{
    if (active) {
        active = 0;
        int n = n_cur, w = win_w, h = win_h, W = w * n;
        vp_restore();
        WR32(G_ZBUF, save_z);
        rt_hr_rowtab = 0;
        u32 base = RD32(G_BASE);
        touched = realloc(touched, (size_t)w * h);
        lo_end = realloc(lo_end, (size_t)w * h);
        if (hr_n != n) {
            hr_n = n; hr_w = 640 * n; hr_h = 480 * n;
            free(hr_pix); free(hr_mask);
            hr_pix = calloc((size_t)hr_w * hr_h, 1);
            hr_mask = calloc((size_t)hr_w * hr_h, 1);
        }
        for (int y = 0; y < h; y++) {
            u8 *lo = MEM + base + (u32)y * 640;
            for (int x = 0; x < w; x++) {
                u8 c0 = lo_begin[y * w + x];
                int t = 0;
                for (int yy = 0; yy < n && !t; yy++) {
                    const u8 *b = MEM + HR_COL + (u32)((y * n + yy) * W + x * n);
                    for (int xx = 0; xx < n; xx++) if (b[xx] != c0) { t = 1; break; }
                }
                touched[y * w + x] = (u8)t;
                if (t) lo[x] = RD8(HR_COL + (u32)((y * n + n / 2) * W + x * n + n / 2));
                lo_end[y * w + x] = lo[x];
            }
            for (int yy = 0; yy < n; yy++)
                memcpy(hr_pix + (size_t)((win_y + y) * n + yy) * hr_w + win_x * n,
                       MEM + HR_COL + (u32)((y * n + yy) * W), W);
        }
        frame_time = GetTickCount();
        frames++;
    }
}

/* presenter: the N x layer over the window where the frame still shows the merged pixels */
int hires3d_frame(const u8 **pix, const u8 **mask, int *w, int *h)
{
    extern const u8 *hires3d_lo;
    extern int hires3d_lo_w, hires3d_lo_h;
    int n = hr_n;
    if (!hr_pix || n != n_cur || want_scale() != n || !frame_time || GetTickCount() - frame_time > 250) return 0;
    if (!hires3d_lo || hires3d_lo_w != 640 || hires3d_lo_h != 480) return 0;
    memset(hr_mask, 0, (size_t)hr_w * hr_h);
    int any = 0;
    for (int y = 0; y < win_h; y++) {
        const u8 *lo = hires3d_lo + (win_y + y) * 640 + win_x;
        for (int x = 0; x < win_w; x++) {
            int i = y * win_w + x;
            if (!touched[i] || lo[x] != lo_end[i]) continue;
            any = 1;
            for (int yy = 0; yy < n; yy++) memset(hr_mask + (size_t)((win_y + y) * n + yy) * hr_w + (win_x + x) * n, 1, n);
        }
    }
    if (!any) return 0;
    *pix = hr_pix; *mask = hr_mask; *w = hr_w; *h = hr_h;
    return 1;
}

/* <ENV>HRSHOT=ms: save the composite (N x layer over the frame, nearest
   upscaled) to shots/hr_NNNN.bmp, and the 1x frame next to it (testing) */
#include <stdio.h>
void hires_test_shot(void)
{
    static int ms = -1; static DWORD next; static unsigned k;
    if (ms < 0) { const char *e = getenv(ENV("HRSHOT")); ms = e ? atoi(e) : 0; }
    if (ms <= 0 || GetTickCount() < next) return;
    const u8 *pix, *mask; int w, h;
    if (!hires3d_frame(&pix, &mask, &w, &h)) return;
    extern const u8 *hires3d_lo;
    extern u8 vga_pal[768];
    next = GetTickCount() + ms;
    int n = w / 640;
    /* crop to the 3D window, N x and 1x side by side */
    int cw = win_w * n, ch = win_h * n, ow = cw * 2;
    char fn[64];
    CreateDirectoryA("shots", NULL);
    snprintf(fn, sizeof fn, "shots/hr_%04u.bmp", k++);
    FILE *f = fopen(fn, "wb");
    if (!f) return;
    int rs = (ow * 3 + 3) & ~3;
    u8 hd[54] = {'B', 'M'};
    *(u32 *)(hd + 2) = 54 + rs * ch; *(u32 *)(hd + 10) = 54; *(u32 *)(hd + 14) = 40;
    *(s32 *)(hd + 18) = ow; *(s32 *)(hd + 22) = ch; *(u16 *)(hd + 26) = 1; *(u16 *)(hd + 28) = 24;
    fwrite(hd, 1, 54, f);
    u8 *row = calloc(rs, 1);
    int sh = 2;   /* VGA palette: 6 bits per component */
    for (int y = ch - 1; y >= 0; y--) {
        for (int x = 0; x < ow; x++) {
            int xx = x % cw, X = win_x * n + xx, Y = win_y * n + y;
            u8 c = (x < cw && mask[(size_t)Y * w + X]) ? pix[(size_t)Y * w + X] : hires3d_lo[(Y / n) * 640 + X / n];
            row[x * 3] = vga_pal[c * 3 + 2] << sh; row[x * 3 + 1] = vga_pal[c * 3 + 1] << sh; row[x * 3 + 2] = vga_pal[c * 3] << sh;
        }
        fwrite(row, 1, rs, f);
    }
    free(row);
    fclose(f);
}
