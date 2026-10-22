#!/usr/bin/env python3
"""Function-level differential test of the lifted code.

The root image is placed at segment 0 (LOADSEG=0) both in unicorn and in a
test build of the lifted C.  Every function whose call tree only does
computation (no interrupts, port I/O, overlays or indirect transfers) is run
on random register / stack / memory states by both, and the final register
file, the flags that are live at the return, and all of memory are
compared.  This checks the lifter's control flow, flag liveness and branch
fusion in their real context.

usage: functest.py GAMEDIR OUTDIR [cases-per-function]
"""
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift                                     # noqa: E402
from modules import DGROUP                     # noqa: E402

from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_16   # noqa: E402
from unicorn import x86_const as X                          # noqa: E402

MEM_TOP = 0x50000          # compared / randomised up to here
STEP_CAP = 300000
SENT_FAR = 0xF000

BAD_MN = {'int', 'int3', 'into', 'in', 'out', 'insb', 'insw', 'outsb',
          'outsw', 'hlt', 'ovlcall', 'bound', 'iret'}


def pure_functions(prog, funcs):
    info = {}
    for f in funcs:
        if f.mod.kind != 'root':
            continue
        ok = True
        callees = set()
        rets = set()
        for o, i in f.insns.items():
            if i is None or i.patched:
                ok = False
                break
            mn = i.mn
            if mn in BAD_MN or mn.startswith('bad') or mn == 'reloc_disp':
                ok = False
                break
            if mn == 'call':
                if i.target is None:
                    ok = False
                    break
                callees.add((f.mod.cname, f.seg, i.target))
            elif mn in ('callf', 'jmpf'):
                if i.far_target is None:
                    ok = False
                    break
                s, o_, rel = i.far_target
                if not rel:
                    ok = False
                    break
                callees.add(('root', s, o_))
            elif mn == 'jmp' and i.target is None and o not in f.tables:
                ok = False
                break
            elif mn in ('ret', 'retf'):
                rets.add(mn)
        # tail jumps into other functions
        for blk in f.blocks:
            for s in lift.successors(f, blk):
                if s not in f.bidx or s in f.entries_other:
                    callees.add((f.mod.cname, f.seg, s))
        if len(rets) != 1:
            ok = False
        info[f.key] = (ok, callees, rets)
    pure = {k for k, v in info.items() if v[0]}
    changed = True
    while changed:
        changed = False
        for k in list(pure):
            for c in info[k][1]:
                if c not in pure:
                    pure.discard(k)
                    changed = True
                    break
    return [prog.funcs[k] for k in sorted(pure)], info


class Rng:
    def __init__(self, seed):
        self.s = seed

    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.s = x
        return x


def base_memory(root, seed):
    img = bytes(root.image)
    m = bytearray(MEM_TOP)
    r = Rng(seed)
    for k in range(0, MEM_TOP, 4):
        m[k:k + 4] = struct.pack('<I', r.next())
    m[:len(img)] = img
    # keep DGROUP initialised data, randomise only beyond the image
    return m


