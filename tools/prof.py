"""Report of a <ENV>PROF=1 run: prof.txt (host rva, samples) -> recompiled functions.
usage: prof.py <game>.exe [prof.txt] [top]"""
import sys, subprocess, bisect, os, collections
exe = sys.argv[1]
pt = sys.argv[2] if len(sys.argv) > 2 else 'prof.txt'
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
nmx = 'C:/msys64/mingw64/bin/nm.exe'
nm = subprocess.run([nmx, exe], capture_output=True, text=True).stdout
syms = sorted((int(a, 16), n) for a, t, n in (l.split() for l in nm.splitlines() if len(l.split()) == 3) if t in 'tT')
addrs = [s[0] for s in syms]
c = collections.Counter()
tot = 0
for l in open(pt):
    r, n = l.split()
    a = int(r, 16) + 0x140000000
    i = bisect.bisect_right(addrs, a) - 1
    c[syms[i][1] if i >= 0 else '?'] += int(n)
    tot += int(n)
print('samples', tot)
for name, n in c.most_common(top):
    print('%6d %5.1f%%  %s' % (n, 100.0 * n / tot, name))
