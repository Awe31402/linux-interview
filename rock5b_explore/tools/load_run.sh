#!/bin/sh
# load_run.sh <秒數> <輸出檔> — 全核滿載 + 記錄溫度/頻率/冷卻狀態（不動風扇與 governor）。
# 任何溫區 >95°C 立刻停止。需 root（讀 cooling_device 不需要，但保險起見一起用 sudo）。
T=${1:-480}; OUT=${2:-/tmp/run.csv}
/tmp/thermlog.sh $((T+90)) $OUT &
sleep 10
/tmp/heat $T > ${OUT%.csv}.heat.txt &
HP=$!
while kill -0 $HP 2>/dev/null; do
	for z in /sys/class/thermal/thermal_zone*/temp; do
		[ $(cat $z) -gt 95000 ] && { echo "ABORT $z $(cat $z)" >> ${OUT%.csv}.heat.txt; kill $HP; }
	done
	sleep 2
done
wait
