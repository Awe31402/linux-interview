#!/bin/sh
# dvfs_trace.sh — 用 function_graph 量一次 CPU 調頻（大核 policy4）每一段各花多久：
#   cpufreq → OPP 核心 → regulator(I2C 調電壓) → SCMI(SMC 陷入 BL31 改 PLL)
# 需 root。跑完自動還原 governor 與 ftrace。
P=/sys/devices/system/cpu/cpufreq/policy4
T=/sys/kernel/tracing
OLD=$(cat $P/scaling_governor)
echo userspace > $P/scaling_governor
echo 0 > $T/tracing_on; echo > $T/trace
echo function_graph > $T/current_tracer
echo 'dev_pm_opp_set_rate' > $T/set_graph_function
echo 6 > $T/max_graph_depth
echo 1 > $T/tracing_on
for f in 408000 2256000 408000 2256000 1800000 1608000 2256000; do
	echo $f > $P/scaling_setspeed; sleep 0.2
done
echo 0 > $T/tracing_on
cat $T/trace
echo nop > $T/current_tracer; echo > $T/set_graph_function; echo 0 > $T/max_graph_depth
echo $OLD > $P/scaling_governor
