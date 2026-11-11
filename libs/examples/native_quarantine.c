/* Quarantine natives that are not video (see src/rt/native_modex.c). */
#include <stdlib.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))

/* 447f14: compare the typed copy-protection answer (manual: "flesh ratio
   factor").  QU_TESTPW=1 lets scripted test runs through; players answer
   from their manual as on DOS. */
void N_pw_check(void)
{
    static int test = -1;
    if (test < 0) test = getenv(ENV("TESTPW")) != NULL;
    if (test) { cpu.r[R_EAX] = 1; RET(0); return; }
    rt_wrap_orig("N_pw_check", NULL)();
}

/* 413250: typed text entry (copy-protection answer, passwords, save names).
   It walks the key-down flags the keyboard IRQ sets (491458 + scancode),
   consuming every one - including Enter, which its caller checks just before.
   Recompiled, the loop's back-edges were interrupt points, so a key press
   landing inside it got eaten (Enter "did nothing" now and then).  Natively
   the loop runs without interrupt points, as short as it was on DOS.
   eax = 1: digits only, edx = last cursor position. */
void N_text_input(void)
{
    extern void play_effect(int);
    int mode = (s16)cpu.r[R_EAX], maxpos = (s16)cpu.r[R_EDX];
    int si = (s16)RD16(0x48e73e);
    for (int d = 0; d < 0x40; d++) {
        if (!RD8(0x491458 + d)) continue;
        WR8(0x491458 + d, 0);
        int ch = (s8)RD8(0x484150 + d);
        if (ch < 0) {                       /* backspace */
            if (si <= 0) continue;
            if (mode == 1) { si -= 1; WR16(0x48e73e, (u16)si); play_effect(5); si = (s16)RD16(0x48e73e); }
            else {
                WR8(0x48e710 + si, 0x20); si--;
                WR16(0x48e73e, (u16)si); play_effect(5); si = (s16)RD16(0x48e73e);
                WR8(0x48e710 + si, 0x20);
            }
        } else if (ch > 0 && (mode == 0 || (mode == 1 && ch >= '0' && ch <= '9'))) {
            WR8(0x48e710 + si, (u8)ch);
            WR16(0x48e73e, (u16)si); play_effect(5); si = (s16)RD16(0x48e73e);
            if (si <= maxpos) si++;
        }
    }
    WR16(0x48e73e, (u16)si);
    RET(0);
}

/* ---- the 3D view's texture span drawers ----
 * On DOS these four routines enter a fully unrolled loop at the code address
 * of the first pixel (tables of entry addresses per x / per y) and stop it by
 * writing a RET opcode over the instruction of the end pixel, restoring it
 * afterwards.  The same pixels, natively.  The 3D view buffer is in plane
 * order: pixel x of a row goes to (x & 3) * 16000 + (x >> 2), rows are 80
 * bytes apart. */
#define W16(a) ((u32)RD16(a))
static inline u32 tex_idx(u32 c) { return (((c >> 10) & 0x3f) << 6) | (c >> 26); }

/* 436134: floor/ceiling span, full detail */
void N_span_full(void)
{
    u32 x0 = RD32(0x488d74), x1 = RD32(0x488d78);
    u32 c = (W16(0x488d7c) << 16) | W16(0x488d80);
    u32 d = (W16(0x488d84) << 16) | W16(0x488d88);
    u32 tex = RD32(0x488d8c), dst = RD32(0x488d90), shade = RD32(0x493b20) & ~0xffu;
    u32 b = tex_idx(c);
    for (u32 x = x0; x < x1 && x < 320; x++) {
        u8 t = RD8(tex + b);
        b = tex_idx(c);
        u8 v = RD8(shade | t);
        c += d;
        WR8(dst + (x & 3) * 0x3e80 + (x >> 2), v);
    }
    RET(0);
}

/* 4361a6: the same at half horizontal detail (every other pixel, planes 0/2) */
void N_span_half(void)
{
    u32 x0 = RD32(0x488d74), x1 = RD32(0x488d78);
    u32 c = (W16(0x488d7c) << 16) | W16(0x488d80);
    u32 d = (W16(0x488d84) << 16) | W16(0x488d88);
    u32 tex = RD32(0x488d8c), dst = RD32(0x488d90), shade = RD32(0x493b20) & ~0xffu;
    u32 b = tex_idx(c);
    for (u32 x = x0; x < x1 && x < 320; x++) {
        if (x & 1) { c += d; continue; }
        u8 t = RD8(tex + b);
        b = tex_idx(c);
        u8 v = RD8(shade | t);
        c += d;
        WR8(dst + ((x >> 1) & 1) * 0x7d00 + (x >> 2), v);
    }
    RET(0);
}

/* 4397e0: wall column, opaque */
void N_col_opaque(void)
{
    u32 y0 = RD32(0x4897b0), y1 = RD32(0x4897b4);
    u32 v = RD32(0x4897b8), dv = RD32(0x4897bc);
    u32 tex = RD32(0x488d8c), dst = RD32(0x488d90), shade = RD32(0x493b20) & ~0xffu;
    u32 b = v >> 10;
    for (u32 y = y0; y < y1 && y < 200; y++) {
        u8 t = RD8(tex + b);
        v += dv;
        WR8(dst + y * 0x50, RD8(shade | t));
        b = v >> 10;
    }
    RET(0);
}

/* 439843: wall column with holes (texel 0 = transparent) */
void N_col_holes(void)
{
    u32 y0 = RD32(0x4897b0), y1 = RD32(0x4897b4);
    u32 v = RD32(0x4897b8), dv = RD32(0x4897bc);
    u32 tex = RD32(0x488d8c), dst = RD32(0x488d90), shade = RD32(0x493b20) & ~0xffu;
    u32 b = v >> 10;
    for (u32 y = y0; y < y1 && y < 200; y++) {
        u8 t = RD8(tex + b);
        if (t) WR8(dst + y * 0x50, RD8(shade | t));
        v += dv;
        b = v >> 10;
    }
    RET(0);
}
