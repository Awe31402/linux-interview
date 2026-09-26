#!/usr/bin/env python3
"""rng_quality.py <檔案> — 快速檢查亂數：byte 分佈卡方、Shannon 熵、位元 1 的比例、zlib 壓縮率、連續相同位元組。"""
import math, sys, zlib, collections
d = open(sys.argv[1], 'rb').read(); n = len(d)
c = collections.Counter(d); e = n / 256
chi = sum((c.get(i, 0) - e) ** 2 / e for i in range(256))
H = -sum(v / n * math.log2(v / n) for v in c.values())
ones = sum(bin(b).count('1') for b in d) / (8 * n)
runs = sum(1 for a, b in zip(d, d[1:]) if a == b)
print('bytes=%d  chi2(255 dof)=%.1f  entropy=%.5f bit/byte  ones=%.5f  zlib=%.4f  repeat-pairs=%d (expect %.0f)'
      % (n, chi, H, ones, len(zlib.compress(d, 9)) / n, runs, (n - 1) / 256))
