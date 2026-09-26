#!/bin/sh
# reg_trace.sh — function_graph 追 regulator_set_voltage 內部（大核 408→2256 MHz 一次）。需 root；跑完還原 ondemand。輸出 /tmp/reg_trace.txt
P=/sys/devices/system/cpu/cpufreq/policy4; T=/sys/kernel/tracing
echo userspace > $P/scaling_governor; echo 408000 > $P/scaling_setspeed; sleep 0.3
echo 0 > $T/tracing_on; echo > $T/trace
echo function_graph > $T/current_tracer
echo regulator_set_voltage > $T/set_graph_function
echo 12 > $T/max_graph_depth
echo 1 > $T/tracing_on
echo 2256000 > $P/scaling_setspeed; sleep 0.3
echo 0 > $T/tracing_on
cat $T/trace > /tmp/reg_trace.txt
echo nop > $T/current_tracer; echo > $T/set_graph_function; echo 0 > $T/max_graph_depth
echo ondemand > $P/scaling_governor
