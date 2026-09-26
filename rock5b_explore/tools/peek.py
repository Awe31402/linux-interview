# peek.py — 用 /dev/mem 唯讀讀取實體位址的 32-bit 暫存器。
# 用法：sudo python3 peek.py 基底:偏移:個數 ...   例：0xfd590000:0x14:2
# （CONFIG_STRICT_DEVMEM=y 但 IO_STRICT_DEVMEM 沒開，所以 MMIO 可讀）
import mmap, os, struct, sys
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
for arg in sys.argv[1:]:
    base, off, n = [int(x, 0) for x in arg.split(":")]
    m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
    print(hex(base), " ".join("%08x" % struct.unpack_from("<I", m, off + 4*i)[0] for i in range(n)))
