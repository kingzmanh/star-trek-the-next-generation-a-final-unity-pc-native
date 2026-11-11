/* Quarantine's video layer, native.
 *
 * The DOS game runs in VGA "Mode X": 320x200, four planes, three pages that
 * are flipped by moving the display start, and a split screen that shows the
 * cab's dashboard (VRAM offset 0, 50 rows) under the 3D view.  Its video code
 * is a small asm module (linked into the data object at 48a059: mode set,
 * page flip, split screen, start/offset/panning), seven asm sprite blitters
 * (4232cc..423690), buffer->page copies (43478a..434dc8) and three outp()
 * wrappers (434768/434771/43477a) used by a few C routines.
 *
 * All of them are replaced here by PC code working on vga_planes[] (the
 * picture's pixels kept in plane order: pixel x of a row is plane x&3, byte
 * x>>2), and the presenter is handed the visible picture composed from the
 * display start, line width, panning and split row - no VGA registers.
 * The C routines that read/write the pages directly (save-under, map,
 * dashboard masks) are recompiled with window-aware accessors
 * (tools/vram_skipit.txt, VRD8/VWR8 in cpu.h) that use the plane selection
 * made through the native outp() wrappers. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))

u8 vga_planes[4][65536];
int vga_rd_plane;
u8 vga_wr_mask = 0x0f;
u8 *vga_capture;
u32 rt_hr_n = 1, rt_hr_k = 0, rt_hr_rowtab = 0x4a87e8u;   /* Quarantine: the floors' row table */

static int mx_on;               /* the game is in its Mode X */
static u32 mx_start = 0;        /* display start (plane byte offset) */
static u32 mx_stride = 80;      /* plane bytes per row (CRTC offset * 2) */
static int mx_pan;              /* horizontal pixel panning 0..3 */
static int mx_split = -1;       /* first row of the split (dashboard), -1 = off */
static u8 frame[320 * 200];

/* the asm module's variables (data object) */
#define V_PG_B      0x48a038    /* u16 plane offsets of the three pages */
#define V_PG_A      0x48a03a
#define V_PG_C      0x48a03c
#define V_ADDR_A    0x48a040    /* u32 linear addresses of the pages (A = drawing) */
#define V_ADDR_B    0x48a044
#define V_ADDR_C    0x48a048
#define V_ADDR_D    0x48a04c
#define V_SPLITROW  0x48a050    /* u16 rows above the dashboard (150) */
#define V_SPLITREM  0x48a052
#define V_SPLITON   0x48a054    /* u8 split screen on */

static inline int in_win(u32 a) { return a - 0xa0000u < 0x10000u; }

static void pw(u32 a, int plane, u8 v)
{
    if (in_win(a)) vga_planes[plane][a - 0xa0000u] = v;
    else {
        static int n;
        if (n++ < 10) rt_log("modex: plane write outside the window %08x\n", a);
        WR8(a, v);
    }
}

static u8 pr(u32 a, int plane)
{
    return in_win(a) ? vga_planes[plane][a - 0xa0000u] : RD8(a);
}

/* 16-bit address arithmetic as the asm does it (inc di / add di) */
static inline u32 a16(u32 a, u32 d) { return (a & 0xffff0000u) | ((a + d) & 0xffffu); }

static const u8 *mx_frame(void)
{
    if (!mx_on) return NULL;
    for (int y = 0; y < 200; y++) {
        u32 lin;
        int pan;
        if (mx_split >= 0 && y >= mx_split) { lin = (u32)(y - mx_split) * mx_stride * 4; pan = 0; }
        else { lin = (mx_start + (u32)y * mx_stride) * 4; pan = mx_pan; }
        lin += (u32)pan;
        u8 *d = frame + y * 320;
        for (int x = 0; x < 320; x++, lin++) d[x] = vga_planes[lin & 3][(lin >> 2) & 0xffff];
    }
    return frame;
}

/* the 320x200 picture as last presented */
const u8 *modex_picture(void) { return mx_on ? frame : NULL; }

