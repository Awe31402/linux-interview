#!/bin/sh
# i2c_ev.sh — 用 i2c/* 與 power/cpu_frequency tracepoint 數一次調頻（408→2256 MHz）的 I2C 交易。需 root；跑完還原。輸出 /tmp/i2c_ev.txt
P=/sys/devices/system/cpu/cpufreq/policy4; T=/sys/kernel/tracing
echo userspace > $P/scaling_governor; echo 408000 > $P/scaling_setspeed; sleep 0.3
echo 0 > $T/tracing_on; echo > $T/trace; echo nop > $T/current_tracer
echo 'adapter_nr==0' > $T/events/i2c/filter
echo 1 > $T/events/i2c/i2c_write/enable; echo 1 > $T/events/i2c/i2c_reply/enable; echo 1 > $T/events/i2c/i2c_result/enable
echo 1 > $T/events/power/cpu_frequency/enable
echo 1 > $T/tracing_on
echo 2256000 > $P/scaling_setspeed; sleep 0.3
echo 0 > $T/tracing_on
cat $T/trace > /tmp/i2c_ev.txt
echo 0 > $T/events/i2c/enable; echo 0 > $T/events/power/cpu_frequency/enable; echo 0 > $T/events/i2c/filter
echo ondemand > $P/scaling_governor
