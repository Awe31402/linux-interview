#!/bin/bash
# ch15_run_all.sh —— 卷2 第6章〈安全漏洞分析〉一鍵在 ROCK 5B 上重現。
# 用法： ./ch15_run_all.sh [radxa@192.168.68.58]
set -u
H="${1:-radxa@192.168.68.58}"
SSH="ssh -o StrictHostKeyChecking=no $H"
DIR="$(cd "$(dirname "$0")" && pwd)"

echo "### 1) 上傳原始碼到 ~/exp/ch15"
$SSH 'mkdir -p ~/exp/ch15'
scp -q "$DIR"/pmu_user.c "$DIR"/sec_probe.c "$DIR"/sidechannel.h \
       "$DIR"/flush_reload.c "$DIR"/spectre_v1.c "$DIR"/meltdown_test.c \
       "$DIR"/nospec_mask.c "$DIR"/branch_pred.c "$H:~/exp/ch15/"
scp -q "$DIR"/Makefile.ch15 "$H:~/exp/ch15/Makefile"

echo "### 2) 編譯模組與使用者態程式"
$SSH 'cd ~/exp/ch15 && make >/tmp/mk.log 2>&1; tail -3 /tmp/mk.log
      for s in flush_reload spectre_v1 meltdown_test nospec_mask branch_pred; do
        gcc -O2 -o $s $s.c 2>/tmp/gcc_$s.log && echo "gcc $s OK" || { echo "gcc $s FAIL"; cat /tmp/gcc_$s.log; }
      done'

echo "### 3) 週期級時鐘：perf_user_access=1（+ 備援 pmu_user.ko）"
$SSH 'cd ~/exp/ch15
  echo radxa | sudo -S sysctl kernel.perf_user_access=1
  echo radxa | sudo -S rmmod pmu_user 2>/dev/null; echo radxa | sudo -S insmod pmu_user.ko && echo "pmu_user inserted"'

echo "### 4) 系統暫存器 / 頁表 / 反組譯證據 + 植入核心祕密 (sec_probe.ko，保持載入)"
$SSH 'cd ~/exp/ch15
  K=/proc/kallsyms
  ks(){ echo radxa | sudo -S grep -E " $1\$" $K 2>/dev/null | head -1 | cut -d" " -f1; }
  val(){ v=$(ks "$1"); [ -z "$v" ] && echo 0 || echo 0x$v; }
  INV=$(val invoke_syscall); [ "$INV" = "0" ] && INV=$(val el0_svc_common)
  echo radxa | sudo -S rmmod sec_probe 2>/dev/null
  echo radxa | sudo -S insmod sec_probe.ko stext=$(val _stext) vectors=$(val vectors) \
       bp_harden=$(val __bp_harden_el1_vectors) tramp_vectors=$(val tramp_vectors) invoke_syscall=$INV
  echo radxa | sudo -S dmesg | grep sec_probe | sed "s/^\[[^]]*\] //" | tail -60'

echo "### 5) 漏洞/緩解狀態 (/sys) + 核心組態"
$SSH 'for f in meltdown spectre_v1 spectre_v2 spec_store_bypass; do
        printf "%-18s %s\n" "$f:" "$(cat /sys/devices/system/cpu/vulnerabilities/$f)"; done
      zcat /proc/config.gz | grep -E "UNMAP_KERNEL_AT_EL0|MITIGATE_SPECTRE_BRANCH_HISTORY|HARDEN_BRANCH_PREDICTOR"'

echo "### 6) Q1 高速緩存側信道 (Flush+Reload 直方圖 + 隱蔽通道)"
$SSH 'cd ~/exp/ch15 && taskset -c 4 ./flush_reload'

echo "### 7) Q6 分支預測誤判懲罰 (已排序 vs 未排序)"
$SSH 'cd ~/exp/ch15 && taskset -c 4 ./branch_pred'

echo "### 8) Q7 幽靈變體1 PoC (越界檢查後推測洩漏)"
$SSH 'cd ~/exp/ch15 && taskset -c 4 ./spectre_v1'

echo "### 9) Q3/Q2 熔斷嘗試 + 例外抑制 (攻擊 sec_probe 植入的已知祕密 0x5a，預期免疫)"
$SSH 'cd ~/exp/ch15
  SVA=$(echo radxa | sudo -S dmesg | grep "secret VA" | tail -1 | grep -oE "0x[0-9a-f]+" | head -1)
  echo "planted secret VA = $SVA (value 0x5a)"
  KADDR=$SVA METHOD=signal taskset -c 4 ./meltdown_test | tail -3'

echo "### 10) Q8/Q9 array_index_nospec 遮罩 + CSDB 機器碼"
$SSH 'cd ~/exp/ch15 && ./nospec_mask && echo "--- objdump csdb ---" && objdump -d ./nospec_mask | grep -B3 csdb | head -20'

echo "### 11) 收工：卸載模組、收回 EL0 權限"
$SSH 'echo radxa | sudo -S rmmod sec_probe pmu_user 2>/dev/null
      echo radxa | sudo -S sysctl kernel.perf_user_access=0; echo done'
