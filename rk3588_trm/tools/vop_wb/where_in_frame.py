#!/usr/bin/env python3
"""where_in_frame.py HTOTAL VTOTAL — 從 /tmp/vop_irq_trace.txt 算每個 POST_BUF_EMPTY 發生在一格的第幾行。
像素率固定 37.125 MHz（4 × 9.28125 MHz，見筆記）。FS（frame start）時間當作該格的起點。"""
import re, sys, collections
ht, vt = int(sys.argv[1]), int(sys.argv[2]); line_us = ht / 37.125
vp = None; last_fs = None; lines = collections.Counter(); per_irq = collections.Counter()
for l in open('/tmp/vop_irq_trace.txt'):
    m = re.search(r'\s(\d+\.\d+): vpin:.*vp=(-?\d+)', l)
    if m: vp = int(m.group(2)); continue
    m = re.search(r'\s(\d+\.\d+): vpout:.*ret=0x([0-9a-f]+)', l)
    if not m or vp != 0: continue
    t, r = float(m.group(1)), int(m.group(2), 16)
    if r & 0x8000: last_fs = t
    if r & 0x1000 and last_fs is not None:
        ln = int((t - last_fs) * 1e6 / line_us); lines[ln] += 1; per_irq[r] += 1
print('line_us=%.2f  status 組合=%s' % (line_us, {hex(k): v for k, v in per_irq.items()}))
print('發生在第幾行（相對 FS）：', sorted(lines.items())[:12], '...' if len(lines) > 12 else '')
