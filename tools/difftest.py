#!/usr/bin/env python3
"""Instruction-level differential test of the lifter.

Every distinct (non control-flow) instruction of the game is executed on
random machine states both by unicorn (the reference CPU) and by the C code
the lifter emits for it.  The script writes a C test program into
build/difftest/ which is then compiled and run by `make test`.

usage: difftest.py GAMEDIR OUTDIR [cases-per-insn]
"""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift                                     # noqa: E402
from lift import F_CF, F_PF, F_AF, F_ZF, F_SF, F_OF, F_ALL   # noqa: E402
from x86dec import is_jcc                        # noqa: E402

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_16   # noqa: E402
from unicorn import x86_const as X                          # noqa: E402

SEG_DS, SEG_ES, SEG_SS, SEG_CS = 0x1000, 0x2000, 0x3000, 0x6000
WIN_LO, WIN_HI = 0x10000, 0x50010      # memory compared after each test
CODE_IP = 0x0100

SKIP = {'jmp', 'call', 'callf', 'jmpf', 'ret', 'retf', 'iret', 'int', 'int3',
        'into', 'ovlcall', 'loop', 'loope', 'loopne', 'jcxz', 'hlt', 'in',
        'out', 'insb', 'insw', 'outsb', 'outsw', 'bound', 'bad', 'bad32',
        'reloc_disp', 'enter', 'lock', 'wait', 'fwait'}


def undefined_flags(i):
    mn = i.mn
    if mn in ('and', 'or', 'xor', 'test'):
        return F_AF
    if mn in ('shl', 'sal', 'shr', 'sar'):
        return F_AF | F_OF
    if mn in ('rol', 'ror', 'rcl', 'rcr'):
        return F_OF
    if mn in ('mul', 'imul'):
        return F_SF | F_ZF | F_AF | F_PF
    if mn in ('div', 'idiv'):
        return F_ALL
    if mn in ('aaa', 'aas'):
        return F_OF | F_SF | F_ZF | F_PF
    if mn in ('aam', 'aad'):
        return F_OF | F_AF | F_CF
    if mn in ('daa', 'das'):
        return F_OF
    return 0


class Rng:
    """xorshift32, identical in the C harness."""
    def __init__(self, seed):
        self.s = seed or 1

    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.s = x
        return x


_mem_cache = {}
SEEDS = [0x1234567, 0x2468ace, 0x13579bd, 0x7654321, 0x0f0f0f1, 0x5a5a5a5,
         0x3c3c3c3, 0x7fffffd]


def fill_mem(seed):
    if seed in _mem_cache:
        return _mem_cache[seed]
    r = Rng(seed)
    out = bytearray(WIN_HI - WIN_LO)
    for k in range(0, len(out), 4):
        v = r.next()
        out[k:k + 4] = struct.pack('<I', v)
    _mem_cache[seed] = out
    return out


REGS16 = [X.UC_X86_REG_AX, X.UC_X86_REG_BX, X.UC_X86_REG_CX, X.UC_X86_REG_DX,
          X.UC_X86_REG_SI, X.UC_X86_REG_DI, X.UC_X86_REG_BP, X.UC_X86_REG_SP]
SREGS = [X.UC_X86_REG_DS, X.UC_X86_REG_ES, X.UC_X86_REG_SS]


def run_unicorn(code, regs, flags, mem, count=1):
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, 0x120000)
    uc.mem_write(WIN_LO, bytes(mem))
    lin = SEG_CS * 16 + CODE_IP
    uc.mem_write(lin, code)
    for r, v in zip(REGS16, regs):
        uc.reg_write(r, v)
    uc.reg_write(X.UC_X86_REG_DS, SEG_DS)
    uc.reg_write(X.UC_X86_REG_ES, SEG_ES)
    uc.reg_write(X.UC_X86_REG_SS, SEG_SS)
    uc.reg_write(X.UC_X86_REG_CS, SEG_CS)
    uc.reg_write(X.UC_X86_REG_IP, CODE_IP)
    uc.reg_write(X.UC_X86_REG_EFLAGS, flags | 2)
    try:
        uc.emu_start(lin, lin + len(code) + 0x10000, count=count)
    except UcError:
        return None
    out = [uc.reg_read(r) & 0xFFFF for r in REGS16]
    out += [uc.reg_read(r) & 0xFFFF for r in SREGS]
    fl = uc.reg_read(X.UC_X86_REG_EFLAGS) & 0xFFFF
    ip = uc.reg_read(X.UC_X86_REG_IP) & 0xFFFF
    after = uc.mem_read(WIN_LO, WIN_HI - WIN_LO)
    return out, fl, ip, after


def collect(prog, funcs):
    seen = {}
    fused = {}
    for f in funcs:
        for o, i in f.insns.items():
            if i is None or i.patched:
                continue
            if i.mn in SKIP or i.mn.startswith('rep'):
                continue
            key = bytes(i.bytes)
            if i.mn in lift.STRING_OPS and i.rep is None:
                pass
            if any(op.reloc for op in i.ops):
                continue
            if key not in seen:
                seen[key] = (f, i)
        for jo, src in f.fused.items():
            j = f.insns[jo]
            key = (bytes(src.bytes), j.mn)
            if src.patched or any(op.reloc for op in src.ops):
                continue
            if key not in fused:
                fused[key] = (f, src, j)
    return seen, fused


def emit_insn(prog, f, i, live):
    em = lift.Emitter(prog, f)
    em.cur = i
    em.insn(i, live)
    return [l.strip() for l in em.lines]


