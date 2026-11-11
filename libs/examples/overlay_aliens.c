/* On-screen messages from the PC side (option toggles), drawn over the
 * presented picture in the game's own menu font: MS Serif 12 pt from the
 * game's SERIFF.FON (Windows 3.x bitmap font, the font the menus use). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "rt.h"

typedef struct { int w; u32 off; } Glyph;
static u8 *fnt;              /* the FONT resource */
static int fh, first, last;
static Glyph gl[256];
static char msg[64];
static u64 msg_until;

static u16 g16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 g32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* pick the font resource with the wanted point size out of an NE .FON */
static int load_fon(const char *path, int want_pt)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc(n);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    u32 ne = g32(d + 0x3c);
    if (ne + 0x40 > (u32)n || d[ne] != 'N' || d[ne + 1] != 'E') { free(d); return 0; }
    u32 rt = ne + g16(d + ne + 0x24);
    int shift = g16(d + rt);
    u32 p = rt + 2;
    while (p + 8 <= (u32)n && g16(d + p)) {
        u16 type = g16(d + p), cnt = g16(d + p + 2);
        p += 8;
        for (int i = 0; i < cnt; i++, p += 12) {
            if (type != 0x8008) continue;
            u32 off = (u32)g16(d + p) << shift, len = (u32)g16(d + p + 2) << shift;
            if (off + len > (u32)n || len < 0x80) continue;
            const u8 *r = d + off;
            if (g16(r) != 0x200 || g16(r + 0x44) != want_pt) continue;
            fnt = malloc(len);
            memcpy(fnt, r, len);
            fh = g16(fnt + 0x58);
            first = fnt[0x5f];
            last = fnt[0x60];
            for (int c = first; c <= last; c++) {
                const u8 *e = fnt + 0x76 + (c - first) * 4;
                gl[c].w = g16(e);
                gl[c].off = g16(e + 2);
            }
            free(d);
            return 1;
        }
    }
    free(d);
    return 0;
}

void overlay_init(void)
{
    char p[700];
    snprintf(p, sizeof p, "%s\\hd\\ALIENS\\SERIFF.FON", rt_root);
    if (!load_fon(p, 12)) rt_log("overlay: SERIFF.FON not available\n");
}

void overlay_message(const char *text, int ms)
{
    snprintf(msg, sizeof msg, "%s", text);
    msg_until = SDL_GetTicks64() + ms;
    rt_log("overlay: %s\n", text);
}

static void glyph(u32 *pix, int pitch, int W, int H, int x, int y, int c, u32 col)
{
    if (c < first || c > last) return;
    int w = gl[c].w;
    for (int yy = 0; yy < fh; yy++) {
        int py = y + yy;
        if (py < 0 || py >= H) continue;
        for (int xx = 0; xx < w; xx++) {
            u8 b = fnt[gl[c].off + (xx / 8) * fh + yy];
            if (!(b & (0x80 >> (xx & 7)))) continue;
            int px = x + xx;
            if (px >= 0 && px < W) pix[py * (pitch / 4) + px] = col;
        }
    }
}

static void text(u32 *pix, int pitch, int W, int H, int x, int y, const char *s, u32 col)
{
    for (; *s; s++) {
        int c = (u8)*s;
        glyph(pix, pitch, W, H, x, y, c, col);
        x += (c >= first && c <= last) ? gl[c].w : 0;
    }
}

/* draw into the frame about to be shown (ARGB, game resolution) */
void overlay_draw(u32 *pix, int pitch, int W, int H)
{
    if (!msg[0] || !fnt) return;
    u64 now = SDL_GetTicks64();
    if (now >= msg_until) { msg[0] = 0; return; }
    /* black outline + white text, like the menu's lettering on its dark panels */
    int x = 8, y = 4;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) text(pix, pitch, W, H, x + dx, y + dy, msg, 0xff000000u);
    text(pix, pitch, W, H, x, y, msg, 0xffffffffu);
}
