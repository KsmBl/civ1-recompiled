#!/usr/bin/env python3
"""List the accesses to the map layers in graphics page 2.

The map is stored as pixels: f_0860_25b9(page, x, y) reads, f_1b05_17c1(x,
y, value, page) writes.  For every call this prints the instructions that
compute the pushed arguments (back to the previous call), so the layer
offsets (+0x50, +0xa0, +0x64 ...) can be turned into patches.

usage: mapscan.py GAMEDIR
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift                                     # noqa: E402

TARGETS = {('root', 0x0860, 0x25b9): 'GET', ('root', 0x1b05, 0x17c1): 'SET'}


def main():
    prog, funcs = lift.build(sys.argv[1])
    seen = set()
    total = 0
    for f in sorted(funcs, key=lambda f: (f.mod.cname, f.seg, f.off)):
        offs = sorted(o for o in f.insns if f.insns[o])
        for k, o in enumerate(offs):
            i = f.insns[o]
            key = lift.call_target_key(prog, f, i) if i.mn in ('call', 'callf') else None
            if key not in TARGETS:
                continue
            site = (f.mod.cname, f.seg, o)
            if site in seen:
                continue
            seen.add(site)
            total += 1
            # walk back to the previous call / label boundary (max 14)
            ctx = []
            for j in range(k - 1, max(-1, k - 15), -1):
                p = f.insns[offs[j]]
                if p.mn in ('call', 'callf', 'ovlcall', 'ret', 'retf', 'jmp') or \
                        (lift.is_jcc(p.mn)):
                    break
                ctx.append(p)
            ctx.reverse()
            print('%s %s %04x:%04x' % (TARGETS[key], f.name, f.seg, o))
            for p in ctx:
                imm = [op for op in p.ops if op.kind == 'imm']
                mark = ''
                if imm and imm[0].imm in (0x50, 0xa0, 0xf0, 0x32, 0x64, 0x96,
                                          0x4f, 0x28, 0x1e):
                    mark = '   <==='
                print('      %s %04x  %s%s' % (f.mod.cname, p.off, p.text, mark))
    print('%d call sites' % total, file=sys.stderr)


if __name__ == '__main__':
    main()
