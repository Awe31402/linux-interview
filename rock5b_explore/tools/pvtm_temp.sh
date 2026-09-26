#!/bin/sh
# pvtm_temp.sh <秒數> <輸出檔> — 把大核叢集1（cpu4-5）固定在 1608 MHz，關風扇，
# 用其他 6 顆核心加熱，每秒記錄：溫度、叢集1電壓、PVTPLL 計數（GRF 0xfd590000+0x18）。
# 用來驗證 DT 的 rockchip,pvtm-temp-prop = <270 270>（每 °C 0.27）。
# 需 root；結束一律還原 governor=ondemand、風扇 state 4。任何溫區 >88°C 立即停止加熱。
T=${1:-420}; OUT=${2:-/tmp/pvtm_temp.csv}
P=/sys/devices/system/cpu/cpufreq/policy4
FAN=/sys/class/thermal/cooling_device5
restore() { pkill -x heat; echo ondemand > $P/scaling_governor; echo 4 > $FAN/cur_state; }
trap restore EXIT INT TERM
echo userspace > $P/scaling_governor; echo 1608000 > $P/scaling_setspeed
echo 0 > $FAN/cur_state
/tmp/heat $T 0,1,2,3,6,7 > /dev/null &
echo "t,soc,big0,uV,f_cur,c14,c18" > $OUT
i=0
while [ $i -lt $((T+120)) ]; do
	[ $i -eq $T ] && echo 4 > $FAN/cur_state
	for z in /sys/class/thermal/thermal_zone*/temp; do [ $(cat $z) -gt 88000 ] && pkill -x heat; done
	c=$(python3 /tmp/peek.py 0xfd590000:0x14:2 | awk '{printf "%d,%d", "0x"$2, "0x"$3}')
	echo "$i,$(cat /sys/class/thermal/thermal_zone0/temp),$(cat /sys/class/thermal/thermal_zone1/temp),$(cat /sys/class/regulator/regulator.30/microvolts),$(cat $P/scaling_cur_freq),$c" >> $OUT
	i=$((i+1)); sleep 1
done
