#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# ch14_run_all.sh —— 卷2 第5章「基於 ARM64 解決宕機難題」全部實驗的一鍵重現
#
# 在**主機**上執行：./ch14_run_all.sh [板子IP]
# 板子：Radxa ROCK 5B（RK3588），帳密 radxa/radxa
#
# 這一章和第 4 章最大的不同：**板子本身就是 ARM64**，所以全部實驗都在板子上做，
# 而且我們有真正的 crash 工具可用（前提是先跑過 build_vmlinux.sh 產生帶 DWARF 的 vmlinux）。
#
#   步驟 0（只需做一次，約 40 分鐘）：./build_vmlinux.sh 佈到板子上跑，
#            會用 /proc/config.gz 的組態 + DWARF 重建一個「和正在跑的核心相符」的 vmlinux。
#   步驟 1 起：本腳本
set -u
HOST=${1:-192.168.68.58}
SSH="ssh -o StrictHostKeyChecking=no radxa@$HOST"
SUDO='echo radxa | sudo -S'
HERE=$(cd "$(dirname "$0")" && pwd)
VMLINUX=${VMLINUX:-/home/radxa/kbuild-src/vmlinux}

echo "########## 0. 佈署與編譯 ##########"
$SSH 'mkdir -p ~/exp/ch14'
scp -q "$HERE"/{arm64_lab.c,arm64_frame.c,crash_probe.c,oops_lab.c,ksym.h} radxa@"$HOST":~/exp/ch14/
scp -q "$HERE"/Makefile.ch14 radxa@"$HOST":~/exp/ch14/Makefile
$SSH 'cd ~/exp/ch14 && for m in 1 2 3; do cp -f oops_lab.c oops_m$m.c; done
      make -s && gcc -O1 -g -fno-omit-frame-pointer -o arm64_frame arm64_frame.c && echo 編譯完成'

echo
echo "########## 1. Q1/Q2：ARM64 函式棧佈局與 FP 指向哪裡（使用者態）##########"
$SSH 'cd ~/exp/ch14 && ./arm64_frame'
echo "----- 四種序幕的反組譯 -----"
$SSH 'cd ~/exp/ch14 && for f in leaf small_frame big_frame vla_frame; do echo "--- $f ---"; objdump -d --no-show-raw-insn arm64_frame | sed -n "/<$f>:/,/^\$/p" | head -6; done'
echo "----- 核心模組（-Os）的序幕：注意 add x29,sp,#N，FP 不在棧底 -----"
$SSH 'cd ~/exp/ch14 && for f in lab_main lab_func1 lab_func2; do echo "--- $f ---"; objdump -d --no-show-raw-insn arm64_lab.ko | sed -n "/<$f>:/,/^\$/p" | head -8; done'

echo
echo "########## 2. 造現場：三層呼叫 + 讀寫信號量死鎖 ##########"
$SSH "cd ~/exp/ch14 && $SUDO sysctl -w kernel.sched_schedstats=1 >/dev/null
$SUDO rmmod arm64_lab 2>/dev/null; $SUDO dmesg -C
$SUDO insmod arm64_lab.ko; $SUDO insmod crash_probe.ko 2>/dev/null; sleep 2
$SUDO dmesg | sed 's/^\[[^]]*\] //'"
echo "----- ground truth（模組自己印的正確答案）-----"
$SSH 'for f in sem_addr priv_addr fp_main fp_func1 fp_func2 magic; do printf "%-10s = %x\n" $f $(cat /sys/module/arm64_lab/parameters/$f); done; grep arm64_lab /proc/modules'

MAIN=$($SSH 'pgrep lab_main | head -1')
SEM=$($SSH 'printf "%x\n" $(cat /sys/module/arm64_lab/parameters/sem_addr)')
echo "lab_main pid=$MAIN  lab_sem=$SEM"

