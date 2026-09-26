#!/usr/bin/env python3
"""mmio.py — 唯讀 MMIO 工具（/dev/mem）。需 root。
  mmio.py dump  <phys> <n_words>            連續印 n 個 32-bit 字
  mmio.py rate  <phys> <ms>                 同一位址隔 ms 毫秒讀兩次，印出差值與每秒變化量
  mmio.py get   <phys>                      讀一個字
安全規則：只讀「時脈與電源域都開著」的模組；關著的區塊讀了可能讓匯流排卡死。"""
import mmap, os, struct, sys, time
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
def rd(pa):
    base = pa & ~0xfff
    m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
    v = struct.unpack_from('<I', m, pa - base)[0]
    m.close()
    return v
cmd, pa = sys.argv[1], int(sys.argv[2], 0)
if cmd == 'get':
    print('%08x' % rd(pa))
elif cmd == 'dump':
    n = int(sys.argv[3], 0)
    for i in range(0, n, 4):
        print('%08x: ' % (pa + 4 * i) + ' '.join('%08x' % rd(pa + 4 * (i + j)) for j in range(min(4, n - i))))
elif cmd == 'rate':
    ms = float(sys.argv[3])
    t0 = time.monotonic(); a = rd(pa); time.sleep(ms / 1000); b = rd(pa); t1 = time.monotonic()
    d = (b - a) & 0xffffffff
    print('%08x -> %08x  delta=%d (%+d signed)  dt=%.6fs  rate=%.0f/s' % (a, b, d, d - (1 << 32) if d >= 1 << 31 else d, t1 - t0, d / (t1 - t0)))
