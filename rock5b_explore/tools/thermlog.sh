#!/bin/sh
# thermlog.sh <秒數> <輸出檔> — 每秒記一次：7 個溫區、3 個 CPU 叢集頻率、GPU/DMC 頻率、冷卻裝置狀態
T=${1:-60}; OUT=${2:-/tmp/therm.csv}
Z=/sys/class/thermal
echo "t,soc,big0,big1,little,center,gpu,npu,f_l,f_b0,f_b1,max_l,max_b0,max_b1,cd_l,cd_b0,cd_b1,fan" > $OUT
i=0; T0=$(cut -d" " -f1 /proc/uptime)
while [ $i -lt $T ]; do
	echo "$(awk -v t0=$T0 "{printf \"%.1f\", \$1-t0}" /proc/uptime),$(cat $Z/thermal_zone0/temp),$(cat $Z/thermal_zone1/temp),$(cat $Z/thermal_zone2/temp),$(cat $Z/thermal_zone3/temp),$(cat $Z/thermal_zone4/temp),$(cat $Z/thermal_zone5/temp),$(cat $Z/thermal_zone6/temp),$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq),$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq),$(cat /sys/devices/system/cpu/cpufreq/policy6/scaling_cur_freq),$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq),$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq),$(cat /sys/devices/system/cpu/cpufreq/policy6/scaling_max_freq),$(cat $Z/cooling_device2/cur_state),$(cat $Z/cooling_device3/cur_state),$(cat $Z/cooling_device4/cur_state),$(cat $Z/cooling_device5/cur_state)" >> $OUT
	i=$((i+1)); sleep 1
done
