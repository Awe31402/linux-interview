#!/bin/bash
# 在機台上重跑 ch11 的全部實驗，每段之間清 dmesg，輸出到 stdout
echo radxa | sudo -S -v 2>/dev/null   # 先取得 sudo 憑證（15 分鐘）
set -u
D=~/exp/irq
S() { echo; echo "################################################################"; echo "##### $*"; echo "################################################################"; }
C() { sudo dmesg -c >/dev/null 2>&1; }
P() { sudo dmesg | sed 's/^\[[^]]*\] //'; }

S "0. 開機後基本狀態"
uname -a; uptime
echo "--- lsmod（應該沒有任何實驗模組）---"
lsmod | grep -E 'irq_probe|irq_live|ctx_probe|softirq_par|wq_probe' || echo "（乾淨）"
echo "--- 重新編譯 ---"
cd $D && make 2>&1 | tail -5
ls -la *.ko

S "1. irq_probe：靜態結構（Q1/Q2/Q3/Q4/Q7/Q14/Q15）"
C; sudo insmod $D/irq_probe.ko show_irqs=0 2>/dev/null; P

S "2. irq_probe：完整 irq_desc 掃描（Q2）—— 只印關鍵幾條"
C; sudo insmod $D/irq_probe.ko 2>/dev/null
sudo dmesg | sed 's/^\[[^]]*\] //' > /tmp/irqmap.txt
grep -E 'nr_irqs|irq (13|14|15|21|22|160|161|169|172|173) ' /tmp/irqmap.txt
echo "--- 用到的 irq_domain ---"
grep -o 'domain=[^ ]*' /tmp/irqmap.txt | sort -u
echo "--- 已註冊 action 的 IRQ 總數 ---"
grep -c '^irq_probe: irq ' /tmp/irqmap.txt

S "3. irq_live：真實中斷現場 —— arch_timer (hwirq 26)（Q1/Q2/Q14/Q15）"
C; sudo insmod $D/irq_live.ko samples=1 want_hwirq=26 2>/dev/null; sleep 1; sudo rmmod irq_live; P

S "4. irq_live：中斷點在 EL0 使用者態（Q1/Q14/Q15）"
C; sudo insmod $D/irq_live.ko samples=1 from_el0=1 2>/dev/null
( for i in $(seq 1 4000000); do :; done ) & sleep 1.5; sudo rmmod irq_live; wait 2>/dev/null; P

S "5. irq_live：每 CPU 中斷棧位址（Q4/Q15）"
C; sudo insmod $D/irq_live.ko samples=0 stackmap=1 2>/dev/null; sleep 3; sudo rmmod irq_live; P

S "6. ctx_probe mode=0：八種上下文指紋（Q4/Q5/Q7/Q8/Q10）"
C; sudo insmod $D/ctx_probe.ko mode=0 2>/dev/null; sudo rmmod ctx_probe; P
echo "--- rmmod 後 lsmod 檢查（這次應該乾淨）---"; lsmod | grep ctx_probe || echo "（已完全卸載 ✓）"

S "7. ctx_probe mode=1：軟中斷期間本地中斷是開的（Q5）"
C; sudo insmod $D/ctx_probe.ko mode=1 spin_ms=30 2>/dev/null; sudo rmmod ctx_probe; P
lsmod | grep ctx_probe || echo "（已完全卸載 ✓）"

S "8. ctx_probe mode=2：軟中斷 vs 行程優先級（Q8）"
C; sudo insmod $D/ctx_probe.ko mode=2 spin_ms=40 cpu_target=3 2>/dev/null; sudo rmmod ctx_probe; P
lsmod | grep ctx_probe || echo "（已完全卸載 ✓）"

S "9. ctx_probe mode=3：work 回呼真的能睡（Q10）"
C; sudo insmod $D/ctx_probe.ko mode=3 2>/dev/null; sudo rmmod ctx_probe; P
lsmod | grep ctx_probe || echo "（已完全卸載 ✓）"

S "10. softirq_par mode=0：同類軟中斷 8 CPU 並行（Q6）"
C; sudo insmod $D/softirq_par.ko mode=0 spin_ms=30 2>/dev/null; sudo rmmod softirq_par; P

S "11. softirq_par mode=1：同一 tasklet 串行（Q9）"
C; sudo insmod $D/softirq_par.ko mode=1 spin_ms=30 2>/dev/null; sudo rmmod softirq_par; P