def main():
    gamedir, outdir = sys.argv[1], sys.argv[2]
    ncase = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    prog, funcs = lift.build(gamedir)
    pure, info = pure_functions(prog, funcs)
    print('%d pure root functions' % len(pure))
    os.makedirs(outdir, exist_ok=True)
    root = prog.root
    rnd = random.Random(99)
    seeds = [0x13572468, 0x2468ace1]
    bases = {s: base_memory(root, s) for s in seeds}
    out = bytearray()
    ntest = 0
    skipped = 0
    names = []
    for f in pure:
        far = 'retf' in info[f.key][2]
        for c in range(ncase):
            seed = seeds[c % 2]
            mem = bases[seed]
            regs = [rnd.getrandbits(16) for _ in range(6)]
            if c % 2:
                regs = [rnd.choice([0, 1, 2, 3, 8, 0x10, 0x7f, 0xff, 0x100,
                                    0x7fff, 0xffff]) for _ in range(6)]
            bp = rnd.getrandbits(16)
            sp = 0xF000 + 2 * rnd.getrandbits(10)
            flags = rnd.getrandbits(12) & 0x0CD5
            # small argument words on the stack make loops terminate
            args = [rnd.choice([0, 1, 2, 3, 5, 0x10, 0x20, rnd.getrandbits(16),
                                rnd.getrandbits(8)]) for _ in range(12)]
            uc = Uc(UC_ARCH_X86, UC_MODE_16)
            uc.mem_map(0, 0x120000)
            uc.mem_write(0, bytes(mem))
            ss = DGROUP
            stack = bytearray()
            if far:
                stack += struct.pack('<HH', 0x0000, SENT_FAR)
                until = SENT_FAR * 16
            else:
                stack += struct.pack('<H', 0xFFFE)
                until = f.seg * 16 + 0xFFFE
            stack += b''.join(struct.pack('<H', a) for a in args)
            uc.mem_write(ss * 16 + sp, bytes(stack))
            for r, v in zip([X.UC_X86_REG_AX, X.UC_X86_REG_BX, X.UC_X86_REG_CX,
                             X.UC_X86_REG_DX, X.UC_X86_REG_SI, X.UC_X86_REG_DI],
                            regs):
                uc.reg_write(r, v)
            uc.reg_write(X.UC_X86_REG_BP, bp)
            uc.reg_write(X.UC_X86_REG_SP, sp)
            uc.reg_write(X.UC_X86_REG_DS, DGROUP)
            uc.reg_write(X.UC_X86_REG_ES, DGROUP)
            uc.reg_write(X.UC_X86_REG_SS, ss)
            uc.reg_write(X.UC_X86_REG_CS, f.seg)
            uc.reg_write(X.UC_X86_REG_IP, f.off)
            uc.reg_write(X.UC_X86_REG_EFLAGS, flags | 2)
            try:
                uc.emu_start(f.seg * 16 + f.off, until, count=STEP_CAP)
            except UcError:
                skipped += 1
                continue
            pc = uc.reg_read(X.UC_X86_REG_CS) * 16 + uc.reg_read(X.UC_X86_REG_IP)
            if pc != until:
                skipped += 1
                continue
            res = [uc.reg_read(r) & 0xFFFF for r in (
                X.UC_X86_REG_AX, X.UC_X86_REG_BX, X.UC_X86_REG_CX,
                X.UC_X86_REG_DX, X.UC_X86_REG_SI, X.UC_X86_REG_DI,
                X.UC_X86_REG_BP, X.UC_X86_REG_SP, X.UC_X86_REG_DS,
                X.UC_X86_REG_ES, X.UC_X86_REG_SS)]
            fl = uc.reg_read(X.UC_X86_REG_EFLAGS) & 0xFFFF
            after = uc.mem_read(0, MEM_TOP)
            # initial memory includes the stack contents we wrote
            init = bytearray(mem)
            init[ss * 16 + sp:ss * 16 + sp + len(stack)] = stack
            diffs = []
            a = bytes(after)
            for k in range(0, MEM_TOP, 4096):
                if a[k:k + 4096] != init[k:k + 4096]:
                    for j in range(k, k + 4096):
                        if a[j] != init[j]:
                            diffs.append((j, a[j]))
            if len(diffs) > 60000:
                skipped += 1
                continue
            mask = f.retlive & 0x0CD5
            idx = len(names)
            if not names or names[-1] != f.name:
                names.append(f.name)
            else:
                idx = len(names) - 1
            out += struct.pack('<HIHH6HHHHHH', idx, seed, f.seg, f.off,
                               *regs, bp, sp, flags, len(stack), 1 if far else 0)
            out += bytes(stack)
            out += struct.pack('<11HHHI', *res, fl, mask, len(diffs))
            for j, v in diffs:
                out += struct.pack('<IB', j, v)
            ntest += 1
    with open(os.path.join(outdir, 'funcs_list.c'), 'w') as c:
        c.write('#include "cpu.h"\n#include "funcs.h"\n')
        c.write('struct ftest { void (*fn)(void); const char *name; };\n')
        c.write('const struct ftest ftests[] = {\n')
        for n in names:
            c.write('    { %s, "%s" },\n' % (n, n))
        c.write('};\nconst int ftest_count = %d;\n' % len(names))
    open(os.path.join(outdir, 'fcases.bin'), 'wb').write(
        struct.pack('<II', ntest, len(seeds)) +
        b''.join(struct.pack('<I', s) for s in seeds) + bytes(out))
    open(os.path.join(outdir, 'root.img'), 'wb').write(bytes(root.image))
    print('%d cases (%d skipped: no termination / fault)' % (ntest, skipped))


if __name__ == '__main__':
    main()
