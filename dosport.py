"""dosport.py - make and drive a native Windows port of a 32-bit DOS game.

Built from what the Aliens: A Comic Book Adventure and Alien Odyssey ports
share: LE loader -> Ghidra function lists -> x86 analysis -> x86-to-C
recompiler -> a PC runtime that replaces DOS, the DOS extender, the BIOS,
interrupts, VGA/VESA, mouse, MSCDEX and (optionally) the sound library ->
a CD installer.  Run it with the Windows Python that has capstone.

  dosport.py scan GAME.EXE...           what kind of program is this? (extender,
                                        LE objects, libraries, DOS/hardware use)
  dosport.py new <dir> --title "Name" --exe GAME.EXE [--exe OTHER.EXE ...]
                 [--short name] [--env XX_] [--libs miles_ail3,hdtext,...]
                 [--cd-file FILE ...] [--icon-lbm PATH\\ON\\CD.LBM]
                                        create a port project from the template
  dosport.py configure                  (in a project) regenerate game.h,
                                        modules.inc, port.mk, run_test.sh ...
                                        from port.json
  dosport.py addlib <lib>               (in a project) copy a library in
  dosport.py ghidra [tag...]            Ghidra headless analysis -> re/<tag>/
  dosport.py gen [tag...]               recompile -> src/gen/<tag>/
  dosport.py libs                       list the libraries in the kit

A project keeps everything it needs (tools, runtime, libraries are copied
in): one repo per game, nothing shared at build time.
"""
import json, os, re, shutil, subprocess, sys, struct

KIT = os.path.dirname(os.path.abspath(__file__))
GHIDRA = os.environ.get('GHIDRA', r'C:\Users\Msi\Tools\ghidra_12.1.4_PUBLIC')

LIBS = {
    'miles_ail3': ('Miles Sound System / AIL 3.x (digital samples, streams, XMIDI music) at the AIL_API level',
                   ['native_ail.c'], ['tools/ail_api.py', 'tools/ail_map.py', 'tools/ail_used.py', 'tools/gen_ail_natives.py'], 'HAVE_AIL'),
    'hmi_sos': ('HMI SOS digital driver API (sosDIGIStartSample ...) -> mixer. Game-state addresses are per game!',
                ['native_sound.c'], [], 'HAVE_SOS'),
    'hmp_midi': ('HMP music player on Windows MIDI (from Aliens; its game-state addresses are per game!)',
                 ['native_midi.c'], [], 'HAVE_HMP'),
    'brender_hires': ('Argonaut BRender: render the 3D scene again at N x resolution (addresses per game)',
                      ['hires3d.c'], [], 'HAVE_HIRES3D'),
    'hdtext': ('HD text: the game\'s glyph blitter wrapped, text redrawn with a TrueType font',
               ['hdtext.c'], [], 'HAVE_HDTEXT'),
    'modex': ('VGA Mode X (unchained planes, page flips, split screen) as a native plane store + '
              'presenter hook; natives for the game\'s mode/flip/blit routines (from Quarantine; addresses per game!)',
              ['native_modex.c'], [], 'HAVE_MODEX'),
}


def die(msg):
    print('error:', msg)
    sys.exit(1)


def load_le(path):
    sys.path.insert(0, os.path.join(KIT, 'tools'))
    from le import LE
    return LE(path)


# ------------------------------------------------------------------ scan
SIGS = [
    (rb'DOS/4G', 'DOS/4GW extender'), (rb'PMODE/W', 'PMODE/W extender'), (rb'DOS/32A', 'DOS/32A extender'),
    (rb'CauseWay', 'CauseWay extender'), (rb'WATCOM', 'Watcom C runtime'), (rb'AIL_', 'Miles AIL (libs/miles_ail3)'),
    (rb'Miles', 'Miles Sound System'), (rb'sosDIGI|HMIDRV|HMIDET', 'HMI SOS (libs/hmi_sos)'), (rb'sosMIDI', 'HMI SOS MIDI'),
    (rb'HMIMIDIP', 'HMP music (libs/hmp_midi)'), (rb'BRender|BrZb|BrPixelmap','Argonaut BRender (libs/brender_hires)'),
    (rb'\.BRP', 'Argonaut BRP video'), (rb'VESA', 'VESA modes'), (rb'MSCDEX', 'MSCDEX use'), (rb'\.SMK', 'Smacker video'),
    (rb'DIGPAK', 'DigPak sound'), (rb'MIDPAK', 'MidPak music'), (rb'\.XMI', 'XMIDI music'), (rb'\.VOC', 'VOC samples'),
    (rb'BLASTER', 'Sound Blaster env'), (rb'ULTRASND', 'Gravis Ultrasound env'),
]


