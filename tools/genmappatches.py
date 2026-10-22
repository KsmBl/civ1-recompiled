#!/usr/bin/env python3
"""Generate tools/patches.txt entries for wider maps (layer offsets in
graphics page 2 and the relocated per-square "explored" table).

usage: genmappatches.py GAMEDIR > patches_map.txt
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift                                     # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))

# pointer computations "add di/si, 0x7ff8" whose following dereferences use
# the default segment (found by hand, see mapscan / ctx.py)
DEREFS = [('root', 0x0402, 0x2af9), ('root', 0x0402, 0x30d2),
          ('ovl07', 0x2388, 0x0b1d), ('ovl07', 0x2388, 0x0b29),
          ('ovl07', 0x2388, 0x1bfc), ('ovl07', 0x2388, 0x1cb5)]


def main():
    prog, funcs = lift.build(sys.argv[1])
    out = []
    # 1. layer offsets from the map access scan; the segment comes from the
    #    call site header "GET f_xxxx_yyyy SEG:OFF" above each block
    scan = open(os.path.join(HERE, '..', 'build', 'analysis', 'mapscan.txt'))
    layer = {0x50: 1, 0xa0: 2, 0xf0: 3}
    seen = set()
    seg = None
    for line in scan:
        h = re.match(r'^(GET|SET) \S+ ([0-9a-f]{4}):[0-9a-f]{4}', line)
        if h:
            seg = int(h.group(2), 16)
            continue
        m = re.match(r'^\s+(\w+) ([0-9a-f]{4})\s+(add \w+, 0x([0-9a-f]+))\s+<===', line)
        if not m or seg is None:
            continue
        mod, off, text, val = m.group(1), int(m.group(2), 16), m.group(3), int(m.group(4), 16)
        if val not in layer:
            continue        # y offsets (50, 100, 150) stay
        key = (mod, seg, off)
        if key in seen:
            continue
        seen.add(key)
        out.append('%s %04x:%04x MAP_LX(%d)            # %s' % (mod, seg, off, layer[val], text))
    # 2. indexed accesses to DS:7FF7..7FFF -> VISSEG
    seen = set()
    for f in funcs:
        for o, i in f.insns.items():
            if i is None:
                continue
            for op in i.ops:
                if op.kind == 'mem' and op.seg == 'ds' and op.override is None and \
                        (op.base or op.index) and 0x7ff0 <= op.disp <= 0x7fff:
                    key = (f.mod.cname, f.seg, o)
                    if key not in seen:
                        seen.add(key)
                        out.append('%s %04x:%04x seg VISSEG        # %s' % (
                            f.mod.cname, f.seg, o, i.text))
    for mod, seg, off in DEREFS:
        out.append('%s %04x:%04x seg VISSEG        # pointer into the table' % (mod, seg, off))
    print('\n'.join(out))


if __name__ == '__main__':
    main()