/* for the high-resolution 3D layer: the page on screen and the split row */
int modex_display(u32 *start, int *split)
{
    *start = mx_start & 0xffff;
    *split = mx_split;
    return mx_on && mx_stride == 80 && mx_pan == 0;
}

void modex_reset(void)
{
    mx_on = 0; mx_start = 0; mx_stride = 80; mx_pan = 0; mx_split = -1;
    vga_rd_plane = 0; vga_wr_mask = 0x0f;
    memset(vga_planes, 0, sizeof vga_planes);
    extern const u8 *(*video_frame_hook)(void);
    video_frame_hook = mx_frame;
}

static void mode13(void)
{
    extern void video_set_mode(int, int, int);
    video_set_mode(320, 200, 8);
    mx_on = 1; mx_stride = 80; mx_pan = 0; mx_split = -1;
    extern void rt_vguard_arm(void);
    rt_vguard_arm();
}

/* 48a181: clear the three pages */
static void clear_pages(void)
{
    memset(vga_planes, 0, sizeof vga_planes);
}

/* 48a059: mode 13h, unchain (Mode X), clear, display start 0FA0h */
void N_mx_init(void)
{
    mode13();
    clear_pages();
    mx_start = 0xfa0;
    RET(0);
}

/* 48a0ec: the same without clearing */
void N_mx_init_keep(void)
{
    mode13();
    mx_start = 0xfa0;
    RET(0);
}

void N_mx_clear(void) { clear_pages(); RET(0); }

/* 48a1fc / 48a213: wait for vertical retrace - show the picture and keep
   the 70 Hz pace of the VGA */
void N_mx_vsync(void)
{
    static u64 next;
    u64 now = SDL_GetPerformanceCounter(), f = SDL_GetPerformanceFrequency();
    u64 per = f / 70;
    extern void video_present(int);
    video_present(1);
    if (next > now && next - now < per * 2) {
        while ((now = SDL_GetPerformanceCounter()) < next) {
            u64 left = (next - now) * 1000 / f;
            if (left > 1) SDL_Delay((Uint32)(left - 1));
        }
        next += per;
    } else next = now + per;
    irq_poll();
    RET(0);
}

/* Frame cap (Video menu / ini FrameLimit; 0 = off).  The wait goes where a
   DOS frame spent its time: in the frame copy into video memory when the
   frame has one (menus: the loop is "any key?" -> copy -> draw -> handle
   keys -> flip, and a key IRQ delivered at the flip would be eaten by the
   next "any key?"), otherwise at the flip.  Interrupts are delivered while
   waiting, as they arrived on DOS. */
extern int opt_fps_cap;
static int cap_pending;     /* a flip happened, its wait not done yet */
static u64 cap_next;

static void cap_wait(void)
{
    int cap = opt_fps_cap;
    { const char *e = getenv(ENV("FPSCAP")); if (e) cap = atoi(e); }
    if (cap <= 0) return;
    u64 f = SDL_GetPerformanceFrequency(), per = f / (u64)cap, now = SDL_GetPerformanceCounter();
    if (cap_next > now && cap_next - now <= per * 2) {
        while ((now = SDL_GetPerformanceCounter()) < cap_next) {
            u64 ms = (cap_next - now) * 1000 / f;
            if (ms > 1) SDL_Delay((Uint32)(ms - 1));
            irq_poll();
        }
        cap_next += per;
    } else cap_next = now + per;
}

/* at_flip = 1 from the flip, 0 from a frame copy */
static void frame_cap(int at_flip)
{
    if (at_flip) {
        if (cap_pending) cap_wait();    /* last frame had no copy: wait here */
        cap_pending = 1;
    } else if (cap_pending) {
        cap_pending = 0;
        cap_wait();
    }
}

/* 48a222: rotate the three pages, show the one just drawn */
void N_mx_flip(void)
{
    u32 a = RD32(V_ADDR_A), b = RD32(V_ADDR_B), c = RD32(V_ADDR_C);
    WR32(V_ADDR_A, b); WR32(V_ADDR_B, c); WR32(V_ADDR_C, a);
    u16 pa = RD16(V_PG_A), pc = RD16(V_PG_C), pb = RD16(V_PG_B);
    WR16(V_PG_A, pb); WR16(V_PG_C, pa); WR16(V_PG_B, pc);
    mx_start = pa;
    extern void video_present(int);
    video_present(0);
    frame_cap(1);
    RET(0);
}

