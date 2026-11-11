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
