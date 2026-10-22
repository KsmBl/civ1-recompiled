#!/usr/bin/env python3
"""List instructions that probably encode the 15x12 tile map view or the
320x200 screen geometry, in functions that use the view origin
(DS:E3EC / DS:E67E).  Output is reviewed by hand and turned into
tools/patches.txt entries.

usage: viewscan.py GAMEDIR
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift                                     # noqa: E402

VIEW_VARS = (0xe3ec, 0xe67e)
CONSTS = {0x140: 'screen width', 0xc0: 'last map row y', 0xc8: 'screen h',
          0x13f: 'screen w-1', 0xc7: 'screen h-1', 0xf: '15 tiles',
          0xe: '14', 0xd: '13 (w-2)', 0xc: '12 tiles', 0xb: '11', 9: '9 (h-3)',
          0x26: '38 (map h - view h)', 7: 'half w', 6: 'half h', 8: '8',
          0xf0: '240 map px', 0xb8: '184', 0x130: '304'}


def uses_view(f):
    for i in f.insns.values():
        if i is None:
            continue
        for op in i.ops:
            if op.kind == 'mem' and not op.base and not op.index and \
                    op.disp in VIEW_VARS:
                return True
    return False


def main():
    prog, funcs = lift.build(sys.argv[1])
    seen = set()
    for f in sorted(funcs, key=lambda f: (f.mod.cname, f.seg, f.off)):
        if not uses_view(f):
            continue
        offs = sorted(o for o in f.insns if f.insns[o])
        hits = []
        for o in offs:
            i = f.insns[o]
            if i.mn not in ('cmp', 'add', 'sub', 'mov', 'push'):
                continue
            imm = [op for op in i.ops if op.kind == 'imm']
            if not imm or imm[0].imm not in CONSTS:
                continue
            v = imm[0].imm
            if v in (6, 7, 8, 9, 0xb, 0xe) and i.mn not in ('cmp', 'add', 'sub'):
                continue
            key = (f.mod.cname, f.seg, o)
            if key in seen:
                continue
            seen.add(key)
            hits.append('  %s %04x:%04x  %-40s ; %s' % (
                f.mod.cname, f.seg, o, i.text, CONSTS[v]))
        if hits:
            print('%s:' % f.name)
            print('\n'.join(hits))


if __name__ == '__main__':
    main()
