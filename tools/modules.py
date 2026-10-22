"""Code modules of the game: the resident root image, the 23 MS-LINK
overlays and the run-time loaded drivers (graphics / sound / misc).

Every module is described by its link-time image, the set of image offsets
that carry segment relocations, and the link-time segment of image offset 0.
"""
import os
import struct

from unexepack import unpack

OVL_SEG = 0x2388          # link-time segment of the overlay area
DGROUP = 0x2A1B           # link-time segment of DGROUP in the root image


class Module:
    def __init__(self, name, cname, image, relocs, base_seg, kind):
        self.name = name          # file name / description
        self.cname = cname        # identifier used in generated C
        self.image = image
        self.relocs = relocs      # set of image offsets of relocated words
        self.base_seg = base_seg  # link-time segment of image offset 0
        self.kind = kind          # 'root', 'ovl' or 'drv'
        self.ovl = None

    def ofs(self, seg, off):
        return (seg - self.base_seg) * 16 + off

    def valid(self, seg, off):
        o = self.ofs(seg, off)
        return 0 <= o < len(self.image)

    def read(self, seg, off, n):
        o = self.ofs(seg, off)
        return self.image[o:o + n]

    def word(self, seg, off):
        o = self.ofs(seg, off)
        return struct.unpack('<H', self.image[o:o + 2])[0]


def _parse_mz(data, start=0):
    h = struct.unpack('<14H', data[start:start + 28])
    size = (h[2] - 1) * 512 + h[1] if h[1] else h[2] * 512
    hs = h[4] * 16
    img = data[start + hs:start + size]
    rel = []
    for i in range(h[3]):
        o, s = struct.unpack('<HH', data[start + h[12] + 4 * i:
                                          start + h[12] + 4 * i + 4])
        rel.append((s, o))
    return h, img, rel, h[2] * 512


def load_all(gamedir):
    mods = []
    raw = open(os.path.join(gamedir, 'CIV.EXE'), 'rb').read()
    exe, trailer, info = unpack(raw)
    h, img, rel, _ = _parse_mz(exe)
    root = Module('CIV.EXE', 'root', img, {s * 16 + o for s, o in rel}, 0,
                  'root')
    root.entry = (info['cs'], info['ip'])
    mods.append(root)

    p = 0
    while p < len(trailer):
        h, img, rel, adv = _parse_mz(trailer, p)
        n = h[13]
        m = Module('CIV.EXE overlay %d' % n, 'ovl%02d' % n, img,
                   {(s - OVL_SEG) * 16 + o for s, o in rel}, OVL_SEG, 'ovl')
        m.ovl = n
        mods.append(m)
        p += adv

    for fn in ('MISC.EXE', 'MGRAPHIC.EXE', 'EGRAPHIC.EXE', 'TGRAPHIC.EXE',
               'NSOUND.CVL', 'ISOUND.CVL', 'ASOUND.CVL', 'RSOUND.CVL',
               'TSOUND.CVL'):
        path = os.path.join(gamedir, fn)
        if not os.path.exists(path):
            continue
        data = open(path, 'rb').read()
        h, img, rel, _ = _parse_mz(data)
        cname = fn.split('.')[0].lower()
        m = Module(fn, cname, img, {s * 16 + o for s, o in rel}, 0, 'drv')
        mods.append(m)
    return mods
