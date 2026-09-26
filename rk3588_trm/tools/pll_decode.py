#!/usr/bin/env python3
"""pll_decode.py — 讀 CRU 的 FRACPLL 暫存器，依 TRM §2.3.3.1 公式算出頻率，對照 Linux clk_summary。
  Fvco = 24 MHz × (M + K/65536) / P ；Fout = Fvco / 2^S   （K 為 16-bit 二補數）
CRU（0xfd7c0000）是常開區塊；用 --force 繞過 safe_mmio 的驅動檢查（CRU 走 CLK_OF_DECLARE，沒有 platform driver）。"""
import subprocess
CRU = 0xfd7c0000
PLLS = {'v0pll': 0x160, 'aupll': 0x180, 'cpll': 0x1a0, 'gpll': 0x1c0, 'npll': 0x1e0}
def rd(a):
    return int(subprocess.check_output(['python3', '/tmp/safe_mmio.py', '--force', 'get', hex(a)]).decode().split()[-1], 16)
summ = {l.split()[0]: int(l.split()[4]) for l in open('/sys/kernel/debug/clk/clk_summary') if len(l.split()) > 5 and l.split()[0] in PLLS}
print('%-6s %8s %8s %8s  %5s %4s %2s %6s  %14s  %14s' % ('pll', 'CON0', 'CON1', 'CON2', 'M', 'P', 'S', 'K', 'calc Hz', 'linux Hz'))
for n, off in PLLS.items():
    c0, c1, c2 = rd(CRU + off), rd(CRU + off + 4), rd(CRU + off + 8)
    m, bp = c0 & 0x3ff, (c0 >> 15) & 1
    p, s, rstb = c1 & 0x3f, (c1 >> 6) & 7, (c1 >> 13) & 1
    k = c2 & 0xffff
    k = k - 0x10000 if k & 0x8000 else k
    f = 24e6 * (m + k / 65536) / p / (1 << s) if p else 0
    if bp: f = 24e6
    print('%-6s %08x %08x %08x  %5d %4d %2d %6d  %14.0f  %14d%s' % (n, c0, c1, c2, m, p, s, k, f, summ.get(n, -1), '  (power-down)' if rstb else ''))
