# 卷2 第 4 章（全書第 13 章）基於 x86_64 解決宕機難題 — 高頻面試題解答

> **來源**：《奔跑吧 Linux內核》（第二版）卷2 第 4 章〈基于x86_64解决宕机难题〉開篇「本章的高頻面試題」，共 13 題。
> 原文純文字檔：`books/running-linux-kernel/running-kernel-2-txt/11_第4章_基于x86_64解决宕机难题.txt`
> （下稱「奔跑吧卷2 §4.x」，**行號一律以該純文字檔為準**）

---

## 本章面試題目列表

1. 请简述Kdump的工作原理。
2. 在x86_64架构里函数参数是如何传递的？
3. 假设函数调用关系为main()→func1()→func2()，请画出x86_64架构的函数栈的布局图。
4. 在x86_64架构中，MOV指令和LEA指令有什么区别？
5. 什么是直接寻址、间接寻址和基址寻址？
6. 在x86_64架构中，“mov -8(%rbp), %rax”和“lea -8(%rbp)，%rax”这两条指令有什么区别？
7. Softlockup机制的实现原理是什么？
8. Hardlockup机制的实现原理是什么？
9. Hung_task机制的实现原理是什么？
10. 在x86_64架构中，如何使用Kdump+Crash工具来分析和推导一个局部变量存在栈的位置？
11. 在x86_64架构中，如何使用Kdump+Crash工具来分析和推导一个读写信号量的持有者？
12. 在x86_64架构中，如何使用Kdump+Crash工具来分析和推导有哪些进程在等待读写信号量？
13. 在x86_64架构中，如何使用Kdump+Crash工具来分析一个进程被阻塞了多长时间？

