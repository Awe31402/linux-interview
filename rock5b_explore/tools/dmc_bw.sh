#!/bin/sh
# dmc_bw.sh — DDR 頻率 vs 記憶體頻寬/延遲。
# 先在預設 dmc_ondemand 下跑，再用 min_freq=max_freq 把 DDR 釘在每個 OPP 各跑一次。
# 需 root；結束還原 min/max。
D=/sys/class/devfreq/dmc
restore() { echo 528000000 > $D/min_freq; echo 2112000000 > $D/max_freq; }
trap restore EXIT INT TERM
run() {
	( i=0; while [ $i -lt 400 ]; do cat $D/cur_freq; sleep 0.05; i=$((i+1)); done ) > /tmp/dmc_trace.$1 &
	TP=$!
	echo "[$1] 1T(cpu4): $(/tmp/membw 1 4 256)"
	echo "[$1] 4T(cpu4-7): $(/tmp/membw 4 4 128)"
	kill $TP 2>/dev/null
	echo "[$1] dmc cur_freq during run: $(sort /tmp/dmc_trace.$1 | uniq -c | tr '\n' ' ')"
}
run ondemand
for f in 528000000 1068000000 1560000000 2112000000; do
	echo 528000000 > $D/min_freq; echo 2112000000 > $D/max_freq
	echo $f > $D/max_freq; echo $f > $D/min_freq
	sleep 0.5
	run $f
done
