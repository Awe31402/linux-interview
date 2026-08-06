# 卷2 第 5 章（全書第 14 章）基於 ARM64 解決宕機難題 — 高頻面試題解答

> **來源**：《奔跑吧 Linux內核》（第二版）卷2 第 5 章〈基于ARM64解决宕机难题〉開篇「本章的高頻面試題」，共 7 題。
> 原文純文字檔：`books/running-linux-kernel/running-kernel-2-txt/12_第5章_基于ARM64解决宕机难题.txt`
> （下稱「奔跑吧卷2 §5.x」，**行號一律以該純文字檔為準**）
> 本章大量引用卷1 第 1 章 §1.6「ARM64 的函數調用標準與棧佈局」：
> `books/running-linux-kernel/running-kernel-1-txt/09_第1章_处理器架构.txt:3595-3690`（表 1.25、圖 1.31）

---

## 本章面試題目列表

1. 假设函数调用关系为main()→func1()→func2()，请画出ARM64架构的函数栈的布局。
2. 在ARM64架构中，子函数的栈空间的FP指向哪里？
3. 在ARM64架构的calltrace日志里，如何推导出函数的名称？
4. 在ARM64架构中，如何使用Kdump+Crash工具来分析和推导一个局部变量存在栈的位置？
5. 在ARM64架构中，如何使用Kdump+Crash工具来分析和推导一个读写信号量的持有者？
6. 在ARM64架构中，如何使用Kdump+Crash工具来分析和推导有哪些进程在等待读写信号量？
7. 在ARM64架构中，如何使用Kdump+Crash工具来分析一个进程被阻塞了多长时间？

---

## 這一章終於是「本機主場」

第 4 章（x86_64）得借一台 x86 主機，這一章不用了 —— **實驗機本身就是 ARM64**：

