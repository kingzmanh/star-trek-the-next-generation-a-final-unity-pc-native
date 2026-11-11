/* Native replacements for game/library functions that talk to PC hardware.
 * Each one stands in for the whole original function (listed in
 * tools/native.txt) and works on the guest register file like the original:
 * arguments in registers (Watcom register convention), result in eax, and
 * it pops its own return address. */
#include <stdlib.h>
#include "rt.h"

#define RET(n) (cpu.r[R_ESP] += 4 + (n))

/* 45e280: program the keyboard typematic rate (al = delay, bl = rate).
   Windows owns key repeat; keep the values the game stores. */
void N_kbd_typematic(void)
{
    u8 ah = cpu.r[R_EAX] & 3;
    u8 bl = cpu.r[R_EBX] & 0x1f;
    WR8(0x4dd499, ah);
    WR8(0x4dd49a, bl);
    cpu.r[R_EAX] = (cpu.r[R_EAX] & 0xffff0000u) | ((u32)(ah | bl) << 8) | 0xfa;
    cpu.r[R_EBX] = (cpu.r[R_EBX] & 0xffffff00u) | bl;
    RET(0);
}

/* 46f6a0: Watcom _chain_intr(far handler in dx:eax).  Called from inside an
   interrupt handler whose frame starts at ebp; the original rewinds to that
   frame and retf's into the previous handler.  Here the previous handler is
   run first and control returns to the caller, whose epilogue (pop segs,
   popad, iretd) then unwinds the same frame. */
extern void irq_magic(u32 n);
extern void irq_call_handler(u32 h);
void N_chain_intr(void)
{
    u32 target = cpu.r[R_EAX];
    cpu.r[R_ESP] = cpu.r[R_EBP];
    if (target >= MAGIC_IRET && target < MAGIC_IRET + 0x100) irq_magic(target - MAGIC_IRET);
    else irq_call_handler(target);
    cpu.r[R_ESP] = cpu.r[R_EBP];
}

/* 473b02: Watcom graph library line (already clipped, physical coords):
   eax=x1 edx=y1 ebx=x2 ecx=y2, stack: colour, line-style mask; ret 8.
   The original assembles its inner loop on the stack; this draws the same
   Bresenham line directly. */
extern void video_put_pixel(int x, int y, u8 c, int op);
void N_gr_line(void)
{
    int x1 = (s16)cpu.r[R_EAX], y1 = (s16)cpu.r[R_EDX];
    int x2 = (s16)cpu.r[R_EBX], y2 = (s16)cpu.r[R_ECX];
    u8 color = (u8)RD32(cpu.r[R_ESP] + 4);
    u32 style = RD32(cpu.r[R_ESP] + 8) & 0xffff;
    int op = (s16)RD16(0x4de29f);
    if (getenv("ALIENS_LINELOG")) {
        static u32 n;
        if ((n++ % 200) < 8) rt_log("line %d,%d-%d,%d col %u style %04x op %d bank %d\n", x1, y1, x2, y2, color, style, op, video_bank_cur);
    }
    int dx = abs(x2 - x1), dy = abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        if (style & 0x8000) video_put_pixel(x1, y1, color, op);
        style = ((style << 1) | (style >> 15)) & 0xffff;
        if (x1 == x2 && y1 == y2) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x1 += sx; }
        if (e2 < dx) { err += dx; y1 += sy; }
    }
    RET(8);
}