/* 48a285: show page D */
void N_mx_show_d(void) { mx_start = RD32(V_ADDR_D) & 0xffff; RET(0); }

/* 48a2a3: make room for the dashboard (pages move down 50 rows) and split */
void N_mx_split_setup(void)
{
    if (RD8(V_SPLITON)) { RET(0); return; }
    WR8(V_SPLITON, 1);
    WR32(V_ADDR_A, RD32(V_ADDR_A) + 0xfa0);
    WR32(V_ADDR_B, RD32(V_ADDR_B) + 0xfa0);
    WR32(V_ADDR_C, RD32(V_ADDR_C) + 0xfa0);
    WR16(V_PG_B, RD16(V_PG_B) + 0xfa0);
    WR16(V_PG_A, RD16(V_PG_A) + 0xfa0);
    WR16(V_PG_C, RD16(V_PG_C) + 0xfa0);
    mx_split = RD16(V_SPLITROW);
    WR16(V_SPLITREM, (u16)(200 - RD16(V_SPLITROW)));
    RET(0);
}

/* 48a3ea / 48a449: dashboard split off / on */
void N_mx_split_off(void)
{
    if (RD8(V_SPLITON)) { WR8(V_SPLITON, 0); mx_split = -1; }
    RET(0);
}

void N_mx_split_on(void)
{
    if (!RD8(V_SPLITON)) { WR8(V_SPLITON, 1); mx_split = RD16(V_SPLITROW); }
    RET(0);
}

/* 48a4ab: line width in pixels (eax) */
void N_mx_set_width(void) { mx_stride = ((cpu.r[R_EAX] & 0xffff) >> 3) * 2; if (!mx_stride) mx_stride = 80; RET(0); }

/* 48a4be: display start (ax) */
void N_mx_set_start(void) { mx_start = cpu.r[R_EAX] & 0xffff; RET(0); }

/* 48a4d7: horizontal pixel panning */
void N_mx_pan(void) { mx_pan = (int)(cpu.r[R_EAX] & 3); RET(0); }

/* ---- the game's outp() wrappers: plane selection for its C routines ---- */
static int seq_idx = 2;
void N_mx_seq_index(void) { seq_idx = cpu.r[R_EAX] & 0xff; RET(0); }          /* 434768 */
void N_mx_seq_data(void) { if (seq_idx == 2) vga_wr_mask = cpu.r[R_EAX] & 0x0f; RET(0); }  /* 434771 */
void N_mx_read_plane(void) { vga_rd_plane = cpu.r[R_EAX] & 3; RET(0); }       /* 43477a */

/* ---- asm sprite blitters ----
   eax = x, edx = (frame << 8 | y) (or frame << 10 | y for the 640-wide
   ones), ebx = page address, ecx = sprite bank.  The bank holds 4-byte
   entries: +1 width, +2 height, +3 u16 offset of the data, which is
   stored plane by plane (rows of width/4 bytes). */
enum { B_OPAQUE, B_TRANSP, B_SHADOW };

