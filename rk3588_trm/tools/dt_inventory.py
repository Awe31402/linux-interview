#!/usr/bin/env python3
"""dt_inventory.py — 在板子上跑：列出執行中 DT 裡每個有 reg 的節點
（位址、名稱、status、compatible、綁定的驅動、runtime PM 狀態），輸出 CSV。
只讀 /sys，不碰硬體。"""
import os, struct
BASE = '/sys/firmware/devicetree/base'
def rd(p):
    try: return open(p, 'rb').read()
    except OSError: return None
def cells(p, dflt):
    b = rd(p); return struct.unpack('>I', b)[0] if b else dflt
rows = []
def walk(path, ac, sc, depth):
    for n in sorted(os.listdir(path)):
        p = os.path.join(path, n)
        if not os.path.isdir(p): continue
        reg = rd(p + '/reg')
        comp = (rd(p + '/compatible') or b'').split(b'\0')[0].decode(errors='replace')
        st = (rd(p + '/status') or b'okay\0').rstrip(b'\0').decode()
        if reg and depth == 0 and len(reg) >= 4 * (ac + sc):
            v = struct.unpack('>%dI' % (len(reg) // 4), reg)
            addr = 0
            for c in v[:ac]: addr = (addr << 32) | c
            size = 0
            for c in v[ac:ac + sc]: size = (size << 32) | c
            dev = '/sys/bus/platform/devices/%x.%s' % (addr, n.split('@')[0])
            drv = os.path.basename(os.readlink(dev + '/driver')) if os.path.exists(dev + '/driver') else ''
            rpm = (rd(dev + '/power/runtime_status') or b'').decode().strip()
            rows.append((addr, size, n, st, comp, drv, rpm))
        if depth == 0 and n in ('firmware', 'cpus', '__symbols__', 'aliases', 'chosen'): continue
        # 只走 root 的直接子節點（SoC 外設都在 root 下，#address-cells=2）
walk(BASE, cells(BASE + '/#address-cells', 2), cells(BASE + '/#size-cells', 2), 0)
print('addr,size,node,status,compatible,driver,runtime_pm')
for r in sorted(rows):
    print('%08x,%x,%s,%s,%s,%s,%s' % r)
