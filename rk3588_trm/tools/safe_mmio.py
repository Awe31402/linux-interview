#!/usr/bin/env python3
"""safe_mmio.py — 有「守門」的唯讀 MMIO。需 root。
讀之前檢查：
  1. 位址落在某個 DT 節點的 reg 範圍內（/sys/firmware/devicetree/base 下的直接子節點）
  2. 該節點對應的 platform device 已綁定驅動
  3. runtime PM 狀態是 active（或 unsupported＝驅動不做 runtime PM，視為常開）
  4. clk_summary 裡這個 device 當 consumer 的每一個時脈 enable_count > 0
  5. 若有電源域，pm_genpd 顯示 on
任何一項不過就拒絕（除非 --force，且只該用在 GRF/syscon 這種常開區塊）。
背景：2026-09-26 直接讀 VOP QoS（0xfdf82000）把整台板子卡死，電源域明明是 on。
用法：safe_mmio.py [--force] dump <phys> <n> | get <phys>"""
import mmap, os, re, struct, sys
BASE = '/sys/firmware/devicetree/base'
def rd(p):
    try: return open(p, 'rb').read()
    except OSError: return None
def find_node(pa):
    for n in os.listdir(BASE):
        reg = rd(os.path.join(BASE, n, 'reg'))
        if not reg or len(reg) < 16: continue
        v = struct.unpack('>%dI' % (len(reg) // 4), reg)
        for i in range(0, len(v) - 3, 4):
            a, s = (v[i] << 32) | v[i + 1], (v[i + 2] << 32) | v[i + 3]
            if a <= pa < a + s: return n, a
    return None, None
def check(pa):
    n, a = find_node(pa)
    if not n: return 'no DT node covers %#x' % pa
    dev = '%x.%s' % (a, n.split('@')[0])
    p = '/sys/bus/platform/devices/' + dev
    if not os.path.exists(p + '/driver'): return '%s: no driver bound' % dev
    rpm = (rd(p + '/power/runtime_status') or b'').decode().strip()
    if rpm not in ('active', 'unsupported'): return '%s: runtime_status=%s' % (dev, rpm)
    bad = []
    for line in open('/sys/kernel/debug/clk/clk_summary'):
        f = line.split()
        if len(f) >= 9 and dev in f[7:]:
            if f[1] == '0': bad.append(f[0])
    if bad: return '%s: clocks disabled: %s' % (dev, ','.join(bad))
    return None
args = sys.argv[1:]
force = args[0] == '--force'
if force: args = args[1:]
cmd, pa = args[0], int(args[1], 0)
why = check(pa)
if why and not force:
    print('REFUSED:', why); sys.exit(2)
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
def r(x):
    b = x & ~0xfff; m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=b)
    v = struct.unpack_from('<I', m, x - b)[0]; m.close(); return v
if cmd == 'get': print('%08x' % r(pa))
else:
    n = int(args[2], 0)
    for i in range(0, n, 4):
        print('%08x: ' % (pa + 4 * i) + ' '.join('%08x' % r(pa + 4 * (i + j)) for j in range(min(4, n - i))))
