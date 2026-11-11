/* debugging natives (test runs only) */
#include "rt.h"

/* 41b126: window bitmap -> back buffer (cdecl bitmap*, rect*): log each new
   bitmap (address + geometry); header: +4 x, +6 y, +8 w, +10 h, +0xc pixels */
void N_dbg_blit(void)
{
    static u32 seen[256][2]; static int n;
    u32 b = RD32(cpu.r[R_ESP] + 4);
    u32 g = RD32(b + 4) ^ (RD32(b + 8) << 1);
    int k;
    for (k = 0; k < n && !(seen[k][0] == b && seen[k][1] == g); k++) ;
    if (k == n && n < 256) {
        seen[n][0] = b; seen[n][1] = g; n++;
        rt_log("blit: bitmap %08x at %d,%d size %dx%d\n", b, (s16)RD16(b + 4), (s16)RD16(b + 6), (s16)RD16(b + 8), (s16)RD16(b + 10));
    }
    rt_wrap_orig("N_dbg_blit", NULL)();
}

/* 48f7cc: camera update - ST_CAMLOG=1 logs mode, subject ship, its target and the result per frame */
#include <stdlib.h>
#include <string.h>
static float f32(u32 a) { float f; u32 v = RD32(a); memcpy(&f, &v, 4); return f; }
void N_dbg_camera(void)
{
    rt_wrap_orig("N_dbg_camera", NULL)();
    static int on = -1, n;
    if (on < 0) on = getenv(ENV("CAMLOG")) != NULL;
    if (!on || n > 4000) return;
    u32 cam = RD32(0x611344);
    if (!cam) return;
    u32 mode = RD32(cam + 0x28), fn = mode ? RD32(mode) : 0, subj = mode ? RD32(mode + 4) : 0;
    n++;
    rt_log("cam t=%u fn=%08x ang=%u,%u,%u pos=%.1f,%.1f,%.1f", RD32(0x559554), fn, RD32(cam + 4), RD32(cam + 8), RD32(cam + 0xc),
           f32(cam + 0x1c), f32(cam + 0x20), f32(cam + 0x24));
    if (subj && subj < 0x10000000) {
        rt_log(" subj=%08x p=%.2f,%.2f,%.2f a=%u,%u,%u", subj, f32(subj + 300), f32(subj + 0x130), f32(subj + 0x134),
               RD32(subj + 0x150), RD32(subj + 0x154), RD32(subj + 0x158));
        u32 tg = RD32(subj + 0x548);
        if (tg && tg < 0x10000000) rt_log(" tgt=%08x p=%.2f,%.2f,%.2f", tg, f32(tg + 300), f32(tg + 0x130), f32(tg + 0x134));
    }
    rt_log("\n");
    {   /* every ship in the scene: index, address, orientation */
        u32 lst = RD32(0x611380);
        if (lst && lst < 0x10000000) {
            u32 cnt = RD32(lst);
            rt_log("ships t=%u", RD32(0x559554));
            for (u32 i = 0; i < cnt && i < 8; i++) {
                u32 s = RD32(lst + 16 + i * 4);
                if (s && s < 0x10000000) rt_log(" %u:%08x:%u,%u,%u", i, s, RD32(s + 0x150), RD32(s + 0x154), RD32(s + 0x158));
            }
            rt_log("\n");
        }
    }
}
