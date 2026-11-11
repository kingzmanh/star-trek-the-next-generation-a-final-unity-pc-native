/* <ENV>PROF=1: sampling profiler for test runs.  A thread stops the game
 * thread every millisecond, records its host instruction pointer and lets it
 * go on; at exit prof.txt gets "host_rva count" lines (most first) -
 * tools/prof.py turns them into recompiled function names.  Unbiased, unlike
 * guest backtraces taken at interrupt points.  <ENV>PROF_AFTER=ms starts late. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include "rt.h"

#define NB (1 << 20)
static HANDLE game_thread;
static unsigned *hist;      /* hashed by rva >> 4 */
static unsigned *keys;
static volatile int stop;
static DWORD64 base;

static void prof_write(void);
static DWORD WINAPI sampler(LPVOID p)
{
    (void)p;
    const char *a = getenv(ENV("PROF_AFTER"));
    if (a) Sleep((DWORD)atoi(a));
    timeBeginPeriod(1);
    DWORD last = GetTickCount();
    while (!stop) {
        if (GetTickCount() - last > 2000) { last = GetTickCount(); prof_write(); }   /* test runs get killed */
        Sleep(1);
        if (SuspendThread(game_thread) == (DWORD)-1) continue;
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(game_thread, &c)) {
            unsigned rva = (unsigned)(c.Rip - base);
            unsigned h = (rva >> 4) * 2654435761u >> 12;
            for (int i = 0; i < 64; i++) {
                unsigned s = (h + i) & (NB - 1);
                if (keys[s] == rva || !keys[s]) { keys[s] = rva; hist[s]++; break; }
            }
        }
        ResumeThread(game_thread);
    }
    return 0;
}

static void prof_write(void)
{
    FILE *f = fopen("prof.txt", "w");
    if (!f) return;
    for (unsigned i = 0; i < NB; i++) if (hist[i]) fprintf(f, "%x %u\n", keys[i], hist[i]);
    fclose(f);
}

void prof_start(void)
{
    if (!getenv(ENV("PROF"))) return;
    hist = calloc(NB, sizeof *hist);
    keys = calloc(NB, sizeof *keys);
    base = (DWORD64)GetModuleHandleA(NULL);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    rt_log("prof: sampling the game thread\n");
}
