#!/bin/sh
# fan_off_run.sh <秒數> <輸出檔> — 關風扇 + 全核滿載，記錄溫度與降頻；
# 任何溫區 >95°C 立刻停止；結束（或中斷）一律把風扇恢復成最大（state 4）。需 root。
T=${1:-600}; OUT=${2:-/tmp/run_fan0.csv}
FAN=/sys/class/thermal/cooling_device5
restore() { echo 4 > $FAN/cur_state; pkill -f "heat $T" ; }
trap restore EXIT INT TERM
echo 0 > $FAN/cur_state
/tmp/thermlog.sh $((T+120)) $OUT &
sleep 10
/tmp/heat $T > ${OUT%.csv}.heat.txt &
HP=$!
while kill -0 $HP 2>/dev/null; do
	for z in /sys/class/thermal/thermal_zone*/temp; do
		[ $(cat $z) -gt 95000 ] && { echo "ABORT $z $(cat $z)" >> ${OUT%.csv}.heat.txt; kill $HP; }
	done
	sleep 2
done
echo 4 > $FAN/cur_state
wait
