#!/bin/bash
# sched_trace.sh —— 第 8/9 章的 ftrace 實驗（必須 root 執行）
#
#   ./sched_trace.sh switch     Q9~Q12, Q41~Q45, Ch9 Q8~Q10：一次進程切換的全貌
#   ./sched_trace.sh newtask    Q12, Ch9 Q11：新行程從 ret_from_fork 開始執行
#   ./sched_trace.sh tick       Q14：調度節拍 -> task_tick_fair -> check_preempt_tick
#   ./sched_trace.sh wakeup     Q28：唤醒時選了哪顆 CPU
#   ./sched_trace.sh balance    Q25~Q27, Ch9 Q5：負載均衡與行程遷移
#
set -u
T=/sys/kernel/debug/tracing
[ -w $T/trace ] || { echo "需要 root"; exit 1; }

reset() {
	echo 0 > $T/tracing_on
	echo nop > $T/current_tracer
	echo > $T/set_ftrace_filter
	echo > $T/set_graph_function
	echo > $T/set_event
	echo > $T/trace
	echo 0 > $T/options/funcgraph-proc 2>/dev/null
	echo 0 > $T/tracing_max_latency 2>/dev/null
	echo 0 > $T/events/enable
}

case "${1:-}" in
switch)
	reset
	echo "== sched_switch / sched_waking：一次完整的切換 =="
	echo 1 > $T/events/sched/sched_switch/enable
	echo 1 > $T/events/sched/sched_waking/enable
	echo 1 > $T/events/sched/sched_wakeup/enable
	echo 1 > $T/tracing_on
	# 兩個綁在同一顆 CPU 上的行程，強迫它們互相搶佔
	taskset -c 2 sh -c 'while :; do :; done' & A=$!
	taskset -c 2 sh -c 'while :; do :; done' & B=$!
	sleep 0.3
	echo 0 > $T/tracing_on
	kill $A $B 2>/dev/null
	grep -E "\[002\]" $T/trace | head -25
	;;

funcgraph)
	reset
	echo "== function_graph：__schedule 路徑上真正被呼叫到的函式 =="
	echo function_graph > $T/current_tracer
	echo 1 > $T/options/funcgraph-proc
	for f in schedule_tail finish_task_switch.isra.0 pick_next_task_fair \
	         check_and_switch_context select_task_rq_fair update_curr \
	         task_tick_fair scheduler_tick load_balance find_busiest_group; do
		echo "$f" >> $T/set_graph_function 2>/dev/null
	done
	echo 1 > $T/tracing_on
	taskset -c 2 sh -c 'while :; do :; done' & A=$!
	sleep 0.2
	echo 0 > $T/tracing_on
	kill $A 2>/dev/null
	head -40 $T/trace
	;;

newtask)
	reset
	echo "== 新行程的第一條指令：schedule_tail <- ret_from_fork =="
	echo function > $T/current_tracer
	echo 'schedule_tail' > $T/set_ftrace_filter
	echo 1 > $T/options/func_stack_trace
	echo 1 > $T/events/sched/sched_process_fork/enable
	echo 1 > $T/events/sched/sched_wakeup_new/enable
	echo 1 > $T/tracing_on
	/bin/true
	echo 0 > $T/tracing_on
	echo 0 > $T/options/func_stack_trace
	grep -A6 -E "schedule_tail|wakeup_new" $T/trace | head -40
	;;

tick)
	reset
	echo "== 調度節拍：scheduler_tick -> task_tick_fair =="
	echo function_graph > $T/current_tracer
	echo 'scheduler_tick' > $T/set_graph_function
	echo 1 > $T/tracing_on
	taskset -c 2 sh -c 'while :; do :; done' & A=$!
	sleep 0.05
	echo 0 > $T/tracing_on
	kill $A 2>/dev/null
	grep -A25 "scheduler_tick" $T/trace | head -35
	;;

wakeup)
	reset
	echo "== 唤醒：sched_waking(要醒的 CPU) -> sched_wakeup(落在哪) =="
	echo 1 > $T/events/sched/sched_waking/enable
	echo 1 > $T/events/sched/sched_wakeup/enable
	echo 1 > $T/events/sched/sched_migrate_task/enable
	echo "comm ~ \"wake_cpu\"" > $T/events/sched/sched_waking/filter 2>/dev/null
	echo 1 > $T/tracing_on
	sleep 1
	echo 0 > $T/tracing_on
	echo > $T/events/sched/sched_waking/filter
	grep -E "wake_cpu|pelt_duty" $T/trace | head -30
	;;

balance)
	reset
	echo "== 負載均衡：sched_migrate_task =="
	echo 1 > $T/events/sched/sched_migrate_task/enable
	echo 1 > $T/events/sched/sched_process_fork/enable
	echo 1 > $T/events/sched/sched_wakeup_new/enable
	echo > $T/trace
	echo 1 > $T/tracing_on
	"${2:-/tmp/lb_case}" ${3:-}
	sleep 2
	echo 0 > $T/tracing_on
	grep -E "migrate|wakeup_new|fork" $T/trace | head -40
	;;

*)
	sed -n '2,12p' "$0"
	;;
esac
