#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# ch12_run_all.sh —— 卷2 第3章「內核調試與性能優化」全部實驗的一鍵重現
#
# 在**主機**上執行：./ch12_run_all.sh [板子IP]
# 會把所有 .c/.h/.S/.sh 推到板子的 ~/exp/ch12，編譯後依序跑完 Q1~Q14。
#
# 板子：Radxa ROCK 5B（RK3588），帳密 radxa/radxa
set -u
HOST=${1:-192.168.68.58}
SSH="ssh -o BatchMode=yes radxa@$HOST"
SUDO='echo radxa | sudo -S'
HERE=$(cd "$(dirname "$0")" && pwd)

echo "===== 0. 佈署 ====="
$SSH 'mkdir -p ~/exp/ch12'
scp -q "$HERE"/{addr_probe.c,reloc_mod.c,opt_lab.c,tp_lab.c,tp_lab_trace.h,slub_lab.c,dl_lab.c,printk_lab.c,dyndbg_lab.c,oops_lab.c,ksym.h,pic_demo.c,pic_asm.S,opt_build.sh} radxa@"$HOST":~/exp/ch12/
$SSH 'cd ~/exp/ch12 && cat > Makefile <<EOF
obj-m += addr_probe.o reloc_mod.o opt_lab.o tp_lab.o slub_lab.o dl_lab.o printk_lab.o dyndbg_lab.o oops_lab.o
ccflags-y += -g -I\$(src)
KDIR ?= /lib/modules/\$(shell uname -r)/build
all:
	\$(MAKE) -C \$(KDIR) M=\$(PWD) modules
EOF
chmod +x opt_build.sh
# oops 會讓模組永遠卡在 COMING 狀態，每個 mode 用一份不同名字的模組
for m in 1 2 3 4 5 6 7; do cp oops_lab.c oops_m$m.c; done
sed -i "s/^obj-m .*/& oops_m1.o oops_m2.o oops_m3.o oops_m4.o oops_m5.o oops_m6.o oops_m7.o/" Makefile
make -s
gcc -O2 -o pic_demo       pic_demo.c pic_asm.S
gcc -O2 -no-pie -o pic_demo_nopie pic_demo.c pic_asm.S
gcc -O2 -o slabinfo ~/disk/kernel-source/tools/vm/slabinfo.c'

echo "===== Q1  -O0 vs -O2（編譯 4 次，約 3 分鐘）====="
$SSH 'cd ~/exp/ch12 && ./opt_build.sh'

echo "===== Q2/Q5/Q7  加載/運行/鏈接地址 ====="
$SSH "cd ~/exp/ch12 && $SUDO dmesg -C >/dev/null 2>&1; $SUDO insmod addr_probe.ko; $SUDO dmesg | sed 's/^\[[^]]*\] //'"
$SSH "$SUDO cat /proc/iomem | head -12"

echo "===== Q3/Q4  位置無關 vs 位置有關（使用者空間）====="
$SSH 'cd ~/exp/ch12 && ./pic_demo; echo "--- PIE 的 .rela.dyn（文字池要靠 ld.so 重定位）---"; readelf -r pic_demo | grep -m4 RELATIV'

echo "===== Q4  模組重定位 ====="
$SSH "cd ~/exp/ch12 && $SUDO dmesg -C >/dev/null 2>&1; $SUDO insmod reloc_mod.ko; $SUDO dmesg | sed 's/^\[[^]]*\] //'
cd ~/exp/ch12 && echo '--- 重定位前（.o 裡的留白 + relocation entry）---' && objdump -dr --disassemble=reloc_probe_site reloc_mod.o | tail -20"

echo "===== Q8  跟蹤點 + static key ====="
$SSH "cd ~/exp/ch12 && $SUDO dmesg -C >/dev/null 2>&1
$SUDO insmod tp_lab.ko
$SUDO cat /sys/kernel/debug/tracing/events/tp_lab/tp_lab_alloc/format
$SUDO sh -c 'echo 1 > /sys/kernel/debug/tracing/events/tp_lab/tp_lab_alloc/enable; echo 1 > /sys/kernel/debug/tracing/tracing_on'
sleep 1
$SUDO sh -c 'echo 1 > /sys/module/tp_lab/parameters/dump; echo 0 > /sys/kernel/debug/tracing/tracing_on'
$SUDO head -20 /sys/kernel/debug/tracing/trace
$SUDO sh -c 'echo 0 > /sys/kernel/debug/tracing/events/tp_lab/tp_lab_alloc/enable; echo > /sys/kernel/debug/tracing/trace'
$SUDO rmmod tp_lab
$SUDO dmesg | sed 's/^\[[^]]*\] //'"

