/* Quarantine's 3D view at 2x / 4x resolution (Video > Internal Resolution).
 *
 * The game draws its 3D view in software at 320x200 (wall columns, floor
 * spans, scaled sprites, a sky panorama) into a planar buffer that it then
 * copies through the dashboard's window into video memory.  Here the frame
 * renderer (440e0c) runs a second time, right after the game's own pass,
 * with:
 *   - every game variable as it was before the game's pass (the data object
 *     is saved and restored around it, so animation, AI and timers see one
 *     pass), interrupts held;
 *   - the projection, floor maths, wall clip and sprite sizes scaled by N
 *     (constants listed in tools/subst_skipit.txt -> rt_hr_n / rt_hr_k), the
 *     clip window scaled by N and the floors' per-row table rebuilt for N;
 *   - output to a scratch buffer: the native span / column drawers
 *     (quarantine.c) work out x / y from the planar addresses the game
 *     computes and write the N x picture (hr_pix), the sky is drawn natively.
 * The presenter shows hr_pix where the game copied 3D pixels into the page
 * it displays (captured while it copies), the dashboard stays 320x200. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))

#define DATA_LO   0x480000u     /* SKIPIT's data object */
#define DATA_HI   0x4ac748u
#define HR_ROWTAB 0x20000000u   /* guest scratch: floor row table for N */
#define HR_GUEST  0x20100000u   /* guest scratch: the pass's "3D buffer" */
#define G_BUF     0x493370u     /* -> the game's 3D buffer */
extern int rt_irq_hold;

u8 *hr_pix;                     /* N x picture of the 3D buffer (palette indices) */
int hr_w, hr_h, hr_n = 1;
static u8 *snap0, *snap1;
static u8 *cap;                 /* written plane bytes while the game copies its 3D view */
static u32 cap_page = 0xffffffffu;
static u32 frame_no, cap_frame, hr_frame;
static u8 *hr_mask;

static int want_scale(void)
{
    extern int opt_scale;
    return opt_scale >= 4 ? 4 : opt_scale >= 2 ? 2 : 1;
}

int hires3d_scales(void) { return (1 << 1) | (1 << 2) | (1 << 4); }   /* menu: 1x 2x 4x */

static void scale_window(int n)
{
    static const u32 w[] = {0x488cbe, 0x488cc0, 0x488cc2, 0x488cc4};
    for (int i = 0; i < 4; i++) WR16(w[i], (u16)((s16)RD16(w[i]) * n));
    for (int v = 0; v < 4; v++) {       /* per-view windows (438f80 reloads them) */
        static const u32 t[] = {0x49d4b0, 0x49d4c8, 0x49d4c0, 0x49d4b8};
        for (int i = 0; i < 4; i++) WR16(t[i] + v * 2, (u16)((s16)RD16(t[i] + v * 2) * n));
    }
}

static void build_rowtab(int n)
{
    /* 443ff0: tab[y] ~ y - 100 below the horizon, tab[201 - y] = -tab[y] */
    int H = 200 * n, c = 100 * n;
    for (int y = 0; y <= H + 1; y++) {
        s32 v = y > c ? y - c : y - c - 1;
        WR32(HR_ROWTAB + y * 4, (u32)v);
    }
}

