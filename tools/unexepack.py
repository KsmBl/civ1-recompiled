#!/usr/bin/env python3
"""Unpack a Microsoft EXEPACK-compressed MZ executable.

Usage: unexepack.py IN.EXE OUT.EXE

Only the resident image is unpacked; any data that follows the load image
(MS-LINK overlays) is appended unchanged after the new image.
"""
import struct
import sys


def unpack(data):
    hdr = struct.unpack('<14H', data[:28])
    e_cblp, e_cp, e_cparhdr = hdr[1], hdr[2], hdr[4]
    e_minalloc, e_maxalloc = hdr[5], hdr[6]
    e_ip, e_cs = hdr[10], hdr[11]
    img_end = (e_cp - 1) * 512 + e_cblp if e_cblp else e_cp * 512
    hdr_size = e_cparhdr * 16
    image = data[hdr_size:img_end]
    trailer = data[e_cp * 512:]      # overlays start on the next page

    ep = e_cs * 16                      # EXEPACK header offset in image
    if e_ip == 0x10:
        (real_ip, real_cs, _mem, ep_size, real_sp, real_ss,
         dest_len, sig) = struct.unpack('<8H', image[ep:ep + 16])
        skip_len = 1
    elif e_ip == 0x12:
        (real_ip, real_cs, _mem, ep_size, real_sp, real_ss,
         dest_len, skip_len, sig) = struct.unpack('<9H', image[ep:ep + 18])
    else:
        raise SystemExit('unknown EXEPACK header size')
    if sig != 0x4252:
        raise SystemExit('EXEPACK signature not found')

    # --- decompress (works backwards) ---
    packed_len = ep - (skip_len - 1) * 16
    buf = bytearray(max(dest_len * 16, ep))
    buf[:ep] = image[:ep]
    src = packed_len - 1
    while buf[src] == 0xFF:
        src -= 1
    dst = dest_len * 16 - 1
    while True:
        cmd = buf[src]; src -= 1
        cnt = (buf[src] << 8) | buf[src - 1]; src -= 2
        op = cmd & 0xFE
        if op == 0xB0:
            v = buf[src]; src -= 1
            for _ in range(cnt):
                buf[dst] = v; dst -= 1
        elif op == 0xB2:
            for _ in range(cnt):
                buf[dst] = buf[src]; dst -= 1; src -= 1
        else:
            raise SystemExit('bad EXEPACK command %02x at %x' % (cmd, src))
        if cmd & 1:
            break
    out_img = bytes(buf[:dest_len * 16])

    # --- relocations ---
    msg = image.find(b'Packed file is corrupt', ep)
    p = msg + len(b'Packed file is corrupt')
    relocs = []
    for seg in range(16):
        n, = struct.unpack('<H', image[p:p + 2]); p += 2
        for _ in range(n):
            off, = struct.unpack('<H', image[p:p + 2]); p += 2
            relocs.append((off, seg * 0x1000))

    # --- build new MZ ---
    nrel = len(relocs)
    hdr_len = 0x1C + 4 * nrel
    hdr_par = (hdr_len + 511) // 512 * 32
    total = hdr_par * 16 + len(out_img)
    # minalloc adjusted: packed image needed more memory for the unpacker
    extra = (len(image) + e_minalloc * 16) - len(out_img)
    min_par = max(0, (extra + 15) // 16)
    h = struct.pack('<14H', 0x5A4D, total % 512, (total + 511) // 512, nrel,
                    hdr_par, min_par, e_maxalloc, real_ss, real_sp, 0,
                    real_ip, real_cs, 0x1C, 0)
    h += b''.join(struct.pack('<HH', o, s) for o, s in relocs)
    h = h.ljust(hdr_par * 16, b'\0')
    return h + out_img, trailer, dict(cs=real_cs, ip=real_ip, ss=real_ss,
                                      sp=real_sp, relocs=nrel,
                                      size=len(out_img))


if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    exe, trailer, info = unpack(data)
    exe = exe.ljust((len(exe) + 511) // 512 * 512, b'\0')
    open(sys.argv[2], 'wb').write(exe + trailer)
    print(info, 'overlay bytes:', len(trailer))
