#!/bin/sh
# mpp_timing.sh — 打開 /proc/mpp_service 的 timing，編 20 幀，從 dmesg 取每個任務的「硬體執行時間」（run→irq）
# 與「兩個任務的建立間隔」。需 root；結束會關掉 timing。
S=/proc/mpp_service
SRC="videotestsrc num-buffers=20 pattern=smpte ! video/x-raw,format=NV12,width=1920,height=1080,framerate=60/1"
echo 1 > $S/timing_en
for c in rkvenc-core0 rkvenc-core1 jpege-core0 jpege-core1 jpege-core2 jpege-core3; do echo 1 > $S/$c/timing_check; done
for e in mpph265enc mpph264enc mppjpegenc; do
	dmesg -C
	gst-launch-1.0 -q $SRC ! $e ! fakesink >/dev/null 2>&1
	dmesg | awk -v e=$e '
		/timing: create /     {c=$(NF-1)}
		/timing: run  /       {r=$(NF-1)}
		/timing: irq /        {q=$(NF-1); n++; hw+=q; if (pc) gap+=c-pc; pc=c}
		END { if (n) printf "%-11s tasks=%d  hw(run→irq) avg=%.0f us  create-gap avg=%.0f us\n", e, n, hw/n, (n>1?gap/(n-1):0) }'
done
echo 0 > $S/timing_en
for c in rkvenc-core0 rkvenc-core1 jpege-core0 jpege-core1 jpege-core2 jpege-core3; do echo 0 > $S/$c/timing_check; done
