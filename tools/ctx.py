#!/usr/bin/env python3
"""Print disassembly context: ctx.py FILE.asm FUNC ADDR [before] [after]"""
import sys
fn, func, addr = sys.argv[1], sys.argv[2], sys.argv[3]
b = int(sys.argv[4]) if len(sys.argv) > 4 else 8
a = int(sys.argv[5]) if len(sys.argv) > 5 else 4
lines, cur = [], None
for l in open(fn):
    if l.startswith('; ----'):
        cur = l.split()[2]
        continue
    if cur == func:
        lines.append(l.rstrip())
for k, l in enumerate(lines):
    if l[2:6] == addr:
        print('\n'.join(lines[max(0, k - b):k + a + 1]))
        print('-----')
        break
