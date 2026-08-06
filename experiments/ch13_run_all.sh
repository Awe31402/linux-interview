#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# ch13_run_all.sh —— 卷2 第4章「基於 x86_64 解決宕機難題」全部實驗的一鍵重現
#
# 在**主機**上執行：./ch13_run_all.sh [板子IP]
#   主機（x86_64 Ubuntu）負責 Q2~Q6 的 x86_64 組合語言實驗，
#   板子（RK3588 aarch64）負責 Q7~Q13 的核心機制實驗與 ARM64 對照。
#
# 板子：Radxa ROCK 5B（RK3588），帳密 radxa/radxa
#
# 最後一段（BOOK_MODE=1）是書上 §4.10 原版的 mmap_lock 自鎖，
# 會讓 insmod 永遠停在 D 狀態、ps/pgrep 跟著卡死，跑完必須重開機。預設不跑。
set -u
HOST=${1:-192.168.68.58}
BOOK_MODE=${BOOK_MODE:-0}
SSH="ssh -o StrictHostKeyChecking=no radxa@$HOST"
SUDO='echo radxa | sudo -S'
HERE=$(cd "$(dirname "$0")" && pwd)
CPU=${CPU:-3}				# 拿來做 lockup 實驗的 CPU（3 = A55 小核）

echo "########## 0. 主機端 x86_64 實驗（Q2~Q6、Q10）##########"
cd "$HERE"
gcc -O0 -g -c x86_abi.c -o /tmp/x86_abi.o
echo "----- Q2 參數傳遞：caller() 的反組譯 -----"
objdump -d --no-show-raw-insn /tmp/x86_abi.o | sed -n '/<caller>:/,/^$/p'
echo "----- Q3/Q10 堆疊佈局 -----"
gcc -O0 -g -fno-omit-frame-pointer -o /tmp/x86_frame x86_frame.c && /tmp/x86_frame
echo "----- Q4/Q5/Q6 MOV vs LEA、五種定址 -----"
gcc -O0 -g -o /tmp/x86_addr x86_addr.c && /tmp/x86_addr
echo "----- Q10 書上 §4.10 的原始碼用 x86_64 編一次 -----"
gcc -O2 -fno-omit-frame-pointer -c x86_oops_case.c -o /tmp/x86_oops.o
objdump -d --no-show-raw-insn /tmp/x86_oops.o | sed -n '/<my_oops_init>:/,/^$/p'
echo "----- Q1 主機的 Kdump 狀態 -----"
grep -E "CONFIG_(KEXEC|CRASH_DUMP|HARDLOCKUP_DETECTOR|SOFTLOCKUP_DETECTOR|DETECT_HUNG_TASK)=" /boot/config-"$(uname -r)"
echo "kexec_loaded=$(cat /sys/kernel/kexec_loaded) kexec_crash_loaded=$(cat /sys/kernel/kexec_crash_loaded) kexec_crash_size=$(cat /sys/kernel/kexec_crash_size)"

echo
echo "########## 1. 佈署到板子 ##########"
$SSH 'mkdir -p ~/exp/ch13'
scp -q "$HERE"/{crash_probe.c,rwsem_lab.c,lockup_lab.c,ksym.h,x86_frame.c} radxa@"$HOST":~/exp/ch13/
scp -q "$HERE"/Makefile.ch13 radxa@"$HOST":~/exp/ch13/Makefile
$SSH 'cd ~/exp/ch13 && make -s && gcc -O0 -g -fno-omit-frame-pointer -o x86_frame x86_frame.c && echo 編譯完成'

echo
echo "########## 2. Q1：板子有沒有 Kdump？##########"
$SSH 'zcat /proc/config.gz | grep -E "CONFIG_(KEXEC|CRASH_DUMP|SOFTLOCKUP_DETECTOR|HARDLOCKUP_DETECTOR|DETECT_HUNG_TASK|ARM64_PSEUDO_NMI)[= ]"
echo "--- cmdline 有沒有 crashkernel= ---"; cat /proc/cmdline
echo "--- /sys/kernel/kexec* ---"; ls /sys/kernel/ | grep -i kexec || echo "(一個都沒有)"
echo "--- /proc/sys/kernel 有沒有 watchdog/hung_task ---"; ls /proc/sys/kernel/ | grep -E "watchdog|hung|softlockup" || echo "(一個都沒有)"'