def cmd_scan(args):
    for path in args:
        d = open(path, 'rb').read()
        print('==', path, '(%d bytes)' % len(d))
        try:
            le = load_le(path)
        except Exception as e:
            print('  not an LE program (%s) - 16-bit real-mode or another format: this kit does not cover it' % e)
            continue
        print('  LE at file offset %x (MZ at %x)%s' % (le.le, le.mz, '  - extender bound in front' if le.mz else ''))
        for o in le.objs:
            print('  object %d: base %06x -> %06x  size %6x  pages %3d  %s' % (o.index, o.orig_base, o.base, o.vsize, o.npages,
                  'code' if o.exec else 'data'))
        print('  entry %06x' % (le.objs[le.eip_obj - 1].base + le.eip))
        found = [n for s, n in SIGS if re.search(s, d, re.I)]
        print('  found:', ', '.join(found) or '-')
        code = le.objs[0]
        mem = bytes(le.mem[code.base - le.lo:code.base - le.lo + code.vsize])
        ints = {}
        for m in re.finditer(rb'\xcd([\x00-\xff])', mem):
            ints[m.group(1)[0]] = ints.get(m.group(1)[0], 0) + 1
        top = sorted(ints.items(), key=lambda x: -x[1])
        interesting = [(n, c) for n, c in top if n in (0x10, 0x16, 0x21, 0x2f, 0x31, 0x33, 0x1a, 0x15)]
        print('  int sites (rough):', ', '.join('%02Xh x%d' % x for x in interesting))


# ------------------------------------------------------------------ project
def ident(title):
    return re.sub(r'[^A-Za-z0-9]', '', title) or 'Game'


def proj():
    if not os.path.exists('port.json'):
        die('run this inside a port project (no port.json here)')
    return json.load(open('port.json'))


def wr(p, s, keep=False):
    if keep and os.path.exists(p):
        return
    os.makedirs(os.path.dirname(p) or '.', exist_ok=True)
    if not os.path.exists(p) or open(p, encoding='utf-8').read() != s:
        open(p, 'w', encoding='utf-8', newline='\n').write(s)