echo "===== Q9  slub_debug 的五種錯誤 ====="
for m in 1 2 4 8 16; do
  echo "--- mode=$m ---"
  $SSH "cd ~/exp/ch12 && $SUDO dmesg -C >/dev/null 2>&1; $SUDO insmod slub_lab.ko mode=$m
  $SUDO sh -c 'for f in red_zone poison store_user sanity_checks; do printf \"%s=%s \" \$f \$(cat /sys/kernel/slab/slub_lab/\$f); done; echo'
  $SUDO ./slabinfo slub_lab | head -12
  $SUDO rmmod slub_lab; $SUDO dmesg | sed 's/^\[[^]]*\] //' | head -45"
done

echo "===== Q10/Q11  死鎖 ====="
for m in 0 3 1 2 4; do
  echo "--- dl_lab mode=$m ---"
  $SSH "cd ~/exp/ch12 && $SUDO dmesg -C >/dev/null 2>&1; $SUDO insmod dl_lab.ko mode=$m escape_ms=4000
  sleep 2; ps -eo pid,stat,comm | grep -E 'dl_(aa|ab|ba|book)'
  for p in \$(pgrep 'dl_(aa|ab|ba|book)'); do $SUDO cat /proc/\$p/stack | head -4; done
  sleep 6; $SUDO rmmod dl_lab; $SUDO dmesg | sed 's/^\[[^]]*\] //' | grep -v tty"
done

echo "===== Q12  printk 等級 ====="
$SSH "cat /proc/sys/kernel/printk
cd ~/exp/ch12 && $SUDO dmesg -C >/dev/null 2>&1; $SUDO insmod printk_lab.ko
$SUDO dmesg -x | grep printk_lab | head -20
$SUDO dmesg -r | grep printk_lab | head -10"

echo "===== Q13  動態輸出 ====="
$SSH "cd ~/exp/ch12 && $SUDO insmod dyndbg_lab.ko
$SUDO grep dyndbg_lab /proc/dynamic_debug/control
$SUDO sh -c 'echo func dyndbg_two +pflmt > /proc/dynamic_debug/control'
$SUDO dmesg -C >/dev/null 2>&1
$SUDO sh -c 'echo 1 > /sys/module/dyndbg_lab/parameters/fire'
$SUDO dmesg | sed 's/^\[[^]]*\] //'
$SUDO rmmod dyndbg_lab
$SUDO dmesg -C >/dev/null 2>&1
$SUDO insmod dyndbg_lab.ko dyndbg=+pfl
$SUDO dmesg | sed 's/^\[[^]]*\] //'
$SUDO rmmod dyndbg_lab"

echo "===== Q14  oops ====="
for m in 1 2 5 6 7; do
  echo "--- oops mode=$m ---"
  $SSH "cd ~/exp/ch12 && $SUDO dmesg -C >/dev/null 2>&1; $SUDO insmod oops_m$m.ko mode=$m
  sleep 1; $SUDO dmesg | sed 's/^\[[^]]*\] //' | grep -vE '^ *x[0-9]|^Modules linked|^[0-9a-f]{4}  ' | head -45"
done
echo "--- 解碼 ---"
$SSH "cd ~/exp/ch12
echo 'Code: 0b030000 0b020021 0b010000 d2800001 (b9000020)' > code.txt
ARCH=arm64 ~/disk/kernel-source/scripts/decodecode < code.txt
~/disk/kernel-source/scripts/faddr2line ./oops_m1.ko create_oops+0x4c/0x58
objdump -dS --disassemble=create_oops oops_m1.o | tail -25"

echo "===== 完成。注意：oops 過的模組會卡在 COMING 狀態，重開機才會清掉 ====="
