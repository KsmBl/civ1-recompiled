#!/usr/bin/env python3
"""Quick disassembler for the unpacked root image: dis.py SEG:OFF [count]"""
import capstone, struct, sys
d = open('build/CIV_UNP.EXE', 'rb').read()
h = struct.unpack('<14H', d[:28]); hs = h[4] * 16
img = d[hs:]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
seg, off = (int(x, 16) for x in sys.argv[1].split(':'))
n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
base = seg * 16
for k, i in enumerate(md.disasm(img[base + off:base + 0x10000], off)):
    if k >= n: break
    print('%04x:%04x  %-16s %s %s' % (seg, i.address, i.bytes.hex(), i.mnemonic, i.op_str))
