#!/bin/sh
# wdt_magic_test.sh — 驗證「magic close 之後，watchdog core 會替已停不下來的 DW WDT 自動餵狗」。
# 流程：python 開 /dev/watchdog0、設 60 s、寫 'V' 正常關閉 → 觀察 CCVR 150 秒，記錄每次被重設的時間。
# 保險：剩餘 < 20 秒還沒被重設 → 自己開裝置持續餵（此時裝置已關閉，所以打得開）。
# 必須用 setsid 在板子上脫離 ssh 執行：  sudo setsid sh /tmp/wdt_magic_test.sh </dev/null >/tmp/wdt_magic.log 2>&1 &
python3 - <<'PY'
import fcntl, os, struct
fd = os.open('/dev/watchdog0', os.O_WRONLY)
fcntl.ioctl(fd, 0xC0045706, struct.pack('i', 60), False)
os.write(fd, b'V'); os.close(fd)
print('opened, timeout=60, magic-closed')
PY
prev=0
for i in $(seq 1 150); do
	d=$((0x$(python3 /tmp/mmio.py get 0xfeaf0008)))
	[ $prev -ne 0 ] && [ $d -gt $prev ] && echo "t=${i}s kernel ping: $((prev / 24000000))s left -> $((d / 24000000))s"
	if [ $d -lt $((20 * 24000000)) ]; then
		echo "t=${i}s SAFETY feed"
		exec python3 -c "
import os,time
fd=os.open('/dev/watchdog0',os.O_WRONLY)
while True: os.write(fd,b'k'); time.sleep(10)"
	fi
	prev=$d; sleep 1
done
echo "done: CR=$(python3 /tmp/mmio.py get 0xfeaf0000)"