static void blit(int shift, u32 ymask, u32 stride, int mode, u32 base, int three)
{
    u32 x = cpu.r[R_EAX], d = cpu.r[R_EDX], bank = cpu.r[R_ECX];
    u32 e = bank + ((d >> shift) & 0xfffc) + 1;
    u32 wb = RD8(e) >> 2, h = RD8(e + 1);
    u32 src = bank + RD16(e + 2);
    u32 dst = (d & ymask) * stride + (base ? base : cpu.r[R_EBX]) + (x >> 2);
    u8 mask = (u8)(0x11 << (x & 3));
    for (int p = 0; p < 4; p++) {
        int plane = (mask & 1) ? 0 : (mask & 2) ? 1 : (mask & 4) ? 2 : 3;
        u32 di = dst;
        for (u32 r = 0; r < h; r++) {
            for (u32 c = 0; c < wb; c++) {
                u8 v = RD8(src++);
                if (mode == B_OPAQUE) {
                    pw(di, plane, v);
                    if (three) { pw(di + 0x3e80, plane, v); pw(di + 0x7d00, plane, v); }
                    di++;    /* rep movs: 32-bit */
                    continue;
                }
                if (mode == B_TRANSP) {
                    if (v) {
                        pw(di, plane, v);
                        if (three) { pw(di + 0x3e80, plane, v); pw(di + 0x7d00, plane, v); }
                    }
                } else {   /* shadow: bit 7 of the page pixel follows the sprite's shape */
                    u8 o = pr(di, plane);
                    pw(di, plane, v ? (u8)(o | 0x80) : (u8)(o & 0x7f));
                }
                di = a16(di, 1);
            }
            di = a16(di, stride - wb);
        }
        int carry = (mask & 0x80) != 0;
        mask = (u8)((mask << 1) | carry);
        if (carry) dst = a16(dst, 1);
    }
    RET(0);
}

void N_blit_t80(void)   { blit(6, 0xff, 0x50, B_TRANSP, 0, 0); }        /* 4232cc */
void N_blit_t160(void)  { blit(8, 0x3ff, 0xa0, B_TRANSP, 0, 0); }       /* 423361 */
void N_blit_sh160(void) { blit(8, 0x3ff, 0xa0, B_SHADOW, 0, 0); }       /* 4233f9 */
void N_blit_o80(void)   { blit(6, 0xff, 0x50, B_OPAQUE, 0, 0); }        /* 4234bc */
void N_blit_o160(void)  { blit(8, 0x3ff, 0xa0, B_OPAQUE, 0, 0); }       /* 423552 */
void N_blit_t3(void)    { blit(6, 0xff, 0x50, B_TRANSP, 0xa0fa0, 1); }  /* 4235eb: all three pages */
void N_blit_o3(void)    { blit(6, 0xff, 0x50, B_OPAQUE, 0xa0fa0, 1); }  /* 423690: all three pages */

/* ---- linear buffer -> planar page copies ---- */
static void deinterleave(u32 src, u32 dst, u32 n)
{
    for (int p = 0; p < 4; p++)
        for (u32 i = 0; i < n; i++) pw(dst + i, p, RD8(src + p + i * 4));
}

/* 43478a: dashboard picture (rows 150..199 of the buffer in eax) -> VRAM 0 */
void N_cp_dash(void) { deinterleave(cpu.r[R_EAX] + 48000, 0xa0000, 4000); RET(0); }

/* 43481e: the frame buffer -> all three pages */
void N_cp_all(void)
{
    u32 s = RD32(0x493370);
    frame_cap(0);
    deinterleave(s, RD32(V_ADDR_A), 16000);
    deinterleave(s, RD32(V_ADDR_B), 16000);
    deinterleave(s, RD32(V_ADDR_C), 16000);
    RET(0);
}

/* The frame copy into video memory was most of a DOS frame's time, so that
   is where the keyboard / timer IRQs mostly arrived - after the loop's
   "any key" check and before its key handling.  Deliver pending ones here. */
void N_cp_page(void)  { frame_cap(0); deinterleave(cpu.r[R_EAX], RD32(V_ADDR_A), 16000); RET(0); }   /* 434b7a, 434c8c */
void N_cp_page150(void) { frame_cap(0); deinterleave(cpu.r[R_EAX], RD32(V_ADDR_A), 12000); RET(0); } /* 434c03 */

/* 434da8..434dc0: 320x200 buffer -> one half of a 640-wide screen */
static void cp640(u32 dst)
{
    u32 src = cpu.r[R_EAX];
    for (int p = 0; p < 4; p++) {
        u32 s = src + p, d = dst;
        for (int y = 0; y < 200; y++, d += 0xa0)
            for (int x = 0; x < 80; x++, s += 4) pw(d + x, p, RD8(s));
    }
    RET(0);
}
void N_cp640_a(void) { cp640(0xa0000); }
void N_cp640_b(void) { cp640(0xa0050); }
void N_cp640_c(void) { cp640(0xa7d00); }
void N_cp640_d(void) { cp640(0xa7d50); }
