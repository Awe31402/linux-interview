#!/bin/sh
# pvtpll_sweep.sh — 大核叢集 2（policy6，BIGCORE1_GRF 0xfd592000）逐一設定頻率，
# 讀 PVTPLL CON0_L/CON0_H/CON1/CON2/CON3/STATUS0/STATUS1 與電壓。需 root；結束還原 ondemand。
P=/sys/devices/system/cpu/cpufreq/policy6
echo userspace > $P/scaling_governor
printf "%8s %6s  %8s %8s %8s %8s %8s  %6s %6s  ring_sel len\n" freq mV CON0_L CON0_H CON1 CON2 CON3 osc avg
for f in $(cat $P/scaling_available_frequencies); do
	echo $f > $P/scaling_setspeed; sleep 0.3
	set -- $(python3 /tmp/safe_mmio.py --force dump 0xfd592000 7 | sed 's/^[0-9a-f]*: //' | tr '\n' ' ')
	mv=$(( $(cat /sys/class/regulator/regulator.31/microvolts) / 1000 ))
	printf "%8d %6d  %8s %8s %8s %8s %8s  %6d %6d  %d %d\n" $((f/1000)) $mv $1 $2 $3 $4 $5 $((0x$6)) $((0x$7)) $(((0x$1>>8)&7)) $((0x$2&0x3f))
done
echo ondemand > $P/scaling_governor