/* 440e0c (wrapped): the 3D frame */
void N_render3d(void)
{
    void (*orig)(void) = rt_wrap_orig("N_render3d", NULL);
    int n = want_scale();
    if (n <= 1) { orig(); return; }
    if (!snap0) { snap0 = malloc(DATA_HI - DATA_LO); snap1 = malloc(DATA_HI - DATA_LO); }
    u32 pre[8];
    memcpy(pre, cpu.r, sizeof pre);
    u32 ret = RD32(cpu.r[R_ESP]);
    memcpy(snap0, MEM + DATA_LO, DATA_HI - DATA_LO);
    orig();                                         /* the game's own pass */
    u32 post[8];
    memcpy(post, cpu.r, sizeof post);
    memcpy(snap1, MEM + DATA_LO, DATA_HI - DATA_LO);

    if (hr_n != n || !hr_pix) {
        hr_n = n; hr_w = 320 * n; hr_h = 200 * n;
        free(hr_pix); hr_pix = calloc((size_t)hr_w * hr_h, 1);
        free(hr_mask); hr_mask = calloc((size_t)hr_w * hr_h, 1);
    }
    memcpy(MEM + DATA_LO, snap0, DATA_HI - DATA_LO);  /* as before the game's pass */
    memcpy(cpu.r, pre, sizeof pre);
    WR32(cpu.r[R_ESP], ret);
    rt_hr_n = (u32)n; rt_hr_k = n == 4 ? 2 : 1; rt_hr_rowtab = HR_ROWTAB;
    build_rowtab(n);
    scale_window(n);
    WR32(G_BUF, HR_GUEST);
    rt_irq_hold++;
    orig();                                         /* the N x pass */
    rt_irq_hold--;
    rt_hr_n = 1; rt_hr_k = 0; rt_hr_rowtab = 0x4a87e8u;
    memcpy(MEM + DATA_LO, snap1, DATA_HI - DATA_LO);  /* back to after the game's pass */
    memcpy(cpu.r, post, sizeof post);
    hr_frame = ++frame_no;
    {   /* QU_HRSHOT=<ms>: save the N x layer to shots/hr_NNNN.bmp (testing) */
        static int ms = -1; static u32 next, k;
        if (ms < 0) { const char *e = getenv(ENV("HRSHOT")); ms = e ? atoi(e) : 0; }
        if (ms > 0 && GetTickCount() >= next) {
            extern u8 vga_pal[768];
            char fn[64];
            next = GetTickCount() + ms;
            CreateDirectoryA("shots", NULL);
            snprintf(fn, sizeof fn, "shots/hr_%04u.bmp", k++);
            FILE *f = fopen(fn, "wb");
            if (f) {
                int rs = (hr_w * 3 + 3) & ~3;
                u32 sz = 54 + (u32)rs * hr_h;
                u8 hd[54] = {'B', 'M'};
                memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
                memcpy(hd + 18, &hr_w, 4); memcpy(hd + 22, &hr_h, 4);
                hd[26] = 1; hd[28] = 24;
                fwrite(hd, 1, 54, f);
                u8 *row = calloc(rs, 1);
                for (int y = hr_h - 1; y >= 0; y--) {
                    for (int x = 0; x < hr_w; x++) {
                        const u8 *c = vga_pal + hr_pix[y * hr_w + x] * 3;
                        row[x * 3] = (u8)(c[2] << 2); row[x * 3 + 1] = (u8)(c[1] << 2); row[x * 3 + 2] = (u8)(c[0] << 2);
                    }
                    fwrite(row, 1, rs, f);
                }
                free(row);
                fclose(f);
            }
            /* the game's own 320x200 3D buffer of the same frame (planar) */
            snprintf(fn, sizeof fn, "shots/lo_%04u.bmp", k - 1);
            f = fopen(fn, "wb");
            if (f) {
                int w = 320, h = 200, rs = w * 3;
                u32 sz = 54 + (u32)rs * h, buf = RD32(G_BUF);
                u8 hd[54] = {'B', 'M'};
                memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
                memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4);
                hd[26] = 1; hd[28] = 24;
                fwrite(hd, 1, 54, f);
                u8 row[960];
                for (int y = h - 1; y >= 0; y--) {
                    for (int x = 0; x < w; x++) {
                        const u8 *c = vga_pal + RD8(buf + (x & 3) * 16000 + y * 80 + (x >> 2)) * 3;
                        row[x * 3] = (u8)(c[2] << 2); row[x * 3 + 1] = (u8)(c[1] << 2); row[x * 3 + 2] = (u8)(c[0] << 2);
                    }
                    fwrite(row, 1, rs, f);
                }
                fclose(f);
            }
        }
    }
}

/* 43932c (wrapped): the sky panorama (eax = 1: half detail, edx = buffer).
   N x: drawn natively, the panorama picture scaled up. */
void N_sky(void)
{
    {   static int nl;
        if (getenv(ENV("SKYLOG")) && nl++ < 6)
            rt_log("sky n=%u eax=%x edx=%08x ymin=%d ymax=%d 493488=%08x level=%d on=%d ang=%08x\n", rt_hr_n, cpu.r[R_EAX], cpu.r[R_EDX],
                   (s16)RD16(0x488cbe), (s16)RD16(0x488cc0), RD32(0x493488), (s16)RD16(0x49391a), RD8(0x4897aa + (s16)RD16(0x49391a)), RD32(0x49387a));
    }
    if (rt_hr_n <= 1) { rt_wrap_orig("N_sky", NULL)(); return; }
    int n = (int)rt_hr_n;
    int ymin = (s16)RD16(0x488cbe), ymax = (s16)RD16(0x488cc0);   /* already N x */
    u8 color = RD8(0x493953);
    int level = (s16)RD16(0x49391a);
    if (ymin < 0) ymin = 0;
    if (ymax > hr_h) ymax = hr_h;
    if (!RD8(0x4897aa + level)) {
        for (int y = ymin; y < ymax; y++) memset(hr_pix + y * hr_w, color, hr_w);
        RET(0);
        return;
    }
    u32 sky = RD32(0x493358);
    int sh = (s32)RD32(0x493488) >> 2;
    if (sh < 0) sh = 0;
    int ang = ((s32)RD32(0x49387a) >> 16) & 0xfff0;
    int s = (6000 - ang / 18) % 80;
    if (s < 0) s += 80;
    int ymin1 = ymin / n;
    int horizon = 100 * n;
    for (int y = ymin; y < ymax; y++) {
        u8 *d = hr_pix + y * hr_w;
        int i = y / n - ymin1;
        if (y < horizon && i < (100 - ymin1) - sh) {
            u32 row = sky + (u32)(ymin1 + sh + i) * 80;
            for (int x = 0; x < hr_w; x++) {
                int xs = (x / n + 4 * s) % 320;
                d[x] = RD8(row + (xs & 3) * 8000 + (xs >> 2));   /* the panorama: 100 rows, planes 8000 apart */
            }
        } else memset(d, color, hr_w);
    }
    RET(0);
}

