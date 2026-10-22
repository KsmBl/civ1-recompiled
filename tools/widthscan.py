#!/usr/bin/env python3
"""List immediates that may be the map width (80, 79, 78, 77, 40, ...) in
functions that touch the map, with a few instructions of context.

usage: widthscan.py GAMEDIR [all]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift                                     # noqa: E402

MAPFUNCS = {('root', 0x0860, 0x25b9), ('root', 0x1b05, 0x17c1),
            ('root', 0x1b05, 0x133c), ('root', 0x1e4a, 0x1191),
            ('root', 0x1b05, 0x1318), ('root', 0x1dde, 0x0287),
            ('root', 0x1b05, 0x1577), ('root', 0x1b05, 0x1562),
            ('root', 0x1b05, 0x14d2), ('root', 0x1b05, 0x144a),
            ('root', 0x1b05, 0x135b), ('root', 0x1b05, 0x192f)}
VALUES = set(int(v, 16) for v in os.environ["WS_VALUES"].split(",")) if os.environ.get("WS_VALUES") else {0x50, 0x4f, 0x4e, 0x4d, 0x4c, 0x28, 0x27, 0x29, 0xfa0, 0x7d0,
          0xffb0, 0xffb1}


def main():
    prog, funcs = lift.build(sys.argv[1])
    show_all = len(sys.argv) > 2
    patched = set()
    for line in open(os.path.join(os.path.dirname(__file__), 'patches.txt')):
        p = line.split('#')[0].split()
        if len(p) >= 2:
            s, o = p[1].split(':')
            patched.add((p[0], int(s, 16), int(o, 16)))
    total = 0
    for f in sorted(funcs, key=lambda f: (f.mod.cname, f.seg, f.off)):
        offs = sorted(o for o in f.insns if f.insns[o])
        calls = set()
        for o in offs:
            i = f.insns[o]
            if i.mn in ('call', 'callf'):
                calls.add(lift.call_target_key(prog, f, i))
        maprel = bool(calls & MAPFUNCS) or any(
            op.kind == 'mem' and op.disp in (0x7ff8, 0x7ff7)
            for o in offs for op in f.insns[o].ops)
        if not maprel and not show_all:
            continue
        hits = []
        for k, o in enumerate(offs):
            i = f.insns[o]
            imm = [op for op in i.ops if op.kind == 'imm' and (op.imm & 0xffff) in VALUES]
            if not imm or i.mn in ('int', 'ret', 'retf', 'enter'):
                continue
            hits.append(k)
        if not hits:
            continue
        print('==== %s %s' % (f.mod.cname, f.name))
        for k in hits:
            o = offs[k]
            mark = 'P' if (f.mod.cname, f.seg, o) in patched else ' '
            ctx = [f.insns[offs[j]].text for j in range(max(0, k - 3), min(len(offs), k + 3))]
            print('%s %s %04x:%04x  %-34s | %s' % (mark, f.mod.cname, f.seg, o,
                                                   f.insns[o].text, ' ; '.join(ctx)))
            total += 1
    print('%d sites' % total, file=sys.stderr)


if __name__ == '__main__':
    main()
