#!/usr/bin/env python3
"""Compare state dumps nat_N.bin / ws_N.bin (see tools/widestore_check.sh).
Prints the first differing pair: DS ranges and per-layer counts."""
import os
import sys

d = sys.argv[1]
same = None
SKIP = [(0x54d6, 0x57ff),          # span tables of the polygon filler
        # picture (.MAP) codec buffer and state, and text buffer leftovers:
        # the saved picture of page 2 differs with the wide layout
        (0xe856, 0xea56), (0x6a7e, 0x6a8c), (0xc18e, 0xc190), (0xc926, 0xc930)]
# page 2 at x 160-319, y 50-199 holds pictures, not map layers
PICTURE_LAYERS = {6, 7, 10, 11, 14, 15}
for n in sorted(os.listdir(d)):
    if not n.startswith('nat_'):
        continue
    w = os.path.join(d, 'ws_' + n[4:])
    if not os.path.exists(w):
        print(n, 'missing ws')
        continue
    a = open(os.path.join(d, n), 'rb').read()
    b = open(w, 'rb').read()
    diffs = [i for i in range(len(a)) if a[i] != b[i] and
             not any(lo <= i < hi for lo, hi in SKIP) and
             not (i >= 0xf000 and (i - 0xf000) // 4000 in PICTURE_LAYERS)]
    if not diffs:
        same = n
        continue
    ds = [i for i in diffs if i < 0xf000]
    rng = []
    for i in ds:
        if rng and i - rng[-1][1] <= 4:
            rng[-1][1] = i
        else:
            rng.append([i, i])
    lay = {}
    for i in diffs:
        if i >= 0xf000:
            k = (i - 0xf000) // 4000
            lay[k] = lay.get(k, 0) + 1
    print(n, 'DS:', ' '.join('%04x-%04x' % tuple(r) for r in rng[:40]))
    print('   layers', lay)
    break
else:
    print('all dumps match, last', same)
