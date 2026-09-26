#!/usr/bin/env python3
"""check_custom.py W H — 檢查 wbtest custom 情境寫回的 ARGB 漸層影像是否逐像素正確。"""
import sys
W, H = int(sys.argv[1]), int(sys.argv[2]); P = W * 4
d = open('/tmp/wb_custom.raw', 'rb').read()
bad = 0; worst = 0; first = None
for y in range(H):
    for x in range(W):
        o = y * P + x * 4
        b, g, r = d[o], d[o + 1], d[o + 2]
        e = (x * 255 // (W - 1), y * 255 // (H - 1), 64)
        dd = max(abs(r - e[0]), abs(g - e[1]), abs(b - e[2]))
        if dd:
            bad += 1; worst = max(worst, dd)
            if first is None: first = (x, y, (r, g, b), e)
print('pixels=%d  wrong=%d  worst=%d  first=%s' % (W * H, bad, worst, first))