| | 書上 | 本機 |
|---|---|---|
| 平台 | QEMU virt 虛擬機（§5.1，行 22-40） | **Radxa ROCK 5B 真硬體**（RK3588，4×A76 + 4×A55） |
| 系統 | Debian（QEMU）+ Ubuntu 20.04 主機 | Debian 12，`Linux rock-5b 6.1.115+ #1 SMP aarch64` |
| 核心 | Linux 5.0.0+ | Rockchip 6.1.115（`-Os` 編、`CONFIG_DEBUG_INFO_NONE=y`） |
| Kdump | `kdump-tools` 服務正常，抓得到 `dump.2019…` | **`# CONFIG_KEXEC is not set`**，沒有 vmcore |
| crash 工具 | 有 | **這次裝起來了**（crash 8.0.2），過程與踩到的牆見[附錄 A](#appendix-a) |

因為書上這 7 題有 4 題（Q4~Q7）開頭就是「如何使用 Kdump+Crash 工具」，這次我花了不少力氣去**把真正的
crash 工具搞起來**：

1. `apt install crash` → crash 8.0.2 裝好；
2. 用板子上的原始碼 + `/proc/config.gz` 的組態 **重建一個帶 DWARF 的 vmlinux**，
   而且把版本橫幅、符號位址都對到**和正在跑的核心一模一樣**（[附錄 A](#appendix-a) 有逐項比對）；
3. 編譯 crash 官方的 memory driver → `/dev/crash` 出現，`dd` 讀實體記憶體 OK。

結果卡在最後一步：**crash 8.0.2 在「沒有 vmcore、也沒有 `/proc/kcore`」的活系統上，
猜錯了本機的 VA 佈局（猜 VA_BITS=47，實際 48）**，所以拒絕認這個 vmlinux。
詳細分析在附錄 A —— 這本身就是一條有價值的結論：**Kdump 這條路在嵌入式板子上不是「裝個工具」就有，
它從核心組態就要先開**。

所以本章的做法是：**書上的每一個推導步驟都照做，只是把 crash 的子命令換成等價的工具**：

| 書上用的 crash 子命令 | 本機的替代 | 為什麼等價 |
|---|---|---|
| `bt` / `bt -f` | [`crash_probe.ko`](./experiments/crash_probe.c) 的 `bt`/`btf` | 同一套 x29 框架鏈演算法（見 Q1、Q2） |
| `dis` / `dis -l` | `objdump -d vmlinux`（**位址完全對得上的那個 vmlinux**）、`objdump -d xxx.ko` | 同一份機器碼 |
| `rd` | `crash_probe` 的 `rd` | 直接讀核心記憶體 |
| `struct rw_semaphore` / `list -s rwsem_waiter` | `crash_probe` 的 `rwsem` | 照抄 `kernel/locking/rwsem.c` 的私有定義 |
| `task -R sched_info` / `runq -t` | `crash_probe` 的 `sched` / `runq` | 同樣讀 `task->sched_info` 與 `sched_clock()` |
| `sym` / 模組符號 | `/proc/kallsyms` + `/sys/module/*/sections/.text` + `nm` | kallsyms 就是核心自己的符號表 |
| （書上沒有） | `scripts/faddr2line` | 位址 → 原始碼行號 |

**實驗程式碼**：全部在 [`notes/experiments/`](./experiments/)，一鍵重現
[`experiments/ch14_run_all.sh`](./experiments/ch14_run_all.sh)。

---

## 書是 5.0 + QEMU 寫的，本機是 6.1.115 + RK3588 —— 11 處差異

| # | 書上（5.0 / QEMU virt） | 本機實測（6.1.115 / ROCK 5B） | 題號 |
|---|------|------|------|
| 1 | QEMU 裡 Kdump 服務正常，`crash dump.xxx vmlinux` | **`CONFIG_KEXEC`/`CONFIG_CRASH_DUMP` 都沒開**，連 `/sys/kernel/kexec_crash_loaded` 都不存在；`crash` 在活系統上因為**猜錯 VA_BITS（47 vs 48）**而拒絕啟動 | Q4~Q7 |
| 2 | 「處理器的 FP 和 SP 寄存器相同……指向該函數棧空間的 FP 處，即棧底」（卷1 行 3669-3672） | **只對小框架成立**。本機核心（`-Os`）的 `lab_func2` 是 `sub sp,#0x40; stp x29,x30,[sp,#16]; add x29,sp,#0x10` → **x29 = sp + 0x10，框架記錄在框架中間** | Q1、Q2 |
| 3 | 沒提葉子函式 | **真葉子函式連框架記錄都不建**（實測 `leaf` 的反組譯裡沒有任何 `stp x29,x30`），所以 **calltrace 上永遠不會出現**——本章實測的 oops 就少了一層 `oops_lab_init` | Q2、Q3 |
| 4 | 式(5.2)：`PC_f = *(FP_c + 8) − 4` | 6.1 核心自己印 calltrace 時用 **`%pSb`**（`stacktrace.c:139`）→ `sprint_backtrace()` → `__sprint_symbol(..., -1, ...)`，**減的是 1 不是 4**（`kernel/kallsyms.c:605-621`，註解說明是為了處理 tail-call 到 noreturn 函式的情況） | Q3 |
| 5 | 手動 `rd` 一格一格爬 FP 鏈 | 6.1 的 unwinder 已經改寫成 `struct unwind_state` + **`unwind_next_frame_record()`**（`arch/arm64/include/asm/stacktrace/common.h:155-174`），內容就是 `fp = *(fp); pc = *(fp+8)`——**和書上的式(5.1)/(5.2) 一模一樣** | Q1~Q3 |
| 6 | 沒提回溯要怎麼停 | 6.1 用 **`pt_regs.stackframe[2]`** 當終結標記（`arch/arm64/include/asm/ptrace.h:199`、`process.c:475-478`），`unwind_next()` 看到 `fp == task_pt_regs(tsk)->stackframe` 就停（`stacktrace.c:85-87`）。實測最後一格確實是 `{0, 0}` | Q1、Q3 |
| 7 | `rwsem_down_read_failed` | 6.1 叫 **`rwsem_down_read_slowpath`**；`down_write_killable` 之類的名字也都變了 | Q4~Q6 |
| 8 | `mm_struct.mmap_sem` 偏移 **0x60**（書上特別提醒「3.10 是 0x78，5.0 是 0x60」） | 6.1 改名 **`mmap_lock`**，本機偏移 **0x88** —— 書上這句提醒本身就證明「偏移量一定要自己查」 | Q4~Q6 |
| 9 | `rw_semaphore.owner` 是指標，`owner=0x1` 表示讀者持有；`count=0xffffffff00000001` | 6.1 的 `count`/`owner` 都是 **`atomic_long_t` 且是位元編碼**：本機同情境 **`count = 0x3`**（WRITER_LOCKED\|FLAG_WAITERS）、`owner` 低 3 位是旗標 | Q5 |
| 10 | 參數 1/2/3 存在 `[sp+40]/[sp+32]/[sp+24]`（§5.4 行 300-310） | 本機 `-Os` 把參數搬進 **callee-saved 的 x19~x22**，堆疊上找不到；要照書上 §5.5.2 的手法**往子函式的框架裡找**——實測 4 個參數全部找回來（見 Q4） | Q4 |
| 11 | `task -R sched_info` 直接有值 | 6.1 的 sched_info 被 static key 管控，**`sysctl kernel.sched_schedstats` 預設 0**，不開的話 `last_arrival` 永遠是 0 | Q7 |

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | 畫出 ARM64 的 main→func1→func2 函式棧佈局 | 使用者態 FP 鏈 `func2(0x…ca0)→func1(0x…cf0)→main(0x…d30)`，逐格 dump；核心態三層 `lab_func2(0x…bd90)→lab_func1(0x…bdd0)→lab_main(0x…be50)`，**和模組自己印的 ground truth 一位不差** |
| [2](#q2) | 子函式棧空間的 FP 指向哪裡 | 四種序幕實測：**真葉子函式沒有框架記錄**、小框架 `x29==sp`、**核心 `-Os` 是 `add x29,sp,#0x10` → x29 = sp+0x10**、VLA 讓 `sp` 再掉 64 B 而 `x29` 不動。唯一不變：`[x29]=父x29、[x29+8]=LR` |
| [3](#q3) | calltrace 裡如何推導函式名稱 | 真 oops 的 calltrace + 手算：`P_LR=0xffff800008014c44` − `do_one_initcall(0xffff800008014bbc)` = **+0x88**，和核心印的一致；並抓到 **calltrace 少了一層 `oops_lab_init`**（葉子函式沒建框架記錄，名字只出現在 `lr :` 那行） |
| [4](#q4) | 推導局部變數在棧的位置 | `lab_main` 的 `priv` 從反組譯的 `sub x19, x29, #0x60` 推出 = **`0xffff80001056bdf0`**，讀出 `68737568736e6562`（"benshushu"）；**參數不在棧上**，照書上 §5.5.2 從 `rwsem_down_read_slowpath` 的 `stp x19,x20,[sp,#96]` 推回去，**4 個參數 4/4 完全命中** |
| [5](#q5) | 推導讀寫信號量的持有者 | `count=0x3`、`owner=0xffff000140114d80` → **PID 28346 `lab_holder`**（寫者持有） |
| [6](#q6) | 推導哪些行程在等這個信號量 | `wait_list` 走出 2 個等待者：`lab_main`(READ)、`lab_writer`(WRITE)；並證明 **`rwsem_waiter` 就住在等待者自己的核心堆疊上** |
| [7](#q7) | 一個行程被阻塞了多久 | `last_arrival` 與 `sched_clock()` 相減 = **3276.599 秒**，與實際經過的 **3276.9 秒**吻合 |

---

## 一鍵重現

```bash
cd notes/experiments && ./ch14_run_all.sh 192.168.68.58
# 想連「重建 vmlinux」一起做（約 25 分鐘）：
scp build_vmlinux.sh radxa@192.168.68.58:/tmp/ && ssh radxa@192.168.68.58 'sudo bash /tmp/build_vmlinux.sh'
```

| 檔案 | 用途 | 題號 |
|------|------|------|
| [`arm64_frame.c`](./experiments/arm64_frame.c) | 使用者態：四種序幕（葉子/小框架/大框架/VLA）+ 三層 FP 鏈 + 逐格 dump | Q1、Q2 |
| [`arm64_lab.c`](./experiments/arm64_lab.c) | 核心模組：三層呼叫最後卡死在 rwsem，**每層的 FP、局部變數位址、參數值都先印出來當標準答案** | Q1、Q3~Q7 |
| [`crash_probe.c`](./experiments/crash_probe.c) | crash 子命令的替身（`ps`/`bt`/`btf`/`rd`/`rwsem`/`sched`/`runq`），第 4 章寫的，這章直接沿用 | Q1、Q4~Q7 |
| [`oops_lab.c`](./experiments/oops_lab.c) | 產生真的 oops（第 3 章寫的） | Q3 |
| [`build_vmlinux.sh`](./experiments/build_vmlinux.sh) | 重建「和正在跑的核心逐位元組相符、但帶 DWARF」的 vmlinux | Q3、Q4、附錄 A |
| [`ch14_run_all.sh`](./experiments/ch14_run_all.sh) | 一鍵重現 | 全部 |

---

<a name="q1"></a>
## Q1：假設呼叫關係為 main()→func1()→func2()，畫出 ARM64 的函式棧佈局

### 書上怎麼說（卷1 §1.6，行 3649-3690，圖 1.31；卷2 §5.3，行 132-160）

> 「所有的函數調用棧都會組成一個單鏈表。每個棧由兩個地址來構成這個鏈表……
> **低地址存放**：指向上一個棧幀（父函數的棧幀）的棧基地址 FP，類似於鏈表的 prev 指針，本書把它稱為 **P_FP**。
> **高地址存放**：當前函數的返回地址，也就是進入該函數時 LR 的值，本書把它稱為 **P_LR**。」

也就是書上的兩條公式（卷2 §5.3 行 150-160）：

```
式(5.1)   FP_f = *(FP_c)          ← 父函式的框架指標
式(5.2)   PC_f = *(FP_c + 8) − 4  ← 父函式呼叫子函式那一條指令的位址
```

### 本機驗證 1：使用者態（[`arm64_frame.c`](./experiments/arm64_frame.c)）

```console
$ ssh radxa@192.168.68.58 'cd ~/exp/ch14 && gcc -O1 -g -fno-omit-frame-pointer -o arm64_frame arm64_frame.c && ./arm64_frame'
==== Q1：main → func1 → func2 三層框架鏈 ====
main           x29=0xffffc64e2d30 sp=0xffffc64e2d30 x29-sp=0  [x29]=父x29=0xffffc64e2e50 [x29+8]=LR=0xffff98427744
func1          x29=0xffffc64e2cf0 sp=0xffffc64e2cf0 x29-sp=0  [x29]=父x29=0xffffc64e2d30 [x29+8]=LR=0xaaaae8e00c54
func1          &local1=0xffffc64e2d28 → x29+56
func2          x29=0xffffc64e2ca0 sp=0xffffc64e2ca0 x29-sp=0  [x29]=父x29=0xffffc64e2cf0 [x29+8]=LR=0xaaaae8e00ab0
func2          &local2=0xffffc64e2ce8 → x29+72；八個參數全部走 x0~x7，堆疊上沒有參數
```

逐格 dump（= crash 的 `bt -f`）：

```
0xffffc64e2ca0  0x0000ffffc64e2cf0  <= func2 的框架記錄：P_FP（指向 func1 的框架記錄）
0xffffc64e2ca8  0x0000aaaae8e00ab0  <= func2 的 P_LR（返回 func1 的位址）
        …（func2 的局部變數區，local2 = 0x2222222222222222 在 0xffffc64e2ce8）
0xffffc64e2cf0  0x0000ffffc64e2d30  <= func1 的框架記錄：P_FP（指向 main 的框架記錄）
0xffffc64e2cf8  0x0000aaaae8e00c54  <= func1 的 P_LR（返回 main 的位址）
        …（func1 的局部變數區，local1 = 0x1111111111111111 在 0xffffc64e2d28）
0xffffc64e2d30  0x0000ffffc64e2e50  <= main 的框架記錄：P_FP
0xffffc64e2d38  0x0000ffff98427744  <= main 的 P_LR（返回 libc 的位址）

FP 鏈：func2(0xffffc64e2ca0) → func1(0xffffc64e2cf0) → main(0xffffc64e2d30)
每層框架大小：func2 = 80、func1 = 64 位元組
```

### 本機驗證 2：核心態（[`arm64_lab.c`](./experiments/arm64_lab.c) + `crash_probe` 的 `bt`）

模組先把**標準答案**印出來：

```
arm64_lab: [答案] lab_main : x29=ffff80001056be50 sp=ffff80001056bdf0  &priv=ffff80001056bdf0
arm64_lab: [答案] lab_func1: x29=ffff80001056bdd0 sp=ffff80001056bdc0  &local1=ffff80001056bdc0(local1=0x1122334455667788)
arm64_lab: [答案] lab_func2: x29=ffff80001056bd90 sp=ffff80001056bd80  &local2=ffff80001056bd80(local2=0xdeadbeefcafe0001)
arm64_lab: [答案] lab_func2 的參數：priv=ffff80001056bdf0 sem=ffff800001101030 magic=0x1122334455667788 a8=0x8888
```

然後**假裝不知道**，純用框架鏈爬（`echo 'bt <pid>' > /proc/crash_probe`）：

```
crash_probe: === PID 28369 COMMAND "lab_main" TASK ffff000140115d00 CPU 5 STATE UN(D) ===
crash_probe: 核心堆疊 [ffff800010568000, ffff80001056c000)  THREAD_SIZE=16384
crash_probe: thread.cpu_context: fp(x29)=ffff80001056bbf0 sp=ffff80001056bbf0 pc=ffff80000904eb38 (__switch_to+0x108/0x124)
crash_probe: #0  [ffff80001056bbf8] __schedule+0x580/0x688      (框架 x29=ffff80001056bbf0 → 父 x29=ffff80001056bc30)
crash_probe: #1  [ffff80001056bc38] schedule+0x88/0xd4          (框架 x29=ffff80001056bc30 → 父 x29=ffff80001056bc90)
crash_probe: #2  [ffff80001056bc98] schedule_preempt_disabled+0x14/0x1c  (框架 x29=ffff80001056bc90 → 父 x29=ffff80001056bcb0)
crash_probe: #3  [ffff80001056bcb8] rwsem_down_read_slowpath+0x26c/0x28c (框架 x29=ffff80001056bcb0 → 父 x29=ffff80001056bd10)
crash_probe: #4  [ffff80001056bd18] down_read+0x54/0x80         (框架 x29=ffff80001056bd10 → 父 x29=ffff80001056bd60)
crash_probe: #5  [ffff80001056bd68] lab_func2+0xa4/0xf4 [arm64_lab]   (框架 x29=ffff80001056bd60 → 父 x29=ffff80001056bd90)
crash_probe: #6  [ffff80001056bd98] lab_func1+0x8c/0xb8 [arm64_lab]   (框架 x29=ffff80001056bd90 → 父 x29=ffff80001056bdd0)
crash_probe: #7  [ffff80001056bdd8] lab_main+0xa0/0xe8 [arm64_lab]    (框架 x29=ffff80001056bdd0 → 父 x29=ffff80001056be50)
crash_probe: #8  [ffff80001056be58] kthread+0xc0/0xd0                 (框架 x29=ffff80001056be50 → 父 x29=ffff80001056be70)
crash_probe: #9  [ffff80001056be78] ret_from_fork+0x10/0x20           (框架 x29=ffff80001056be70 → 父 x29=ffff80001056bfe0)
crash_probe: #10 [ffff80001056bfe8] 0x0                          (框架 x29=ffff80001056bfe0 → 父 x29=0)  ← 終結標記
```

**爬出來的 `lab_func2/lab_func1/lab_main` 的框架指標 = `bd90 / bdd0 / be50`，
和模組自己印的 `x29` 一位不差 ✓**

### 佈局圖（核心態實測數值）

```
高位址 ↑                    lab_main 的核心堆疊（16 KB，[ffff800010568000, ffff80001056c000)）
  ffff80001056bfe0 ┌─────────────────────────┐
                   │ pt_regs.stackframe = {0,0} │ ← 回溯的終結標記（process.c:475-478）
  ffff80001056be50 ├─────────────────────────┤
                   │ P_FP → 0xffff80001056be70 │ ← lab_main 的框架記錄
  ffff80001056be58 │ P_LR → kthread+0xc0       │
                   │ …                         │
  ffff80001056bdf0 │ priv = "benshushu"…       │ ← lab_main 的局部變數（= x29 − 0x60）
  ffff80001056bdd0 ├─────────────────────────┤
                   │ P_FP → 0xffff80001056be50 │ ← lab_func1 的框架記錄
  ffff80001056bdd8 │ P_LR → lab_main+0xa0      │
  ffff80001056bdc0 │ local1 = 0x1122334455667788│ ← （= x29 − 0x10）
  ffff80001056bd90 ├─────────────────────────┤
                   │ P_FP → 0xffff80001056bdd0 │ ← lab_func2 的框架記錄
  ffff80001056bd98 │ P_LR → lab_func1+0x8c     │
  ffff80001056bd80 │ local2 = 0xdeadbeefcafe0001│
  ffff80001056bd60 ├─────────────────────────┤ ← down_read 的框架記錄
  ffff80001056bd10 ├─────────────────────────┤ ← rwsem_down_read_slowpath 的框架記錄
                   │ 這裡面存著 lab_func2 的     │
                   │ x19~x22（= 它的參數！見 Q4）│
  ffff80001056bbf0 └─────────────────────────┘ ← 被切換出去時的 x29（thread.cpu_context.fp）
低位址 ↓
```

**和 x86_64 的差別**（對照 [ch13 Q3](./ch13_x86_64_crash_debugging.md#q3)）：

| | x86_64 | ARM64 |
|---|---|---|
| 誰把返回位址放到堆疊 | `call` 指令**自動 push** | `bl` 只寫進 **LR(x30)**，要不要存堆疊由被呼叫者決定 |
| 框架記錄 | `push %rbp`（8 B） | `stp x29, x30, [sp, #N]`（16 B，一次存兩個） |
| `[框架指標]` / `[+8]` | 父 RBP / 返回位址 | 父 x29 / LR（**完全同構**） |
| 局部變數 | `RBP − x`（方向固定） | **`x29 ± x` 都可能**（見 Q2） |
| 參數 | 第 7 個開始在堆疊 | 前 8 個全在 x0~x7，**堆疊上通常一個參數都沒有** |
| 葉子函式 | 至少有返回位址在堆疊 | **可能完全不碰堆疊**（連返回位址都只在 LR 裡） |

---

<a name="q2"></a>
## Q2：在 ARM64 架構中，子函式的棧空間的 FP 指向哪裡？

### 書上怎麼說（卷1 §1.6，行 3669-3672）

> 「處理器的 FP 和 SP 寄存器相同。在函數執行時 FP 和 SP 寄存器會指向該函數棧空間的 FP 處，即棧底。」

### 正確答案（一句話）

**x29 永遠指向「本函式的框架記錄」**——也就是存放 `{父函式的 x29, 本函式的返回位址}` 這 16 個位元組的地方：

```
[x29]     = P_FP  = 父函式的 x29
[x29 + 8] = P_LR  = 本函式的返回位址
```

至於「x29 是不是等於 sp」「框架記錄是不是在棧底」——**那是編譯器的自由**，書上那句話只對其中一種情形成立。
本機把四種情形全部跑出來了。

### 本機驗證：四種序幕

（下面是另一次執行的輸出，使用者態有 ASLR 所以位址和 Q1 那段不同，但相對關係一模一樣）

```console
$ ./arm64_frame        # 使用者態，gcc 12.2 -O1
vla_frame      配置 VLA 前 x29=0xffffc0989280 sp=0xffffc0989280；配置後 x29=0xffffc0989280 sp=0xffffc0989240
               → **x29 不動、sp 掉了 64 位元組**
big_frame      x29=0xffffc0989010 sp=0xffffc0989010 x29-sp=0  [x29]=父x29=0xffffc0989280
small_frame    x29=0xffffc0988ff0 sp=0xffffc0988ff0 x29-sp=0  [x29]=父x29=0xffffc0989010
leaf           sp=0xffffc0988ff0  ← 真葉子函式：它的 sp 和呼叫者 small_frame 的 sp 一模一樣，
               代表它連框架記錄都沒建；x29 完全沒動，所以 calltrace 上永遠看不到葉子函式
```

四種序幕的反組譯：

```asm
;--- (A) 真葉子函式：沒有 stp x29,x30，完全不碰堆疊 ---
0000000000000814 <leaf>:
 814:   mov     x3, sp
 818:   adrp    x2, 20000
 81c:   str     x3, [x2, #80]
 820:   mul     x0, x0, x1
 824:   ret                          ← 直接用 LR 回去

;--- (B) 小框架：stp …[sp,#-32]! + mov x29,sp → x29 == sp（書上講的情形）---
0000000000000ab4 <small_frame>:
 ab4:   stp     x29, x30, [sp, #-32]!
 ab8:   mov     x29, sp
 abc:   str     x19, [sp, #16]

;--- (C) 大框架：先開空間再放框架記錄，x29 依然等於 sp（框架記錄在棧底）---
0000000000000b34 <big_frame>:
 b34:   sub     sp, sp, #0x230
 b38:   stp     x29, x30, [sp]
 b3c:   mov     x29, sp

;--- (D) 核心模組（-Os，arm64_lab.ko）：框架記錄放在「框架中間」，x29 = sp + 0x10 ---
0000000000000000 <lab_func2>:
   8:   sub     sp, sp, #0x40
  18:   stp     x29, x30, [sp, #16]
  1c:   add     x29, sp, #0x10       ← ★ 書上那句「FP 和 SP 相同」在這裡就不成立
  24:   stp     x19, x20, [sp, #32]

00000000000001ac <lab_main>:
 1b4:   sub     sp, sp, #0x80
 1bc:   stp     x29, x30, [sp, #96]
 1c0:   add     x29, sp, #0x60       ← x29 = sp + 0x60
 1c8:   sub     x19, x29, #0x60      ← &priv = x29 − 0x60（Q4 的關鍵）
```

**結論表**：

| 情形 | 序幕 | x29 與 sp 的關係 | calltrace 看得到嗎 |
|---|---|---|---|
| 葉子函式 | 沒有框架記錄 | x29 完全不變（還是呼叫者的） | **看不到** |
| 小框架非葉子 | `stp x29,x30,[sp,#-N]!` + `mov x29,sp` | x29 == sp | 看得到 |
| 大框架非葉子 | `sub sp,#N` + `stp x29,x30,[sp]` + `mov x29,sp` | x29 == sp | 看得到 |
| **核心 `-Os` 常見** | `sub sp,#N` + `stp x29,x30,[sp,#M]` + `add x29,sp,#M` | **x29 = sp + M** | 看得到 |
| 有 VLA/alloca | 同上，之後 `sub sp, sp, xN` | **x29 固定、sp 再往下掉**（實測差 64 B） | 看得到 |

### 核心自己怎麼看這件事（6.1 原始碼）

```c
/* arch/arm64/include/asm/stacktrace/common.h:155-174 —— 這就是書上的式(5.1)/(5.2) */
static inline int unwind_next_frame_record(struct unwind_state *state)
{
        unsigned long fp = state->fp;
        ...
        state->fp = READ_ONCE(*(unsigned long *)(fp));       /* = P_FP */
        state->pc = READ_ONCE(*(unsigned long *)(fp + 8));   /* = P_LR */
        return 0;
}
```

起點與終點：

- **活著的行程**：`unwind_init_from_regs()`（`stacktrace.c:28-34`）用 `regs->regs[29]`（x29）與 `regs->pc`。
- **睡著的行程**：`unwind_init_from_task()`（`stacktrace.c:63-69`）用 `thread_saved_fp/pc`，
  也就是 `task->thread.cpu_context.{fp,pc}`——本機實測 `pc = __switch_to+0x108`，
  正是 `cpu_switch_to()` 存下來的返回點。
- **終點**：`unwind_next()`（`stacktrace.c:85-87`）看到 `fp == task_pt_regs(tsk)->stackframe` 就停；
  這格是 fork 時特意鋪好的 `{0,0}`（`arch/arm64/kernel/process.c:475-478`、`ptrace.h:199`）。
  本機 `bt` 的最後一格 `#10 [ffff80001056bfe8] 0x0（父 x29=0）` 就是它。

**面試講法**：「FP 指向本函式的框架記錄，`[FP]` 是父框架、`[FP+8]` 是返回位址；
但**不要假設 FP 等於 SP**，也不要假設每個函式都有框架記錄——葉子函式沒有，
所以 ARM64 的 calltrace 天生就可能少掉最內層那一格（Q3 有實例）。」

---

<a name="q3"></a>
## Q3：在 ARM64 的 calltrace 日誌裡，如何推導出函式的名稱？

### 書上怎麼說（卷2 §5.3，行 150-212）

> 「根據本函數棧幀裡保存的 LR 可以間接獲取父函數調用子函數時的 PC 值，從而根據符號表得到具體的函數名。
> 在調用子函數時，LR 指向子函數返回的下一條指令，**通過 LR 指向的地址再減去 4 字節偏移量**就得到了本函數的入口地址。」
> 
> 書上的示範：`rd ffff00000b903b28` → `ffff000000e590a0`，減 4 之後 `dis ffff000000e5909c` →
> `bl 0xffff000000e54000 <create_oops>`，於是知道父函式是 `_MODULE_INIT_START_oops()`。

### 本機驗證：抓一個真的 oops 來推

```console
$ sudo insmod oops_lab.ko mode=1      # 故意寫空指標
```

> oops 之後模組會永遠卡在 `MODULE_STATE_COMING`（`rmmod` 移不掉、同名模組不能再載入），
> 所以 [`ch14_run_all.sh`](./experiments/ch14_run_all.sh) 會準備 `oops_m1`~`oops_m3` 三個複本，
> 一次開機最多重跑三次。

```
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
  ESR = 0x0000000096000044     ← EC=0x25 DABT，WnR=1（寫入）
Internal error: Oops: 0000000096000044 [#1] SMP
CPU: 6 PID: 31652 Comm: insmod Tainted: G           O       6.1.115+ #1
Hardware name: Radxa ROCK 5B (DT)
pc : create_oops+0x4c/0x58 [oops_lab]
lr : oops_lab_init+0xa0/0x1000 [oops_lab]      ← ★ 注意這一行
sp : ffff800010263ac0
x29: ffff800010263ad0 x28: ffff800009f6a498 …
Call trace:
 create_oops+0x4c/0x58 [oops_lab]
 do_one_initcall+0x88/0x1cc
 do_init_module+0x54/0x1d8
 load_module+0x18f8/0x1974
 __do_sys_finit_module+0x104/0x124
 …
Code: 0b030000 0b020021 0b010000 d2800001 (b9000020)
```

#### 步驟 1：核心其實已經幫你算好了（6.1 的做法）

```c
/* arch/arm64/kernel/stacktrace.c:136-141 */
static bool dump_backtrace_entry(void *arg, unsigned long where)
{
        char *loglvl = arg;
        printk("%s %pSb\n", loglvl, (void *)where);   /* ← %pSb，不是 %pS */
        return true;
}
```

`%pSb` 走的是 `sprint_backtrace()`：

```c
/* kernel/kallsyms.c:605-621 */
 * This function is for stack backtrace and does the same thing as
 * sprint_symbol() but with modified/decreased @address. If there is a
 * tail-call to the function marked "noreturn", gcc optimized out code after
 * the call so that the stack-saved return address could point outside of the caller.
int sprint_backtrace(char *buffer, unsigned long address)
{
        return __sprint_symbol(buffer, address, -1, 1, 0);   /* ← 減 1 */
}
```

**這就是書上式(5.2) 的現代版**：書上減 4（正好一條 A64 指令，得到「那條 `bl` 的位址」），
核心減 1（只要落回前一條指令的位元組範圍內，足夠讓 kallsyms 查到正確的函式）。
兩者的目的一樣：**LR 指向的是「呼叫的下一條指令」，直接拿去查符號可能查到下一個函式頭上**。

#### 步驟 2：自己手動推一遍（完全照書上的方法）

從 oops 印出的原始堆疊裡，`x29 = 0xffff800010263ad0` 那一格的內容是：

```
0xffff800010263ad0:  10263b40 ffff8000   08014c44 ffff8000
                     └ P_FP = 0xffff800010263b40   └ P_LR = 0xffff800008014c44
```

查符號表（不需要 crash，`/proc/kallsyms` 就夠）：

```console
$ sudo grep -n " do_one_initcall$" /proc/kallsyms
ffff800008014bbc T do_one_initcall
```

```
P_LR − do_one_initcall = 0xffff800008014c44 − 0xffff800008014bbc = 0x88
→ do_one_initcall+0x88          ← 和核心印的 calltrace 第 2 行完全一致 ✓
再套書上的式(5.2)：0xffff800008014c44 − 4 = 0xffff800008014c40
→ objdump 一看就是那條 `blr x20`（呼叫 module_init 的地方）
```

#### 步驟 3：模組裡的位址怎麼查？

模組不在 `/proc/kallsyms` 的靜態符號表裡，要用「模組載入基底 + `.ko` 裡的偏移」：

```console
$ cat /sys/module/oops_lab/sections/.text
0xffff80000125c000
$ nm oops_lab.ko | grep " t create_oops"
0000000000000000 t create_oops
→ create_oops 的執行位址 = 0xffff80000125c000 + 0 = 0xffff80000125c000
→ PC = 0xffff80000125c04c 落在它裡面，偏移 0x4c ✓（= 核心印的 create_oops+0x4c）
```

再往下鑽到原始碼行號（書上沒提，但實務上最好用）：

```console
$ ~/disk/kernel-source/scripts/faddr2line oops_lab.ko create_oops+0x4c
create_oops+0x4c/0x58:
create_oops at /home/radxa/exp/ch14/oops_lab.c:46      ← 正是 *(int *)0 = a+b+c+d 那一行
```

#### 步驟 4：**本機抓到的坑——calltrace 少了一層**

比對一下就會發現問題：`lr : oops_lab_init+0xa0`，但 **Call trace 裡根本沒有 `oops_lab_init`**，
`create_oops` 的下一行直接就是 `do_one_initcall`。

原因就是 [Q2](#q2) 講的：**`create_oops()` 是葉子函式，沒有建立自己的框架記錄**，
所以出錯當下 `x29` 還是 `oops_lab_init` 的框架記錄。unwinder 從 `regs->pc` 印出 `create_oops`，
再從 `[x29+8]` 拿到 `do_one_initcall+0x88` ——**中間那層 `oops_lab_init` 的名字只存在 LR 暫存器裡**，
剛好被 oops 訊息的 `lr :` 那行印出來了。

> **面試講法**：ARM64 的 calltrace 只是「框架記錄鏈」的投影。
> 看到 calltrace 覺得「怎麼跳過一層」時，先去看 `pc :`/`lr :` 這兩行——
> 葉子函式與被內聯（inline）的函式都不會有自己的框架記錄。
> 這也是為什麼 x86 走 ORC、ARM64 走框架指標時，`CONFIG_FRAME_POINTER=y`（本機是 y）這麼重要。

#### 步驟 5：其他把位址變成名字的工具

| 工具 | 用途 | 本機範例 |
|---|---|---|
| `/proc/kallsyms` | 核心靜態符號（本機 `CONFIG_KALLSYMS_ALL=n`，**只有 text 符號**） | `grep " do_one_initcall$" /proc/kallsyms` |
| `/sys/module/<m>/sections/.text` | 模組載入基底 | `0xffff80000125c000` |
| `nm` / `objdump -d` | `.ko` 內的偏移與指令 | `nm oops_lab.ko` |
| `scripts/faddr2line` | 位址 → 檔名:行號 | `faddr2line arm64_lab.ko lab_func2+0xa4` → `arm64_lab.c:109` |
| `scripts/decodecode` | 把 oops 的 `Code:` 行反組譯 | 見 [ch12 Q14](./ch12_kernel_debugging_and_performance_optimization.md) |
| crash 的 `dis`/`sym` | 同上（需要 vmcore + vmlinux） | 見[附錄 A](#appendix-a) |

> ⚠ 一個容易忽略的細節：`faddr2line arm64_lab.ko lab_func2+0xa4` 指到的是 **`arm64_lab.c:109`**，
> 也就是 `down_read()` 的**下一行**。因為 `+0xa4` 是**返回位址**（LR），不是呼叫點——
> 這正是書上要你「減 4」的理由，在原始碼行號上一樣要記得往回看一行。

---

<a name="q4"></a>
## Q4：如何用 Kdump+Crash 分析和推導一個局部變數存在棧的位置？

### 書上怎麼說（卷2 §5.4，行 213-370）

書上的 ARM64 版做法（和 x86_64 版最大的不同）：

> 「create_oops() 函數執行時會把棧空間往下延伸 64 字節，然後把調用者的 FP 和 LR 壓入棧……
> **把參數 1 存放到 SP 寄存器+40 字節（[sp+40]）的地方，參數 2 存放到 [sp+32]，參數 3 存放到 [sp+24]**……
> 因此可以得到第 2 個參數存放的地址，即 SP 寄存器+32 字節的地址為 0xffff00000c49bb10。」

以及 §5.5.2（行 700-741）處理「參數不在棧上」的情形：

> 「ARM64 架構的函數參數調用規則中有一條規定，**x19～x28 寄存器作為臨時寄存器，子函數使用它們時必須保存到棧裡**。
> 因此，可以沿著函數調用關係 backtrace 繼續分析……在反匯編 rwsem_down_write_failed_killable() 函數時發現了 x24 寄存器的蹤影。」

### 本機驗證：兩種情況都遇到了

實驗對象是 [`arm64_lab.c`](./experiments/arm64_lab.c)：`lab_main → lab_func1 → lab_func2 → down_read`（卡死）。

#### 情況一：局部變數 `priv`（在 `lab_main` 的框架裡）

**步驟 1**：`bt` 找出 `lab_main` 的框架記錄 = `0xffff80001056be50`（見 [Q1](#q1)）。

**步驟 2**：反組譯（等同 crash 的 `dis lab_main`）：

```console
$ objdump -d --no-show-raw-insn arm64_lab.ko | sed -n '/<lab_main>:/,/^$/p'
 1b4:   sub     sp, sp, #0x80
 1bc:   stp     x29, x30, [sp, #96]
 1c0:   add     x29, sp, #0x60        ← x29 = sp + 0x60
 1c8:   sub     x19, x29, #0x60       ← ★ x19 = x29 − 0x60，這就是 &priv
 1e8:   stp     xzr, x20, [x29, #-24] ← priv.mm = NULL、priv.sem = &lab_sem
```

`sub x19, x29, #0x60` 就是 ARM64 版的 `lea -0x60(%rbp),%rcx`：**取局部變數的位址**。

**步驟 3**：套公式

```
priv = lab_main 的框架記錄 − 0x60 = 0xffff80001056be50 − 0x60 = 0xffff80001056bdf0
```

**步驟 4**：`rd` 驗證（`echo 'btf <pid>' > /proc/crash_probe`）：

```
crash_probe: ffff80001056bdf0: 68737568736e6562     ← "benshushu"（小端反過來讀）
crash_probe: ffff80001056be00: ffff000140116588
```

**與模組印出的標準答案 `&priv=ffff80001056bdf0` 完全一致 ✓**

同理可得另外兩層的局部變數：

| 變數 | 由反組譯得到的位置 | 推導出的位址 | 讀到的值 | 標準答案 |
|---|---|---|---|---|
| `lab_main` 的 `priv` | `x29 − 0x60` | `0xffff80001056bdf0` | `68737568736e6562`("benshushu") | ✓ |
| `lab_func1` 的 `local1` | `stur x4,[x29,#-16]` → `x29 − 0x10` | `0xffff80001056bdc0` | `1122334455667788` | ✓ |
| `lab_func2` 的 `local2` | `stur x4,[x29,#-16]` → `x29 − 0x10` | `0xffff80001056bd80` | `deadbeefcafe0001` | ✓ |

#### 情況二：函式參數——**它們根本不在棧上**（書上 §5.5.2 的手法）

`lab_func2` 的反組譯顯示，四個參數一進來就被搬進 callee-saved 暫存器：

```asm
0000000000000000 <lab_func2>:
   8:   sub     sp, sp, #0x40
  18:   stp     x29, x30, [sp, #16]
  1c:   add     x29, sp, #0x10
  28:   mov     x20, x0      ← 參數1 priv  → x20
  34:   mov     x19, x1      ← 參數2 sem   → x19
  38:   mov     x21, x2      ← 參數3 magic → x21
  48:   mov     x22, x7      ← 參數8 a8    → x22
```

**堆疊上一個參數都沒有**。照書上 §5.5.2 的辦法：**往子函式的框架裡找**，
因為子函式用到 x19~x22 時一定得先存起來。子函式是 `down_read()` → `rwsem_down_read_slowpath()`：

```console
$ objdump -d vmlinux    # ← 這裡用的是「和跑著的核心位址完全對得上」的 vmlinux（附錄 A）
ffff800009050efc <rwsem_down_read_slowpath>:
ffff800009050f04:   sub     sp, sp, #0x90
ffff800009050f08:   stp     x29, x30, [sp, #80]
ffff800009050f0c:   add     x29, sp, #0x50      ← x29 = sp + 0x50
ffff800009050f10:   stp     x19, x20, [sp, #96]   ← ★ 呼叫者的 x19、x20 存這裡
ffff800009050f1c:   stp     x21, x22, [sp, #112]  ← ★ 呼叫者的 x21、x22 存這裡

ffff800009051188 <down_read>:
ffff800009051190:   sub     sp, sp, #0x30
ffff800009051194:   stp     x29, x30, [sp, #16]
ffff800009051198:   add     x29, sp, #0x10
ffff80000905119c:   str     x19, [sp, #32]        ← 呼叫者的 x19 存這裡
```

從 `bt` 知道 `rwsem_down_read_slowpath` 的框架記錄在 `0xffff80001056bd10`，於是：

```
sp  = x29 − 0x50 = 0xffff80001056bd10 − 0x50 = 0xffff80001056bcc0
x19 @ sp+96  = 0xffff80001056bd20      x20 @ sp+104 = 0xffff80001056bd28
x21 @ sp+112 = 0xffff80001056bd30      x22 @ sp+120 = 0xffff80001056bd38
```

`rd` 一次讀 8 格：

```console
$ echo 'rd ffff80001056bd20 8' > /proc/crash_probe
crash_probe: ffff80001056bd20: ffff800001101030      ← x19 = sem
crash_probe: ffff80001056bd28: ffff80001056bdf0      ← x20 = priv
crash_probe: ffff80001056bd30: 1122334455667788      ← x21 = magic
crash_probe: ffff80001056bd38: 0000000000008888      ← x22 = a8
```

對照模組印出的標準答案：

```
[答案] lab_func2 的參數：priv=ffff80001056bdf0 sem=ffff800001101030 magic=0x1122334455667788 a8=0x8888
```

**四個參數 4/4 完全命中 ✓**

### 方法總結（ARM64 版，可以直接背）

1. `bt` 拿到**目標函式那一格的框架記錄位址**（= 該函式的 x29）。
2. `dis`（或 `objdump`）看它的序幕：`add x29, sp, #M` → **`sp = x29 − M`**；
   之後所有 `[sp+N]` / `[x29±N]` 都能換算成絕對位址。
3. **局部變數**：找 `sub xN, x29, #K` / `add xN, sp, #K`（取址）或 `str/stur` 到 `[x29,#-K]`（存值）。
4. **參數**：ARM64 前 8 個走 x0~x7，函式一開頭常被搬到 x19~x28。
   → **去子函式（甚至孫函式）的序幕找 `stp x19,x20,[sp,#N]`**，那裡才有它們的備份。
5. `rd` 讀出來，用 `struct`（或知道型別後自己解）驗證內容合不合理（字串、小整數、像位址的值）。

> **與 x86_64 的對比**（[ch13 Q10](./ch13_x86_64_crash_debugging.md#q10)）：
> x86_64 因為只有 6 個暫存器傳參且 `-O2` 常把參數 spill 回堆疊，通常在**呼叫者的框架**裡就找得到；
> ARM64 有 8 個參數暫存器 + 10 個 callee-saved 暫存器，**參數往往一路活在暫存器裡**，
> 得沿著呼叫鏈往下找到「誰把它存下來了」。這是 ARM64 崩潰分析最花時間的一步。

---

<a name="q5"></a>
## Q5：如何分析和推導一個讀寫信號量的持有者？

### 書上怎麼說（卷2 §5.5.1，行 560-600）

> 「counter 為 0xffffffff00000001，表示有一個活躍的讀者以及有寫者在睡眠等待；或者一個寫者持有了鎖以及多個讀者在等待。
> **owner 為 1，這表示被持有的鎖是一個讀者鎖**。」
> 「在 mm_struct 數據結構裡，rw_semaphore 是作為一個數據結構存放在裡面的。因此，知道了 rw_semaphore 的地址就可以反推出 mm_struct 的地址……
> `mm_struct 地址 = 0xffff80005f733120 − 0x60`」

### 本機驗證

先用 [Q4](#q4) 的方法從堆疊拿到鎖的位址（`x19 = 0xffff800001101030`），再解碼：

```console
$ echo 'rwsem ffff800001101030' > /proc/crash_probe     # = crash 的 struct rw_semaphore
```

```
crash_probe: === struct rw_semaphore ffff800001101030 ===
crash_probe: count = 0x3
crash_probe:   bit0 WRITER_LOCKED = 1   ← 有寫者持有
crash_probe:   bit1 FLAG_WAITERS  = 1   ← wait_list 非空
crash_probe:   bit2 FLAG_HANDOFF  = 0
crash_probe:   bit63 READFAIL     = 0
crash_probe:   讀者個數 (count >> 8) = 0
crash_probe: owner = 0xffff000140114d80  (旗標: READER_OWNED=0 NONSPINNABLE=0)
crash_probe: → 寫者持有：PID 28346 (lab_holder) task=ffff000140114d80  ★ 這就是 Q5 要的鎖持有者
```

### 6.1 的 `rw_semaphore` 和書上 5.0 已經完全不同

```c
/* include/linux/rwsem.h:48-60 */
struct rw_semaphore {
        atomic_long_t count;     /* 5.0 是 long，語意也全變了 */
        atomic_long_t owner;     /* 5.0 是 struct task_struct *，現在低 3 位是旗標 */
        struct optimistic_spin_queue osq;
        raw_spinlock_t wait_lock;
        struct list_head wait_list;
};

/* kernel/locking/rwsem.c */
:63  #define RWSEM_READER_OWNED   (1UL << 0)     /* owner 的旗標 */
:64  #define RWSEM_NONSPINNABLE   (1UL << 1)
:117 #define RWSEM_WRITER_LOCKED  (1UL << 0)     /* count 的位元編碼 */
:118 #define RWSEM_FLAG_WAITERS   (1UL << 1)
:119 #define RWSEM_FLAG_HANDOFF   (1UL << 2)
:120 #define RWSEM_FLAG_READFAIL  (1UL << 63)
:122 #define RWSEM_READER_SHIFT   8              /* 讀者個數在 bit8 以上 */
```

| | 書上 5.0 | 本機 6.1 |
|---|---|---|
| 一個寫者持有 + 有人等 | `count = 0xffffffff00000001` | **`count = 0x3`** |
| 讀者持有 | `owner = 0x1` | `owner = (最後一個讀者的 task) \| 0x1` |
| 寫者持有 | `owner = task 指標` | `owner = task 指標`（低 3 位為 0）**← 相同** |

### 另一條路：從鎖反推 `mm_struct`（書上圖 5.10 的手法）

這條路只有「鎖是內嵌在 `mm_struct` 裡的 `mmap_lock`」時才能用。本機的偏移量是 **0x88**：

```
arm64_lab: offsetof(mm_struct, mmap_lock) = 0x88（書上 5.0 是 0x60、3.10 是 0x78）
```

在 [ch13 Q11](./ch13_x86_64_crash_debugging.md#q11) 裡我用同一個手法在 ARM64 上實測過：
`mm = 0xffff000107675488 − 0x88 = 0xffff000107675400`，`mm->owner` → `PID 7100 insmod`，
和從 `sem->owner` 得到的 task **完全一致**。

**面試講法**：兩條路互相驗證——
`sem->owner` 直接給你持有者（**但讀者鎖只給你其中一個讀者**）；
`mm_struct` 反推法只適用於 `mmap_lock`，但可以在 `owner` 被清掉時救援。

---

<a name="q6"></a>
## Q6：如何分析和推導有哪些行程在等待這個讀寫信號量？

### 書上怎麼說（卷2 §5.5.1，行 601-625）

> 「使用 list 命令遍歷 rw_semaphore 數據結構中的 wait_list 成員，以找到哪些進程在等待這個鎖。」
> ```
> crash> list -s rwsem_waiter.task,type -h 0xffff00000b26bda8
> ffff00000b26bda8   task = 0xffff8000613e3900   type = RWSEM_WAITING_FOR_WRITE
> ffff00000b273bb8   task = 0xffff8000613e0000   type = RWSEM_WAITING_FOR_READ
> ```

### 本機驗證

```console
$ echo 'rwsem ffff800001101030' > /proc/crash_probe    # 同一條指令會一起走 wait_list
```

```
crash_probe: --- wait_list（Q6：誰在等這把鎖）head=ffff800001101060 ---
crash_probe: waiter[0] @ffff80001056bcd8  task=ffff000140115d00 PID 28369 (lab_main)    type=RWSEM_WAITING_FOR_READ   state=UN(D)
crash_probe: waiter[1] @ffff800010b7bd98  task=ffff000140111f00 PID 28410 (lab_writer)  type=RWSEM_WAITING_FOR_WRITE  state=UN(D)
crash_probe: 共 2 個等待者
```

`struct rwsem_waiter` 是 `kernel/locking/rwsem.c:341-347` 的**私有結構**（不在任何標頭檔裡），
crash 靠 DWARF 認得它，我們則把定義照抄一份到模組裡：

```c
struct rp_rwsem_waiter {          /* = struct rwsem_waiter */
        struct list_head list;
        struct task_struct *task;
        enum rp_rwsem_waiter_type type;   /* 0 = FOR_WRITE, 1 = FOR_READ */
        unsigned long timeout;
        bool handoff_set;
};
```

### 一個書上沒講、但很好用的細節

`waiter[0]` 的位址 `0xffff80001056bcd8` **落在 `lab_main` 自己的核心堆疊
`[ffff800010568000, ffff80001056c000)` 裡面**——因為 `struct rwsem_waiter` 是
`rwsem_down_read_slowpath()` 的區域變數，睡著的時候就掛在自己的堆疊上。

三個推論（面試很好用）：

1. **看 waiter 的位址就能反查是誰**：位址落在哪條執行緒的核心堆疊區間裡，就是誰。
2. 等待者一定是 **D 狀態**：它的 waiter 結構還活在堆疊上，函式沒返回。
3. `wait_list` 是 **FIFO**（`list_add_tail`），所以順序就是排隊順序——
   本機實測 `lab_main`（先到，等讀者鎖）排在 `lab_writer`（後到，等寫者鎖）前面 ✓

> 在 [ch13 Q12](./ch13_x86_64_crash_debugging.md#q12) 裡我用同一個手法抓到了更精彩的畫面：
> `insmod` 自己排在自己持有的鎖的 wait_list 上（AA 自鎖），後面跟著兩個被牽連的 `pgrep`/`ps`。

---

<a name="q7"></a>
## Q7：如何分析一個行程被阻塞了多長時間？

### 書上怎麼說

卷2 第 5 章沒有重複寫這一節，方法和 x86_64 版（§4.11.4，行 3242-3304）完全相同：

> `set <pid>` → `task -R sched_info` 拿 `last_arrival` → `runq -t` 拿當前時間戳 → 相減。

### 本機驗證

**先踩坑**：6.1 的 `sched_info` 受 static key 管控，本機預設是關的：

```console
$ cat /proc/sys/kernel/sched_schedstats
0                                  ← 不打開的話 last_arrival 永遠是 0
$ sudo sysctl -w kernel.sched_schedstats=1
```

```console
$ echo 'sched 28369' > /proc/crash_probe        # = crash 的 task -R sched_info + runq -t
```

```
crash_probe: === PID 28369 (lab_main) state=UN(D) cpu=5 ===
crash_probe: sched_info = {
crash_probe:   pcount       = 2   (被排程上 CPU 的次數)
crash_probe:   run_delay    = 3258209 ns
crash_probe:   last_arrival = 2161201595921 ns   ← 最後一次真正跑在 CPU 上的時刻
crash_probe:   last_queued  = 0 ns
crash_probe: }
crash_probe: 目前 CPU5 的 rq clock = 5437801146837 ns
crash_probe: ★ 被阻塞時間 = 5437801146837 - 2161201595921 = 3276599550916 ns = 3276.599 秒
crash_probe: nvcsw=2 nivcsw=0
```

**對答案**：模組是在 dmesg 時間 `[2173.92]` 載入的、`lab_main` 隨即卡住；
這次量測發生在 dmesg 時間 `[5450.85]` → 實際經過 **3276.9 秒**；
推算出來的 **3276.599 秒** 吻合 ✓（誤差來自 `insmod` 到 `down_read` 之間的 0.3 秒）

### 三個實作上的注意事項

1. **時間基準要同源**。本機實測 `sched_clock()` 與 `/proc/uptime` 一致
   （`5437.80` vs `5437.66`），但 **printk 的時間戳比它們快約 13 秒**（`5450.85`）。
   所以絕對不能拿 `last_arrival` 去跟 dmesg 的時間戳相減，**只能用同一個時鐘的兩個取樣點**。
2. `crash` 的 `runq -t` 讀的是 `rq->clock`；模組拿不到 `struct rq` 的定義（`sched_clock_cpu()` 也沒 export），
   所以用 `sched_clock()`——同一個時間基準，差別只在 `rq->clock` 是「上次更新的快照」，會舊一個 tick 以內。
3. **`pcount = 2`、`nvcsw = 2`** 本身就是證據：這條執行緒從誕生到現在只被排程過 2 次，
   完全符合「一跑起來就卡死」。這也正是 hung_task 機制的判斷依據（見
   [ch13 Q9](./ch13_x86_64_crash_debugging.md#q9)）。

---

<a name="appendix-a"></a>
## 附錄 A：把真正的 crash 工具搬到這塊板子上——做到哪裡、卡在哪裡

書上這 4 題（Q4~Q7）開頭都是「如何使用 Kdump+Crash 工具」，所以我認真試了一次。過程與結論如下。

### 做成功的部分

**1. 裝 crash（30 秒）**

```console
$ sudo apt-get install -y crash
$ crash --version
crash 8.0.2
```

**2. 重建一個「和正在跑的核心逐位元組相符、但帶 DWARF」的 vmlinux（約 25 分鐘）**

板子上的核心是**在板子上自己編的**（`/proc/version` 顯示 `radxa@rock-5b`，gcc 12.2.0），
原始碼樹就在 `~/disk/kernel-source`，所以只要**用同一份原始碼 + `/proc/config.gz` 的組態 + 只加 DWARF**
重編一次，位址就會一模一樣（本機 `CONFIG_RELOCATABLE=y` 但**沒有 KASLR**，見 ch12）。
腳本：[`experiments/build_vmlinux.sh`](./experiments/build_vmlinux.sh)

```bash
cp -a ~/disk/kernel-source ~/kbuild-src        # 不動 /lib/modules/…/build 那一棵
cd ~/kbuild-src
printf '+' > .scmversion                        # 版本尾巴的 "+"
zcat /proc/config.gz > .config
./scripts/config -d DEBUG_INFO_NONE -e DEBUG_INFO_DWARF4 -d DEBUG_INFO_REDUCED -d DEBUG_INFO_BTF
make olddefconfig
KBUILD_BUILD_TIMESTAMP='Mon Apr 27 08:30:35 UTC 2026' \
KBUILD_BUILD_USER=radxa KBUILD_BUILD_HOST=rock-5b KBUILD_BUILD_VERSION=1 \
        make -j8 vmlinux
```

比對結果（**這一步是關鍵，crash 會逐字比對版本橫幅**）：

```console
$ strings vmlinux | grep -m1 "^Linux version"
Linux version 6.1.115+ (radxa@rock-5b) (gcc (Debian 12.2.0-14+deb12u1) 12.2.0, GNU ld … 2.40) #1 SMP Mon Apr 27 08:30:35 UTC 2026
$ cat /proc/version
Linux version 6.1.115+ (radxa@rock-5b) (gcc (Debian 12.2.0-14+deb12u1) 12.2.0, GNU ld … 2.40) #1 SMP Mon Apr 27 08:30:35 UTC 2026
                                                                                        ↑ 一字不差
```

符號位址也對得上（`nm vmlinux` vs 執行中的 `/proc/kallsyms`）：

| 符號 | 重建的 vmlinux | 執行中的 kallsyms |
|---|---|---|
| `_stext` | `ffff800008010000` | `ffff800008010000` ✓ |
| `do_one_initcall` | `ffff800008014bbc` | `ffff800008014bbc` ✓ |
| `kthread` | `ffff8000080a04cc` | `ffff8000080a04cc` ✓ |

> **這個 vmlinux 本身就是本章最有用的產物**：Q3、Q4 裡那些 `objdump -d vmlinux` 的反組譯
> （`rwsem_down_read_slowpath`、`down_read` 的序幕）全都靠它，等於補上了 crash 的 `dis` 命令。

**3. 讓 crash 讀得到記憶體**

本機 `CONFIG_PROC_KCORE is not set`、`CONFIG_STRICT_DEVMEM=y`，所以 crash 一開始說
`cannot find a live memory device`。解法是編 crash 官方的 memory driver：

```console
$ curl -O https://raw.githubusercontent.com/crash-utility/crash/master/memory_driver/crash.c
$ curl -O https://raw.githubusercontent.com/crash-utility/crash/master/memory_driver/Makefile
$ make && sudo insmod crash.ko
$ ls -l /dev/crash
crw------- 1 root root 10, 121 …
$ sudo dd if=/dev/crash bs=1 skip=$((0x148e0c0)) count=16 2>/dev/null | od -A x -t x1
000000 49 4b 43 46 47 5f 53 54 1f 8b 08 00 00 00 00 00     ← "IKCFG_ST" + gzip magic
```

**實體記憶體讀得到，而且內容正確**（那是 `kernel_config_data` 的 magic）。

### 卡住的地方：crash 猜錯了 VA 佈局

```console
$ sudo crash --machdep phys_offset=0x200000 --machdep vabits_actual=48 ~/kbuild-src/vmlinux
…
VA_BITS: 47                              ← ★ 錯了，本機是 48
kimage_voffset: ffff800007c00000         ← 這個是對的
physvirt_offset: 800000200000            ← 跟著錯
<readmem: ffff80000908e0c0, KVADDR, "kernel_config_data MAGIC_START", 8, …>
WARNING: could not find MAGIC_START!
crash: …/vmlinux and /dev/crash do not match!
```

原因（讀 crash 8.0.2 的 `arm64.c` 原始碼確認）：

- crash 取得 `VA_BITS` 有三條路：**VMCOREINFO**（vmcore 才有）、
  讀核心的 `vabits_actual` 變數（**需要 `/proc/kcore`**）、
  或**從 `_text` 的數值去猜**（`arm64_calc_VA_BITS()`）。
- 本機三條路全斷：沒有 vmcore（沒 kexec）、沒有 `/proc/kcore`、
  而 5.11 之後 arm64 的 VA 佈局「翻轉」了（核心映像固定在 `0xffff8000…`，
  線性映射在 `0xffff0000…`），**`_text` 的數值不再能反推 VA_BITS** —— crash 猜成 47。
- VA_BITS 一錯，`PAGE_OFFSET`、`physvirt_offset` 全錯，於是每一次 `readmem` 都讀到垃圾，
  最後連 `linux_banner` 都對不起來 → `do not match`。

### 結論：在這塊板子上要用真 crash，只有一條正路

**把 Kdump 補起來**（三件事缺一不可）：

1. 核心組態：`CONFIG_KEXEC=y`、`CONFIG_CRASH_DUMP=y`（順便把
   `CONFIG_PROC_KCORE=y`、三個 lockup 偵測器、`CONFIG_DEBUG_INFO_DWARF4` 一起打開）；
2. 開機參數加 `crashkernel=256M`（ARM64 還要注意 `crashkernel=X,high` 與保留位置）；
3. 使用者空間裝 `kexec-tools` / `kdump-tools`，`kexec -p` 載入捕獲核心。

這樣崩潰時產生的 vmcore 會帶著 **VMCOREINFO**（裡面就有 `NUMBER(VA_BITS)`、`NUMBER(PHYS_OFFSET)`、
`NUMBER(kimage_voffset)`），crash 就不用猜了，書上 §5.1~§5.5 的每一條命令都能照跑。

**我沒有做這一步的原因**：那需要編一顆新核心並重開機進去，而這塊板子只有 SSH、沒有串列主控台，
新核心萬一開不起來就只能靠實體接觸救援。權衡之後，選擇「用等價工具把 7 題全部做完」，
並把這條路徑的完整做法記錄在這裡。

> 順帶一提：`/dev/crash` 那個 driver 還留在板子上，`~/kbuild-src/vmlinux`（430 MB，帶 DWARF）也留著——
> 後續要 `objdump`、`gdb`、`faddr2line` 核心本體都用得上。

---

## 附錄 B：踩過的坑

| 坑 | 症狀 | 解法 |
|---|---|---|
| 用 `__builtin_frame_address(0)` 測「葉子函式」 | 它會**逼 GCC 生出框架記錄**，於是永遠測不到真正的葉子函式 | 葉子函式裡只用 inline asm 讀 `sp` |
| 模組函式被 `constprop/isra` 改寫 | 符號變成 `lab_func2.constprop.0.isra.0`，**參數整個被優化掉**，沒有東西可以推導 | 加 `__attribute__((noipa))` + 讓參數來自 runtime 變數 |
| 重建 vmlinux 時忘了設 `KBUILD_BUILD_VERSION` | 版本橫幅變成 `# SMP …`（少了 `1`），crash 直接拒絕 | `KBUILD_BUILD_VERSION=1`，並逐字比對 `strings vmlinux` 與 `/proc/version` |
| 直接在 `~/disk/kernel-source` 裡重編 | 那棵樹是 `/lib/modules/6.1.115+/build` 指過去的，編壞了所有模組都編不出來 | `cp -a` 複製一份再編（5.5 GB，板子上有 63 GB 可用） |
| `crash` 在活系統上 | `cannot find a live memory device` → 補了 `/dev/crash` 之後變成 `VA_BITS: 47` → `do not match` | 見附錄 A，正解是把 Kdump 補起來 |
| 重跑 Q3 的 oops | 第二次 `insmod oops_lab.ko` 直接失敗（模組還卡在 COMING） | 準備 `oops_m1`~`oops_m3` 三個複本輪流用（同 ch12 的做法） |
| `sprint_symbol()` 解資料位址 | `lab_sem` 被解成 `lab_exit+0x1c70/0xc40`（亂猜） | 本機 `CONFIG_KALLSYMS_ALL=n`，**kallsyms 裡只有 text 符號**；資料位址要靠 vmlinux 的 DWARF 或模組的 `nm` |

---

## 相關筆記

- 📝 [ch13 基於 x86_64 解決宕機難題](./ch13_x86_64_crash_debugging.md)（同樣 7 題的 x86_64 版 + Kdump 原理 + 三個 lockup 偵測器）
- 📝 [ch12 內核調試與性能優化](./ch12_kernel_debugging_and_performance_optimization.md)（oops 解讀、`Code:` 行、`faddr2line`、模組重定位）
- 📝 [ch11 中斷管理](./ch11_interrupt_management.md)（`pt_regs`、每 CPU 中斷堆疊、VBAR_EL1）
- 📝 [ch02 ARM64 在 Linux 內核中的實現](./ch02_arm64_in_linux_kernel.md)（VA 佈局、`kimage_voffset`、頁表）
