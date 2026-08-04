#!/bin/bash
# irq_trace.sh —— 卷2 第 2 章「中斷管理」的 ftrace 實驗腳本
# 平台：Radxa ROCK 5B (RK3588)，Linux 6.1.115+
#
#   sudo ./irq_trace.sh path        Q3：一個真實硬體中斷的完整呼叫鏈（function_graph）
#   sudo ./irq_trace.sh events      Q3/Q5/Q7：irq / softirq / tasklet / workqueue tracepoints
#   sudo ./irq_trace.sh softirq     Q5/Q7/Q8：軟中斷在哪個 comm 裡跑、跑多久
#   sudo ./irq_trace.sh wq          Q12/Q13：workqueue 的 worker 動態
#   sudo ./irq_trace.sh pools       Q11/Q12：系統上的 workqueue 數 vs kworker 執行緒數
#   sudo ./irq_trace.sh irqs        Q2：/proc/interrupts ↔ DTS ↔ TRM 對帳
#   sudo ./irq_trace.sh off         關掉 tracing、還原

set -u
T=/sys/kernel/debug/tracing
[ -d "$T" ] || T=/sys/kernel/tracing

need_root() { [ "$(id -u)" = 0 ] || { echo "請用 sudo 執行"; exit 1; }; }
need_root

reset() {
    echo 0 > $T/tracing_on
    echo nop > $T/current_tracer
    echo > $T/set_ftrace_filter
    echo > $T/set_graph_function
    echo 0 > $T/events/enable
    echo > $T/trace
    echo 0 > $T/options/funcgraph-proc  2>/dev/null
    echo 0 > $T/options/funcgraph-abstime 2>/dev/null
}

case "${1:-path}" in

path)
    # Q3：抓 gic_handle_irq 往下的完整呼叫鏈
    reset
    echo 8 > $T/max_graph_depth
    echo function_graph > $T/current_tracer
    echo gic_handle_irq > $T/set_graph_function
    echo 1 > $T/options/funcgraph-proc
    echo 1 > $T/options/funcgraph-abstime
    echo > $T/trace
    echo 1 > $T/tracing_on
    # 製造一點 I/O + 網路中斷
    dd if=/dev/nvme0n1 of=/dev/null bs=1M count=64 iflag=direct 2>/dev/null
    sleep 0.2
    echo 0 > $T/tracing_on
    echo "=== Q3：gic_handle_irq 往下的呼叫鏈（function_graph, depth 8）==="
    head -n 120 $T/trace
    ;;

events)
    reset
    for e in irq/irq_handler_entry irq/irq_handler_exit \
             irq/softirq_raise irq/softirq_entry irq/softirq_exit \
             workqueue/workqueue_queue_work workqueue/workqueue_execute_start \
             workqueue/workqueue_execute_end; do
        echo 1 > $T/events/$e/enable 2>/dev/null
    done
    echo > $T/trace
    echo 1 > $T/tracing_on
    dd if=/dev/nvme0n1 of=/dev/null bs=1M count=32 iflag=direct 2>/dev/null
    ping -c 2 -W 1 127.0.0.1 >/dev/null 2>&1
    sleep 0.3
    echo 0 > $T/tracing_on
    echo "=== irq / softirq / workqueue tracepoints ==="
    head -n 150 $T/trace
    ;;

softirq)
    # Q5/Q7/Q8：軟中斷在誰的身上跑
    reset
    echo 1 > $T/events/irq/softirq_entry/enable
    echo 1 > $T/events/irq/softirq_exit/enable
    echo 1 > $T/events/irq/irq_handler_entry/enable
    echo > $T/trace
    echo 1 > $T/tracing_on
    dd if=/dev/nvme0n1 of=/dev/null bs=1M count=64 iflag=direct 2>/dev/null
    sleep 0.3
    echo 0 > $T/tracing_on
    echo "=== 軟中斷在哪個行程/上下文裡執行（看 comm 欄）==="
    grep -E "softirq_(entry|exit)" $T/trace | head -n 60
    echo
    echo "=== 各 comm 執行軟中斷的次數 ==="
    grep "softirq_entry" $T/trace | awk '{print $1}' | sed 's/-[0-9]*$//' | sort | uniq -c | sort -rn | head
    echo
    echo "=== 各軟中斷類型的次數 ==="
    grep "softirq_entry" $T/trace | sed 's/.*vec=\([0-9]*\).*\[action=\([A-Z_]*\)\].*/\1 \2/' | sort | uniq -c | sort -rn
    ;;

wq)
    reset
    echo 1 > $T/events/workqueue/enable
    echo 1 > $T/events/sched/sched_switch/enable
    echo > $T/trace
    echo 1 > $T/tracing_on
    sleep 2
    echo 0 > $T/tracing_on
    echo "=== workqueue tracepoints ==="
    grep workqueue $T/trace | head -n 80
    ;;

pools)
    echo "=== Q11/Q12：系統上有多少個 workqueue？多少個 kworker 執行緒？ ==="
    n_wq=$(ls /sys/bus/workqueue/devices | wc -l)
    n_kw=$(ps -eo comm | grep -c '^kworker/')
    n_cpu=$(nproc)
    echo "workqueue 數量（/sys/bus/workqueue/devices）= $n_wq"
    echo "kworker 執行緒數                            = $n_kw"
    echo "CPU 數                                      = $n_cpu"
    echo "舊機制（每個 workqueue 每個 CPU 一個線程）需要 = $((n_wq * n_cpu)) 個線程"
    echo
    echo "--- workqueue 清單 ---"
    ls /sys/bus/workqueue/devices | tr '\n' ' '; echo
    echo
    echo "--- kworker 執行緒（依 CPU 分組）---"
    ps -eo pid,comm | grep '^ *[0-9]* kworker/' | sort -k2 | awk '{print $2}' | \
        sed 's/:.*//' | sort | uniq -c
    echo
    echo "--- 每個 workqueue 的屬性 ---"
    printf "%-24s %-10s %-8s\n" NAME max_active per_cpu
    for d in /sys/bus/workqueue/devices/*; do
        printf "%-24s %-10s %-8s\n" "$(basename $d)" \
            "$(cat $d/max_active 2>/dev/null)" "$(cat $d/per_cpu 2>/dev/null)"
    done
    ;;

irqs)
    echo "=== Q2：/proc/interrupts（virq / hwirq / chip / name）==="
    cat /proc/interrupts
    echo
    echo "=== 從 DTB 反查幾個外設的 interrupts 屬性（hwirq = 值 + 32）==="
    for n in mmc@fe2e0000 serial@feb50000 gpio@fd8a0000; do
        f=$(find /sys/firmware/devicetree/base -maxdepth 2 -name "$n" 2>/dev/null | head -1)
        [ -n "$f" ] || continue
        echo -n "$n  interrupts = "
        hexdump -e '4/1 "%02x" " "' "$f/interrupts" 2>/dev/null; echo
    done
    ;;

off)
    reset
    echo "已還原"
    ;;

*)
    sed -n '2,20p' "$0"
    ;;
esac
