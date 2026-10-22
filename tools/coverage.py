#!/usr/bin/env python3
"""Report how much of each code segment / module the lifter reached."""
import os, sys, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift
from modules import load_all, OVL_SEG, DGROUP
mods = load_all(sys.argv[1])
prog = lift.Program(mods)
cs, ip = prog.root.entry
prog.add_entry(prog.root, cs, ip, indirect=True)
lift.seed_data_pointers(prog)
lift.seed_drivers(prog)
lift.load_seeds(prog, os.path.join(os.path.dirname(__file__), 'seeds.txt'))
prog.run()
while lift.seed_gaps(prog): prog.run()
cov = collections.defaultdict(set)
for f in prog.funcs.values():
    for o, i in f.insns.items():
        if i: 
            for k in range(i.size): cov[f.mod.cname].add(f.mod.ofs(f.seg, o + k))
segs = sorted({v for v in [0x0,0x181,0x1a7,0x237,0x402,0x860,0xad5,0xd07,0x1439,0x14f6,0x15da,0x1a19,0x1b05,0x1c9d,0x1d1e,0x1dde,0x1e4a,0x1f65,0x1fb4,0x2058,0x2388]})
for a,b in zip(segs, segs[1:]+[DGROUP]):
    if a==0x2388: continue
    n=sum(1 for x in cov['root'] if a*16<=x<b*16)
    print('root seg %04x size %5x covered %5x  %3d%%'%(a,(b-a)*16,n,100*n//((b-a)*16)))
for m in mods:
    if m.kind!='root':
        n=len(cov[m.cname]); print('%-8s size %5x covered %5x %3d%%'%(m.cname,len(m.image),n,100*n//len(m.image)))
