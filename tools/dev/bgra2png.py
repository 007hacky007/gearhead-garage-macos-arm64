#!/usr/bin/env python3
"""Development helper: convert a raw 640x480 BGRA dump to PNG. Usage: bgra2png.py IN OUT [W H]"""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
w, h = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (640, 480)
rows = []
for y in range(h):
    row = bytearray(1 + 3 * w)
    for x in range(w):
        o = (y * w + x) * 4
        row[1 + 3 * x:4 + 3 * x] = bytes((d[o + 2], d[o + 1], d[o]))
    rows.append(bytes(row))
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))
