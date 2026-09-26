#!/usr/bin/env python3
"""to_png.py <raw> <out.png> [縮小倍數=4] — 把 wbtest 的 ARGB8888 1920x1080 寫回影像轉成縮小的 PNG（純標準庫）。"""
import struct, sys, zlib
raw, out = sys.argv[1], sys.argv[2]; k = int(sys.argv[3]) if len(sys.argv) > 3 else 4
d = open(raw, 'rb').read(); W, H, P = 1920, 1080, 7680
w, h = W // k, H // k
rows = b''.join(b'\0' + bytes(c for x in range(w) for c in (d[y * k * P + x * k * 4 + 2], d[y * k * P + x * k * 4 + 1], d[y * k * P + x * k * 4])) for y in range(h))
chunk = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))
