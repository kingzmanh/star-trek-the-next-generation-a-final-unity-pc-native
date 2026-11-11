/* PC native runtime - entry point and core.
 *
 * Every program of the game (src/gen/<module>, recompiled by tools/recomp.py)
 * runs on a fresh 4 GB guest address space loaded from the original
 * executable (game\cd\<EXE>); src/game/game.c (game_main) is the launcher
 * that decides which program runs with which arguments - for a game with a
 * DOS launcher (MCP.EXE, a batch file...) it is that launcher rewritten.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <SDL2/SDL.h>
#include "rt.h"

CPU cpu;
const FuncEnt *rt_functab;
int rt_nfuncs;
u32 rt_tail_ret;
u8 *MEM;
u8 rt_parity_tab[256];
FILE *rt_logf;
char rt_root[512];

void rt_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (rt_logf) { vfprintf(rt_logf, fmt, ap); fflush(rt_logf); }
    va_end(ap);
}

void rt_fatal(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s\n", buf);
    rt_log("regs: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
           cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7]);
    for (int i = 0; i < 16; i++) rt_log(" [esp+%02x]=%08x", i * 4, RD32(cpu.r[4] + i * 4));
    rt_log("\n");
    if (!getenv(ENV("HIDDEN"))) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_TITLE, buf, NULL);
    exit(1);
}

/* ---- function dispatch ---- */
void (*rt_lookup(u32 addr))(void)
{
    int lo = 0, hi = rt_nfuncs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (rt_functab[mid].addr == addr) return rt_functab[mid].fn;
        if (rt_functab[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

extern void irq_magic(u32 n);
static void magic_return(u32 target)
{
    /* the game chained to the handler that was installed before it: do what
       the original BIOS handler did, then return like iretd */
    irq_magic(target - MAGIC_IRET);
    cpu.r[R_ESP] += 12;
}

void rt_call(u32 target, u32 at)
{
    rt_poll();
    if (target >= MAGIC_IRET && target < MAGIC_IRET + 0x100) {
        /* far call/jmp to an old handler through a 48-bit pointer is handled
           by the caller sites; a near call into it just returns */
        irq_magic(target - MAGIC_IRET);
        cpu.r[R_ESP] += 4;
        return;
    }
    void (*fn)(void) = rt_lookup(target);
    if (!fn) rt_fatal("call to unknown code %08x from %08x", target, at);
    fn();
}

/* call m16:32 (pushfd; call far [old handler]): an interrupt handler chaining
   to the one installed before it - that handler's iretd pops eip, cs, eflags */
void rt_call_far(u32 target, u32 at)
{
    if (target == MAGIC_SVC) {          /* far call into the (native) sound driver's service */
        extern void sound_service(void) __attribute__((weak));
        if (sound_service) sound_service();
        cpu.r[R_ESP] += 8;
        return;
    }
    if (target >= MAGIC_IRET && target < MAGIC_IRET + 0x100) {
        irq_magic(target - MAGIC_IRET);
        cpu.r[R_ESP] += 12;
        return;
    }
    rt_call(target, at);
}

/* retf from an interrupt handler into the handler that was there before it
   (Miles AIL chains the BIOS timer this way): run the BIOS side and drop the
   interrupt frame that handler's iretd would have popped */
void rt_magic_chain(u32 ra)
{
    irq_magic(ra - MAGIC_IRET);
    cpu.r[R_ESP] += 12;
}

void rt_jump(u32 target, u32 at)
{
    if (target >= MAGIC_IRET && target < MAGIC_IRET + 0x100) { magic_return(target); return; }
    void (*fn)(void) = rt_lookup(target);
    if (!fn) rt_fatal("jump to unknown code %08x from %08x", target, at);
    fn();
}

void rt_call_guest(u32 addr)
{
    void (*fn)(void) = rt_lookup(addr);
    if (!fn) rt_fatal("rt_call_guest: no function at %08x", addr);
    cpu.r[R_ESP] -= 4;
    WR32(cpu.r[R_ESP], MAGIC_IRET + 0xff);
    fn();
}

/* heuristic backtrace: return addresses into the code object found on the guest stack */
void rt_backtrace(const char *tag)
{
    u32 sp = cpu.r[R_ESP];
    int n = 0;
    rt_log("  %s backtrace:", tag);
    for (u32 i = 0; i < 256 && n < 12; i++) {
        u32 v = RD32(sp + i * 4);
        if (v >= rt_code_lo && v < rt_code_hi && RD8(v - 5) == 0xe8) { rt_log(" %08x", v - 5); n++; }
    }
    rt_log("\n");
}

static int unimpl_count;
void rt_unimpl(u32 at, const char *what)
{
    if (unimpl_count++ < 200) rt_log("unimplemented instruction at %08x: %s\n", at, what);
}
void rt_bad_code(u32 at) { rt_fatal("execution reached undecodable code at %08x", at); }
void rt_fell_off(u32 at) { rt_fatal("fell off the end of a block at %08x", at); }
void rt_noreturn(u32 at) { rt_fatal("noreturn call returned at %08x", at); }
void rt_divide_error(u32 at) { rt_fatal("divide error at %08x", at); }

/* ---- x87 helpers ---- */
long double rt_fround(long double v)
{
    switch ((cpu.fcw >> 10) & 3) {
    case 0: return nearbyintl(v);
    case 1: return floorl(v);
    case 2: return ceill(v);
    default: return truncl(v);
    }
}

s64 rt_fist(long double v, int bits)
{
    long double r = rt_fround(v);
    long double lim = bits == 16 ? 32768.0L : bits == 32 ? 2147483648.0L : 9223372036854775808.0L;
    if (!(r >= -lim && r < lim)) return bits == 16 ? (s64)(s16)0x8000 : bits == 32 ? (s64)(s32)0x80000000 : INT64_MIN;
    return (s64)r;
}

void rt_fpu_save(u32 a)
{
    memset(MEM + a, 0, 108);
    WR16(a, cpu.fcw);
    WR16(a + 4, FPU_SW());
    for (int i = 0; i < 8; i++) WRF80(a + 28 + i * 10, ST(i));
    FPU_INIT();
}

void rt_fpu_restore(u32 a)
{
    cpu.fcw = RD16(a);
    u16 sw = RD16(a + 4);
    cpu.fsw = sw & ~0x3800;
    cpu.ftop = (sw >> 11) & 7;
    for (int i = 0; i < 8; i++) ST(i) = RDF80(a + 28 + i * 10);
}

/* ---- guest memory ---- */
static u32 watch_page, watch_lo, watch_hi, watch_left;
static int watch_rearm;
void rt_backtrace(const char *tag);

/* AO_WATCH=<hex addr>[,<hex len>[,<max hits>]]: log every write into that
   guest range: the page is made read-only, a write faults, is logged
   (host code address -> tools/hostsym.py), single-stepped, and the page is
   protected again (debugging) */
void rt_watch_arm(void)
{
    const char *e = getenv(ENV("WATCH"));
    if (!e) return;
    char *end;
    watch_lo = (u32)strtoul(e, &end, 16);
    u32 len = *end == ',' ? (u32)strtoul(end + 1, &end, 16) : 4;
    watch_left = *end == ',' ? (u32)strtoul(end + 1, NULL, 10) : 200;
    watch_hi = watch_lo + len;
    watch_page = watch_lo & ~0xfffu;
    DWORD old;
    VirtualProtect(MEM + watch_page, 0x1000, PAGE_READONLY, &old);
    rt_log("watch: armed on %08x-%08x\n", watch_lo, watch_hi);
}

/* <ENV>VGUARD=1 (debugging Mode X ports): every access to the A0000h window
   by code that does not go through the native plane store faults once per
   host code address and is logged (tools/hostsym.py maps it to a function) */
static int vguard_on, vguard_rearm;
void rt_vguard_arm(void)
{
    static int checked = -1;
    if (checked < 0) checked = getenv(ENV("VGUARD")) != NULL;
    if (!checked) return;
    DWORD old;
    VirtualProtect(MEM + 0xa0000, 0x10000, PAGE_NOACCESS, &old);
    vguard_on = 1;
}

static LONG WINAPI guest_fault(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && vguard_rearm) {
        DWORD old;
        vguard_rearm = 0;
        VirtualProtect(MEM + 0xa0000, 0x10000, PAGE_NOACCESS, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && vguard_on) {
        ULONG_PTR addr = ep->ExceptionRecord->ExceptionInformation[1];
        if (addr >= (ULONG_PTR)MEM + 0xa0000 && addr < (ULONG_PTR)MEM + 0xb0000) {
            static ULONG_PTR seen[256];
            static int nseen;
            ULONG_PTR rip = ep->ContextRecord->Rip;
            int k = 0;
            for (; k < nseen; k++) if (seen[k] == rip) break;
            if (k == nseen && nseen < 256) {
                seen[nseen++] = rip;
                rt_log("vguard: %s %08x host %p base %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                       (u32)(addr - (ULONG_PTR)MEM), (void *)rip, (void *)GetModuleHandleA(NULL));
            }
            DWORD old;
            VirtualProtect(MEM + 0xa0000, 0x10000, PAGE_READWRITE, &old);
            vguard_rearm = 1;
            ep->ContextRecord->EFlags |= 0x100;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && watch_rearm) {
        DWORD old;
        watch_rearm = 0;
        if (watch_left) VirtualProtect(MEM + watch_page, 0x1000, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR addr = ep->ExceptionRecord->ExceptionInformation[1];
        if (addr >= (ULONG_PTR)MEM && addr < (ULONG_PTR)MEM + GUEST_RESERVE) {
            u32 ga = (u32)(addr - (ULONG_PTR)MEM);
            if (watch_page && (ga & ~0xfffu) == watch_page) {
                DWORD old;
                VirtualProtect(MEM + watch_page, 0x1000, PAGE_READWRITE, &old);
                if (ga >= watch_lo && ga < watch_hi && watch_left) {
                    watch_left--;
                    rt_log("watch: write to %08x (now %08x) host %p base %p esp %08x\n", ga, RD32(ga & ~3u),
                           (void *)ep->ContextRecord->Rip, (void *)GetModuleHandleA(NULL), cpu.r[R_ESP]);
                    if (getenv(ENV("WATCH_BT"))) rt_backtrace("watch");   /* guest call sites (writes inside C library copies) */
                }
                watch_rearm = 1;
                ep->ContextRecord->EFlags |= 0x100;   /* single-step the write, then protect again */
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            /* commit on demand: guest code may touch memory it never asked for */
            if (VirtualAlloc((void *)(addr & ~(ULONG_PTR)0xffff), 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
                static int warned;
                if (warned++ < 20) rt_log("note: committed guest page at %08x on demand host %p base %p\n", ga,
                                          (void *)ep->ContextRecord->Rip, (void *)GetModuleHandleA(NULL));
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            rt_log("guest access fault at %08x\n", ga);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* every program starts on zeroed guest memory */
static void mem_reset(void)
{
    VirtualFree(MEM, GUEST_RESERVE, MEM_DECOMMIT);
    if (!VirtualAlloc(MEM, GUEST_COMMIT, MEM_COMMIT, PAGE_READWRITE)) rt_fatal("cannot commit guest memory");
    VirtualAlloc(MEM + 0xe0000000u, 0x400000, MEM_COMMIT, PAGE_READWRITE);   /* VESA frame buffer */
}

static void mem_init(void)
{
    MEM = VirtualAlloc(NULL, GUEST_RESERVE, MEM_RESERVE, PAGE_NOACCESS);
    if (!MEM) rt_fatal("cannot reserve guest address space");
    if (!VirtualAlloc(MEM, GUEST_COMMIT, MEM_COMMIT, PAGE_READWRITE)) rt_fatal("cannot commit guest memory");
    AddVectoredExceptionHandler(1, guest_fault);
    for (int i = 0; i < 256; i++) {
        int b = i, p = 1;
        while (b) { p ^= b & 1; b >>= 1; }
        rt_parity_tab[i] = (u8)p;
    }
}

/* ---- the programs (src/rt/modules.inc, generated from port.json) ----
   name, function tables, the file to load (relative to the game root:
   cd\X.EXE = the CD copy, hd\DIR\X.EXE = installed on the game's C:) and the
   path DOS sees (its drive and directory are where the program starts) */
typedef struct {
    const char *name;
    const FuncEnt *tab;
    const int *n;
    const WrapEnt *wrap;
    const char *load, *dospath;
} Module;
#include "modules.inc"
const char *rt_module = "";
u32 rt_code_lo, rt_code_hi;
int rt_start_state = 1;
static const WrapEnt *cur_wrap;

/* the original of a wrapped function in the running program */
void (*rt_wrap_orig(const char *native, u32 *addr))(void)
{
    for (const WrapEnt *w = cur_wrap; w && w->orig; w++)
        if (!strcmp(w->native, native)) { if (addr) *addr = w->addr; return w->orig; }
    return NULL;
}

static jmp_buf exit_jb;
static int exit_code;

/* int 21h/4Ch ends the program: back to the launcher */
void rt_exit(int code)
{
    exit_code = code;
    longjmp(exit_jb, 1);
}

/* DOS process for the program's own startup code: PSP at 0x1000 with the
   command line, environment at 0x1200 ending in the program path */
static void setup_process(const char *dospath, const char *args)
{
    u32 psp = 0x1000, env = 0x1200;
    memset(MEM + psp, 0, 0x100);
    WR16(psp, 0x20cd);
    WR16(psp + 0x2c, SEL_ENV);
    size_t n = strlen(args);
    if (n > 126) n = 126;
    WR8(psp + 0x80, (u8)(n + 1));
    WR8(psp + 0x81, ' ');
    memcpy(MEM + psp + 0x82, args, n);
    WR8(psp + 0x82 + n, 0x0d);
    char envs[512];
    int o = 0;
    o += sprintf(envs + o, "PATH=C:\\") + 1;
    o += sprintf(envs + o, "COMSPEC=C:\\COMMAND.COM") + 1;
    o += sprintf(envs + o, "DOS4G=QUIET") + 1;
    o += sprintf(envs + o, "DOS4GPATH=.;..;") + 1;
    envs[o++] = 0;
    envs[o++] = 1;
    envs[o++] = 0;
    o += sprintf(envs + o, "%s", dospath) + 1;
    memcpy(MEM + env, envs, o);
}

/* run one program like the DOS extender would; returns its exit code */
int rt_run_program(const char *name, const char *args)
{
    const Module *m = NULL;
    for (size_t i = 0; i < sizeof modules / sizeof modules[0]; i++)
        if (!_stricmp(modules[i].name, name)) m = &modules[i];
    if (!m) rt_fatal("unknown program %s", name);
    rt_log("==== %s %s\n", m->name, args);
    rt_module = m->name;
    mem_reset();
    dpmi_init();
    dos_reset();
    game_program_start(m->name);
    rt_functab = m->tab;
    rt_nfuncs = *m->n;
    cur_wrap = m->wrap;
    char exep[700];
    snprintf(exep, sizeof exep, "%s\\%s", rt_root, m->load);
    LoadInfo li;
    if (!le_load(exep, &li))
        rt_fatal("Could not load %s\nRun the installer first (it needs your " GAME_TITLE " CD).", exep);
    rt_code_lo = li.obj_base[0];
    rt_code_hi = li.obj_base[0] + li.obj_size[0];
    rt_log("loaded %s: entry %08x stack %08x\n", m->name, li.entry, li.stack);
    setup_process(m->dospath, args);
    dos_set_start(m->dospath);
    memset(&cpu, 0, sizeof cpu);
    FPU_INIT();
    cpu.r[R_ESP] = li.stack;
    rt_load_seg(S_CS, SEL_CODE);
    rt_load_seg(S_DS, SEL_DATA);
    rt_load_seg(S_SS, SEL_DATA);
    rt_load_seg(S_FS, 0);
    rt_load_seg(S_GS, 0);
    rt_load_seg(S_ES, SEL_PSP);
    if (setjmp(exit_jb)) {
        rt_log("==== %s exit %d\n", m->name, exit_code);
        return exit_code;
    }
    rt_call_guest(li.entry);
    rt_log("%s returned from its entry point\n", m->name);
    return cpu.r[R_EAX] & 0xff;
}

int main(int argc, char **argv)
{
    char exe[600];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(rt_root, sizeof rt_root, "%s\\game", exe);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(rt_root, sizeof rt_root, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) rt_start_state = (int)strtol(argv[++i], NULL, 16);
    }
    char logp[700];
    snprintf(logp, sizeof logp, "%s\\" GAME_SHORT ".log", exe);
    rt_logf = fopen(logp, "w");
    rt_log(GAME_TITLE " - PC native runtime\nroot: %s\n", rt_root);
    { extern void prof_start(void); prof_start(); }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) rt_fatal("SDL: %s", SDL_GetError());
    mem_init();
    dpmi_init();
    dos_init(rt_root);
    settings_load(exe);
    video_init();
    game_main(rt_start_state);
    rt_log("game finished\n");
    SDL_Quit();
    return 0;
}

/* function tracing (tools/trace.txt) */
void rt_trace_enter(u32 f)
{
    rt_log("> %08x eax=%08x edx=%08x ebx=%08x ecx=%08x esi=%08x edi=%08x [esp+4]=%08x [esp+8]=%08x\n", f,
           cpu.r[0], cpu.r[2], cpu.r[3], cpu.r[1], cpu.r[6], cpu.r[7], RD32(cpu.r[4] + 4), RD32(cpu.r[4] + 8));
}

void rt_trace_leave(u32 f)
{
    rt_log("< %08x eax=%08x edx=%08x\n", f, cpu.r[0], cpu.r[2]);
}
