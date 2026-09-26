#!/bin/sh
# wdt_kernel_ping.sh — 停掉使用者態餵狗程式後，觀察核心 watchdog core 會不會自己餵（HW_RUNNING 路徑）。
# 保險：CCVR 掉到 0x20000000（約剩 22 秒）以下還沒被重設，就立刻自己重新餵。需 root。
pkill -f 'dev/watchdog0' ; sleep 0.5
prev=0
for i in $(seq 1 70); do
	v=$(python3 /tmp/mmio.py get 0xfeaf0008); d=$((0x$v))
	[ $d -gt $prev ] && [ $prev -ne 0 ] && echo "t=${i}s  RESET seen: $prev -> $d (kernel pinged)"
	[ $((i % 10)) -eq 0 ] && echo "t=${i}s  CCVR=$v ($((d / 24000000)) s left)"
	if [ $d -lt $((0x20000000)) ]; then echo "SAFETY: low counter, feeding"; exec python3 -c "
import os,time
fd=os.open('/dev/watchdog0',os.O_WRONLY)
while True: os.write(fd,b'k'); time.sleep(10)"; fi
	prev=$d; sleep 1
done
