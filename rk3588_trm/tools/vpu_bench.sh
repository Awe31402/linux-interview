#!/bin/sh
# vpu_bench.sh — 用 GStreamer + Rockchip MPP 量硬體編解碼速度，並記錄是哪個硬體核心在做事（/proc/interrupts 差值）。
# 先量「只產生畫面不編碼」的基準，確定瓶頸不是 videotestsrc。不需要 root（讀 interrupts 不用）。
N=${N:-300}
irq() { grep -E "rkvenc|rkvdec|jpeg|vepu|vdpu|av1|iep|rga" /proc/interrupts | awk '{s=0; for(i=2;i<=9;i++) s+=$i; print $NF, s}'; }
run() { # $1=名稱 $2...=pipeline
	name=$1; shift
	irq > /tmp/irq_a
	t0=$(date +%s.%N); gst-launch-1.0 -q "$@" >/dev/null 2>&1; rc=$?; t1=$(date +%s.%N)
	irq > /tmp/irq_b
	fps=$(echo "$N / ($t1 - $t0)" | bc -l)
	printf "%-28s rc=%d  %6.1f fps   irq: %s\n" "$name" $rc "$fps" "$(join /tmp/irq_a /tmp/irq_b | awk '$3>$2{printf "%s+%d ", $1, $3-$2}')"
}
for R in "1920 1080" "3840 2160"; do
	set -- $R; W=$1; H=$2
	SRC="videotestsrc num-buffers=$N pattern=smpte ! video/x-raw,format=NV12,width=$W,height=$H,framerate=60/1"
	run "src only ${W}x$H" $SRC ! fakesink
	run "h264 enc ${W}x$H" $SRC ! mpph264enc ! fakesink
	run "h265 enc ${W}x$H" $SRC ! mpph265enc ! fakesink
	gst-launch-1.0 -q $SRC ! mpph265enc ! h265parse ! filesink location=/tmp/t_${H}.h265 >/dev/null 2>&1
	gst-launch-1.0 -q $SRC ! mpph264enc ! h264parse ! filesink location=/tmp/t_${H}.h264 >/dev/null 2>&1
	run "h265 dec ${W}x$H" filesrc location=/tmp/t_${H}.h265 ! h265parse ! mppvideodec ! fakesink
	run "h264 dec ${W}x$H" filesrc location=/tmp/t_${H}.h264 ! h264parse ! mppvideodec ! fakesink
	run "jpeg enc ${W}x$H" $SRC ! mppjpegenc ! fakesink
done