echo
echo "########## 3. Q3：真的 oops，從 calltrace 推函式名稱 ##########"
# oops 之後模組永遠卡在 MODULE_STATE_COMING，同名模組不能再載入 → 每次挑一個還沒用過的複本
$SSH "cd ~/exp/ch14 && M=\$(for m in oops_m1 oops_m2 oops_m3; do lsmod | grep -q \"^\$m \" || { echo \$m; break; }; done)
      if [ -z \"\$M\" ]; then echo '（本次開機三個 oops 複本都用過了，重開機後才能再跑；輸出見筆記 Q3）'; exit 0; fi
      echo \"用 \$M.ko 觸發 oops\"
      $SUDO dmesg -C
      $SUDO insmod \$M.ko mode=1 2>&1 | head -1
      sleep 2
      $SUDO dmesg | grep -E 'Unable to handle|pc :|lr :|Call trace:|^\[.*\]  [a-z_]+\+0x' | sed 's/^\[[^]]*\] //' | head -18
      echo '--- 模組載入基底與符號偏移 ---'
      $SUDO cat /sys/module/\$M/sections/.text; nm \$M.ko | grep -E ' t create_oops| t oops_lab_init'"
echo "----- 手動把 P_LR 換算成函式名稱（= 書上式(5.2)）-----"
$SSH "$SUDO grep -m1 ' do_one_initcall\$' /proc/kallsyms"

echo
echo "########## 4. Q4：局部變數與參數的推導 ##########"
echo "----- 4a. lab_main 的序幕：sub x19,x29,#0x60 就是 &priv -----"
$SSH "cd ~/exp/ch14 && objdump -d --no-show-raw-insn arm64_lab.ko | sed -n '/<lab_main>:/,/^\$/p' | head -10"
echo "----- 4b. 參數被搬到 x19~x22（callee-saved），堆疊上找不到 -----"
$SSH "cd ~/exp/ch14 && objdump -d --no-show-raw-insn arm64_lab.ko | sed -n '/<lab_func2>:/,/^\$/p' | head -16"
if $SSH "test -f $VMLINUX"; then
	echo "----- 4c. 子函式把 x19~x22 存到哪（書上 §5.5.2 的手法）-----"
	$SSH "cd ~/kbuild-src && objdump -d --no-show-raw-insn --start-address=\$(nm vmlinux | awk '/ rwsem_down_read_slowpath\$/{print \"0x\"\$1}') --stop-address=\$((\$(nm vmlinux | awk '/ rwsem_down_read_slowpath\$/{print \"0x\"\$1}')+0x28)) vmlinux | tail -7"
else
	echo "（沒有重建的 vmlinux，跳過核心反組譯；請先跑 build_vmlinux.sh）"
fi
echo "----- 4d. 把推導出來的位址讀出來，和 ground truth 對答案 -----"
$SSH "$SUDO sh -c \"echo 'bt $MAIN' > /proc/crash_probe\"; $SUDO dmesg -c | sed 's/^\[[^]]*\] //' | grep -E 'lab_|down_read|rwsem_down'"
echo "  （lab_func2 的 x29 → rwsem_down_read_slowpath 的 x29 − 0x50 = sp，x19@sp+96 …）"

echo
echo "########## 5. Q5~Q7：鎖的持有者／等待者／阻塞時間 ##########"
$SSH "$SUDO sh -c \"echo ps > /proc/crash_probe; echo 'rwsem $SEM' > /proc/crash_probe; echo 'sched $MAIN' > /proc/crash_probe\"; $SUDO dmesg -c | sed 's/^\[[^]]*\] //'"

echo
echo "########## 6. 附錄 A：真 crash 工具（預期會卡在 VA_BITS，見筆記附錄 A）##########"
if $SSH "test -f $VMLINUX && test -c /dev/crash"; then
	$SSH "$SUDO sh -c 'echo quit | crash -s --machdep phys_offset=0x200000 --machdep vabits_actual=48 $VMLINUX 2>&1 | tail -6'"
else
	echo "（尚未重建 vmlinux 或未載入 /dev/crash driver，略過）"
fi

echo
echo "########## 7. 收工 ##########"
$SSH "$SUDO rmmod arm64_lab; $SUDO rmmod crash_probe; lsmod | grep -E 'arm64_lab|crash_probe' || echo '模組卸乾淨了（oops_lab 會卡在 Loading，重開機才會消失）'"