/* 4390f0 / 4391f4 (wrapped): the 3D buffer through the dashboard window into
   the page being drawn - note which page bytes are 3D */
static void vcopy(const char *name)
{
    if (!cap) cap = malloc(4 * 65536);
    memset(cap, 0, 4 * 65536);
    vga_capture = cap;
    rt_wrap_orig(name, NULL)();
    vga_capture = NULL;
    cap_page = RD32(0x48a040) - 0xa0000u;
    cap_frame = frame_no;
    if (getenv(ENV("CAPLOG"))) {
        static int nl;
        int c = 0;
        for (int i = 0; i < 4 * 65536; i++) c += cap[i];
        if (nl++ < 8) rt_log("vcopy %s: %d bytes, page %05x, frame %u\n", name, c, cap_page, frame_no);
    }
}
void N_vcopy_full(void) { vcopy("N_vcopy_full"); }
void N_vcopy_half(void) { vcopy("N_vcopy_half"); }

/* presenter: the N x layer and where it shows (video.c) */
int hires3d_frame(const u8 **pix, const u8 **mask, int *w, int *h)
{
    extern int modex_display(u32 *start, int *split);
    u32 start;
    int split;
    if (want_scale() <= 1 || !hr_pix || hr_n != want_scale()) return 0;
    if (!modex_display(&start, &split)) return 0;
    if (cap_frame != hr_frame || start != (cap_page & 0xffff)) return 0;
    int rows = split >= 0 ? split : 200;
    int n = hr_n;
    memset(hr_mask, 0, (size_t)hr_w * hr_h);
    for (int y = 0; y < rows; y++) {
        u8 m[320];
        int any = 0;
        for (int x = 0; x < 320; x++) {
            u32 o = (start + (u32)y * 80 + (x >> 2)) & 0xffff;
            m[x] = cap[(x & 3) * 65536 + o];
            any |= m[x];
        }
        if (!any) continue;
        for (int yy = 0; yy < n; yy++) {
            u8 *d = hr_mask + (y * n + yy) * hr_w;
            for (int x = 0; x < 320; x++) if (m[x]) memset(d + x * n, 1, n);
        }
    }
    *pix = hr_pix; *mask = hr_mask; *w = hr_w; *h = hr_h;
    {   /* QU_HRCOMP=<ms>: save the composite (N x 3D in the window, the rest
           the 320x200 picture) to shots/comp_NNNN.bmp (testing) */
        static int ms = -1; static u32 next, k;
        if (ms < 0) { const char *e = getenv(ENV("HRCOMP")); ms = e ? atoi(e) : 0; }
        if (ms > 0 && GetTickCount() >= next) {
            extern u8 vga_pal[768];
            extern const u8 *modex_picture(void);
            const u8 *lo = modex_picture();
            char fn[64];
            next = GetTickCount() + ms;
            CreateDirectoryA("shots", NULL);
            snprintf(fn, sizeof fn, "shots/comp_%04u.bmp", k++);
            FILE *f = fopen(fn, "wb");
            if (f && lo) {
                int rs = hr_w * 3;
                u32 sz = 54 + (u32)rs * hr_h;
                u8 hd[54] = {'B', 'M'};
                memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
                memcpy(hd + 18, &hr_w, 4); memcpy(hd + 22, &hr_h, 4);
                hd[26] = 1; hd[28] = 24;
                fwrite(hd, 1, 54, f);
                u8 *row = malloc(rs);
                for (int y = hr_h - 1; y >= 0; y--) {
                    for (int x = 0; x < hr_w; x++) {
                        u8 i = hr_mask[y * hr_w + x] ? hr_pix[y * hr_w + x] : lo[(y / n) * 320 + x / n];
                        const u8 *c = vga_pal + i * 3;
                        row[x * 3] = (u8)(c[2] << 2); row[x * 3 + 1] = (u8)(c[1] << 2); row[x * 3 + 2] = (u8)(c[0] << 2);
                    }
                    fwrite(row, 1, rs, f);
                }
                free(row);
            }
            if (f) fclose(f);
        }
    }
    return 1;
}
