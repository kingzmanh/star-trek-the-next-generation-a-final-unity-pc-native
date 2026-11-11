/* What the installer does that is particular to this game.
 *
 * The CD is copied whole to <install>\game\cd (the game's D: drive, always
 * "in the drive").  <install>\game\hd is the game's C: drive: put there what
 * the original DOS installer copied to the hard disk (config files, save
 * folder, a sound or data subset...).  Look at the DOS installer's script
 * or at a real DOS install to know what.  Example: libs/examples/install_odyssey.c */
#include "setup.h"
#include <string.h>
#include "install.h"

int game_is_cd(CdSrc *c)
{
    static const char *files[] = GAME_CD_FILES;
    if (!c) return 0;
    for (int i = 0; files[i]; i++)
        if (!cd_exists(c, files[i])) return 0;
    return 1;
}

int game_install_files(const char *cd, const char *hd)
{
    (void)cd;
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\SAVEGAME", hd);   /* adjust: where the game saves */
    make_dirs(p);
    return 1;
}

const char *game_icon_lbm(void) { return GAME_ICON_LBM; }
