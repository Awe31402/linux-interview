#!/usr/bin/env python3
"""wdt_probe.py — 開啟 /dev/watchdog0 → 設 60 秒 → 讀 DW WDT 暫存器、量計數速度 → 餵狗 → magic close 停掉。
需 root。前提：CONFIG_WATCHDOG_NOWAYOUT 沒開（否則關不掉，會重開機）。"""
import fcntl, mmap, os, struct, time
WDIOC_SETTIMEOUT, WDIOC_GETTIMEOUT, WDIOC_GETTIMELEFT = 0xC0045706, 0x80045707, 0x8004570A
mfd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(mfd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0xfeaf0000)
r = lambda o: struct.unpack_from('<I', m, o)[0]
regs = lambda: 'CR=%08x TORR=%08x CCVR=%08x STAT=%x' % (r(0), r(4), r(8), r(0x10))
print('before open :', regs())
fd = os.open('/dev/watchdog0', os.O_WRONLY)
try:
    print('after open  :', regs())
    b = struct.pack('i', 60); fcntl.ioctl(fd, WDIOC_SETTIMEOUT, b, False)
    t = bytearray(4); fcntl.ioctl(fd, WDIOC_GETTIMEOUT, t, True)
    print('timeout set 60 -> driver says %d s' % struct.unpack('i', t)[0])
    print('after set   :', regs())
    a, t0 = r(8), time.monotonic(); time.sleep(1.0); b2, t1 = r(8), time.monotonic()
    print('CCVR %d -> %d in %.3fs : %.3f MHz' % (a, b2, t1 - t0, (a - b2) / (t1 - t0) / 1e6))
    os.write(fd, b'k')          # 餵狗
    print('after kick  :', regs())
    tl = bytearray(4); fcntl.ioctl(fd, WDIOC_GETTIMELEFT, tl, True)
    print('timeleft = %d s' % struct.unpack('i', tl)[0])
finally:
    os.write(fd, b'V')          # magic close
    os.close(fd)
time.sleep(0.2)
print('after close :', regs())
