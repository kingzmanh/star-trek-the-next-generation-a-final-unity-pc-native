/* What the installer does that is particular to A Final Unity.
 *
 * The CD is copied whole to <install>\game\cd (the game's D: drive).
 * <install>\game\hd is the game's C: drive, made like the DOS installer's
 * "optimal" install (INSTALL.EXE + the *.LST files on the CD):
 *   - every file of STTNGINS.ZIP (STTNG.EXE, STTNG.OVL, MEMCHECK.OVL, HMI drivers...)
 *   - from STTNG.ZIP: *.FON and the files named in MIN.LST, STD1.LST, STD2.LST,
 *     OPTIMAL.LST and XTRACT.LST
 *   - SAVEGAME\ and the game's own sttng.ini (Sound Blaster 16, sound/music/voices/
 *     movies on, C:\ and D:\ paths, no UniVBE).  Everything else comes from STTNG.ZIP
 *     on the CD at run time. */
#include "setup.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "install.h"
#include "unzip.h"

int game_is_cd(CdSrc *c)
{
    static const char *files[] = GAME_CD_FILES;
    if (!c) return 0;
    for (int i = 0; files[i]; i++)
        if (!cd_exists(c, files[i])) return 0;
    return 1;
}

typedef struct { char **names; int n, cap; } NameSet;

static void add_name(NameSet *s, const char *nm)
{
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->names = realloc(s->names, s->cap * sizeof *s->names); }
    s->names[s->n++] = _strdup(nm);
}

/* names in a *.LST file: one per line or tab, '@list' lines name other lists (read separately) */
static void read_list(const char *cd, const char *lst, NameSet *s)
{
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\%s", cd, lst);
    size_t n;
    char *t = (char *)read_file(p, &n);
    if (!t) { setup_log("no %s on the CD", lst); return; }
    char word[128];
    int w = 0;
    for (size_t i = 0; i <= n; i++) {
        char ch = i < n ? t[i] : '\n';
        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ' || ch == 0) {
            word[w] = 0;
            if (w > 0 && word[0] != '@' && strchr(word, '.')) add_name(s, word);
            w = 0;
        } else if (w < (int)sizeof word - 1) word[w++] = ch;
    }
    free(t);
}

static int want_all(const char *name, void *ud) { (void)name; (void)ud; return 1; }

static int want_listed(const char *name, void *ud)
{
    NameSet *s = ud;
    const char *dot = strrchr(name, '.');
    if (dot && !_stricmp(dot, ".fon")) return 1;
    for (int i = 0; i < s->n; i++) if (!_stricmp(s->names[i], name)) return 1;
    return 0;
}

int game_install_files(const char *cd, const char *hd)
{
    char p[MAX_PATH * 2];
    /* 1. the program files */
    snprintf(p, sizeof p, "%s\\STTNGINS.ZIP", cd);
    int n = zip_extract(p, hd, want_all, NULL, NULL);
    setup_log("STTNGINS.ZIP: %d files", n);
    snprintf(p, sizeof p, "%s\\STTNG.OVL", hd);
    if (n <= 0 || GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
        setup_log("STTNGINS.ZIP could not be unpacked");
        return 0;
    }
    /* 2. the "optimal" install from the data archive */
    NameSet s = {0};
    static const char *lists[] = {"MIN.LST", "STD1.LST", "STD2.LST", "OPTIMAL.LST", "XTRACT.LST"};
    for (size_t i = 0; i < sizeof lists / sizeof *lists; i++) read_list(cd, lists[i], &s);
    snprintf(p, sizeof p, "%s\\STTNG.ZIP", cd);
    n = zip_extract(p, hd, want_listed, &s, NULL);
    setup_log("STTNG.ZIP: %d of %d listed files (+ fonts)", n, s.n);
    for (int i = 0; i < s.n; i++) free(s.names[i]);
    free(s.names);
    if (n <= 0) { setup_log("STTNG.ZIP could not be unpacked"); return 0; }
    /* 3. saves and the game's own settings */
    snprintf(p, sizeof p, "%s\\SAVEGAME", hd);
    make_dirs(p);
    snprintf(p, sizeof p, "%s\\sttng.ini", hd);
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) {
        FILE *f = fopen(p, "wb");
        if (f) {
            fputs("; Star Trek: The Next Generation, IBM Version.\r\n"
                  "Audio=SB16\r\nPort=0x220\r\nIRQ=0x5\r\nDMA=0x1\r\nVideo=HI8\r\n"
                  "Sound=ON\r\nMusic=ON\r\nVoices=ON\r\nMovies=ON\r\nAdvice=0x0\r\n"
                  "VolMusic=0x7fff\r\nVolSound=0x7fff\r\nDialogs=ON\r\nScale=ON\r\nDPixel=OFF\r\n"
                  "Path=C:\\\r\nCD=D:\\\r\nUniVESA=OFF\r\n", f);
            fclose(f);
        }
    }
    return 1;
}

const char *game_icon_lbm(void) { return NULL; }   /* the exe carries its icon (build/game.ico) */
