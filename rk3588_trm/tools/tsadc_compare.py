#!/usr/bin/env python3
"""tsadc_compare.py — 讀 TSADC 7 個通道的原始碼（TSADC_DATA0~6，0xfec00000+0x2c+4n），
分別用 TRM Table 14-1 和 Linux 驅動 rk3588_code_table 線性內插成溫度，並對照 thermal_zone。需 root。"""
import subprocess
TRM = [(220, -40), (285, 25), (345, 85), (385, 125)]
DRV = [(215, -40), (285, 25), (350, 85), (395, 125)]
def interp(tab, c):
    for (c0, t0), (c1, t1) in zip(tab, tab[1:]):
        if c <= c1 or (c1, t1) == tab[-1]:
            return t0 + (c - c0) * (t1 - t0) / (c1 - c0)
out = subprocess.check_output(['python3', '/tmp/safe_mmio.py', 'dump', '0xfec0002c', '7']).decode().split()
codes = [int(w, 16) & 0xfff for w in out if len(w) == 8]
zones = ['soc', 'bigcore0', 'bigcore1', 'littlecore', 'center', 'gpu', 'npu']
for i, c in enumerate(codes):
    tz = int(open('/sys/class/thermal/thermal_zone%d/temp' % i).read()) / 1000
    print('ch%d %-10s code=%3d  TRM→%6.2f°C  driver→%6.2f°C  thermal_zone=%6.2f°C' % (i, zones[i], c, interp(TRM, c), interp(DRV, c), tz))