def main():
    gamedir, outdir = sys.argv[1], sys.argv[2]
    ncase = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    prog, funcs = lift.build(gamedir)
    seen, fused = collect(prog, funcs)
    print('%d unique instructions, %d fused compare/branch pairs' %
          (len(seen), len(fused)))
    os.makedirs(outdir, exist_ok=True)
    rnd = random.Random(1234)
    tests_c = ['#include "cpu.h"', '#include "harness.h"', '']
    table = []
    cases = bytearray()
    ncases = 0
    tid = 0

    def add_cases(code, cfunc_lines, undef, desc, check_ip=None,
                  small_cx=False, count=1):
        nonlocal tid, ncases, cases
        name = 't%05d' % tid
        tests_c.append('static void %s(void) { %s }' % (
            name, ' '.join(cfunc_lines)))
        table.append('{ %s, "%s", %d, "%s" },' % (
            name, desc.replace('"', "'").replace('\\', '/'), len(code),
            ''.join('\\x%02x' % b for b in code)))
        for c in range(ncase):
            regs = [rnd.getrandbits(16) for _ in range(8)]
            if c % 3 == 0:
                # small values make comparisons / divisions interesting
                regs = [rnd.choice([0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff,
                                    0x8000, 0xffff, rnd.getrandbits(4)])
                        for _ in range(8)]
            if small_cx:
                regs[2] &= 0x1f
            flags = rnd.getrandbits(12) & (F_ALL | 0x400)
            if c == 1:
                flags &= ~0x400
            seed = rnd.choice(SEEDS)
            mem = fill_mem(seed)
            res = run_unicorn(code, regs, flags, mem, count)
            fault = res is None
            if fault:
                out, fl, ip, after = [0] * 11, 0, 0, mem
            else:
                out, fl, ip, after = res
            diffs = []
            if not fault:
                a = bytes(after)
                if a != bytes(mem):
                    for k in range(0, len(a), 4096):
                        if a[k:k + 4096] != mem[k:k + 4096]:
                            for j in range(k, min(k + 4096, len(a))):
                                if a[j] != mem[j]:
                                    diffs.append((WIN_LO + j, a[j]))
            cmp_mask = (F_ALL | 0x400) & ~undef
            taken = 0
            if check_ip is not None:
                taken = 1 if ip == check_ip else 0
                if check_ip == (CODE_IP + len(code)) & 0xFFFF:
                    taken = 2        # both outcomes give the same IP
            cases += struct.pack('<HI8HH', tid, seed, *regs, flags)
            cases += struct.pack('<11HHHBBH', *out, fl, cmp_mask, fault,
                                 taken, len(diffs))
            for addr, v in diffs:
                cases += struct.pack('<IB', addr, v)
            ncases += 1
        tid += 1

    for key, (f, i) in sorted(seen.items()):
        code = key
        if is_jcc(i.mn):
            # unfused branch: evaluate the flag condition
            cond = lift.JCC_FLAGCOND[i.mn]
            target = (CODE_IP + i.target - i.off) & 0xFFFF
            add_cases(code, ['TAKEN = %s;' % cond], F_ALL, i.text,
                      check_ip=target)
            continue
        try:
            lines = emit_insn(prog, f, i, F_ALL)
        except Exception as e:      # noqa: BLE001
            print('cannot emit', i.text, e)
            continue
        lines = [l for l in lines if not l.startswith('/*')]
        if any('goto' in l or 'return' in l or 'UNIMPL' in l for l in lines):
            continue
        rep = i.mn in lift.STRING_OPS and i.rep is not None
        # unicorn reports stale flags after REP string instructions
        add_cases(code, lines, F_ALL if rep else undefined_flags(i), i.text,
                  small_cx=i.mn in lift.STRING_OPS,
                  count=0 if rep else 1)

    # fused compare + branch: source executed with no live flags
    for (sb, jmn), (f, src, j) in sorted(fused.items()):
        jb = bytes(j.bytes)
        # re-encode jcc as short jump right after the source
        if len(jb) != 2:
            jb = bytes([0x70 + list(lift.JCC_USE).index(jmn), 0x10])
            order = ['jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja',
                     'js', 'jns', 'jp', 'jnp', 'jl', 'jge', 'jle', 'jg']
            jb = bytes([0x70 + order.index(jmn), 0x10])
        code = sb + jb
        disp = jb[1] if jb[1] < 0x80 else jb[1] - 0x100
        target = (CODE_IP + len(code) + disp) & 0xFFFF
        try:
            lines = emit_insn(prog, f, src, 0)
        except Exception:           # noqa: BLE001
            continue
        lines = [l for l in lines if not l.startswith('/*')]
        em = lift.Emitter(prog, f)
        em.cur = src
        cond = em.cond_expr(src, jmn)
        add_cases(code, lines + ['TAKEN = %s;' % cond], F_ALL,
                  '%s ; %s (fused)' % (src.text, jmn), check_ip=target,
                  count=2)

    tests_c.append('')
    tests_c.append('const struct test_fn test_fns[] = {')
    tests_c += table
    tests_c.append('};')
    tests_c.append('const int test_fn_count = %d;' % len(table))
    open(os.path.join(outdir, 'tests.c'), 'w').write('\n'.join(tests_c) + '\n')
    open(os.path.join(outdir, 'cases.bin'), 'wb').write(
        struct.pack('<I', ncases) + bytes(cases))
    print('%d test functions, %d cases' % (len(table), ncases))


if __name__ == '__main__':
    main()