def configure(P):
    tags = [m['tag'] for m in P['programs']]
    have = {LIBS[l][3]: 1 for l in P.get('libs', [])}
    cd_files = ', '.join('"%s"' % f.replace('\\', '\\\\') for f in P.get('cd_files', [])) + (', ' if P.get('cd_files') else '') + 'NULL'
    h = ['/* generated by dosport.py configure from port.json - edit port.json instead */', '#pragma once',
         '#define GAME_TITLE "%s"' % P['title'], '#define GAME_SHORT "%s"' % P['short'],
         '#define GAME_ID "%s"' % P['id'], '#define GAME_ENV "%s"' % P['env'],
         '#define GAME_FIRST_PROGRAM "%s"' % os.path.splitext(os.path.basename(P['programs'][0]['exe']))[0].upper(),
         '#define GAME_CD_FILES {%s}' % cd_files,
         '#define GAME_ICON_LBM %s' % ('"%s"' % P['icon_lbm'].replace('\\', '\\\\') if P.get('icon_lbm') else 'NULL')]
    for k in sorted(set(v[3] for v in LIBS.values())):
        h.append('#define %s %d' % (k, have.get(k, 0)))
    wr('src/rt/game.h', '\n'.join(h) + '\n')
    m = ['/* generated by dosport.py configure from port.json */']
    for t in tags:
        m.append('extern const FuncEnt %s_functab[]; extern const int %s_nfuncs; extern const WrapEnt %s_wraptab[];' % (t, t, t))
    m.append('static const Module modules[] = {')
    for p in P['programs']:
        t = p['tag']
        name = os.path.splitext(os.path.basename(p['exe']))[0].upper()
        load = p.get('load') or 'cd\\' + p['exe']
        dosp = p.get('dospath') or 'C:\\' + os.path.basename(p['exe']).upper()
        m.append('    {"%s", %s_functab, &%s_nfuncs, %s_wraptab, "%s", "%s"},' % (
            name, t, t, t, load.replace('\\', '\\\\'), dosp.replace('\\', '\\\\')))
    m.append('};')
    wr('src/rt/modules.inc', '\n'.join(m) + '\n')
    mods = ' '.join('%s:%s:%s' % (p['tag'], os.path.splitext(p['exe'])[0].upper(), p['tag']) for p in P['programs'])
    mk = ['# generated by dosport.py configure from port.json', 'TITLE := %s' % P['title'], 'SHORT := %s' % P['short'],
          'EXE := %s.exe' % P['short'], 'MODS := %s' % mods,
          'ICON_SRC ?= $(CD)/%s' % P['icon_lbm'].replace('\\', '/') if P.get('icon_lbm') else 'ICON_SRC ?=']
    wr('port.mk', '\n'.join(mk) + '\n')
    wr('run_test.sh', '''#!/bin/sh
# Background test run: hidden window, killed after $1 seconds (default 30).
# Audio goes to the real device; mute it in the game's options for tests.
cd "$(dirname "$0")"
T=${1:-30}; shift
%sHIDDEN=1 ./%s.exe "$@" &
PID=$!
i=0
while kill -0 $PID 2>/dev/null && [ $i -lt $T ]; do sleep 1; i=$((i+1)); done
kill $PID 2>/dev/null && echo "(killed after ${T}s)"
tail -c 3000 %s.log
''' % (P['env'], P['short'], P['short']))
    wr('installer/setup.manifest', '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity version="1.0.0.0" name="%sPCNative.Setup" type="win32"/>
  <description>%s - PC Native Setup</description>
  <dependency><dependentAssembly><assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/></dependentAssembly></dependency>
  <application xmlns="urn:schemas-microsoft-com:asm.v3"><windowsSettings><dpiAware xmlns="http://schemas.microsoft.com/SMI/2005/WindowsSettings">true</dpiAware></windowsSettings></application>
</assembly>
''' % (P['id'], P['title']))
    for p in P['programs']:
        nt = 'tools/native_%s.txt' % p['tag']
        body = ('# Functions of %s replaced by native PC code (src/rt/native*.c).\n'
                '# <hex address> <C function> [wrap]   ("wrap": the original stays callable via rt_wrap_orig)\n' % p['exe'])
        if 'miles_ail3' in P.get('libs', []):
            body += '# --- AIL (written by tools/gen_ail_natives.py, keep this marker last)\n'
        wr(nt, body, keep=True)
        os.makedirs('re/%s' % p['tag'], exist_ok=True)
    print('configured: %s (%s), programs %s, libs %s' % (P['title'], P['short'], ' '.join(tags), ', '.join(P.get('libs', [])) or '-'))


def addlib(name):
    if name not in LIBS:
        die('unknown library %s (dosport.py libs)' % name)
    src = os.path.join(KIT, 'libs', name)
    for f in LIBS[name][1]:
        shutil.copy(os.path.join(src, f), os.path.join('src', 'rt', f))
    for f in LIBS[name][2]:
        shutil.copy(os.path.join(src, f), os.path.join('tools', os.path.basename(f)))


def cmd_new(args):
    import argparse
    ap = argparse.ArgumentParser(prog='dosport.py new')
    ap.add_argument('dir')
    ap.add_argument('--title', required=True)
    ap.add_argument('--short')
    ap.add_argument('--env')
    ap.add_argument('--exe', action='append', required=True, help='program on the CD (relative path), first = started first')
    ap.add_argument('--libs', default='')
    ap.add_argument('--cd-file', action='append', default=[], help='files that identify the CD (installer check)')
    ap.add_argument('--icon-lbm')
    a = ap.parse_args(args)
    if os.path.exists(a.dir) and os.listdir(a.dir):
        die('%s exists and is not empty' % a.dir)
    short = a.short or re.sub(r'[^a-z0-9]', '', a.title.lower())[:12]
    env = a.env or (''.join(w[0] for w in re.findall(r'[A-Za-z0-9]+', a.title)).upper()[:3] + '_')
    progs, seen = [], set()
    for e in a.exe:
        t = re.sub(r'[^a-z0-9]', '', os.path.splitext(os.path.basename(e))[0].lower())
        if t[0].isdigit():
            t = 'p' + t
        while t in seen:
            t += '_'
        seen.add(t)
        e = e.replace('/', '\\')
        progs.append({'tag': t, 'exe': e,                          # under gamedata/cd (recompiler input)
                      'load': 'cd\\' + e,                          # file the runtime loads, game-root relative
                      'dospath': 'C:\\' + os.path.basename(e).upper()})   # path DOS sees = start drive/dir
    libs = [l for l in a.libs.split(',') if l]
    for l in libs:
        if l not in LIBS:
            die('unknown library %s' % l)
    P = {'title': a.title, 'short': short, 'id': ident(a.title), 'env': env, 'programs': progs, 'libs': libs,
         'cd_files': a.cd_file or [progs[0]['exe']], 'icon_lbm': a.icon_lbm or ''}
    shutil.copytree(os.path.join(KIT, 'template'), a.dir, dirs_exist_ok=True)
    shutil.copytree(os.path.join(KIT, 'tools'), os.path.join(a.dir, 'tools'), dirs_exist_ok=True)
    shutil.copy(os.path.abspath(__file__), os.path.join(a.dir, 'dosport.py'))
    shutil.copytree(os.path.join(KIT, 'libs'), os.path.join(a.dir, 'libs'), dirs_exist_ok=True)
    os.chdir(a.dir)
    json.dump(P, open('port.json', 'w'), indent=2)
    for l in libs:
        addlib(l)
    configure(P)
    for f in ('README.md',):
        s = open(f, encoding='utf-8').read().replace('@TITLE@', a.title).replace('@SHORT@', short).replace('@ENV@', env)
        open(f, 'w', encoding='utf-8', newline='\n').write(s)
    print('''
next steps (in %s):
  1. put the game's CD files in gamedata/cd/   (copy of the disc)
  2. python dosport.py ghidra                (function lists -> re/<tag>/functions.tsv)
  3. python dosport.py gen                   (recompile -> src/gen/<tag>/)
  4. make -j16   (MSYS2 MINGW64)  - then ./run_test.sh 30 and read %s.log
  5. edit src/game/game.c (launcher, texts) and installer/game_install.c''' % (a.dir, short))


def cmd_ghidra(tags):
    P = proj()
    head = os.path.join(GHIDRA, 'support', 'analyzeHeadless.bat')
    if not os.path.exists(head):
        die('Ghidra not found at %s (set GHIDRA=...)' % GHIDRA)
    py = sys.executable
    for p in P['programs']:
        t = p['tag']
        if tags and t not in tags:
            continue
        exe = os.path.join('gamedata', 'cd', p['exe'])
        d = os.path.join('re', t)
        os.makedirs(os.path.join(d, 'ghidra'), exist_ok=True)
        subprocess.check_call([py, 'tools/le2pe.py', exe, os.path.join(d, t + '_pe.exe')])
        subprocess.check_call([py, 'tools/fixtargets.py', exe, os.path.join(d, 'fixup_targets.txt')])
        pd = os.path.abspath(os.path.join(d, 'ghidra_proj'))
        shutil.rmtree(pd, ignore_errors=True)
        os.makedirs(pd)
        with open(os.path.join(d, 'ghidra_headless.log'), 'w') as log:
            subprocess.call([head, pd, t, '-import', os.path.abspath(os.path.join(d, t + '_pe.exe')),
                             '-scriptPath', os.path.abspath('tools/ghidra'), '-max-cpu', str(max(1, (os.cpu_count() or 8) // 2)),
                             '-preScript', 'SeedFixups.java', os.path.abspath(os.path.join(d, 'fixup_targets.txt')),
                             '-postScript', 'ExportAll.java', os.path.abspath(os.path.join(d, 'ghidra'))],
                            stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        src = os.path.join(d, 'ghidra', 'functions.tsv')
        if os.path.exists(src):
            shutil.copy(src, os.path.join(d, 'functions.tsv'))
            print('%s: %d functions' % (t, sum(1 for _ in open(src))))
        else:
            print('%s: Ghidra produced no function list - see %s' % (t, os.path.join(d, 'ghidra_headless.log')))


def cmd_gen(tags):
    P = proj()
    for p in P['programs']:
        t = p['tag']
        if tags and t not in tags:
            continue
        fl = os.path.join('re', t, 'functions.tsv')
        if not os.path.exists(fl):
            open(fl, 'w').close()
            print('%s: no function list yet (dosport.py ghidra) - recursive descent from the entry only' % t)
        print('== %s (%s)' % (t, p['exe']))
        r = subprocess.call([sys.executable, 'tools/recomp.py', os.path.join('gamedata', 'cd', p['exe']), fl,
                             'tools/native_%s.txt' % t, os.path.join('src', 'gen', t), t])
        if r:
            sys.exit(r)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    c, a = sys.argv[1], sys.argv[2:]
    if c == 'scan':
        cmd_scan(a)
    elif c == 'new':
        cmd_new(a)
    elif c == 'configure':
        configure(proj())
    elif c == 'addlib':
        P = proj()
        for l in a:
            addlib(l)
            if l not in P['libs']:
                P['libs'].append(l)
        json.dump(P, open('port.json', 'w'), indent=2)
        configure(P)
    elif c == 'ghidra':
        cmd_ghidra(a)
    elif c == 'gen':
        cmd_gen(a)
    elif c == 'libs':
        for k, v in LIBS.items():
            print('%-14s %s' % (k, v[0]))
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