echo
echo "########## 3. Q3 對照組：同一支程式在 ARM64 上的堆疊佈局 ##########"
$SSH 'cd ~/exp/ch13 && ./x86_frame | head -22'

echo
echo "########## 4. Q7/Q8/Q9：自己做的三個偵測器 ##########"
$SSH "cd ~/exp/ch13 && $SUDO insmod lockup_lab.ko 2>/dev/null; $SUDO dmesg -C
$SUDO sh -c \"echo 'thresh 5' > /proc/lockup_lab; echo 'wd on' > /proc/lockup_lab\"
sleep 6; $SUDO sh -c \"echo stat > /proc/lockup_lab\"; $SUDO dmesg -c"

echo "----- Q7 softlockup：CPU$CPU 關搶佔 25 秒 -----"
$SSH "$SUDO sh -c \"echo 'soft $CPU 25' > /proc/lockup_lab\"; sleep 20; $SUDO sh -c \"echo stat > /proc/lockup_lab\"; sleep 8; $SUDO dmesg -c"

echo "----- Q8 hardlockup：CPU$CPU 關中斷 14 秒（鄰居 CPU 抓）-----"
$SSH "$SUDO sh -c \"echo 'hard $CPU 14' > /proc/lockup_lab\"; sleep 10; $SUDO sh -c \"echo stat > /proc/lockup_lab\"; sleep 8; $SUDO dmesg -c"

echo
echo "########## 5. Q10~Q13：讀寫信號量死鎖現場 ##########"
$SSH "cd ~/exp/ch13 && $SUDO sysctl -w kernel.sched_schedstats=1
$SUDO insmod crash_probe.ko 2>/dev/null; $SUDO insmod rwsem_lab.ko 2>/dev/null
sleep 3; $SUDO dmesg -c | tail -12"

SEM=$($SSH 'printf "%x\n" $(cat /sys/module/rwsem_lab/parameters/sem_addr)')
echo "lab_sem = $SEM"
RD=$($SSH "pgrep rwsem_rd1 | head -1")
echo "rwsem_rd1 pid = $RD"

echo "----- Q9 hung_task：迷你 khungtaskd 抓上面兩條等鎖的執行緒 -----"
$SSH "$SUDO sh -c \"echo 'khung on 10' > /proc/lockup_lab\"; sleep 22; $SUDO dmesg -c | head -30; $SUDO sh -c \"echo 'khung off' > /proc/lockup_lab\""

echo "----- Q11/Q12：誰持有鎖、誰在等鎖 -----"
$SSH "$SUDO sh -c \"echo ps > /proc/crash_probe; echo 'rwsem $SEM' > /proc/crash_probe\"; $SUDO dmesg -c"

echo "----- Q10：回溯 + 原始堆疊（priv 的位置）-----"
$SSH "$SUDO sh -c \"echo 'bt $RD' > /proc/crash_probe\"; $SUDO dmesg -c"
$SSH "cd ~/exp/ch13 && objdump -d --no-show-raw-insn rwsem_lab.ko | sed -n '/<reader_fn>:/,/^\$/p' | head -20"

echo "----- Q13：被阻塞了多久 -----"
$SSH "$SUDO sh -c \"echo 'sched $RD' > /proc/crash_probe\"; $SUDO dmesg -c"

echo
echo "########## 6. 收工 ##########"
$SSH "cd ~/exp/ch13 && $SUDO rmmod rwsem_lab; $SUDO rmmod lockup_lab; $SUDO rmmod crash_probe; lsmod | grep -E 'rwsem_lab|lockup_lab|crash_probe' || echo '模組都卸乾淨了'"

if [ "$BOOK_MODE" = "1" ]; then
	echo
	echo "########## 7. 書上 §4.10 原版（跑完要重開機！）##########"
	$SSH "cd ~/exp/ch13 && $SUDO insmod crash_probe.ko; $SUDO dmesg -C; ($SUDO insmod rwsem_lab.ko mode=2 &) ; sleep 3; $SUDO dmesg -c"
	echo "→ insmod 已經卡死。接下來只能用 crash_probe（它不碰 /proc/pid/，不會卡）："
	echo "   ssh radxa@$HOST \"echo radxa | sudo -S sh -c 'echo ps > /proc/crash_probe'\" ; ssh ... dmesg"
	echo "→ 分析完請重開機：ssh radxa@$HOST 'echo radxa | sudo -S reboot -f'"
fi