（每一題的解答見下方 [目錄](#目錄)。）

---

## 這一章為什麼要用「兩台機器」

本章是全書唯一一章**完全綁在 x86_64 上**的（下一章第 5 章才是 ARM64 版）。
專案的主要實驗機是 ARM64 的 ROCK 5B，跑不了 x86_64 的指令，所以這章的實驗拆成兩台：

| | **主機**（負責 x86_64 那一半） | **板子**（負責核心機制那一半） |
|---|---|---|
| 機器 | `awe-Dell-G15-5530`，13th Gen Core i7-13650HX（20 執行緒） | Radxa ROCK 5B（RK3588），`192.168.68.58`，帳密皆 `radxa` |
| 系統 | Ubuntu 22.04，`Linux 6.5.0-21-generic x86_64` | Debian 12，`Linux rock-5b 6.1.115+ #1 SMP aarch64` |
| 工具鏈 | gcc 11.4.0 / objdump / gdb | gcc 12.2.0 / binutils 2.40，板上有完整核心原始碼（`/lib/modules/6.1.115+/build`） |
| 負責題目 | **Q2~Q6**（暫存器、參數傳遞、堆疊佈局、MOV/LEA、定址）、**Q10 的 x86 版推導** | **Q7~Q9**（softlockup/hardlockup/hung_task）、**Q10~Q13 的實機推導** |

> **殘酷的事實**：書上 Q1、Q10~Q13 全部建立在「Kdump 抓到 vmcore + crash 工具打開它」上，
> 而**這兩台機器都做不到**：
>
> ```
> 板子（Rockchip 6.1.115，zcat /proc/config.gz）
>   # CONFIG_KEXEC is not set        # CONFIG_CRASH_DUMP is not set
>   # CONFIG_SOFTLOCKUP_DETECTOR is not set
>   # CONFIG_HARDLOCKUP_DETECTOR is not set
>   # CONFIG_DETECT_HUNG_TASK is not set     CONFIG_DEBUG_INFO_NONE=y
>
> 主機（Ubuntu 6.5，/boot/config-6.5.0-21-generic）
>   CONFIG_KEXEC=y  CONFIG_CRASH_DUMP=y  三個 lockup 偵測器都 =y
>   但 /sys/kernel/kexec_crash_size = 0   ← cmdline 沒有 crashkernel=，一個位元組都沒保留
> ```
>
> 所以這份筆記的做法是：**把 crash 工具的那幾個子命令、以及三個偵測機制，全部用自己寫的核心模組
> 重新實作一遍，在活的系統上跑出同樣的結論**。這反而更接近嵌入式現場——
> 板子上通常沒有 kdump、沒有 vmcore、沒有帶符號的 vmlinux，只有一個 D 狀態卡死的行程和 dmesg。

**實驗程式碼**：全部在 [`notes/experiments/`](./experiments/)，一鍵重現
[`experiments/ch13_run_all.sh`](./experiments/ch13_run_all.sh)（本文所有輸出都是這支腳本跑出來的，**重開機後完整複驗過一次**）。

---

## 書是 CentOS 7.6 + Linux 3.10 + x86_64 寫的，本機是 6.1/6.5 —— 13 處差異

| # | 書上（3.10 / CentOS / x86_64） | 本機實測 | 題號 |
|---|------|------|------|
| 1 | `yum install kexec-tools crash` + `crashkernel=512M` + `kdump.service` | 板子 **`CONFIG_KEXEC` 根本沒開**，`/sys/kernel/` 底下連 `kexec_crash_loaded` 都沒有；主機有編進去但 `kexec_crash_size=0` | Q1 |
| 2 | Softlockup 是「每 CPU 一條 RT 執行緒 `watchdog/N`」 | 6.1 **`watchdog/N` 執行緒已經不存在**：改成 hrtimer 直接用 `stop_one_cpu_nowait()` 把 `softlockup_fn()` 丟給 stop 排程類（`kernel/watchdog.c:470-503`） | Q7 |
| 3 | Hardlockup **一定**要 PMU 的 NMI perf 事件 | 本機核心多了一套 **`HARDLOCKUP_DETECTOR_OTHER_CPU`「鄰居 CPU 互相檢查」**（`lib/Kconfig.debug:1090`、`kernel/watchdog.c:382-448`），不需要 NMI；ARM64 的 NMI 版要靠 GICv3 pseudo-NMI，而本機 cmdline 是 `irqchip.gicv3_pseudo_nmi=0` | Q8 |
| 4 | hung_task 掃「所有 `TASK_UNINTERRUPTIBLE`」 | 6.1 還要排除 **`TASK_NOLOAD`（TASK_IDLE）與 `TASK_WAKEKILL`**（`kernel/hung_task.c:203-210`）。實測不排除的話會被 **95~115 個閒置 kworker** 洗版 | Q9、Q12 |
| 5 | `t->last_switch_count` | 這個欄位被 `#ifdef CONFIG_DETECT_HUNG_TASK` 包起來（`include/linux/sched.h:1086`），本機**編譯直接報 `has no member named 'last_switch_count'`** | Q9 |
| 6 | `mm->mmap_sem` | 5.8 起改名 **`mm->mmap_lock`**（commit 顯示在 `include/linux/mm_types.h`） | Q10~Q12 |
| 7 | `mmap_sem` 在 `mm_struct` 裡的偏移量 **0x78** | 本機 aarch64 6.1 實測 **0x88** | Q10~Q12 |
| 8 | `rw_semaphore.owner` 是 `struct task_struct *`，`owner==1` 代表讀者持有 | 6.1 是 **`atomic_long_t owner`，低 3 個位元是旗標**（`RWSEM_READER_OWNED`/`RWSEM_NONSPINNABLE`，`kernel/locking/rwsem.c:63-65`）；`count` 也改成位元編碼（bit0 寫者、bit1 有等待者、bit2 handoff、bit63 readfail、讀者數在 bit8 以上） | Q11 |
| 9 | `count = 0xffffffff00000001` | 本機同一種情境是 **`count = 0x3`**（= WRITER_LOCKED \| FLAG_WAITERS） | Q11 |
| 10 | `ps \| grep UN` 看到 4 個 UN 行程 | 6.1 的 `/proc/<pid>/cmdline` 讀者改用 **killable** 版鎖 → 卡住的 `ps`/`pgrep` 是 **TASK_KILLABLE**，不是純 D | Q12 |
| 11 | `task -R sched_info` 直接就有 `last_arrival` | 6.1 的 sched_info 受 static key 管控，**`sysctl kernel.sched_schedstats` 預設 0**，不打開的話 `last_arrival` 永遠是 0（而且重開機會恢復 0） | Q13 |
| 12 | 函式參數「≤6 個走 RDI/RSI/RDX/RCX/R8/R9，其餘走堆疊」 | 這只講了整數；實測還有 **XMM0~7 傳浮點、大結構回傳用隱藏的第 0 參數（RDI）、小結構用 RAX:RDX 回傳、可變參數用 AL 記錄向量暫存器個數** | Q2 |
| 13 | 局部變數在 **RBP 減去** 某個偏移量 | ARM64 剛好相反：`x29` 指在框架**底部**，局部變數在 **x29 加上** 偏移量（實測 `x29+0x70`）；而且前 8 個參數全走 x0~x7，堆疊上不會出現參數 | Q3、Q10 |

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | 請簡述 Kdump 的工作原理 | 板子 **`CONFIG_KEXEC` 沒開**、cmdline 沒有 `crashkernel=`、`/sys/kernel/kexec*` 一個都沒有；主機有編進去但保留了 **0 位元組** |
| [2](#q2) | x86_64 架構裡函式參數如何傳遞 | 一次反組譯抓到全部五種規則：`push $0x8/$0x7`（第 7、8 參數）、`xmm0~2`（浮點）、**大結構回傳用 RDI 當隱藏參數**、小結構 **RAX:RDX**、可變參數 **`mov $0x1,%eax`** |
| [3](#q3) | 畫出 main→func1→func2 的 x86_64 函式棧佈局 | 程式自己印出三層 RBP 鏈 `0x…77c0 → 0x…7810 → 0x…7840`，並逐格 dump 堆疊；ARM64 對照組同一支程式跑出 **x29 鏈同構、但局部變數在 x29 正偏移** |
| [4](#q4) | MOV 和 LEA 有什麼區別 | 同一個位址：`mov` 拿到 `0x1122334455667788`（內容），`lea` 拿到 `0x7fff5fd2a9a8`（位址）；`lea (%rax,%rbx,4)` 一條指令算完 `100+7*4=128` 且**不動 flags** |
| [5](#q5) | 什麼是直接／間接／基址定址 | 四種定址各跑一次：`0x12345678`、`array[0]=0xa0`、`16(%rbx)=0xa2`、`(%rbx,%rcx,8)=0xa3` |
| [6](#q6) | `mov -8(%rbp),%rax` 和 `lea -8(%rbp),%rax` 的區別 | 見 Q4 的實測數值；再加上核心裡的真實用法：`lea 0x78(%rax),%rdx` = 取 `&mm->mmap_sem` |
| [7](#q7) | Softlockup 機制的實現原理 | 自己寫的迷你偵測器抓到 **`BUG: soft lockup - CPU#3 stuck for 12s! [lockup_soft:2717]`**；同一時刻 CPU3 的**心跳計數和其他 CPU 一樣是 13**（中斷正常）但 `touch_ts` 落後 21 秒 |
| [8](#q8) | Hardlockup 機制的實現原理 | CPU3 關中斷 14 秒 → 心跳**停在 17 不動**，由**鄰居 CPU2** 抓到；而 CPU3 自己的 softlockup 報告要等到中斷恢復才「補印」——直接證明 softlockup 抓不到 hardlockup |
| [9](#q9) | hung_task 機制的實現原理 | 迷你 khungtaskd 印出 **`INFO: task rwsem_rd1:2845 blocked for more than 10 seconds.`** + 完整堆疊；抱著鎖但每 200 ms 醒一次的 `rwsem_holder`**不會被報**（switch_count 一直在變） |
| [10](#q10) | 如何推導一個局部變數在棧的位置 | 三層驗證：使用者態 x86_64 推導**與 `&priv` 完全相符**；書上原始碼用 x86_64 編出**一模一樣的 `lea` 指令圖案**；板子上從 ARM64 核心堆疊算出 `priv = x29-0x60 = 0xffff80001024ba40`，讀出 **`68737568736e6562` = "benshushu"** |
| [11](#q11) | 如何推導一個讀寫信號量的持有者 | `count=0x3`、`owner=0xffff000106d0ae80` → **PID 7100 insmod**；再用「鎖位址 − 0x88 = mm_struct」反推 `mm->owner`，兩條路得到同一個 task |
| [12](#q12) | 如何推導有哪些行程在等這個信號量 | 走訪 `wait_list` 抓到 **3 個等待者：insmod（自己等自己）、pgrep、ps**，型別全是 `RWSEM_WAITING_FOR_READ`；並證明 `rwsem_waiter` 就住在**等待者自己的核心堆疊**上 |
| [13](#q13) | 如何分析一個行程被阻塞了多久 | `sched_info.last_arrival` 與 `sched_clock()` 相減得 **29.157 秒**，與實際經過的 **29.16 秒**吻合；書上原版案例算出 **264.27 秒** |

---

## 一鍵重現

```bash
cd notes/experiments && ./ch13_run_all.sh 192.168.68.58
# 想連書上 §4.10 那個「會卡死機器、跑完要重開機」的原版一起跑：
BOOK_MODE=1 ./ch13_run_all.sh 192.168.68.58
```

實驗程式一覽：

| 檔案 | 跑在哪 | 用途 | 題號 |
|------|--------|------|------|
| [`x86_abi.c`](./experiments/x86_abi.c) | 主機 x86_64 | 8 個整數參數／浮點／大小結構回傳／可變參數 | Q2 |
| [`x86_frame.c`](./experiments/x86_frame.c) | 主機 **與** 板子 | 三層函式的框架鏈、逐格 dump 堆疊、Q10 的推導自我驗證（一份原始碼兩種架構） | Q3、Q10 |
| [`x86_addr.c`](./experiments/x86_addr.c) | 主機 x86_64 | MOV vs LEA、五種定址方式 | Q4、Q5、Q6 |
| [`x86_oops_case.c`](./experiments/x86_oops_case.c) | 主機 x86_64 | 把書上 §4.10 的模組原始碼用 x86_64 編一次，驗證書上那段反組譯 | Q10 |
| [`lockup_lab.c`](./experiments/lockup_lab.c) | 板子（核心模組） | **自己做的** softlockup／hardlockup／hung_task 三個偵測器 + 三種 lockup 製造機 | Q7、Q8、Q9 |
| [`rwsem_lab.c`](./experiments/rwsem_lab.c) | 板子（核心模組） | 讀寫信號量死鎖現場（mode=1 可回收／mode=2 書上原版） | Q10~Q13 |
| [`crash_probe.c`](./experiments/crash_probe.c) | 板子（核心模組） | **crash 工具的替身**：`ps`/`bt`/`bt -f`/`rd`/`struct rw_semaphore`/`list`/`task -R`/`runq -t` | Q10~Q13 |

---

<a name="q1"></a>
## Q1：請簡述 Kdump 的工作原理

### 書上怎麼說（奔跑吧卷2 §4.1，行 32-60）

> 「Kdump 的核心實現基於 Kexec……Kexec 可以快速啟動一個新的內核，它會跳過 BIOS 或者 bootloader
> 等引導程序的初始化階段……Kdump 會在內存中保留一塊區域，這個區域用來存放捕獲內核。當生產內核
> 在運行過程中遇到崩潰等情況時，Kdump 會通過 Kexec 機制自動啟動捕獲內核，跳過 BIOS，以免破壞了
> 生產內核的內存，然後把生產內核的完整信息（包括 CPU 寄存器、棧數據等）轉儲到指定文件中。」

書上也點出**唯一的前提**（§4.1 結尾）：機器必須能「熱啟動」，記憶體內容不會掉。硬體錯誤導致要斷電重開的，Kdump 無能為力。

### 標準答案（六個步驟）

1. **開機時預留記憶體**：cmdline 給 `crashkernel=512M`，第一內核（生產內核）啟動時把這塊記憶體從自己的 memblock 挖掉，
   登記成 `crashk_res` 資源。ARM64 的實作在本專案原始碼樹 `arch/arm64/mm/init.c:121-193 reserve_crashkernel()`，
   第 191-193 行就是把它 `insert_resource(&iomem_resource, &crashk_res)`，所以在 `/proc/iomem` 裡看得到 `Crash kernel` 這一段。
2. **平時把第二內核載進去**：`kexec -p /boot/vmlinuz --initrd=… --append="…"`（kdump 服務代勞）
   透過 `kexec_load`/`kexec_file_load` 系統呼叫，把捕獲內核的映像、initrd、cmdline 複製進那塊預留區，
   核心用 `struct kimage` 記著，並把 `machine_kexec_prepare()`（`arch/arm64/kernel/machine_kexec.c:68`）先跑一遍。
   之後 `/sys/kernel/kexec_crash_loaded` 就會變成 1。
3. **崩潰時走 `__crash_kexec()`**：`panic()` 裡（`kernel/panic.c:355-358`）直接呼叫 `__crash_kexec(NULL)`，
   繞過正常的 reboot 流程；oops、softlockup panic、hung_task panic、`sysrq-c` 最後都走到這裡。
4. **凍結現場**：把其他 CPU 用 IPI 停下來、每顆 CPU 的暫存器存進 `crash_notes`（ELF note），
   再由 `machine_kexec()`（`machine_kexec.c:178`）跳進 `arm64_relocate_new_kernel`／x86 的等價常式，
   **不經過 BIOS/bootloader** 直接執行第二內核。因為第二內核只跑在預留區裡，第一內核的記憶體原封不動。
5. **捕獲內核轉儲**：第二內核從 cmdline 拿到 `elfcorehdr=`，把「上一個內核的記憶體」包裝成
   `/proc/vmcore`（`fs/proc/vmcore.c`）。使用者空間的 `makedumpfile` 把它壓縮存成
   `/var/crash/<ip>-<date>/vmcore`，順便存一份 `vmcore-dmesg.txt`，然後重開機回到生產內核。
6. **事後分析**：`crash /var/crash/…/vmcore /usr/lib/debug/…/vmlinux`（書上 §4.3 行 288-309）。

一句話版本：**Kexec 讓「崩潰的內核」在不重置硬體的前提下直接跳進一個備用內核，
於是第一內核的記憶體變成第二內核眼中的一個唯讀檔案 `/proc/vmcore`。**

### 本機實測：兩台都沒有 Kdump，原因不同

板子（`ch13_run_all.sh` 第 2 段）：

```console
$ ssh radxa@192.168.68.58 'zcat /proc/config.gz | grep -E "CONFIG_(KEXEC|CRASH_DUMP)"'
# CONFIG_KEXEC is not set
# CONFIG_CRASH_DUMP is not set

$ cat /proc/cmdline
root=UUID=2e6a37fe-… console=ttyFIQ0,1500000n8 quiet splash loglevel=4 rw earlycon consoleblank=0
console=tty1 coherent_pool=2M irqchip.gicv3_pseudo_nmi=0 cgroup_enable=cpuset …
                                     ↑ 沒有 crashkernel=

$ ls /sys/kernel/ | grep -i kexec
(一個都沒有)
```

主機：

```console
$ grep -E "CONFIG_(KEXEC|CRASH_DUMP)=" /boot/config-6.5.0-21-generic
CONFIG_KEXEC=y
CONFIG_CRASH_DUMP=y
$ cat /sys/kernel/kexec_loaded /sys/kernel/kexec_crash_loaded /sys/kernel/kexec_crash_size
0
0
0          ← 功能編進去了，但沒有 crashkernel=，預留 0 位元組 → 等於沒有
$ ls /proc/vmcore
ls: cannot access '/proc/vmcore': No such file or directory
```

**結論與面試講法**：Kdump 不是「裝個套件」就有，它是**三件事的交集**——
核心組態（`KEXEC`+`CRASH_DUMP`）、開機參數（`crashkernel=`）、使用者空間服務（`kexec-tools`/`kdump-tools`）。
少任何一個，崩潰時就只會直接重開機，什麼都不留。嵌入式板子（像這台 Rockchip）為了省記憶體通常三個都沒有，
所以現場只剩「serial console 上的 oops + 你自己埋的 log」，這也是為什麼下面 Q7~Q13 要自己造工具。

---

<a name="q2"></a>
## Q2：在 x86_64 架構裡函式參數是如何傳遞的？

### 書上怎麼說（奔跑吧卷2 §4.2.2，行 116-158，表 4.2）

> 「當函數參數的數量小於或等於 6 的時候，使用通用寄存器來傳遞函數的參數。當函數參數的數量大於 6
> 的時候，採用棧空間來傳遞函數的參數。」
> RDI / RSI / RDX / RCX / R8 / R9 依序傳第 1~6 個參數，RAX 是第 1 個回傳值，RDX 可當第 2 個回傳值。

### 本機驗證（主機 x86_64，[`x86_abi.c`](./experiments/x86_abi.c)）

```console
$ gcc -O0 -g -c x86_abi.c && objdump -d --no-show-raw-insn x86_abi.o
```

```asm
<caller>:
  ;--- (1) callee8(1,2,3,4,5,6,7,8)：前 6 個進暫存器，第 7、8 個「反序 push」進堆疊
 2dc:   push   $0x8                 ← 第 8 個參數（最後 push，離 RSP 遠）
 2de:   push   $0x7                 ← 第 7 個參數（在 (%rsp)）
 2e0:   mov    $0x6,%r9d            ← 第 6
 2e6:   mov    $0x5,%r8d            ← 第 5
 2ec:   mov    $0x4,%ecx            ← 第 4
 2f1:   mov    $0x3,%edx            ← 第 3
 2f6:   mov    $0x2,%esi            ← 第 2
 2fb:   mov    $0x1,%edi            ← 第 1
 300:   call   callee8
 305:   add    $0x10,%rsp           ← 呼叫者負責把 2 個堆疊參數清掉（caller-clean）

  ;--- (2) callee_mix(long,double,long,double,long,double)：整數與浮點各自排隊
 324:   movapd %xmm1,%xmm2          ← 第 3 個 double → XMM2
 328:   mov    $0x1e,%edx           ← 第 3 個 long   → RDX
 32d:   movapd %xmm0,%xmm1          ← 第 2 個 double → XMM1
 331:   mov    $0x14,%esi           ← 第 2 個 long   → RSI
 336:   movq   %rax,%xmm0           ← 第 1 個 double → XMM0
 33b:   mov    $0xa,%edi            ← 第 1 個 long   → RDI
 340:   call   callee_mix

  ;--- (3) callee_big(100,200) 回傳 32 位元組結構：RDI 變成「隱藏的第 0 個參數」
 34e:   lea    -0x30(%rbp),%rax     ← 呼叫者自己準備回傳緩衝區
 352:   mov    $0xc8,%edx           ← 200 變成「第 3 個」暫存器 RDX
 357:   mov    $0x64,%esi           ← 100 變成「第 2 個」暫存器 RSI
 35c:   mov    %rax,%rdi            ← RDI = 回傳緩衝區位址
 35f:   call   callee_big

  ;--- (4) callee_small(300,400) 回傳 16 位元組結構：RAX:RDX 兩個暫存器直接帶回來
 36e:   call   callee_small
 373:   mov    %rax,-0x40(%rbp)
 377:   mov    %rdx,-0x38(%rbp)

  ;--- (5) 可變參數 callee_var("x", 7L, 8.0)：AL = 用掉幾個向量暫存器
 3a4:   mov    $0x7,%esi
 3b3:   mov    $0x1,%eax            ← AL=1，告訴被呼叫者「XMM 只用了 1 個」
 3b8:   call   callee_var
```

### 完整答案（System V AMD64 ABI）

| 類別 | 規則 | 本機證據 |
|------|------|----------|
| 整數／指標參數 | RDI, RSI, RDX, RCX, R8, R9，超過 6 個走堆疊，**由呼叫者反序 push、呼叫者清堆疊** | `push $0x8; push $0x7` … `add $0x10,%rsp` |
| 浮點參數 | XMM0~XMM7，**和整數各自獨立排隊** | 整數走 RDI/RSI/RDX 的同時浮點走 XMM0/1/2 |
| 回傳值 | ≤16 B 的 INTEGER 類：RAX（+RDX）；>16 B（MEMORY 類）：呼叫者給緩衝區，位址放 **RDI**，真正的第 1 參數順延到 RSI | `lea -0x30(%rbp),%rax; mov %rax,%rdi` |
| 可變參數 | AL 存放「使用了幾個向量暫存器」，`va_start` 靠它決定要不要存 XMM | `mov $0x1,%eax` |
| 呼叫者／被呼叫者保存 | caller-saved：RAX RCX RDX RSI RDI R8-R11；callee-saved：RBX RBP R12-R15 | `caller` 用到 R12 前先 `push %r12` |
| 核心的特例 | Linux 核心是 `-mno-red-zone`（中斷會踩壞 red zone）、系統呼叫用的是**另一套**（第 4 參數是 R10 不是 RCX，因為 `syscall` 指令會毀掉 RCX） | — |

### ARM64 對照（板子，同一份 `x86_frame.c` 編出來的）

```
func2  ARM64：a1~a8 全部走 x0~x7，堆疊上沒有參數（a7=7 a8=8）
```

ARM64（AAPCS64）是 **x0~x7 傳 8 個**、浮點走 v0~v7、大結構回傳用 **x8** 當隱藏指標——
比 x86_64 多兩個整數暫存器，所以「第 7、8 個參數在堆疊上」這件事在 ARM64 上根本不會發生。
面試時如果對方問的是 ARM 平台，這一點答錯很容易被抓。

---

<a name="q3"></a>
## Q3：假設呼叫關係為 main()→func1()→func2()，畫出 x86_64 的函式棧佈局

### 書上怎麼說（奔跑吧卷2 §4.2.3，行 159-170，圖 4.2）

> 「函數的調用與棧有著密切的聯繫……無論嵌套有多深，程序總能正確地返回原來的位置，
> 這就要依賴於棧的結構、RSP 和 RBP。」

### 本機驗證：讓程式自己把圖印出來

[`x86_frame.c`](./experiments/x86_frame.c) 用 `__builtin_frame_address(0)` / `__builtin_return_address(0)`
把三層框架印出來（注意：這兩個內建函式**必須寫在巨集裡**，寫成輔助函式就會取到輔助函式自己的框架——第一版就踩過這個坑）。

```console
$ gcc -O0 -g -fno-omit-frame-pointer -o x86_frame x86_frame.c && ./x86_frame
==== main ====
main   RBP=0x7ffda7cc7840  [RBP]=父框架=0x1  [RBP+8]=返回位址欄位=0x7ffda7cc7848 → 0x7ff1a3229d90
main   &local_m=0x7ffda7cc7820  → RBP-0x20
==== func1 ====
func1  RBP=0x7ffda7cc7810  [RBP]=父框架=0x7ffda7cc7840  [RBP+8]=返回位址欄位=0x7ffda7cc7818 → 0x556fbfb59ac5
func1  &local_1=0x7ffda7cc77f0  → RBP-0x20
==== func2 ====
func2  RBP=0x7ffda7cc77c0  [RBP]=父框架=0x7ffda7cc7810  [RBP+8]=返回位址欄位=0x7ffda7cc77c8 → 0x556fbfb59760
func2  SP =0x7ffda7cc76f0  框架大小 = RBP-SP = 0xd0
func2  &priv   =0x7ffda7cc7760  → RBP-0x60
func2  &local_x=0x7ffda7cc7730  → RBP-0x90
func2  &local_y=0x7ffda7cc772c  → RBP-0x94
func2  第7個參數 a7=7 在 RBP+0x10=0x7ffda7cc77d0；第8個 a8=8 在 RBP+0x18=0x7ffda7cc77d8
```

三層 RBP 串成一條鏈：**func2 (0x…77c0) → func1 (0x…7810) → main (0x…7840)**，
這就是 crash 的 `bt` 能一路往上爬的原理。

原始堆疊逐格 dump（= crash 的 `bt -f`，位址由低到高。**這是另一次執行的輸出，
使用者態有 ASLR，所以位址和上面那段不同，但相對關係完全一樣**）：

```
0x7ffecfd1f5e0  0x68737568736e6562     func2 的局部變數區（"benshushu" 的前 8 個位元組）
        …
0x7ffecfd1f640  0x00007ffecfd1f690  <= func2 的框架指標：存放 func1 的框架指標
0x7ffecfd1f648  0x0000556fd10405f0  <= func2 的返回位址（回到 func1）
0x7ffecfd1f650  0x0000000000000007  <= 第 7 個參數 a7（由 func1 push）
0x7ffecfd1f658  0x0000000000000008  <= 第 8 個參數 a8（由 func1 push）
0x7ffecfd1f670  0x1111111111111111     func1 的局部變數區
0x7ffecfd1f690  0x00007ffecfd1f6c0  <= func1 的框架指標：存放 main 的框架指標
0x7ffecfd1f698  0x0000556fd104092d  <= func1 的返回位址（回到 main）
0x7ffecfd1f6a0  0x2222222222222222     main 的局部變數區
0x7ffecfd1f6c0  0x0000000000000001  <= main 的框架指標
0x7ffecfd1f6c8  0x00007fbda9429d90  <= main 的返回位址（回到 libc）
```

### 佈局圖（實測數值版）

```
高位址
        ┌──────────────────────────┐
        │ main 的返回位址           │ 0x…f6c8
        │ main 存的父框架指標        │ 0x…f6c0  ← main 的 RBP
        │ main 的局部變數 local_m    │ 0x…f6a0
        ├──────────────────────────┤
        │ func1 的返回位址（回 main）│ 0x…f698
        │ func1 存的 main RBP        │ 0x…f690  ← func1 的 RBP
        │ func1 的局部變數 local_1   │ 0x…f670
        ├──────────────────────────┤
        │ 第 8 個參數 a8            │ 0x…f658  ← 參數由「呼叫者」放
        │ 第 7 個參數 a7            │ 0x…f650
        │ func2 的返回位址（回 func1）│ 0x…f648  ← call 指令自動 push
        │ func2 存的 func1 RBP       │ 0x…f640  ← func2 的 RBP（push %rbp; mov %rsp,%rbp）
        │ func2 的局部變數 priv/x/y  │ 0x…f5e0…
        │ （呼叫下一層前的暫存區）    │
        └──────────────────────────┘ 0x…f580  ← func2 的 RSP
低位址（堆疊往下長）
```

三個不變式（面試最常追問的）：

1. `[RBP]` = 父函式的 RBP；`[RBP+8]` = 返回位址 → **RBP = 返回位址欄位 − 8**（Q10 的推導公式就是這條）。
2. 局部變數在 `RBP − x`，堆疊參數在 `RBP + 0x10` 起跳（`+8` 那格是返回位址）。
3. `push %rbp; mov %rsp,%rbp` 是序幕，`leave`(= `mov %rbp,%rsp; pop %rbp`)`; ret` 是尾聲；
   `-O2` 會省掉 RBP（`-fomit-frame-pointer`），這時候 crash 只能靠 ORC/DWARF 而不是框架鏈——
   這也是書上第 3 章要你用 `-O0`/`CONFIG_FRAME_POINTER=y` 的理由。

### ARM64 對照（同一支程式在板子上跑）

```
func2  x29=0xffffeb488100  [x29]=父框架=0xffffeb488200  [x29+8]=返回位址欄位=0xffffeb488108
func2  SP =0xffffeb4880f0  框架大小 = x29-SP = 0x10
func2  &priv   =0xffffeb488170  → x29+0x70     ← 注意是「加」
func2  ARM64：a1~a8 全部走 x0~x7，堆疊上沒有參數
```

| | x86_64 | ARM64 |
|---|---|---|
| 框架記錄 | `push %rbp` + `call` 自動 push 的返回位址 | `stp x29, x30, [sp, #N]`（一次存 FP+LR） |
| `[框架指標]` | 父框架指標 | 父框架指標（**相同**） |
| `[框架指標+8]` | 返回位址 | LR（**相同**） |
| 框架指標位置 | 框架**頂端**，局部變數在 `RBP−x` | 框架**底部附近**，局部變數在 `x29+x` |
| 返回位址怎麼來 | `call` 自動壓堆疊 | `bl` 寫進 **LR 暫存器**，葉子函式甚至不用存堆疊 |
| 堆疊參數 | 第 7 個開始 | 第 9 個開始（實測 8 個全在暫存器） |

**框架鏈的結構是同構的**，所以本文後面在 ARM64 上寫的 `bt`（[`crash_probe.c`](./experiments/crash_probe.c)）
和 x86 的 crash `bt` 演算法完全一樣，只是把 RBP 換成 x29。

---

<a name="q4"></a>
## Q4：MOV 指令和 LEA 指令有什麼區別？

### 書上怎麼說（奔跑吧卷2 §4.2.4 第 4 小節，行 195-210）

> 「MOV 指令用來搬移數據，而 LEA 指令用來加載有效地址……
> 第一條 MOV 指令取出 RBP 寄存器的值，再減去 8，得到一個新地址，然後讀取該新地址的內容……
> 第二條 LEA 指令……把該新地址賦給 RAX 寄存器。LEA 指令不會做間接尋址的動作。」

### 本機驗證（[`x86_addr.c`](./experiments/x86_addr.c)）

```console
$ gcc -O0 -g -o x86_addr x86_addr.c && ./x86_addr
slot 位於 0x7fff5fd2a9a8，內容 = 0x1122334455667788
Q6  mov (%rbx),%rax  -> 0x1122334455667788    <- 讀出「內容」
Q6  lea (%rbx),%rax  -> 0x7fff5fd2a9a8        <- 只算「位址」(= 0x7fff5fd2a9a8)
Q4  lea (%rax,%rbx,4),%rcx  -> 128  (= 100 + 7*4，一條指令做完乘加，且不動 flags)
Q4  lea 5(%rax,%rax,2),%rcx -> 305  (= 100*3 + 5)
附  lea global_var(%rip),%rax -> 0x55bcb25c1060 (== &global_var = 0x55bcb25c1060)
```

### 四個差別（比書上多三個）

1. **要不要碰記憶體**：`mov` 會發出一次記憶體讀取；`lea` **只做位址計算，不存取記憶體**。
   所以 `lea` 對不存在／未對映的位址也不會出錯（`lea 0(%rax),%rbx` 即使 rax=0 也安全，`mov` 會直接 oops）。
2. **`lea` 是編譯器的迷你算術單元**：`lea disp(base,index,scale)` 一條指令算完
   `base + index*scale + disp`（scale 只能是 1/2/4/8），實測 `100+7*4=128`、`100*3+5=305`。
3. **`lea` 不影響 EFLAGS**，`add`/`sub` 會。編譯器常拿 `lea` 做「不想毀掉條件碼」的加法。
4. **在崩潰分析裡的意義**（這才是本章要的）：看到 `lea` 就知道**「這是在取某個東西的位址」**——
   十之八九是「把某個局部變數／結構成員的位址當參數傳出去」。書上 §4.10 推導 `priv` 的位置，
   關鍵就是那一條 `lea -0x68(%rbp),%rcx`。反過來看到 `mov -0x80(%rbp),%rax` 就知道
   「那格堆疊裡放的是一個**指標**，現在把指標本身載出來」。

核心裡的真實例子（本機主機用 gcc 11 編書上的原始碼，見 Q10）：

```asm
 62:   lea    0x78(%rax),%r12     ; r12 = &mm->mmap_sem（結構成員取址，不讀記憶體）
 97:   lea    -0x70(%rbp),%rsi    ; rsi = &priv（局部變數取址 → 當第 2 個參數）
```

---

<a name="q5"></a>
## Q5：什麼是直接定址、間接定址和基址定址？

### 書上怎麼說（奔跑吧卷2 §4.2.4，行 171-194）

> 直接定址：`mov address, %rax`，指令裡直接包含要存取的位址。
> 間接定址：`mov (%rax), %rbx`，從暫存器指定的位址載入值。
> 基址定址：`mov 8(%rax), %rbx`，暫存器的值加上偏移量後再定址，偏移量可正可負。

### 本機驗證（同一支 [`x86_addr.c`](./experiments/x86_addr.c)）

```console
Q5  直接定址 mov global_var(%rip),%rax -> 0x12345678 (global_var=0x12345678)
Q5  間接定址 mov (%rbx),%rax          -> 0xa0 (array[0])
Q5  基址定址 mov 16(%rbx),%rax        -> 0xa2 (array[2])
Q5  變址定址 mov (%rbx,%rcx,8),%rax  -> 0xa3 (array[3])
```

### 完整表格（AT&T 語法：`disp(base, index, scale)`）

| 定址方式 | 寫法 | 有效位址 | C 語言對應 | 何時會在核心反組譯裡看到 |
|---|---|---|---|---|
| 立即數 | `mov $0x10,%rax` | 無 | `x = 16` | 常數、旗標 |
| 暫存器 | `mov %rbx,%rax` | 無 | `x = y` | 到處都是 |
| 直接（絕對） | `mov 0x601040,%rax` | disp | 全域變數 | x86_64 核心其實**很少**用純絕對定址 |
| **RIP 相對** | `mov global(%rip),%rax` | RIP+disp | 全域變數 | x86_64 的預設，位置無關碼的關鍵（**書上沒提**） |
| 間接 | `mov (%rax),%rbx` | rax | `*p` | 解引用指標 |
| 基址（帶位移） | `mov 0x78(%rax),%rbx` | rax+0x78 | `p->field` | **結構成員存取**，本章推導的主力 |
| 變址+比例 | `mov (%rax,%rcx,8),%rbx` | rax+rcx*8 | `array[i]` | 陣列 |
| 完整型 | `mov 0x10(%rax,%rcx,8),%rbx` | rax+rcx*8+0x10 | `s->arr[i]` | 陣列成員 |

**分析崩潰時的讀法**：`0x78(%rax)` 這種「小小的正偏移量」幾乎一定是**結構成員**，
把偏移量丟進 `struct -o` 對表（書上 §4.10 用 `struct -o mm_struct` 查到 `[0x78] mmap_sem`）就知道是誰。
負偏移量 `-0x68(%rbp)` 則幾乎一定是**局部變數**。整個第 4 章的推導功夫就靠這兩句心法。

> 本機的對照數字：`mmap_lock` 在 6.1 aarch64 的 `mm_struct` 裡偏移量是 **0x88**（不是書上的 0x78），
> 實測見 Q11。同一個結構的偏移量會隨版本／組態／架構改變，**永遠要在自己的機器上查**。

---

<a name="q6"></a>
## Q6：`mov -8(%rbp),%rax` 和 `lea -8(%rbp),%rax` 有什麼區別？

### 書上怎麼說（奔跑吧卷2 §4.2.4，行 196-210）

> `mov -8(%rbp), %rax` 等同於 `long *p = %rbp - 8; %rax = *p;`
> `lea -8(%rbp), %rax` 等同於 `%rax = %rbp - 8;`

### 本機實測數值

| 指令 | 語意 | 實測結果 |
|---|---|---|
| `mov (%rbx),%rax`（rbx = 堆疊上某格的位址） | 讀出**內容** | `0x1122334455667788`（就是那格存的值） |
| `lea (%rbx),%rax` | 只算**位址** | `0x7fff5fd2a9a8`（等於 `&slot`） |

一句話：**差一次記憶體存取**。`mov` 是「去把東西拿回來」，`lea` 是「只把地址算出來」。

### 在崩潰分析裡怎麼用（書上 §4.10 的推導邏輯，行 2380-2430）

```asm
0xffffffffc0d130e1 <init_module+225>:   mov    -0x80(%rbp),%rax   ; ← 堆疊裡放的是「指標」
0xffffffffc0d130e5 <init_module+229>:   lea    0x78(%rax),%rdx    ; ← rax 是 mm，+0x78 取 &mmap_sem
0xffffffffc0d130e9 <init_module+233>:   lea    -0x68(%rbp),%rcx   ; ← rcx = &priv（局部變數的位址）
```

- 看到 `mov -0x80(%rbp),%rax` → 推論「**`rbp-0x80` 這格存的是一個指標**」，所以要 `rd` 一次才拿得到真正的 `mm`。
- 看到 `lea -0x68(%rbp),%rcx` → 推論「**`priv` 這個結構就『長』在 `rbp-0x68`**」，位址算出來就是它本人，不用再 `rd`。

這個「`mov` 要多讀一次、`lea` 不用」的差別，決定了你在 crash 裡到底該不該多下一次 `rd`，
**答錯就會把一個指標值當成結構起始位址去解讀，整串推導就崩了**。這就是為什麼書上把這條列進面試題。

ARM64 沒有 `lea`，對應的是 `add xd, xn, #imm`（算位址）vs `ldr xd, [xn, #imm]`（讀內容）。
本機實測（`rwsem_lab.ko` 的反組譯，見 Q10/Q11）：

```asm
 1f8:   add    x20, x0, #0x88      ; x20 = &mm->mmap_lock   ← 等同 lea 0x78(%rax),%rdx
 29c:   mov    x0, sp              ; x0  = &priv            ← 等同 lea -0x68(%rbp),%rcx
```

---

<a name="q7"></a>
## Q7：Softlockup 機制的實現原理是什麼？

### 書上怎麼說（奔跑吧卷2 §4.8 第 1 小節，行 1935-1975）

> 「Softlockup 機制用於檢測系統調度是否正常。當發生 Softlockup 時，內核不能調度，但還能響應中斷。
> ……為每個 CPU 啟動一個實時調度類的內核線程（名稱為 watchdog/N）。在該內核線程得到調度時，
> 更新相應的計數（時間戳），同時啟動定時器。當定時器到期時檢查相應的時間戳，如果超過指定時間都
> 沒有更新，則說明這段時間內沒有發生調度。」
> 書上也提到：「在 Linux 5.0 內核中已經把 watchdog 內核線程修改成 stop 調度類的線程。」

### 6.1 的實作（本專案原始碼樹）—— 書上的說法要更新

| 元件 | 6.1 位置 | 說明 |
|---|---|---|
| 門檻 | `kernel/watchdog.c:48` `watchdog_thresh = 10` | `/proc/sys/kernel/watchdog_thresh` |
| 取樣週期 | `kernel/watchdog.c:284-294` `sample_period = get_softlockup_thresh() * (NSEC_PER_SEC/5)` | = `thresh*2/5` 秒 → 預設 **4 秒**，一個門檻週期內取樣 5 次 |
| 心跳 | `kernel/watchdog.c:456` `__this_cpu_inc(hrtimer_interrupts)` | 每個 CPU 的 hrtimer 每次到期就 +1 |
| 「餵狗」 | `kernel/watchdog.c:470` `softlockup_fn()` → `update_touch_ts()`（:303） | **不再是 `watchdog/N` RT 執行緒**，而是 `:499` 用 `stop_one_cpu_nowait()` 把它丟給 **stop 排程類**（`migration/N`）執行 |
| 判定 | `kernel/watchdog.c:358` `is_softlockup()` | `now > period_ts + 2*watchdog_thresh` → 回報「stuck for N s」 |
| 要不要 panic | `kernel/watchdog.c:575` `if (softlockup_panic) panic(...)` | `/proc/sys/kernel/softlockup_panic`，這才會觸發 Kdump |

**為什麼要用 stop 類？** 因為 stop 類優先於 deadline/RT/CFS 所有排程類，
只要 CPU 還肯排程，`softlockup_fn` 就一定跑得到；用 RT 執行緒的話，一條 SCHED_DEADLINE 的行程就能餓死它，
造成誤報（書上也承認了這一點）。

一句話原理：**hrtimer（靠中斷）負責「問」，stop 類的工作（靠排程）負責「答」；
問了 5 次都沒人答 → 這顆 CPU 的排程器停擺了 → soft lockup。**

### 本機驗證：核心沒編這功能，那就自己寫一個

```console
$ ls /proc/sys/kernel/ | grep -E "watchdog|softlockup"
(一個都沒有)                      ← # CONFIG_SOFTLOCKUP_DETECTOR is not set
```

[`lockup_lab.c`](./experiments/lockup_lab.c) 把上表的演算法照抄一份（每 CPU 一個 hrtimer + 每 CPU 一條
SCHED_FIFO 的 `mywd/N` 執行緒，等同 3.10 的 `watchdog/N`），再故意在某顆 CPU 上 `preempt_disable()` 死迴圈：

```console
$ echo 'thresh 5'  > /proc/lockup_lab     # 門檻 5 秒 → softlockup 門檻 10 秒、取樣週期 2000 ms
$ echo 'wd on'     > /proc/lockup_lab
$ echo 'soft 3 25' > /proc/lockup_lab     # 在 CPU3 上關搶佔死迴圈 25 秒
```

```
[  136.761967] lockup_lab: [soft   ] pid=2717 在 CPU3 上 preempt_disable() 死迴圈 25 秒
[  147.839994] lockup_lab: BUG: soft lockup - CPU#3 stuck for 12s! [lockup_soft:2717]
[  147.840041] lockup_lab:       → 時鐘中斷還在（心跳 9），但 watchdog 執行緒排不上來
[  156.791396] lockup_lab: CPU0  ticks=13   saved=12  touch_ts=142  落後 1 秒
[  156.791473] lockup_lab: CPU3  ticks=13   saved=12  touch_ts=122  落後 21 秒   ← 兇手
[  161.764678] lockup_lab: [soft   ] pid=2717 死迴圈結束
[  161.841833] lockup_lab: CPU#3 的 soft lockup 結束（watchdog 執行緒又跑起來了）
```

**這份輸出正好把書上那句話拆成兩半證明了**：

- 「**還能響應中斷**」：CPU3 的心跳 `ticks=13` 和其他 7 顆 CPU **一模一樣**——hrtimer 中斷照樣進來。
- 「**不能調度**」：CPU3 的 `touch_ts` 停在 122 秒，落後 21 秒——優先權 99 的 RT 執行緒都排不上去。

報告的時間點也符合公式：迴圈從 136.76 開始，門檻 `2*thresh = 10` 秒，
下一次取樣（每 2 秒）落在 147.84 → 報「stuck for 12s」（12 = 147.84 − 135.88，上一次餵狗的時刻）。

---

<a name="q8"></a>
## Q8：Hardlockup 機制的實現原理是什麼？

### 書上怎麼說（奔跑吧卷2 §4.8 第 2 小節，行 1976-1990）

> 「在 Hardlockup 機制下，CPU 不僅無法執行其他進程，而且不再響應中斷。Hardlockup 機制的實現方式
> 利用了 PMU 的 NMI perf 事件。因為 NMI 是不可屏蔽的，所以在 CPU 不再響應中斷的情況下仍然可以得到執行。
> 另外，要檢查時鐘中斷計數器 hrtimer_interrupts 是否在遞增，如果停滯就意味著時鐘中斷未得到響應。」

### 6.1 的實作：**有兩套**，書上只講了第一套

**（a）NMI perf 版**（x86 的標準做法）—— `kernel/watchdog_hld.c`

```c
kernel/watchdog_hld.c:100  static struct perf_event_attr wd_hw_attr = {
                     :103      .config = PERF_COUNT_HW_CPU_CYCLES,   // 用「跑了幾個 cycle」當計數器
                     :110  static void watchdog_overflow_callback(...)   // 溢位時在 NMI 上下文執行
                     :131      if (is_hardlockup()) { ... }
                     :155      if (hardlockup_panic) nmi_panic(regs, "Hard LOCKUP");
                     :176  evt = perf_event_create_kernel_counter(wd_attr, cpu, ...);
```

`is_hardlockup()`（`kernel/watchdog.c:373-379`）就是比對 `hrtimer_interrupts` 有沒有前進——
**判斷依據和 softlockup 不同：softlockup 看「排程」，hardlockup 看「中斷」**。

**（b）鄰居 CPU 版（本機這顆核心才有的）** —— `CONFIG_HARDLOCKUP_DETECTOR_OTHER_CPU`

```
lib/Kconfig.debug:1090  config HAVE_HARDLOCKUP_DETECTOR_OTHER_CPU
                 :1091      def_bool y
                 :1092      depends on NO_GKI          ← Android/Rockchip 這條線才有
                 :1094      depends on !HAVE_HARDLOCKUP_DETECTOR_PERF && !HAVE_HARDLOCKUP_DETECTOR_ARCH

kernel/watchdog.c:382  watchdog_next_cpu()                    // 我負責檢查「下一顆」CPU
                 :398  is_hardlockup_other_cpu(cpu)           // 看它的 hrtimer_interrupts 有沒有動
                 :411  watchdog_check_hardlockup_other_cpu()  // 每 3 個取樣週期檢查一次
```

**為什麼 ARM64 需要這一套？** 因為 ARM64 傳統上**沒有真正的 NMI**：
IRQ 是可以被 `PSTATE.I` 遮蔽的，關中斷就等於連 watchdog 也一起關掉了。
ARMv8.4 之後才能用 **GICv3 的中斷優先權**做出「pseudo-NMI」。本機的狀況：

```console
$ zcat /proc/config.gz | grep PSEUDO_NMI
CONFIG_ARM64_PSEUDO_NMI=y                     ← 編進去了
$ cat /proc/cmdline
… irqchip.gicv3_pseudo_nmi=0 …                ← 但開機參數把它關了
$ grep -E "pmu|arch_timer" /proc/interrupts
 13:  67219  51526 … GICv3  26 Level  arch_timer
 23:      0      0 … GICv3  23 Level  arm-pmu   ← PMU 中斷全 0，沒人在用
```

硬體本身是有的：RK3588 用的是 **GIC-600（GICv3 架構，1 個 cluster / 8 顆 CPU / 480 個 SPI）**，
見 `books/rk3588_trm/part1/chapter_11.txt`「11.1 Overview」與 Table 11-1。
所以這台機器**理論上做得到 pseudo-NMI hardlockup 偵測**，只是 Rockchip 把三個偵測器全關了。

### 本機驗證：讓鄰居 CPU 抓到一個真的 hard lockup

```console
$ echo 'hard 3 14' > /proc/lockup_lab      # CPU3 上 local_irq_disable() 死迴圈 14 秒
```

```
[  165.339010] lockup_lab: [hard   ] pid=2779 在 CPU3 上 local_irq_disable() 死迴圈 14 秒
[  171.842775] lockup_lab: BUG: hard lockup - CPU#3 的時鐘中斷停在 17 不再前進！(由 CPU#2 發現)
[  171.842816] lockup_lab:       → 那顆 CPU 連中斷都不回應了，只有「別人」或 NMI 抓得到
[  175.363608] lockup_lab: CPU0  ticks=22  saved=21  touch_ts=160  落後 2 秒
[  175.363653] lockup_lab: CPU3  ticks=17  saved=17  touch_ts=150  落後 12 秒   ← 心跳凍住
[  179.340222] lockup_lab: BUG: soft lockup - CPU#3 stuck for 16s! [lockup_hard:2779]
[  179.340321] lockup_lab: [hard   ] pid=2779 死迴圈結束（中斷已恢復）
[  183.843649] lockup_lab: CPU#3 心跳恢復（17 → 20），hard lockup 結束
```

**這段輸出把 Q7/Q8 的分界線畫得清清楚楚**：

| | soft lockup（Q7 的實驗） | hard lockup（本實驗） |
|---|---|---|
| CPU3 的 `hrtimer_interrupts` | 13，**和別人一樣** | 17，**別人 22，它凍住了** |
| 誰發現的 | CPU3 **自己**的 hrtimer | **CPU2**（鄰居），CPU3 自己動不了 |
| 本地 hrtimer 有沒有跑 | 有 | **沒有**（中斷被關了） |
| softlockup 有沒有報 | 有，即時 | **有，但是遲到的**——`179.34` 那行是中斷恢復、hrtimer 補跑時才印的，事後諸葛 |

最後一列是這個實驗最值得講的一點：**softlockup 偵測器對 hardlockup 完全無效**，
因為它自己就是靠中斷驅動的。所以才需要「NMI」或「鄰居 CPU」這種**不依賴受害 CPU 的**觀察者。

### 面試補充

- hardlockup 常見成因：長時間關中斷、spinlock 死鎖（拿著鎖關中斷等一個永遠不來的東西）、
  硬體/韌體卡住（SMC/PSCI 呼叫沒回來）、記憶體匯流排鎖死。
- 沒有 hardlockup 偵測器的機器（像本機）發生 hard lockup 時，你能看到的只有：
  RCU stall 警告（其他 CPU 抱怨這顆 CPU 沒回報 quiescent state）、
  `csd_lock` timeout（別人對它發 IPI 等不到回應）、或者整台機器連 ping 都不回。
- `hardlockup_panic=1` + Kdump 才會留下 vmcore；否則只會在 console 上印一行就繼續卡著。

---

<a name="q9"></a>
## Q9：hung_task 機制的實現原理是什麼？

### 書上怎麼說（奔跑吧卷2 §4.8 第 3 小節，行 1991-2004）

> 「長時間處於不可中斷（TASK_UNINTERRUPTIBLE）狀態的進程即我們常說的 D 狀態的進程。
> 內核的 hung_task 機制主要實現在 kernel/hung_task.c 文件中。它的實現原理是，創建一個普通優先級的
> 內核線程，定時掃描系統中所有的進程和線程。如果有 D 狀態線程，則檢查最近是否有調度切換。
> 如果沒有切換，則說明發生了 hung_task。」

### 6.1 的實作細節（`kernel/hung_task.c`）

```c
:394  watchdog_task = kthread_run(watchdog, NULL, "khungtaskd");   // 普通優先級的 kthread
:359  static int watchdog(void *dummy)
:366      unsigned long timeout = sysctl_hung_task_timeout_secs;   // 預設 120 秒（:45）
:377      check_hung_uninterruptible_tasks(timeout);               // 睡 timeout 秒掃一次

:178  check_hung_uninterruptible_tasks()
:203-210   /* skip the TASK_KILLABLE tasks -- these can be killed
            * skip the TASK_IDLE tasks -- those are genuinely idle */
           if ((state & TASK_UNINTERRUPTIBLE) &&
               !(state & TASK_WAKEKILL) && !(state & TASK_NOLOAD))
                   check_hung_task(t, timeout);

:90   check_hung_task()
:92       unsigned long switch_count = t->nvcsw + t->nivcsw;       // 自願+非自願切換次數
:109      if (switch_count != t->last_switch_count) {              // 有動過 → 不算 hung
:110              t->last_switch_count = switch_count; return; }
:119      if (sysctl_hung_task_panic) { ... panic(); }             // 才會觸發 Kdump
```

三個關鍵設計（面試常追問）：

1. **判斷依據不是「D 狀態多久」而是「切換次數有沒有變」**。
   一條每 200 ms 醒一次的 D 狀態行程（例如 `msleep()`）切換次數一直在增加 → **不會**被報。
2. **掃描週期就是 timeout 本身**，所以最壞情況會晚報一個 timeout；書上寫的 `hung_task_timeout_secs`
   是「兩次掃描的間隔」，也是判定門檻。
3. 6.1 多排除了 **TASK_NOLOAD（TASK_IDLE）** 和 **TASK_WAKEKILL（TASK_KILLABLE）**——
   前者是閒著的 kworker，後者殺得掉不算掛死。書上 3.10 的年代還沒有 `TASK_IDLE`。

### 本機驗證：核心沒有 khungtaskd，那就自己養一條

```console
$ ls /proc/sys/kernel | grep hung
(一個都沒有)                  ← # CONFIG_DETECT_HUNG_TASK is not set
```

**順便撞到一個只有實作才會發現的事實**：`task_struct.last_switch_count` 這個欄位是
`#ifdef CONFIG_DETECT_HUNG_TASK` 包起來的（`include/linux/sched.h:1086`），本機沒開這個選項，
所以欄位**根本不存在**，模組直接編不過：

```
/home/radxa/exp/ch13/lockup_lab.c:258:30: error: 'struct task_struct' has no member named 'last_switch_count'
```

（[`lockup_lab.c`](./experiments/lockup_lab.c) 因此自備一張 512 格的表來記每個 pid 上一輪的切換次數。）

跑起來：先用 [`rwsem_lab.c`](./experiments/rwsem_lab.c) 造出兩條卡在讀寫信號量上的執行緒，
再打開迷你 khungtaskd（timeout 設 10 秒）：

```console
$ echo 'khung on 10' > /proc/lockup_lab
```

```
[  209.838866] lockup_lab: INFO: task rwsem_rd1:2845 blocked for more than 10 seconds.
[  209.838912] lockup_lab:       "echo 0 > /proc/sys/kernel/hung_task_timeout_secs" disables this message.
[  209.838922] lockup_lab:       switch_count(nvcsw+nivcsw)=2 一直沒變 → 從頭到尾沒被排程過
[  209.838946] lockup_lab:   #0  __schedule+0x580/0x688
[  209.838958] lockup_lab:   #1  schedule+0x88/0xd4
[  209.838971] lockup_lab:   #2  schedule_preempt_disabled+0x14/0x1c
[  209.838984] lockup_lab:   #3  rwsem_down_read_slowpath+0x26c/0x28c
[  209.838996] lockup_lab:   #4  down_read+0x54/0x80
[  209.839015] lockup_lab:   #5  lab_create_oops.constprop.0.isra.0+0x3c/0x64 [rwsem_lab]
[  209.839027] lockup_lab:   #6  reader_fn+0x74/0xb8 [rwsem_lab]
[  209.839044] lockup_lab:   #7  kthread+0xc0/0xd0
[  209.839058] lockup_lab:   #8  ret_from_fork+0x10/0x20
[  209.839083] lockup_lab: INFO: task rwsem_wr2:2846 blocked for more than 10 seconds.
[  209.839138] lockup_lab:   #2  rwsem_down_write_slowpath+0x3f4/0x440
[  209.839149] lockup_lab:   #3  down_write+0x38/0x44
[  209.839160] lockup_lab:   #4  writer2_fn+0x38/0x78 [rwsem_lab]
```

**兩個值得注意的地方**：

1. 報告格式和真核心一字不差（連那句 "echo 0 > …" 都是照抄 `hung_task.c:114`），
   而且堆疊直接指出**卡在哪一把鎖的哪一條路徑**（`rwsem_down_read_slowpath` vs `rwsem_down_write_slowpath`）。
2. **抱著鎖的 `rwsem_holder` 沒有被報**——它也是 D 狀態（`msleep()` 走
   `schedule_timeout_uninterruptible`），但每 200 ms 醒一次，`switch_count` 一直在變。
   這正好反證了「hung_task 比的是切換次數，不是狀態持續時間」。

### 三個機制的比較（面試一次講完）

| | Softlockup | Hardlockup | hung_task |
|---|---|---|---|
| 症狀 | 某顆 CPU 不排程，但中斷正常 | 某顆 CPU 連中斷都不回應 | 某條**行程**長期 D 狀態（CPU 本身是好的） |
| 觀察者 | 該 CPU 自己的 hrtimer | NMI／鄰居 CPU | 一條普通優先權的 kthread `khungtaskd` |
| 觀察什麼 | `watchdog_touch_ts`（排程有沒有發生） | `hrtimer_interrupts`（中斷有沒有進來） | `nvcsw+nivcsw`（該行程有沒有被排程過） |
| 預設門檻 | `2*watchdog_thresh` = 20 秒 | `watchdog_thresh` = 10 秒 | `hung_task_timeout_secs` = 120 秒 |
| 典型成因 | 關搶佔死迴圈、拿著自旋鎖不放 | 關中斷死迴圈、硬體卡住 | 拿不到 mutex/rwsem、I/O 永遠不回、死鎖 |
| 要 panic 才有 vmcore | `softlockup_panic` | `hardlockup_panic` | `hung_task_panic` |

---

<a name="q10"></a>
## Q10：如何用 Kdump+Crash 分析和推導一個局部變數存在棧的位置？

### 書上怎麼說（奔跑吧卷2 §4.10，行 2121-2478）

書上的步驟（案例 5，`init_module()` 呼叫 `create_oops(vma, &priv, &mm->mmap_sem)`）：

1. `crash` 打開 vmcore → `PANIC: "hung_task: blocked tasks"`。
2. `ps | grep UN` 找到 D 狀態的 `insmod`（PID 4304）。
3. `bt 4304` 看呼叫關係，`bt -f 4304` 印出每個框架的原始內容，讀出
   **`init_module()` 的棧返回地址 = 0xffff8c4bc4aefd38**。
4. `mod -s oops <ko>` 載入模組符號，`dis init_module` 反組譯，看到
   `lea -0x68(%rbp),%rcx; mov %rcx,%rsi` → **第 2 個參數 `priv` 存在 `rbp-0x68`**。
5. 套公式（行 2430）：**`priv 的地址 = 棧返回地址 − 0x8 − 0x68`**（因為 `RBP = 返回位址欄位 − 8`），
   算出 `0xffff8c4bc4aefcc8`。
6. `rd` 驗證 → `000000006f676966`／`benshushu`；`struct mydev_priv <addr>` 印出完整結構。

### 驗證一：在真的 x86_64 上，公式對不對？

[`x86_frame.c`](./experiments/x86_frame.c) 讓程式**自己套一遍書上的公式**再和 `&priv` 對答案：

```
func2  RBP=0x7ffda7cc77c0  [RBP+8]=返回位址欄位=0x7ffda7cc77c8
func2  &priv   =0x7ffda7cc7760  → RBP-0x60
Q10 推導：priv = 栈返回地址欄位(0x7ffda7cc77c8) - 0x8 - 0x60 = 0x7ffda7cc7760  →  與 &priv 相符 ✓
Q10 讀出該位址的內容：name="benshushu" i=10 mm=0xdeadbeef00001000 sem=0xdeadbeef00001078
```

**公式成立**。（偏移量是 0x60 不是書上的 0x68，因為局部變數的配置本來就跟編譯器版本、變數宣告順序有關。）

### 驗證二：把書上那段模組原始碼，用今天的 x86_64 編譯器編一次

[`x86_oops_case.c`](./experiments/x86_oops_case.c) 是書上 §4.10 的原始碼（連 `benshushu`、`priv.i = 10`
都照抄），用 `gcc -O2 -fno-omit-frame-pointer`（等同 CentOS 3.10 核心的 `CONFIG_FRAME_POINTER=y`）編出來：

```asm
<my_oops_init>:                       ; 書上叫 init_module
  5d:   call   get_task_mm
  62:   lea    0x78(%rax),%r12        ; ← 書上：lea 0x78(%rax),%rdx   （+0x78 = &mm->mmap_sem）
  66:   mov    %rax,-0x28(%rbp)       ; ← 書上：mm 指標存在 rbp-0x80
  6a:   mov    %r12,%rdi              ; down_write(&mm->mmap_sem)
  71:   call   down_write
  …
  97:   lea    -0x70(%rbp),%rsi       ; ← 書上：lea -0x68(%rbp),%rcx + mov %rcx,%rsi（第 2 參數 = &priv）
  9b:   mov    %r12,%rdx              ; ← 第 3 參數 = sem
  a5:   movabs $0x68737568736e6562,%rax   ; "benshushu"
  af:   mov    %rax,-0x70(%rbp)       ; ← 確認 priv 真的就在 rbp-0x70
  bc:   call   create_oops
```

**指令圖案和書上一模一樣**（`lea 0x78(%rax)` 取結構成員、`lea -0x??(%rbp)` 取局部變數、
三個參數分別進 RDI/RSI/RDX），只有偏移量因為 GCC 版本不同而從 `-0x68` 變成 `-0x70`。
書上的推導方法**在 2026 年的編譯器上依然成立**。

### 驗證三：在板子上，對一個**真的卡死的核心行程**做同樣的推導

書上的情境（§4.10）：模組先 `down_write(&mm->mmap_sem)`，再在 `create_oops()` 裡 `down_read()`
同一把鎖 → 自己鎖死自己。[`rwsem_lab.c`](./experiments/rwsem_lab.c) `mode=2` 把它原樣搬到 ARM64：

```console
$ sudo insmod rwsem_lab.ko mode=2      # insmod 從此永遠卡在 D 狀態
[ 2661.615322] rwsem_lab: insmod pid=7100 task=ffff000106d0ae80 mm=ffff000107675400
               &mm->mmap_lock=ffff000107675488 (offset 0x88)
[ 2661.615385] rwsem_lab: 局部變數 &priv=ffff80001024ba40（Q10 要推導的就是這個位址）
[ 2661.615404] rwsem_lab: create_oops: 進來了，priv=ffff80001024ba40 sem=ffff000107675488，準備 down_read
```

`&priv` 是模組自己印的**標準答案**，接下來假裝不知道它，用 crash 的方法推。

**步驟 1：`ps` 找 D 狀態行程**（`crash_probe` 的 `ps`）

```
crash_probe: === 真正的 D 狀態行程（UNINTERRUPTIBLE && !NOLOAD && !WAKEKILL）===
crash_probe:      PID     PPID  CPU TASK               COMM
crash_probe:     7100        1    2 ffff000106d0ae80 insmod
crash_probe: 共 1 個真 D 狀態；另有 98 個 TASK_IDLE、2 個 TASK_KILLABLE 被濾掉
```

**步驟 2：`bt 7100` 看呼叫關係**（框架鏈走訪，和 crash 的 `bt` 同一套演算法）

```
crash_probe: === PID 7100 COMMAND "insmod" TASK ffff000106d0ae80 CPU 2 STATE UN(D) ===
crash_probe: 核心堆疊 [ffff800010248000, ffff80001024c000)  THREAD_SIZE=16384
crash_probe: thread.cpu_context: fp(x29)=ffff80001024b890 sp=ffff80001024b890 pc=__switch_to+0x108
crash_probe: #0  [ffff80001024b898] __schedule+0x580/0x688
crash_probe: #1  [ffff80001024b8d8] schedule+0x88/0xd4
crash_probe: #2  [ffff80001024b938] schedule_preempt_disabled+0x14/0x1c
crash_probe: #3  [ffff80001024b958] rwsem_down_read_slowpath+0x26c/0x28c
crash_probe: #4  [ffff80001024b9b8] down_read+0x54/0x80
crash_probe: #5  [ffff80001024ba08] create_oops.constprop.0+0x38/0x60 [rwsem_lab]
crash_probe: #6  [ffff80001024ba28] book_case+0x11c/0x134 [rwsem_lab]      ← 相當於書上的 init_module
crash_probe: #7  [ffff80001024baa8] lab_init+0x38/0x1000 [rwsem_lab]
crash_probe: #8  [ffff80001024bad8] do_one_initcall+0x88/0x1cc
crash_probe: #9  [ffff80001024bb48] do_init_module+0x54/0x1d8
crash_probe: #10 [ffff80001024bb78] load_module+0x18f8/0x1974
crash_probe: #11 [ffff80001024bc88] __do_sys_finit_module+0x104/0x124
…（一路到 el0t_64_sync）
```

和書上的 `insmod` 回溯（行 2270-2290）結構完全一致：
`load_module → do_one_initcall → init_module → create_oops → down_read → schedule`。
框架 #6 的「父 x29」就是 `book_case` 的框架指標 = **`0xffff80001024baa0`**，
它的返回位址欄位在 **`0xffff80001024baa8`**（`bt` 印在 `#7` 那一行的中括號裡）。

**步驟 3：反組譯，找出 `priv` 相對框架指標的偏移量**（= 書上的 `dis init_module`）

```console
$ objdump -d rwsem_lab.ko | sed -n '/<book_case>:/,/^$/p'
 194:   sub     sp, sp, #0x90            ; 開 0x90 位元組的框架
 19c:   stp     x29, x30, [sp, #96]      ; 框架記錄放在 sp+0x60
 1a0:   add     x29, sp, #0x60           ; ★ x29 = sp + 0x60
 …
 1f8:   add     x20, x0, #0x88           ; x20 = mm + 0x88 = &mm->mmap_lock（= 書上的 lea 0x78(%rax)）
 218:   stp     x19, x20, [sp, #72]      ; 把 mm 和 sem 存到堆疊 sp+72（= 書上的 mov %r11,-0x68(%rbp)）
 224:   bl      down_write
 …
 28c:   str     w0, [sp, #64]            ; priv.i = 10   → priv 從 sp+0 開始
 29c:   mov     x0, sp                   ; ★ 第 1 個參數 = &priv（= 書上的 lea -0x68(%rbp),%rcx）
 2a0:   mov     x1, x20                  ;   第 2 個參數 = sem
 2a4:   bl      create_oops.constprop.0
```

`priv` 在 `sp+0`，而 `x29 = sp+0x60` → **`priv = x29 − 0x60`**。

> ⚠ **注意方向**：Q3 的使用者態程式裡局部變數是在 `x29 **+** 0x70`，這裡卻是 `x29 **−** 0x60`。
> 原因是 ARM64 的框架記錄（`stp x29,x30`）**放在框架的哪個位置由編譯器自己決定**：
> 這裡 GCC 把它放在 `sp+0x60`（框架中間），Q3 那支 `-O0` 程式則放在接近底部。
> 所以偏移量與方向**一定要從反組譯讀出來**，不能背——x86_64 因為 `push %rbp` 的關係方向固定（永遠是負的），
> 這是 ARM64 分析時最容易翻車的地方。

**步驟 4：套公式算出位址**

```
書上（x86_64）：priv = 棧返回地址欄位 − 0x8 − 偏移量
本機（ARM64）： priv = 框架返回地址欄位 − 0x8 − 0x60
              = 0xffff80001024baa8 − 0x8 − 0x60
              = 0xffff80001024ba40      ← 和模組印出來的 &priv 一位不差 ✓
```

**步驟 5：`rd` 驗證**（`crash_probe` 的 `btf`，= crash 的 `bt -f`）

```
crash_probe: ffff80001024ba20: ffff80001024baa0  [堆疊位址 = 某個框架的 x29]
crash_probe: ffff80001024ba28: ffff80000111f2a8  book_case+0x11c/0x134 [rwsem_lab]
crash_probe: ffff80001024ba30: ffff000107675400              ← mm（步驟 3 的 stp 存進來的）
crash_probe: ffff80001024ba38: ffff000107675488              ← sem = &mm->mmap_lock
crash_probe: ffff80001024ba40: 68737568736e6562              ← ★ priv.name = "benshushu"
crash_probe: ffff80001024ba48: 00000000ffff0075
```

`0x68737568736e6562` 反過來讀就是 `b e n s h u s h u`——和書上 `rd ffff8c4bc4aefcc8` 得到
`000000006f676966 / benshushu....` 是同一個把戲。

### 方法總結（可以直接背的四句）

1. `bt -f` 找到**目標函式那一格框架的返回位址欄位**（框架指標 = 返回位址欄位 − 8）。
2. `dis` 反組譯該函式，找呼叫子函式前的 `lea`（ARM64 是 `add`／`mov …, sp`），
   讀出**局部變數相對框架指標的偏移量**。
3. 相加減得到位址；**如果指令是 `mov`（`ldr`）而不是 `lea`（`add`），那格裡放的是指標，要多 `rd` 一次**。
4. 用 `struct <型別> <位址>` 或 `rd` 印出來，看內容合不合理（字串、小整數、看起來像位址的值）當作驗證。

---

<a name="q11"></a>
## Q11：如何分析和推導一個讀寫信號量的持有者？

### 書上怎麼說（奔跑吧卷2 §4.11.2，行 3040-3110）

書上兩條路：

- **路徑 A（看 owner）**：`struct rw_semaphore <addr>` 印出 `owner`；3.10 裡 `owner` 是
  `struct task_struct *`，**寫者持有時指向持有者**；讀者持有時是 `1`（書上行 3080：「owner 為 1，
  這表示被持有的鎖是一個讀者鎖」）。
- **路徑 B（反推 mm）**：`mmap_sem` 內嵌在 `mm_struct` 裡，所以
  **`mm_struct 位址 = 鎖位址 − 0x78`**，再用 `struct mm_struct.owner` 找到 task（書上圖 4.9）。

### 6.1 的 `rw_semaphore` 已經不一樣了

```c
include/linux/rwsem.h:48   struct rw_semaphore {
                     :49       atomic_long_t count;      // ← 3.10 是 long，語意也全變了
                     :55       atomic_long_t owner;      // ← 3.10 是 struct task_struct *
                     :57       struct optimistic_spin_queue osq;
                     :59       raw_spinlock_t wait_lock;
                     :60       struct list_head wait_list;

kernel/locking/rwsem.c:63  #define RWSEM_READER_OWNED   (1UL << 0)   // owner 的低 3 位是旗標
                     :64  #define RWSEM_NONSPINNABLE   (1UL << 1)
                     :117 #define RWSEM_WRITER_LOCKED  (1UL << 0)   // count 改成位元編碼
                     :118 #define RWSEM_FLAG_WAITERS   (1UL << 1)
                     :119 #define RWSEM_FLAG_HANDOFF   (1UL << 2)
                     :120 #define RWSEM_FLAG_READFAIL  (1UL << 63)
                     :122 #define RWSEM_READER_SHIFT   8            // 讀者個數在 bit8 以上
```

所以書上「`owner == 1` 代表讀者鎖」在 6.1 要改成
「**`owner & 1` 代表讀者持有，而且 `owner & ~7` 只是『最後一個讀者』，不是全部讀者**」。

### 本機驗證（`crash_probe` 的 `rwsem` 指令 = crash 的 `struct rw_semaphore`）

書上原版情境（`insmod` 自鎖，PID 7100）：

```console
$ echo 'rwsem ffff000107675488' > /proc/crash_probe
```

```
crash_probe: === struct rw_semaphore ffff000107675488 ===
crash_probe: count = 0x3
crash_probe:   bit0 WRITER_LOCKED = 1   ← 有寫者持有
crash_probe:   bit1 FLAG_WAITERS  = 1   ← wait_list 非空
crash_probe:   bit2 FLAG_HANDOFF  = 0
crash_probe:   bit63 READFAIL     = 0
crash_probe:   讀者個數 (count >> 8) = 0
crash_probe: owner = 0xffff000106d0ae80  (旗標: READER_OWNED=0 NONSPINNABLE=0)
crash_probe: → 寫者持有：PID 7100 (insmod) task=ffff000106d0ae80  ★ 這就是 Q11 要的鎖持有者
```

**路徑 B（書上圖 4.9 的反推法）也走一遍**：

```console
$ echo 'mmowner ffff000107675488' > /proc/crash_probe
```

```
crash_probe: === 從 rw_semaphore 反推 mm_struct（書上 §4.11.2 圖 4.9）===
crash_probe: mmap_lock 位址 0xffff000107675488 - offsetof(mm_struct, mmap_lock)=0x88 → mm=ffff000107675400
crash_probe: mm->mm_users=4 mm->map_count=30（數值合理就代表推導正確）
crash_probe: mm->owner = ffff000106d0ae80 → PID 7100 (insmod)  ★ 鎖的主人
```

**兩條路得到同一個 `task_struct`（`0xffff000106d0ae80`）**，互相驗證成功。
偏移量是 **0x88** 而不是書上的 0x78——這正是為什麼書上一直強調要用 `struct -o mm_struct` 在**自己的**
vmcore 上查偏移量，而不是背數字。

模組自己的鎖（`mode=1`，非 `mm` 內嵌的鎖）也是同樣的判讀：

```
crash_probe: === struct rw_semaphore ffff800001101038 ===
crash_probe: count = 0x3   （WRITER_LOCKED | FLAG_WAITERS）
crash_probe: owner = 0xffff000107192e80 → 寫者持有：PID 4770 (rwsem_holder)
```

### 判讀 `count`／`owner` 的速查表（6.1）

| 狀態 | `count` 特徵 | `owner` 特徵 |
|---|---|---|
| 沒人持有 | 0 | 0（或殘留的舊值＋旗標） |
| 一個寫者 | `bit0 = 1` | 指向持有者 task，低 3 位為 0 |
| N 個讀者 | `count >> 8 == N`，`bit0 = 0` | **最後一個**讀者 \| `RWSEM_READER_OWNED(1)` |
| 有人在等 | `bit1 = 1` | — |
| 交棒中 | `bit2 = 1`（等太久，禁止插隊） | — |
| 本機實測（寫者持有 + 有人等） | **`0x3`** | task 指標，旗標 0 |
| 書上 3.10 同情境 | `0xffffffff00000001` | `1`（讀者持有） |

**面試講法**：拿到一把卡住的 rwsem，先看 `count` 的 bit0 決定「是寫者卡住還是讀者卡住」，
再看 `owner`：寫者的話 `owner` 就是兇手；**讀者的話 `owner` 只給你其中一個讀者，
剩下的要靠 `wait_list` 和每個行程的堆疊回溯去湊**（這是 rwsem 比 mutex 難查的根本原因——
讀者鎖沒有完整的持有者清單）。

---

<a name="q12"></a>
## Q12：如何分析和推導有哪些行程在等待這個讀寫信號量？

### 書上怎麼說（奔跑吧卷2 §4.11.2，行 3110-3147）

> 「所有在等待讀寫信號量的進程都會在鎖的一個等待隊列裡等待，這個等待隊列就是 rw_semaphore 數據結構
> 中的 wait_list 成員。我們只需要使用 list 命令輸出這個鏈表的成員就可以知道哪個進程在等待這個鎖。」

```
crash> list -s rwsem_waiter.task,type -h 0xffff94941535bd90
ffff94941535bd90   task = 0xffff94940f64b0c0   type = RWSEM_WAITING_FOR_WRITE
ffff949412e83d70   task = 0xffff94940f64d140   type = RWSEM_WAITING_FOR_READ
```

### 本機驗證：走訪 `wait_list`

`struct rwsem_waiter` 是 `kernel/locking/rwsem.c:341-347` 的**私有結構**（沒有出現在任何標頭檔），
所以 [`crash_probe.c`](./experiments/crash_probe.c) 照抄一份定義（這也是 crash 工具靠 DWARF 做的事）：

```c
struct rp_rwsem_waiter {          /* = struct rwsem_waiter，rwsem.c:341-347 */
        struct list_head list;
        struct task_struct *task;
        enum rp_rwsem_waiter_type type;   /* 0 = FOR_WRITE, 1 = FOR_READ */
        unsigned long timeout;
        bool handoff_set;
};
```

**情境 A：書上原版（`insmod` 抱著 `mmap_lock` 寫者鎖自鎖）**

```
crash_probe: --- wait_list（Q12：誰在等這把鎖）head=ffff0001076754b8 ---
crash_probe: waiter[0] @ffff80001024b978  task=ffff000106d0ae80 PID 7100 (insmod)  type=RWSEM_WAITING_FOR_READ  state=UN(D)
crash_probe: waiter[1] @ffff800011123b08  task=ffff000104a38f80 PID 7105 (pgrep)   type=RWSEM_WAITING_FOR_READ  state=UN(D)
crash_probe: waiter[2] @ffff800010c43b08  task=ffff00014165be00 PID 7213 (ps)      type=RWSEM_WAITING_FOR_READ  state=UN(D)
crash_probe: 共 3 個等待者
```

**這三行完整重現了書上 §4.11 的災難現場**，而且是自己撞出來的：

- `waiter[0]` 是 **`insmod` 自己**——它持有寫者鎖，又去排隊要讀者鎖，
  典型的 AA 自鎖（書上 §4.10 的案例）。
- `waiter[1]`、`waiter[2]` 是 **`pgrep` 和 `ps`**：它們掃 `/proc` 的時候會去讀被卡住那個行程的
  `/proc/7100/cmdline`，路徑是
  `proc_pid_cmdline_read() → get_mm_cmdline()`（`fs/proc/base.c:255,357`）
  `→ access_remote_vm() → __access_remote_vm()`（`mm/memory.c:5678-5685`）
  `→ mmap_read_lock_killable(mm)` —— 正好是同一把鎖的讀者側，於是一起卡死。
  這就是書上 §4.11.2 裡那個「連 `ps` 都卡住」的 `ps` 行程，**一字不差地在 ARM64 上重演**。
- 附帶收穫：那次實驗之後，**任何會掃 `/proc` 的指令（`ps`、`top`、`pgrep`）在這台機器上都會卡死**，
  連 ssh 進去執行 `ps` 都會掛住。只有不碰 `/proc/<pid>/` 的工具（我的 `crash_probe`、`dmesg`）還能用。
  這也是嵌入式現場「機器還能 ping、ssh 進得去、但一打 `ps` 就沒反應」的經典徵狀。

**情境 B：模組自己的鎖（`mode=1`，兩種型別的等待者都有）**

```
crash_probe: waiter[0] @ffff80001024bd38  task=… PID 4771 (rwsem_rd1)  type=RWSEM_WAITING_FOR_READ   state=UN(D)
crash_probe: waiter[1] @ffff80001026bd98  task=… PID 4772 (rwsem_wr2)  type=RWSEM_WAITING_FOR_WRITE  state=UN(D)
crash_probe: 共 2 個等待者
```

和書上 `list -s rwsem_waiter.task,type` 的輸出格式、內容都對得起來（一個等讀、一個等寫）。

### 一個書上沒講、但很好用的細節

等待者的位址 `0xffff80001024b978` 落在 `insmod` 自己的核心堆疊
`[ffff800010248000, ffff80001024c000)` 裡面——因為 **`struct rwsem_waiter` 是
`rwsem_down_read_slowpath()` 的區域變數**，睡著的時候就掛在堆疊上。

實務上的兩個用途：

1. 反過來從 waiter 的位址就能判斷「這個等待者是誰」（位址落在哪條執行緒的核心堆疊裡）。
2. 這也解釋了為什麼**等待者一定是 D 狀態**：它的 waiter 結構還活在堆疊上，函式沒返回，
   誰也不能把它叫醒然後讓它離開這個函式，除非鎖真的到手（`down_read_killable` 除外，見下）。

### 6.1 的差異：卡住的 `ps` 已經**不是**純 D 狀態了

`crash_probe` 的 `ps` 印出 `另有 … 2 個 TASK_KILLABLE 被濾掉`，那兩個就是 `pgrep`/`ps`：
6.1 的 `proc_pid_cmdline_read()` 用的是 **`mmap_read_lock_killable()`**，狀態是
`TASK_UNINTERRUPTIBLE | TASK_WAKEKILL`。所以：

- 書上 3.10 的 `ps | grep UN` 會把它們列出來；6.1 如果照抄 `hung_task` 的判斷式（排除 `TASK_WAKEKILL`）
  反而會**漏掉它們**，`khungtaskd` 也不會替它們發警告（因為「可以殺掉」＝不算真的掛死）。
- 對應的實務手法：這種行程 `kill -9` 是**殺得掉**的，殺掉之後 `ps` 就恢復正常，
  但真正的兇手（拿著寫者鎖的那位）還在。

---

<a name="q13"></a>
## Q13：如何分析一個行程被阻塞了多長時間？

### 書上怎麼說（奔跑吧卷2 §4.11.4，行 3242-3304）

1. `set <pid>` 切換 crash 的行程上下文。
2. `task -R sched_info` 印出 `sched_info`：`pcount`、`run_delay`、**`last_arrival`**（上次真正在 CPU 上開始跑的時間戳）。
3. `runq -t` 印出**每顆 CPU 就緒佇列的當前時間戳**。
4. 相減：`1755597424093 − 1658412338927 = 97185085166` ns ≈ **97 秒**（書上用 `pd` 算）。

### 原理（為什麼這樣算是對的）

- `sched_info.last_arrival` 在 `sched_info_arrive()` 裡被寫成 `rq_clock(rq)`——
  也就是「這條行程**最後一次被排上 CPU** 的時刻」。
- 一條卡在 D 狀態的行程，從被 `schedule()` 換出去之後就再也沒有 arrival，
  所以 `now − last_arrival` 就是它「離開 CPU 到現在」的時間 ≈ 被阻塞的時間。
- `runq -t` 給的是每顆 CPU 的 `rq->clock`，和 `last_arrival` 同一個時間基準（`sched_clock`），才能相減。

### 本機驗證

**先踩一個坑**：6.1 的 `sched_info` 由 static key `sched_schedstats` 管控，本機預設是關的：

```console
$ cat /proc/sys/kernel/sched_schedstats
0                       ← 不打開的話 last_arrival 永遠是 0，這題就沒得算
$ sudo sysctl -w kernel.sched_schedstats=1
```

（`CONFIG_SCHED_INFO=y`、`CONFIG_SCHEDSTATS=y` 都有，只是 runtime 預設關；**重開機會恢復 0**。）

然後對卡在讀寫信號量上的 `rwsem_rd1`（PID 2845）下手：

```console
$ echo 'sched 2845' > /proc/crash_probe
```

```
crash_probe: === PID 2845 (rwsem_rd1) state=UN(D) cpu=4 ===
crash_probe: sched_info = {
crash_probe:   pcount       = 2   (被排程上 CPU 的次數)
crash_probe:   run_delay    = 0 ns (在 runqueue 上等 CPU 的累計時間)
crash_probe:   last_arrival = 171375290136 ns (上次真正在 CPU 上開始跑的時間戳)
crash_probe:   last_queued  = 0 ns
crash_probe: }
crash_probe: 目前 CPU4 的 rq clock = 200532745859 ns
crash_probe: ★ 被阻塞時間 = 200532745859 - 171375290136 = 29157455723 ns = 29.157 秒
crash_probe: nvcsw=2 nivcsw=0 (自願/非自願切換次數，hung_task 就是比這個)
```

**對答案**：這條執行緒在 dmesg 時間 `[184.42]` 建立並馬上卡住，量測發生在 `[213.58]`，
實際經過 **29.16 秒** —— 推算出來的 **29.157 秒**吻合到小數點後兩位 ✓

書上原版情境（`insmod` 自鎖）量到的是 **264.27 秒**：

```
crash_probe:   last_arrival = 2648508482937 ns
crash_probe: 目前 CPU2 的 rq clock = 2912782137845 ns
crash_probe: ★ 被阻塞時間 = 264273654908 ns = 264.273 秒
```

### 三個實作上的注意事項（書上沒寫）

1. **`sched_clock()` 的絕對值和 dmesg 時間戳不一樣**（本機實測兩者差十幾秒，方向還會隨開機而變），
   所以**只能拿兩個同源的時間戳相減**，不要拿 `last_arrival` 去和 printk 的時間比。
2. `crash` 的 `runq -t` 讀的是 `rq->clock`；模組裡拿不到 `struct rq` 的定義（也沒 export
   `sched_clock_cpu`），本機改用 **`sched_clock()`**——同一個時間基準，差別只在 `rq->clock` 是
   「上次更新時的快照」，會比 `sched_clock()` 舊一點（最多一個 tick）。
3. **`pcount = 2`、`nvcsw = 2`** 這兩個數字本身就是強力證據：這條執行緒從誕生到現在**只被排程過 2 次**，
   完全符合「一跑起來就卡死」。分析時可以拿它和 `hung_task` 的判斷依據互相印證。

### 其他量阻塞時間的方法（現場更常用）

| 方法 | 指令 | 適用 |
|---|---|---|
| `sched_info`（本題） | crash `task -R sched_info` + `runq -t` | 有 vmcore；精度 ns |
| `/proc/<pid>/stat` 第 22 欄 `starttime` + 第 15/16 欄 utime/stime | `cat /proc/PID/stat` | 活的系統，粗略 |
| `/proc/<pid>/sched` | `se.exec_start`、`nr_switches` | 活的系統，要 `CONFIG_SCHED_DEBUG` |
| `/proc/<pid>/wchan` + `stack` | `cat /proc/PID/stack` | 活的系統，直接看卡在哪個函式 |
| delay accounting | `getdelays -d -p <pid>` | 要 `CONFIG_TASK_DELAY_ACCT`（**本機沒開**） |
| hung_task 警告 | dmesg | 有 `CONFIG_DETECT_HUNG_TASK`（**本機沒開**，見 Q9 自製版） |

---

## 附錄 A：`crash_probe` 與 crash 子命令對照表

| crash 子命令 | 本文的替身 | 實作方式 |
|---|---|---|
| `ps \| grep UN` | `echo ps > /proc/crash_probe` | `for_each_process_thread()` + `hung_task.c:206-210` 的同一組判斷式 |
| `bt <pid>` | `bt <pid>` | 從 `task->thread.cpu_context.fp` 開始走 x29 框架鏈，`sprint_symbol()` 解符號 |
| `bt -f <pid>` | `btf <pid>` | 從 `cpu_context.sp` 到堆疊頂端逐 8 位元組 dump，標註「是核心程式碼位址／是堆疊位址」 |
| `rd <addr>` | `rd <hex> [n]` | 直接解引用（先做 `virt_addr_valid` 檢查） |
| `struct rw_semaphore <addr>` | `rwsem <hex>` | 照抄 `rwsem.c` 的私有巨集解碼 count/owner |
| `list -s rwsem_waiter.task,type -h <addr>` | `rwsem <hex>`（同一個指令一起印） | 照抄 `struct rwsem_waiter` 定義後 `list_for_each_entry` |
| `struct mm_struct.owner <addr>` | `mm <pid>` / `mmowner <hex>` | `offsetof()` 反推 + `mm->owner` |
| `task -R sched_info` | `sched <pid>` | 直接讀 `task->sched_info` |
| `runq -t` | `runq` | `sched_clock()`（`sched_clock_cpu` 沒 export） |
| `sym <name>` | `sym <name>` | kprobe 取回 `kallsyms_lookup_name`（見 [`ksym.h`](./experiments/ksym.h)） |
| `dis <func>` | 板子上 `objdump -d xxx.ko` | 模組符號用 `/proc/kallsyms` 對位址 |
| `log` | `dmesg` | — |

**`crash_probe` 相對於真 crash 的優勢**（在這種現場意外地重要）：它不碰 `/proc/<pid>/`，
所以當 `mmap_lock` 被卡死、`ps`/`top`/`pgrep` 全部掛住時，**它仍然能用**。

---

## 附錄 B：踩過的坑

| 坑 | 症狀 | 解法 |
|---|---|---|
| `__builtin_frame_address(0)` 寫在輔助函式裡 | 印出來的「父框架」全部錯位，三層框架看起來互相重疊 | 改寫成巨集 `DUMP_FRAME()` |
| 在 `func1` 返回之後才 dump 堆疊 | 下層框架已被 `printf` 覆蓋，dump 出來是垃圾 | 在 `func2` 還活著的時候 dump |
| `t->last_switch_count` | 編譯失敗 `has no member named` | 該欄位受 `CONFIG_DETECT_HUNG_TASK` 保護，模組自備一張表 |
| `__kernel_text_address` / `sched_clock_cpu` | `modpost: undefined!` | 用 `sprint_symbol()` 的回傳字串判斷是否為符號；時間戳改用 `sched_clock()` |
| `kallsyms_lookup_name` | 5.7 起不再 export | kprobe 取回（[`ksym.h`](./experiments/ksym.h)） |
| 只看 `TASK_UNINTERRUPTIBLE` 找 D 行程 | 一次印出 95~115 個閒置 kworker | 照 `hung_task.c` 排除 `TASK_NOLOAD`、`TASK_WAKEKILL` |
| 關中斷死迴圈用 `jiffies` 當結束條件 | 若剛好凍住 timekeeping CPU，`jiffies` 不再前進 → 永遠出不來 | 改用 NMI-safe 的 `ktime_get_mono_fast_ns()` |
| 跑完 `mode=2` 之後想用 `ps` 看狀況 | ssh 進去打 `ps`／`pgrep` 直接掛住，連帶把腳本卡死 | 只用 `dmesg` + `crash_probe`；分析完 `reboot -f` |

---

## 相關筆記

- 📝 [ch10 併發與同步](./ch10_concurrency_and_synchronization.md)（rwsem/qspinlock 的內部結構、樂觀自旋）
- 📝 [ch11 中斷管理](./ch11_interrupt_management.md)（GIC-600、pt_regs、每 CPU 中斷堆疊）
- 📝 [ch12 內核調試與性能優化](./ch12_kernel_debugging_and_performance_optimization.md)（oops 解讀、`-O0` vs `-O2`、死鎖實測）
- 📝 [ch14 基於 ARM64 解決宕機難題](./ch14_arm64_crash_debugging.md)（下一章：同樣的題目在 ARM64 上怎麼做）