S "12. softirq_par mode=2：重現書上 CPU0/CPU1 時序（Q9）"
C; sudo insmod $D/softirq_par.ko mode=2 spin_ms=60 2>/dev/null; sudo rmmod softirq_par; P

S "13. wq_probe mode=0：會睡的 work → 線程池長大（Q12/Q13）"
C; sudo insmod $D/wq_probe.ko mode=0 nwork=6 sleep_ms=300 cpu_target=3 2>/dev/null; sudo rmmod wq_probe; P

S "14. wq_probe mode=1：燒 CPU 的 work → keep_working（Q12）"
C; sudo insmod $D/wq_probe.ko mode=1 nwork=6 burn_ms=60 cpu_target=3 2>/dev/null; sudo rmmod wq_probe; P

S "15. wq_probe mode=2：alloc_ordered_workqueue 串行（Q11）"
C; sudo insmod $D/wq_probe.ko mode=2 nwork=4 sleep_ms=150 cpu_target=3 2>/dev/null; sudo rmmod wq_probe; P

S "16. wq_probe mode=3：max_active=2（Q11）"
C; sudo insmod $D/wq_probe.ko mode=3 nwork=6 sleep_ms=150 cpu_target=3 2>/dev/null; sudo rmmod wq_probe; P

S "17. ftrace：中斷完整呼叫鏈（Q3）"
sudo $D/irq_trace.sh path 2>/dev/null | head -45

S "18. ftrace：mt7921e 上半部 → 下半部（Q3/Q5）"
T=/sys/kernel/debug/tracing
sudo bash -c "
echo 0 > $T/tracing_on; echo nop > $T/current_tracer; echo > $T/set_graph_function; echo 0 > $T/events/enable
for e in irq/irq_handler_entry irq/irq_handler_exit irq/softirq_raise irq/softirq_entry irq/softirq_exit; do echo 1 > $T/events/\$e/enable; done
echo > $T/trace; echo 1 > $T/tracing_on
ping -c 3 -i 0.2 -W 1 192.168.68.1 >/dev/null 2>&1
dd if=/dev/nvme0n1 of=/dev/null bs=1M count=32 iflag=direct 2>/dev/null
echo 0 > $T/tracing_on
echo '--- 上半部→raise→軟中斷（挑一個網卡中斷）---'
awk '/irq_handler_entry.*mt7921e/{f=1} f{print; n++} n>10{exit}' $T/trace
echo
echo '--- 軟中斷裡巢狀硬中斷（大寫 H 旗標）---'
grep -E 'd\.H' $T/trace | head -5
echo
echo '--- 執行軟中斷的 comm 統計 ---'
grep softirq_entry $T/trace | awk '{print \$1}' | sed 's/-[0-9]*\$//' | sort | uniq -c | sort -rn | head -8
echo
echo '--- 軟中斷類型統計 ---'
grep softirq_entry $T/trace | grep -o 'action=[A-Z_]*' | sort | uniq -c | sort -rn
"

S "19. ftrace：workqueue vs kworker（Q11/Q12）"
sudo $D/irq_trace.sh pools 2>/dev/null | head -30
sudo bash -c "
echo 0 > $T/events/enable; echo 1 > $T/events/workqueue/enable; echo > $T/trace; echo 1 > $T/tracing_on
sleep 3; echo 0 > $T/tracing_on
echo '--- 3 秒內實際被用到的 workqueue ---'
grep -o 'workqueue=[^ ]*' $T/trace | sort | uniq -c | sort -rn
"

S "20. Q2：TRM ↔ DTB ↔ /proc/interrupts 對帳"
for n in "mmc@fe2e0000" "serial@feb50000"; do
  f=$(find /sys/firmware/devicetree/base -maxdepth 3 -name "$n" | head -1)
  echo -n "$n interrupts = "; hexdump -e '3/4 "%08x " "\n"' "$f/interrupts" 2>/dev/null | head -1
done
grep -E 'mmc0|debug$|arch_timer' /proc/interrupts

S "21. 收尾：還原機台"
sudo $D/irq_trace.sh off 2>/dev/null
for m in irq_live ctx_probe softirq_par wq_probe; do sudo rmmod $m 2>/dev/null; done
echo "--- tracing 狀態 ---"; sudo cat /sys/kernel/debug/tracing/current_tracer /sys/kernel/debug/tracing/tracing_on
echo "--- 殘留模組 ---"; lsmod | grep -E 'irq_probe|irq_live|ctx_probe|softirq_par|wq_probe' || echo "（全部乾淨 ✓）"
echo
echo "===== 全部重跑完畢 ====="
