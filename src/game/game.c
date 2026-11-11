/* What is particular to this game.  The runtime (src/rt) calls these hooks.
 *
 * game_main is the launcher.  A game that is one DOS program just runs it;
 * a game with a DOS launcher (a menu program, a batch file, MCP.EXE in
 * Alien Odyssey) gets that launcher rewritten here: rt_run_program(name,
 * args) runs one program on fresh guest memory and returns its exit code.
 * See libs/examples/main_odyssey.c (mcp()) for a six-program chain driven
 * by exit codes, with save-slot and password arguments. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "rt.h"

void game_main(int state)
{
    (void)state;   /* --state <hex>: free for the launcher (Alien Odyssey: which program/save to start with) */
    rt_run_program(GAME_FIRST_PROGRAM, "");
}

/* a program is about to start on fresh memory: reset library state */
void game_program_start(const char *name)
{
    (void)name;
#if HAVE_AIL || HAVE_SOS
    sound_reset();
#endif
#if HAVE_HIRES3D
    { extern void hires3d_reset(void); hires3d_reset(); }
#endif
#if HAVE_HDTEXT
    hdtext_reset();
#endif
}

const char *game_controls_text(void)
{
    return "The game's own controls, as on DOS.\n\n"
           "PC: Alt+Enter switches fullscreen; in a window the menu bar has the video options.";
}

const char *game_about_text(void)
{
    return GAME_TITLE " - PC Native\n\n"
           "The DOS game's code converted to native Windows code, running on a PC\n"
           "runtime that replaces DOS, the extender, and the sound and video hardware.\n\n"
           "Fan preservation project - you need your own copy of the game.";
}

/* game items in the Input menu (ids from 2000), e.g. switching the game's
   own control setting in its config file - see the Alien Odyssey port */
void game_menu_build(void *input_menu) { (void)input_menu; }

/* written into a new sttng_pc.ini: the best options on (all switchable in the Video menu) */
const char *game_default_ini(void)
{
    return "\r\n[game]\r\n"
           "; FullMovies: 1 = movies fill the window, 0 = the original small picture in the middle\r\n"
           "FullMovies=1\r\n"
           "; MovieColors: 16 = movies in 16-bit colour (no banding), 8 = the original 256 colours\r\n"
           "MovieColors=16\r\n"
           "; SpaceFps: frame rate of space and combat - 24 = steady ships, as in 1995 (the AI steering was made\r\n"
           ";           for it), 36 or 72 = smoother pictures but enemy ships wobble, 0 = no limit\r\n"
           "SpaceFps=24\r\n";
}

/* Video menu: the movie options (src/game/movies.c reads them per movie) */
#define ID_FULLMOVIES  2001
#define ID_MOVIE16     2002
#define ID_SPACEFPS    2100      /* + fps (24/36/72) - src/game/hires.c paces the space loop */
extern int space_fps_get(void);
extern void space_fps_set(int fps);
extern int settings_int(const char *sec, const char *key, int def, int lo, int hi);
extern void settings_set_int(const char *sec, const char *key, int v);

void game_video_menu_build(void *video_menu)
{
    HMENU m = (HMENU)video_menu;
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_FULLMOVIES, L"Full-size &movies  (fill the screen)");
    AppendMenuW(m, MF_STRING, ID_MOVIE16, L"16-bit &colour movies  (no 256-colour banding)");
    {
        HMENU sp = CreatePopupMenu();
        AppendMenuW(sp, MF_STRING, ID_SPACEFPS + 24, L"24 fps  (steady ships, as in 1995 - default)");
        AppendMenuW(sp, MF_STRING, ID_SPACEFPS + 36, L"36 fps  (smoother, enemy ships sway)");
        AppendMenuW(sp, MF_STRING, ID_SPACEFPS + 72, L"72 fps  (smoothest, enemy ships wobble)");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sp, L"Space combat &frame rate");
    }
}

void game_menu_update(void *menu)
{
    HMENU m = (HMENU)menu;
    int full = settings_int("game", "FullMovies", 1, 0, 1);
    CheckMenuItem(m, ID_FULLMOVIES, MF_BYCOMMAND | (full ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_MOVIE16, MF_BYCOMMAND | (settings_int("game", "MovieColors", 16, 8, 16) >= 15 ? MF_CHECKED : MF_UNCHECKED));
    /* 16-bit colour needs the full-size layout (the 640x480 screen stays 256 colours) */
    EnableMenuItem(m, ID_MOVIE16, MF_BYCOMMAND | (full ? MF_ENABLED : MF_GRAYED));
    int fps = space_fps_get();
    CheckMenuRadioItem(m, ID_SPACEFPS + 24, ID_SPACEFPS + 72, ID_SPACEFPS + (fps >= 72 || fps == 0 ? 72 : fps >= 36 ? 36 : 24), MF_BYCOMMAND);
}

int game_menu_command(int id)
{
    if (id == ID_FULLMOVIES) { settings_set_int("game", "FullMovies", !settings_int("game", "FullMovies", 1, 0, 1)); return 1; }
    if (id == ID_MOVIE16) { settings_set_int("game", "MovieColors", settings_int("game", "MovieColors", 16, 8, 16) >= 15 ? 8 : 16); return 1; }
    if (id == ID_SPACEFPS + 24 || id == ID_SPACEFPS + 36 || id == ID_SPACEFPS + 72) { space_fps_set(id - ID_SPACEFPS); return 1; }
    return 0;
}
