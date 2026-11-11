"""Control-flow analysis of the game code for the recompiler.

Functions are found by recursive descent from the entry point and direct
call targets.  Code pointers (LE fixups into the code object) that are not
switch-table entries are accepted as extra function entries only when their
code decodes cleanly.  Functions that never return are found by fixpoint so
descent does not run into the data that follows calls to them.
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86 as X
from le import LE

JCC = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp',
       'jl', 'jge', 'jle', 'jg', 'jcxz', 'jecxz', 'loop', 'loope', 'loopne'}
# instructions that never appear in this compiler's / these libraries' real code
SUSPICIOUS = {'arpl', 'bound', 'insb', 'insd', 'insw', 'outsb', 'outsd', 'outsw', 'into', 'salc', 'aam', 'aad', 'daa', 'das', 'aaa', 'aas',
              'int3', 'int1', 'hlt', 'ljmp', 'lcall', 'fdivr', 'sldt', 'lar', 'lsl',
              'cvttps2pi', 'xlatb'}


class Prog:
    def __init__(self, exe):
        self.le = LE(exe)
        le = self.le
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        # every executable object is code (obj1 = C code, obj2 = hand-written asm)
        self.cranges = [(o.base, o.base + o.vsize) for o in le.objs if o.exec]
        # hand-written asm linked into a DATA object (Quarantine's Mode X code at
        # 48a059): code calls it directly (self-relative fixups into a non-exec
        # object).  Each such target starts a code island that runs until a ret
        # followed by zero padding (or an undecodable byte).
        self.cranges += self._data_islands(le)
        self.code = le.objs[0]
        self.clo = min(r[0] for r in self.cranges)
        self.chi = max(r[1] for r in self.cranges)
        self.fix = {}  # source linear -> target linear
        for (s, k, t, ob) in le.fixups:
            if k in (7, 8):
                self.fix[s] = t
        self.icache = {}

    def _data_islands(self, le):
        def nonexec(a):
            for o in le.objs:
                if o.base <= a < o.base + o.vsize:
                    return not o.exec
            return False
        tg = sorted(set(t for (s, k, t, ob) in le.fixups if k == 8 and nonexec(t)))
        out = []
        for t in tg:
            if out and out[-1][0] <= t < out[-1][1]:
                continue
            a = t
            while True:
                off = a - le.lo
                buf = bytes(le.mem[off:off + 16])
                try:
                    i = next(self.md.disasm(buf, a, 1))
                except StopIteration:
                    break
                a += i.size
                if i.mnemonic in ('ret', 'retf', 'iretd') and bytes(le.mem[a - le.lo:a - le.lo + 4]) == b'\0\0\0\0':
                    break
            out.append((t, a))
        merged = []
        for lo, hi in out:
            if merged and lo <= merged[-1][1] + 16:
                merged[-1] = (merged[-1][0], max(hi, merged[-1][1]))
            else:
                merged.append((lo, hi))
        if merged:
            print('code islands in data objects:', ', '.join('%x-%x' % r for r in merged))
        return merged

    def in_code(self, a):
        for lo, hi in self.cranges:
            if lo <= a < hi:
                return True
        return False

    def insn(self, a):
        i = self.icache.get(a)
        if i is None:
            if not self.in_code(a):
                return None
            off = a - self.le.lo
            buf = bytes(self.le.mem[off:off + 16])
            try:
                i = next(self.md.disasm(buf, a, 1))
                # capstone calls a 3Eh (DS) prefix "notrack": it is a plain jmp/call
                if i.mnemonic.startswith('notrack '):
                    i._raw.mnemonic = i.mnemonic[8:].encode()
            except StopIteration:
                i = False
            self.icache[a] = i
        return i or None


def mem_operand(ins):
    for op in ins.operands:
        if op.type == X.X86_OP_MEM:
            return op.mem
    return None


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.blocks = {}     # start -> list of insn addrs
        self.succ = {}       # start -> successor block starts
        self.tables = {}     # indirect jmp addr -> [targets]
        self.tailcalls = {}  # jmp addr -> target function entry
        self.bad = []
        self.has_ret = False
        self.calls = set()
        self.multi = False
        self.pushcalls = {}  # jmp addr -> continuation pushed before it
        self.popret = {}     # call addr -> reg (call to a 'pop reg; ret' thunk)


def is_exit_int(p, lst):
    """int 21h preceded by mov ah,4ch / mov ax,4cxxh  -> program exit"""
    if len(lst) < 2:
        return False
    for a in reversed(lst[-4:-1]):
        i = p.insn(a)
        if i.mnemonic == 'mov' and i.operands[0].type == X.X86_OP_REG:
            r = i.reg_name(i.operands[0].reg)
            if i.operands[1].type == X.X86_OP_IMM:
                v = i.operands[1].imm
                if r == 'ah' and v == 0x4c:
                    return True
                if r in ('ax', 'eax') and (v >> 8) & 0xff == 0x4c:
                    return True
            if r in ('ah', 'ax', 'eax'):
                return False
    return False


def popret_reg(p, t):
    """t is 'pop r32; ret' -> 'r32' (a 'load the address after the call' thunk)"""
    i = p.insn(t)
    if i is None or i.mnemonic != 'pop' or i.operands[0].type != X.X86_OP_REG:
        return None
    r = i.reg_name(i.operands[0].reg)
    j = p.insn(t + i.size)
    if r in ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp') and j is not None and j.mnemonic == 'ret' and not j.operands:
        return r
    return None


def descend(p, e, entries, noreturn, tables_out, strict=False):
    f = Func(e)
    bw = [e]
    while bw:
        b = bw.pop()
        if b in f.blocks:
            continue
        a = b
        lst = []
        succ = []
        while True:
            ins = p.insn(a)
            if ins is None:
                f.bad.append(a)
                if strict:
                    return None
                break
            m = ins.mnemonic
            if strict and m in SUSPICIOUS:
                return None
            lst.append(a)
            nxt = a + ins.size
            if m in JCC:
                t = ins.operands[0].imm & 0xffffffff
                if strict and not p.in_code(t):
                    return None
                succ += [t, nxt]
                break
            if m == 'jmp':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if t != e and t in entries:
                        f.tailcalls[a] = t
                        # "push <continuation>; jmp <function>" = a hand-made call.
                        # The pushed address must be nearby code of this routine, not a
                        # function pointer argument (A Final Unity 4b76b6: push free; jmp ...)
                        if len(lst) >= 2:
                            pi = p.insn(lst[-2])
                            if (pi.mnemonic == 'push' and pi.operands[0].type == X.X86_OP_IMM and
                                    p.in_code(pi.operands[0].imm & 0xffffffff)):
                                c = pi.operands[0].imm & 0xffffffff
                                if abs(c - a) < 0x1000:
                                    f.pushcalls[a] = c
                                succ.append(c)
                    else:
                        succ.append(t)
                else:
                    mo = mem_operand(ins)
                    # Watcom switch: jmp [idx*4 + table] or jmp cs:[reg + table]
                    # (reg already scaled); the table holds fixed-up code pointers
                    if mo and ((mo.index and mo.scale == 4 and not mo.base) or
                               (bool(mo.base) != bool(mo.index) and (mo.disp & 0xffffffff) in p.fix)):
                        tb = []
                        ta = mo.disp & 0xffffffff
                        while ta in p.fix and p.in_code(p.fix[ta]) and len(tb) < 1024:
                            tb.append(p.fix[ta])
                            tables_out.add(ta)
                            ta += 4
                        if tb:
                            f.tables[a] = tb
                            succ += tb
                    if a not in f.tables and not strict:
                        # computed jump (e.g. into an unrolled loop that follows it):
                        # the function becomes multi-entry, keep decoding after it
                        f.multi = True
                        if p.insn(nxt) is not None:
                            succ.append(nxt)
                break
            if m in ('ret', 'iretd', 'iret', 'retf'):
                f.has_ret = True
                break
            if m in ('hlt', 'ud2'):
                break
            if m == 'int' and ins.operands[0].imm == 0x21 and is_exit_int(p, lst):
                break
            if m == 'call':
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if strict and not p.in_code(t):
                        return None
                    pr = popret_reg(p, t)
                    if pr:
                        # "call thunk" where thunk = pop reg; ret: reg = address after
                        # the call (data follows - a table), then return to our caller
                        f.popret[a] = pr
                        f.has_ret = True
                        break
                    f.calls.add(t)
                    if t in noreturn:
                        break
            if nxt in f.blocks:
                succ.append(nxt)
                break
            a = nxt
        f.blocks[b] = lst
        f.succ[b] = succ
        for s in succ:
            if s not in f.blocks:
                bw.append(s)
    # split blocks so every jump target starts a block
    changed = True
    while changed:
        changed = False
        starts = set(f.blocks)
        for b in list(f.blocks):
            lst = f.blocks[b]
            for i, a in enumerate(lst[1:], 1):
                if a in starts:
                    f.blocks[b] = lst[:i]
                    f.succ[b] = [a]
                    changed = True
                    break
                # a jump into the middle of an instruction: keep both decodings
    return f


def analyze(p, extra_entries=(), verbose=False, returning=()):
    noreturn = set()
    rejected = set()
    while True:
        tables = set()
        entries = {p.le.entry} | set(extra_entries)
        funcs = {}
        work = sorted(entries)
        while work:
            e = work.pop()
            if e in funcs or not p.in_code(e):
                continue
            f = descend(p, e, entries, noreturn, tables)
            funcs[e] = f
            for c in f.calls:
                if c not in entries:
                    entries.add(c)
                    work.append(c)
                    # re-descend functions that may jump to the new entry later
            for t in f.tailcalls.values():
                if t not in funcs:
                    work.append(t)
        # candidate entries from code pointers
        cand = sorted(set(t for s, t in p.fix.items()
                          if p.in_code(t) and s not in tables) - set(funcs) - rejected)
        added = False
        for c in cand:
            inside = any(c in f.blocks for f in funcs.values())
            if inside:
                # a pointer to a block start inside a known function (e.g. pushed
                # continuation): make it its own entry too
                pass
            f = descend(p, c, entries, noreturn, set(), strict=True)
            if f is None or f.bad:
                rejected.add(c)
                continue
            extra_entries = set(extra_entries) | {c}
            added = True
        # returns(f): has a ret, an indirect jump, or tail-calls a returning function
        rets = {e for e, f in funcs.items() if f.has_ret or f.bad or any(
            p.insn(l[-1]) and p.insn(l[-1]).mnemonic == 'jmp' and p.insn(l[-1]).operands[0].type != X.X86_OP_IMM
            for l in f.blocks.values() if l)} | (set(returning) & set(funcs))
        ch = True
        while ch:
            ch = False
            for e, f in funcs.items():
                if e not in rets and any(t in rets or t not in funcs for t in f.tailcalls.values()):
                    rets.add(e); ch = True
        nr = set(funcs) - rets - noreturn
        if nr:
            noreturn |= nr
            added = True
        if not added:
            return funcs, noreturn, rejected
        extra_entries = set(extra_entries)


if __name__ == '__main__':
    p = Prog(sys.argv[1])
    funcs, noreturn, rejected = analyze(p)
    ninsn = sum(len(l) for f in funcs.values() for l in f.blocks.values())
    print('functions', len(funcs), 'insns', ninsn, 'noreturn', len(noreturn), 'rejected ptrs', len(rejected))
    mn = collections.Counter()
    for f in funcs.values():
        for l in f.blocks.values():
            for a in l:
                mn[p.insn(a).mnemonic] += 1
    print(' '.join('%s:%d' % kv for kv in mn.most_common()))
    bad = [(hex(f.entry), [hex(b) for b in f.bad]) for f in funcs.values() if f.bad]
    print('bad decode', len(bad), bad[:20])
    ind = []
    for f in funcs.values():
        for b, l in f.blocks.items():
            if not l:
                continue
            ins = p.insn(l[-1])
            if ins.mnemonic == 'jmp' and ins.operands[0].type != X.X86_OP_IMM and l[-1] not in f.tables:
                ind.append((hex(l[-1]), ins.op_str, hex(f.entry)))
    print('unresolved indirect jmps', len(ind), ind[:20])
    print('noreturn', sorted(hex(x) for x in noreturn)[:40])
