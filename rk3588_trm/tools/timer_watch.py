#!/usr/bin/env python3
"""timer_watch.py — 高速輪詢 TIMER_NS_0 ch0（0xfeae0000）的 CONTROL 與 CURRENT_VALUE0，
抓到它被 Linux 當 broadcast clockevent 啟動的瞬間，用兩次讀值估算計數頻率。唯讀。"""
import mmap, os, struct, time
fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0xfeae0000)
r = lambda o: struct.unpack_from('<I', m, o)[0]
armed, pairs = 0, []
t_end = time.monotonic() + 3
last = None
while time.monotonic() < t_end:
    t = time.perf_counter_ns(); ctl = r(0x10); cur = r(0x08); ld = r(0x00)
    if ctl & 1:
        armed += 1
        if last and last[2] == ld and cur < last[1]:
            pairs.append(((last[1] - cur) / ((t - last[0]) / 1e9), ld))
        last = (t, cur, ld)
    else:
        last = None
print('armed samples:', armed)
if pairs:
    pairs.sort()
    print('rate samples:', len(pairs), ' median = %.3f MHz' % (pairs[len(pairs) // 2][0] / 1e6),
          ' ctl example load=%d ticks' % pairs[0][1])
