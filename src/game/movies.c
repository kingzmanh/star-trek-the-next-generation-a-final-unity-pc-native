/* Full-size, full-colour movies.  At 640x480 (Video=HI8) the game plays its
 * 320x200 FVF movies in the middle of the screen (or pixel-doubled with
 * DPixel=ON), reduced to the 256-colour palette.  Its movie player also has
 * the 320x200 layouts of the low-res video modes: LO8 (mode 13h), LO15 (VESA
 * 10d) and LO16 (VESA 10e, 65536 colours, no palette banding).  41a1c6 (play
 * a movie) picks the layout from the video setting 502ab8 and sets the mode
 * through the table at 5594ec; after the movie it always goes back to VESA 101
 * (434cc3).  So the setting is switched just for the call: the movie fills
 * the window in 16-bit colour and gets the presenter's filters.
 * sttng_pc.ini [game]: FullMovies=0 keeps the original; MovieColors=8 the
 * 256-colour picture. */
#include <stdlib.h>
#include "rt.h"

#define G_VIDEO 0x502ab8u       /* LO8 0, HI8 1, LO15 2, HI15 3, LO16 4, HI16 5 */

static int setting(const char *key, int def, int lo, int hi)
{
    extern int settings_int(const char *sec, const char *key, int def, int lo, int hi);
    return settings_int("game", key, def, lo, hi);
}

/* 41a1c6: play a movie (cdecl name) */
void N_movie_play(void)
{
    u32 v = RD32(G_VIDEO);
    int sw = setting("FullMovies", 1, 0, 1) && v == 1;      /* HI8: 640x480x256 */
    if (sw) WR32(G_VIDEO, setting("MovieColors", 16, 8, 16) >= 15 ? 4 : 0);
    rt_wrap_orig("N_movie_play", NULL)();
    if (sw) WR32(G_VIDEO, v);
}
