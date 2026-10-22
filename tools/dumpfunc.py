#!/usr/bin/env python3
"""Dump the disassembly of lifted functions: dumpfunc.py GAMEDIR MODULE [SEG:OFF ...]
With no addresses, dumps every function of the module."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift
prog, funcs = lift.build(sys.argv[1])
mod = sys.argv[2]
want = None
if len(sys.argv) > 3:
    want = {tuple(int(x, 16) for x in a.split(':')) for a in sys.argv[3:]}
for f in sorted(funcs, key=lambda f: (f.seg, f.off)):
    if f.mod.cname != mod or (want and (f.seg, f.off) not in want):
        continue
    print('\n; ---- %s  (retlive %03x)' % (f.name, f.retlive))
    for o in sorted(f.insns):
        i = f.insns[o]
        lab = 'L' if o in f.leaders else ' '
        print('%s %04x  %s' % (lab, o, i.text if i else '??'))
