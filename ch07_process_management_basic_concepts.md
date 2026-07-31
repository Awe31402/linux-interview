# 第 7 章 進程管理之基本概念 — 高頻面試題解答

> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588，4×Cortex-A76 + 4×Cortex-A55），`192.168.68.57`
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`
> **關鍵組態**（`zcat /proc/config.gz`）：
> `CONFIG_THREAD_INFO_IN_TASK=y`、`CONFIG_VMAP_STACK=y`、`CONFIG_ARM64_VA_BITS=48`、
> `CONFIG_PGTABLE_LEVELS=4`、`CONFIG_ARM64_PAGE_SHIFT=12`
>
> **書目對照**
> - 《奔跑吧 Linux 內核》（第二版）卷 1 第 7 章 —
>   `books/running-linux-kernel/running-kernel-1-txt/15_第7章_进程管理之基本概念.txt`
>   （下稱「奔跑吧 §7.x」）
> - 核心程式碼路徑相對於本專案樹
>   `/home/awe/disk/yocto-rockchip-sdk/build/tmp/work-shared/rockchip-rk3588-rock-5b/kernel-source`
>
> **登入方式**：`ssh radxa@192.168.68.57`（帳密皆為 `radxa`）

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | 行程是什麼 | — |
| [2](#q2) | OS 如何描述和抽象一個行程 | `task_struct` 尺寸、`/proc/1/*` |
| [3](#q3) | 行程是否有生命週期 | **僵屍態實測** |
| [4](#q4) | 如何標識一個行程 | **PID vs TGID 實測** |
| [5](#q5) | 行程與行程之間的關係 | **孤兒行程託孤實測** |
| [6](#q6) | 行程 0 是什麼 | **ftrace 抓到 `swapper/N` pid=0** |
| [7](#q7) | 行程 1 是什麼 | `/proc/1` = systemd，PPid=0 |
| [8](#q8) | fork / vfork / clone 的區別 | **strace 抓到三者的 clone flags** |
| [9](#q9) | 寫時複製的工作原理 | **16384 次 COW 缺頁精準命中** |
| [10](#q10) | ARM64 如何取得 `task_struct` | `SP_EL0` + 原始碼 |
| [11](#q11) | 那段 fork 程式輸出幾個 `_` | **6 或 8，實測兩種都重現** |
| [12](#q12) | 使用者行程的頁表何時分配 | **VmPTE 逐級變化精準命中** |
| [13](#q13) | 什麼是調度器？O(n)/O(1) 如何工作 | `/proc/1/sched` |
| [14](#q14) | fork 為何返回兩次？子行程如何回 0 | `copy_thread()` 原始碼 |
| [15](#q15) | 第一次返回使用者空間時回到哪 | `ret_from_fork` 原始碼 |

---

<a name="q1"></a>
## 1. 進程是什麼？

### 結論

> **行程 = 程式 + 執行（Process = Program + Execution）**

- **程式（program）**：一個靜態的、有序的指令集合（磁碟上的 ELF 檔），**沒有生命力**。
- **行程（process）**：程式的一次**執行實例**，是一個**有生命力**的個體。除了可執行碼（程式碼段），
  還包含它的活動資訊與資料：存放形參/區域變數/回傳值的**使用者堆疊**、**資料段**、
  切換時用的**核心堆疊**、動態配置的**堆積（heap）**、開啟的檔案、訊號狀態、位址空間等。

**行程存在的理由是「抽象」**——為了提高 CPU 使用率。作業系統讓多個程式同時載入記憶體並發執行，
每個行程都**以為自己獨佔一顆 CPU**。實現這個「CPU 虛擬化」的兩大核心技術是
**上下文切換（context switch）** 與 **行程調度（schedule）**。
在單核上這叫「偽並行」——某一瞬間只跑一個，但一段時間內輪流跑很多個。

**行程 vs 執行緒（Linux 的觀點，很重要）**：

| | 傳統 OS 理論 | **Linux 核心的實作** |
|---|---|---|
| 資源管理單位 | 行程 | `mm_struct`（可被多個 task 共享） |
| 調度單位 | 執行緒 | **`task_struct`（統稱 task）** |
| 資料結構 | PCB + TCB 兩套 | **只有 `task_struct` 一套** |
| 建立介面 | `fork` / `pthread_create` | **都是 `clone()`，只差在 flags** |

Linux **沒有為執行緒設計任何特殊的資料結構或調度演算法**。所謂「執行緒」不過是
一群共享 `mm_struct`／`files_struct`／`signal_struct` 的 `task_struct`（見 [Q8](#q8)）。

> **書目**：奔跑吧 §7.1.1「行程的來由」。書中的比喻：
> 「把做菜看作行程，做菜的工序可以被看作程式，大廚可以被看作處理器，廚房可以被看作執行環境。」

### 實機驗證

```bash
ssh radxa@192.168.68.57 'ps -eL --no-headers | wc -l; ls /proc | grep -c "^[0-9]"'
```

```
558      ← 系統中的 task（執行緒）總數
295      ← /proc 底下的 PID（thread group）數
```

**558 個 task 但只有 295 個 PID**——差額 263 就是那些「執行緒」。
對核心而言它們都是平等的 `task_struct`，只是被歸類到同一個 thread group（見 [Q4](#q4)）。

---

<a name="q2"></a>
## 2. 操作系統如何描述和抽象一個進程？

### 結論

用**行程控制塊（Process Control Block, PCB）**，在 Linux 中就是
**`struct task_struct`**（`include/linux/sched.h`）。它至少要描述：

| 類別 | Linux `task_struct` 的對應成員 |
|------|------------------------------|
| **執行狀態** | `__state`（TASK_RUNNING / INTERRUPTIBLE / …）、`exit_state`（ZOMBIE / DEAD） |
| **程式計數器 + CPU 暫存器** | `thread`（`struct thread_struct`，ARM64 存 `cpu_context`：x19–x28、fp、sp、pc）；使用者態的 pt_regs 存在核心堆疊頂端 |
| **調度資訊** | `prio`/`static_prio`/`normal_prio`、`se`（`sched_entity`，含 `vruntime`）、`rt`、`dl`、`sched_class`、`cpus_mask` |
| **記憶體管理** | `mm`（自己的位址空間）、`active_mm`（核心執行緒借用的） |
| **識別** | `pid`、`tgid`、`comm[16]`、`cred`（uid/gid/capabilities） |
| **家族關係** | `real_parent`、`parent`、`children`、`sibling`、`group_leader` |
| **資源** | `files`（fd 表）、`fs`（cwd/root）、`signal`/`sighand`、`nsproxy`（namespaces） |
| **統計** | `utime`/`stime`、`start_time`、`nvcsw`/`nivcsw`（自願/非自願切換次數） |

**ARM64 的兩個實作重點**：

1. **`thread_info` 內嵌在 `task_struct` 裡**（`CONFIG_THREAD_INFO_IN_TASK=y`），
   而不是放在核心堆疊底部。這樣（a）堆疊溢位時不會踩壞 `thread_info`；
   （b）堆疊位址洩漏也推不出 `task_struct` 位址，提高攻擊難度。
2. **核心堆疊 16 KB**（`arch/arm64/include/asm/memory.h:83`）：
   ```c
   #define MIN_THREAD_SHIFT   (14 + KASAN_THREAD_SHIFT)   /* 1<<14 = 16 KB */
   #define THREAD_SIZE        (UL(1) << THREAD_SHIFT)
   ```
   而且本機 `CONFIG_VMAP_STACK=y`，核心堆疊用 `vmalloc()` 配置並在兩端留 guard page，
   溢位會直接觸發缺頁而不是默默踩壞鄰居。

> **書目**：奔跑吧 §7.1.2「行程描述符」；核心堆疊大小見該章註腳 [2]
> 「ARM32 架構中內核棧大小是 8KB，ARM64 架構中內核棧大小是 16KB」。

### 實機驗證

```bash
# 每個 task 的核心堆疊確實是 16 KB
ssh radxa@192.168.68.57 'grep KernelStack /proc/meminfo; ps -eL --no-headers | wc -l'
```

```
KernelStack:  9040 kB
558 個 task
→ 9040 / 558 = 16.2 kB / task     ✅ 印證 ARM64 THREAD_SIZE = 16 KB
```

```bash
# task_struct 的實際大小（從 slab 看）
ssh radxa@192.168.68.57 'sudo grep -E "^task_struct " /proc/slabinfo'
```

```
task_struct  645  672  3968  8  8 : ...
                       ↑ objsize = 3968 bytes ≈ 3.9 KB / 個
```

```bash
# 從 /proc 觀察各類抽象
ssh radxa@192.168.68.57 'cat /proc/1/status | head -20; echo ---; cat /proc/1/sched | head -8'
```

```
se.vruntime          :  41165.815840     ← 調度資訊
se.nr_migrations     :          4062     ← 跨 CPU 遷移次數
nr_switches          :         60249
nr_voluntary_switches:         33183
```

---

<a name="q3"></a>
## 3. 進程是否有生命週期？

### 結論

**有。** Linux 的行程狀態機（`include/linux/sched.h` 的 `TASK_*` 巨集）：

```
                    fork()
                      │
                      ▼
                ┌───────────┐   被調度器選中     ┌───────────┐
                │  TASK_    │ ───────────────► │  TASK_    │
                │  RUNNING  │ ◄─────────────── │  RUNNING  │
                │ (就緒/R)  │   時間片用完      │ (執行中/R)│
                └───────────┘   被搶佔          └───────────┘
                   ▲     ▲                          │
       wake_up()   │     │  signal                  │ 等待資源
                   │     │                          ▼
            ┌──────┴──┐ ┌┴─────────┐          ┌──────────────┐
            │ TASK_   │ │ TASK_UN- │          │ 睡眠         │
            │ INTERR- │ │ INTERR-  │◄─────────┤              │
            │ UPTIBLE │ │ UPTIBLE  │          └──────────────┘
            │   (S)   │ │   (D)    │
            └─────────┘ └──────────┘
                                                exit()
                                                  │
                                                  ▼
                                          ┌───────────────┐
                                          │ EXIT_ZOMBIE   │ ← 只剩 task_struct
                                          │      (Z)      │   其他資源已歸還
                                          └───────────────┘
                                                  │ 父行程 wait()
                                                  ▼
                                          ┌───────────────┐
                                          │  EXIT_DEAD    │ → task_struct 釋放
                                          └───────────────┘
```

| 代號 | 狀態 | 說明 |
|------|------|------|
| `R` | `TASK_RUNNING` | 就緒**或**執行中（Linux 不區分這兩者，都在 runqueue 上） |
| `S` | `TASK_INTERRUPTIBLE` | 淺睡眠，**可被訊號喚醒**（等鍵盤、等網路） |
| `D` | `TASK_UNINTERRUPTIBLE` | 深睡眠，**訊號叫不醒**（等磁碟 I/O 完成）→ 這就是 `kill -9` 殺不掉的行程 |
| `T` | `__TASK_STOPPED` | 被 SIGSTOP 暫停 |
| `t` | `__TASK_TRACED` | 被 ptrace 追蹤中 |
| `Z` | `EXIT_ZOMBIE` | **僵屍**：資源已釋放，但 `task_struct` 保留以供父行程 `wait()` 讀取結束碼 |
| `X` | `EXIT_DEAD` | 已被回收，即將消失（幾乎看不到） |
| `I` | `TASK_IDLE` | `TASK_UNINTERRUPTIBLE | TASK_NOLOAD`，不計入 loadavg 的核心執行緒 |

**為什麼要有僵屍態？** 因為「清理資源」和「釋放 `task_struct`」被刻意分開：
資源可以立刻歸還，但**結束原因/退出碼**必須留到父行程來收，否則父行程永遠不知道
子行程是正常退出還是被 SIGSEGV 打死。

> **書目**：奔跑吧 §7.1.3「行程的生命週期」、§7.2.6「終止行程」、§7.2.7「僵屍行程和行程託孤」。

### 實機驗證（僵屍態實測）

實驗程式 `/tmp/life.c`：fork 出子行程立刻 `_exit(42)`，父行程**故意不 wait**，
睡 1 秒後去讀 `/proc/<child>/status`。

```bash
scp life.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -w -o life life.c && ./life'
```

```
child exited, no wait:   Name: life   State: Z (zombie)   Pid: 74983   PPid: 74982
after wait():            (空 —— /proc/74983 已經不存在了)
                         exit status = 42
```

**三件事同時被證明**：
1. 子行程 `_exit()` 後**沒有立刻消失**，狀態變成 `Z (zombie)`，`/proc/74983/status` 還讀得到。
2. 父行程呼叫 `waitpid()` 後，`/proc/74983` **整個目錄消失**——`task_struct` 才真正被釋放。
3. `WEXITSTATUS(st) == 42` ——退出碼確實是靠僵屍態保存下來的。

**觀察系統中的 D 態行程**（面試常問「怎麼找出卡在 I/O 的行程」）：

```bash
ssh radxa@192.168.68.57 'ps -eo pid,stat,wchan:20,comm | awk "\$2 ~ /^D/"'   # 找 D 態（卡 I/O）
ssh radxa@192.168.68.57 'ps -eo pid,ppid,stat,comm | awk "\$3 ~ /^Z/"'      # 找僵屍
```

本機兩者都是空的（系統健康）。要**製造**一個僵屍來觀察，就用上面的 `life` 程式，
或最短版本：`bash -c 'sleep 0 & sleep 5; ps -o pid,stat,comm -p $!'`。

---

<a name="q4"></a>
## 4. 如何標識一個進程？

### 結論

用 **PID（Process Identifier）**，存在 `task_struct->pid`。但**關鍵在於 Linux 有兩層識別**：

| 欄位 | 名稱 | 意義 | 使用者空間的 API |
|------|------|------|-----------------|
| `task->pid` | PID | **每個 task（執行緒）獨一無二** | `gettid()` |
| `task->tgid` | TGID | **thread group ID** = 主執行緒的 PID | `getpid()` ← **注意！** |

**這是最經典的陷阱題**：
- POSIX 1003.1c 要求「同一個多執行緒程式中的所有執行緒必須有相同的 PID」（這樣才能把訊號送給整組）。
- 但 Linux 核心裡每個執行緒都是獨立的 `task_struct`，各有自己的 `pid`。
- 解法：引入 `tgid`。**`getpid()` 回傳的其實是 `tgid`，`gettid()` 才回傳真正的 `task->pid`。**
- 單執行緒行程：`pid == tgid`。

**PID 的管理**：核心用 bitmap（`struct pid_namespace` 的 `idr`）配置，保證唯一並可循環使用。
上限由 `/proc/sys/kernel/pid_max` 決定。

**PID namespace**：同一個 task 在不同 namespace 中有不同的 PID。
`/proc/PID/status` 的 `NStgid`/`NSpid`/`NSpgid`/`NSsid` 會列出它在**各層 namespace** 中的編號。

**其他識別方式**：`comm[16]`（行程名，只有 15 字元 + `\0`）、`/proc/PID/exe`（執行檔）、
`cred`（uid/gid）、`start_time`（配合 PID 才能唯一識別，因為 PID 會被回收再利用）。

> **書目**：奔跑吧 §7.1.4「行程標識」——
> 「`getpid()` 系統調用返回當前行程的 TGID，而不是線程的 PID…`gettid()` 系統調用會返回線程的 PID。」

### 實機驗證（PID vs TGID）

實驗程式 `/tmp/tid.c`：主執行緒建 2 個 pthread，各自印 `getpid()` 與 `gettid()`。

```bash
scp tid.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -o tid tid.c -lpthread && ./tid'
```

```
  main   : getpid()=74747  gettid()=74747      ← 主執行緒 pid == tgid
  thread : getpid()=74747  gettid()=74748      ← getpid 一樣！gettid 不同
  thread : getpid()=74747  gettid()=74749      ← getpid 一樣！gettid 不同

$ ls /proc/74747/task          ← 核心眼中的三個 task
74747
74748
74749

$ grep -E '^(Tgid|Pid|Threads):' /proc/74747/status
Tgid:    74747      ← thread group id
Pid:     74747      ← 主執行緒自己的 pid
Threads: 3          ← 這組有 3 個 task
```

**三個執行緒的 `getpid()` 全都回傳 74747，但 `gettid()` 是 74747/74748/74749。**
核心的 `/proc/74747/task/` 底下確實有三個目錄——這就是 PID 與 TGID 的差別。

```bash
# PID 上限與 namespace
ssh radxa@192.168.68.57 'cat /proc/sys/kernel/pid_max; grep -E "^NS" /proc/self/status'
```

```
4194304                       ← pid_max（22 bits）
NStgid: 73581  NSpid: 73581  NSpgid: 73580  NSsid: 73580
```

---

<a name="q5"></a>
## 5. 進程與進程之間的關係如何？

### 結論

Linux 維護一棵**行程家族樹**，靠 `task_struct` 的這幾個成員串起來：

```c
struct task_struct {
    struct task_struct __rcu *real_parent;  /* 真正的親生父行程 */
    struct task_struct __rcu *parent;       /* 回報 SIGCHLD 的對象（ptrace 時會不同） */
    struct list_head          children;     /* 子行程鏈結串列的頭 */
    struct list_head          sibling;      /* 掛在父行程 children 上的節點 */
    struct task_struct       *group_leader; /* thread group 的主執行緒 */
};
```

| 關係 | 說明 |
|------|------|
| **父子（parent/child）** | `fork()` 的直接產物 |
| **兄弟（sibling）** | 同一個父行程的子行程們 |
| **祖先** | `init_task`（行程 0）是所有行程的祖先 |
| **thread group** | 共享 `tgid` 的一組 task，`group_leader` 指向主執行緒 |
| **行程組（process group）** | 同一條 shell 管線的行程，`setpgid()`，`kill(-pgid)` 可整組送訊號 |
| **會話（session）** | 一次登入 / 一個終端機，`setsid()` |
| **託孤（re-parent）** | **父行程先死 → 子行程變孤兒 → 被託孤給 init（PID 1）或最近的 subreaper** |

**託孤機制**（`kernel/exit.c` `forget_original_parent()` / `find_new_reaper()`）：
父行程退出時，核心會把它所有的子行程重新掛到新的 reaper 底下。
現代系統上這個 reaper **不一定是 PID 1**——`prctl(PR_SET_CHILD_SUBREAPER)` 註冊過的行程
（systemd 的 user manager、容器的 init）會優先接手。**沒有託孤機制的話，
孤兒行程死掉後沒人 `wait()`，就會變成永遠回收不掉的僵屍。**

> **書目**：奔跑吧 §7.1.5「行程間的家族關係」、§7.2.7「僵屍行程和行程託孤」——
> 「如果父行程先於子行程消亡，那麼子行程就變成『孤兒』行程，Linux 內核會把它『託孤』給
> init 行程（行程 1）。」

### 實機驗證（託孤實測）

`/tmp/life.c` 的第二部分：主行程 fork 出 A，A 再 fork 出 B（孫行程），
**A 立刻 `_exit(0)`**，B 睡 2 秒後再看自己的 `getppid()`。

```bash
ssh radxa@192.168.68.57 'cd /tmp && ./life'
```

```
grandchild pid=74992 ppid=1    ← 父行程 A 已經退出，B 立刻被託孤給 PID 1
grandchild pid=74992 ppid=1    ← 2 秒後依然是 1
```

**孫行程的 `ppid` 變成 1**——它的親生父行程 A 已經不存在，核心把它重新掛到 init 底下。

```bash
# 看整棵家族樹
ssh radxa@192.168.68.57 'pstree -p 1 | head -8'
```

```
systemd(1)-+-NetworkManager(1268)-+-{NetworkManager}(1300)
           |                      `-{NetworkManager}(1301)
           |-accounts-daemon(1210)-+-{accounts-daemon}(1246)
           |                       `-{accounts-daemon}(1273)
           |-agetty(73167)
```

`{NetworkManager}(1300)` 這種**大括號**表示法就是 pstree 在標示「同一 thread group 裡的執行緒」
（`tgid=1268` 但 `pid=1300`），和 [Q4](#q4) 的結論完全對應。

```bash
# 行程組 / 會話
ssh radxa@192.168.68.57 'ps -eo pid,ppid,pgid,sid,tty,comm | head -12'
```

---

<a name="q6"></a>
## 6. Linux 操作系統的進程 0 是什麼？

### 結論

**行程 0 = `init_task` = idle 行程 = swapper 行程**，它有三個特點：

1. **唯一一個「無中生有」的行程**。所有其他行程都是 fork 出來的，
   只有它是在編譯期就**靜態初始化**在資料段裡：
   ```c
   /* init/init_task.c:64 */
   struct task_struct init_task = {
       .__state    = 0,
       .stack      = init_stack,       /* 靜態核心堆疊，不是 vmalloc 來的 */
       .active_mm  = &init_mm,         /* 沒有自己的 mm，借用 init_mm */
       .comm       = INIT_TASK_COMM,   /* "swapper" */
       ...
   };
   ```
2. **它是所有行程的祖先**。`start_kernel()` 一路跑到 `rest_init()`，
   由行程 0 建立行程 1（`kernel_init`）和行程 2（`kthreadd`），之後所有行程都源自這兩支。
3. **SMP 上每顆 CPU 各有一個**。開機後主 CPU 的行程 0 執行 `cpu_startup_entry()` →
   `do_idle()`；其他 CPU 由 `fork_idle()` 複製出各自的 idle task，名字是
   `swapper/0`、`swapper/1` … `swapper/7`。**它們的 PID 全都是 0。**

**它的工作**：當該 CPU 的 runqueue 上**沒有任何可執行的行程**時，調度器（`idle_sched_class`，
優先級最低的調度類）才會選中它，然後讓 CPU 進入低功耗狀態（WFI / cpuidle）。
在 RK3588 上這直接關係到 A76/A55 的 cluster 能不能進 retention/power-down。

**「swapper」這個名字是歷史遺留**——早期 UNIX 的行程 0 負責 swapping，現在只負責 idle。

> **書目**：奔跑吧 §7.1.7「行程 0 和行程 1」——
> 「行程 0 是指 Linux 內核初始化階段從無到有創建的一個內核線程，它是所有行程的祖先，
> 有好幾個別名，如行程 0、idle 行程或者 swapper 行程…在 SMP 中，每個 CPU 都有一個行程 0。」

### 實機驗證（`/proc` 看不到它，但 ftrace 抓得到）

**第一步：確認 `/proc` 裡沒有它**

```bash
ssh radxa@192.168.68.57 'ls -d /proc/0; ps -eo pid,ppid,comm | awk "\$1<=3"'
```

```
ls: cannot access '/proc/0': No such file or directory      ← 行程 0 不在 /proc 裡
    PID    PPID COMMAND
      1       0 systemd          ← 但 systemd 的 PPid 是 0！
      2       0 kthreadd         ← kthreadd 的 PPid 也是 0
```

`ps` 看不到 PID 0（`fs/proc` 不為 idle task 建目錄），
但 **PID 1 和 PID 2 的 `PPid` 都是 0** ——這是行程 0 存在的間接證據。

**第二步：用 ftrace 直接抓到 `swapper/N` pid=0**

```bash
ssh radxa@192.168.68.57 'sudo sh -c "cd /sys/kernel/debug/tracing &&
  echo 1 > events/sched/sched_switch/enable && echo > trace && sleep 0.2 &&
  grep -i swapper trace | head -5; echo 0 > events/sched/sched_switch/enable"'
```

```
<idle>-0  [005] d.... sched_switch: prev_comm=swapper/5 prev_pid=0 prev_prio=120
                                    ==> next_comm=kworker/5:2 next_pid=73222
kworker/5:2-73222 [005] d.... sched_switch: prev_comm=kworker/5:2 prev_pid=73222 prev_state=D
                                    ==> next_comm=swapper/5 next_pid=0
<idle>-0  [000] d.... sched_switch: prev_comm=swapper/0 prev_pid=0
                                    ==> next_comm=sh next_pid=74887
<idle>-0  [002] d.... sched_switch: prev_comm=swapper/2 prev_pid=0
                                    ==> next_comm=sh next_pid=74886
```

**`prev_comm=swapper/5 prev_pid=0`** ——鐵證。而且可以看到
`swapper/0`、`swapper/2`、`swapper/5` 分別在 CPU 0/2/5 上，**每顆 CPU 一個**。

**第三步：用 perf 看調度地圖**

```bash
ssh radxa@192.168.68.57 'sudo perf sched record -o /tmp/p.data -- sleep 1 >/dev/null 2>&1
  sudo perf sched map -i /tmp/p.data | head -6'
```

```
  *A0                   118689.540908 secs  A0 => migration/0:15
  *.                    118689.540939 secs  .  => swapper:0        ← PID 0
   .  *B0               118689.541049 secs  B0 => migration/1:18
   .  *.                118689.541079 secs
```

`perf` 用 `.` 這個符號代表 idle，並在圖例中明確標出 **`swapper:0`**。

---

<a name="q7"></a>
## 7. Linux 操作系統的進程 1 是什麼？

### 結論

**行程 1 = init 行程**，是**第一個使用者空間行程**，也是所有使用者行程的祖先。

**誕生過程**（`init/main.c` `rest_init()`）：

```c
/* init/main.c:697 —— 本機 6.1 核心 */
noinline void __ref rest_init(void)
{
    ...
    /* 先生 init，才能拿到 PID 1 */
    pid = user_mode_thread(kernel_init, NULL, CLONE_FS);
    ...
    pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);   /* PID 2 */
    ...
}
```

> **📌 與書中的版本差異**：奔跑吧 §7.1.7 寫的是
> `kernel_thread(kernel_init, NULL, CLONE_FS)`。
> Linux 6.0（commit `cf587db2ee44`）把它改成了 **`user_mode_thread()`**，
> 明確區分「將來要回到使用者空間的 task」和「純核心執行緒」。
> 面試時能指出這點會加分。

**「行程 1 必須先於行程 2 建立」**——原始碼註解寫得很清楚：
```
/* We need to spawn init first so that it obtains pid 1, however
   the init task will end up wanting to create kthreads, which, if
   we schedule it before we create kthreadd, will OOPS. */
```

**從核心執行緒蛻變成使用者行程**：行程 1 一開始執行 `kernel_init()`（還在核心態），
它會做完剩餘的初始化，然後呼叫 `run_init_process()` → `kernel_execve()` 依序嘗試：
`/sbin/init` → `/etc/init` → `/bin/init` → `/bin/sh`。
`execve` 成功後，行程 1 就**丟掉核心執行緒的身分（清掉 `PF_KTHREAD`）、擁有了自己的 `mm`**，
變成一個普通的使用者行程。

**行程 1 的兩個永久職責**：
1. 依 `/etc/inittab`（SysV）或 unit 檔（systemd）啟動所有系統服務。
2. **當所有孤兒行程的 reaper**——`wait()` 掉那些沒人收屍的行程（見 [Q5](#q5)）。
   **行程 1 一旦死掉，核心會 panic**（`Attempted to kill init!`）。

**行程 2 = `kthreadd`**：所有**核心執行緒**的父行程。核心執行緒沒有 `mm`（`task->mm == NULL`），
只跑在核心位址空間，借用 `active_mm`。

> **書目**：奔跑吧 §7.1.7「行程 0 和行程 1」、§7.2.5「內核線程」。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'ps -p 1 -o pid,ppid,comm,args --no-headers
  sudo readlink /proc/1/exe
  grep -E "^(Name|Pid|PPid|Threads):" /proc/1/status'
```

```
1  0  systemd  /sbin/init splash          ← argv[0] 仍是 /sbin/init
/usr/lib/systemd/systemd                  ← 實際執行檔（/sbin/init 是符號連結）
Name:    systemd
Pid:     1
PPid:    0          ← 父行程是行程 0 ✅
Threads: 1
```

**核心執行緒沒有 mm 的實證**：

```bash
ssh radxa@192.168.68.57 'for p in 1 2 3; do
    printf "pid=%s comm=%-12s maps_lines=%s VmSize欄位數=%s\n" $p "$(cat /proc/$p/comm)" \
      "$(sudo cat /proc/$p/maps | wc -l)" "$(grep -c VmSize /proc/$p/status)"; done'
```

```
pid=1  comm=systemd     maps_lines=122   VmSize欄位數=1    ← 使用者行程，有 mm
pid=2  comm=kthreadd    maps_lines=0     VmSize欄位數=0    ← 核心執行緒，mm == NULL
pid=3  comm=rcu_gp      maps_lines=0     VmSize欄位數=0    ← 核心執行緒，mm == NULL
```

`/proc/2/maps` 是**空的**，`/proc/2/status` 裡**完全沒有 `Vm*` 系列欄位**——
因為 `task_mem()` 在 `mm == NULL` 時直接跳過。這是判斷「核心執行緒 vs 使用者行程」
最快的方法。

```bash
# 所有核心執行緒都掛在 kthreadd(2) 底下
ssh radxa@192.168.68.57 'ps -eo pid,ppid,comm | awk "\$2==2" | head -8'
```

---

<a name="q8"></a>
## 8. 請簡述 fork()、vfork() 和 clone() 之間的區別

### 結論

**三者在核心裡走的是同一條路**：`kernel_clone()`（舊名 `_do_fork()`，`kernel/fork.c:2641`）
→ `copy_process()`。差別**只在傳進去的 `clone_flags`**。

```c
/* kernel/fork.c:2762 */
SYSCALL_DEFINE0(fork) {
    struct kernel_clone_args args = { .exit_signal = SIGCHLD };
    return kernel_clone(&args);
}
/* kernel/fork.c:2778 */
SYSCALL_DEFINE0(vfork) {
    struct kernel_clone_args args = {
        .flags = CLONE_VFORK | CLONE_VM, .exit_signal = SIGCHLD,
    };
    return kernel_clone(&args);
}
```

| | `fork()` | `vfork()` | `clone()` |
|---|---|---|---|
| **位址空間 `mm`** | **COW 複製**一份 | **完全共享**（`CLONE_VM`） | 由 `CLONE_VM` 決定 |
| **父行程行為** | 立刻並行執行 | **阻塞**，直到子行程 `_exit()` 或 `execve()`（`CLONE_VFORK`） | 由 flags 決定 |
| **誰先跑** | 不保證 | **保證子行程先跑** | 不保證 |
| **成本** | 複製頁表（見 [Q12](#q12)） | 幾乎為 0 | 依 flags |
| **典型用途** | 一般建立行程 | `fork+exec` 的極致最佳化（MMU-less 系統必用） | **建立執行緒**（pthread）、容器（namespace flags） |
| **危險性** | 安全 | **子行程不可 return、不可改變數、不可呼叫非 async-signal-safe 函式**（會踩壞父行程的堆疊） | 需自行管理堆疊 |

**歷史脈絡**：`vfork()` 誕生於 **COW 發明之前**——那時 `fork()` 真的要複製整個位址空間，
而 99% 的情況下馬上就 `execve()` 全部丟掉，浪費驚人。有了 COW 之後 `fork()` 已經很便宜，
`vfork()` 的價值大減，POSIX.1-2008 甚至把它標為 obsolete。但在**沒有 MMU 的嵌入式系統**
（uClinux）上它仍是唯一選擇。

**現代補充**：Linux 5.3 起有 `clone3()`（`struct clone_args`，可擴充），
以及 `posix_spawn()`（glibc 用 `CLONE_VM|CLONE_VFORK` 實作，比 `fork+exec` 快很多）。

> **書目**：奔跑吧 §7.2「與行程創建和終止相關的操作系統原語」、§7.2.2 fork()、
> §7.2.3 vfork()、§7.2.4 clone()、§7.3.1 `_do_fork()` 函數分析。
> 書中：「vfork() 函數和 fork() 函數類似，但是 vfork() 的父行程會一直阻塞，
> 直到子行程調用 exit() 或者 execve() 為止。」

### 實機驗證（一）：strace 抓出三者的真實系統呼叫

```bash
scp prims.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -o prims prims.c -lpthread
  for m in fork vfork pthread; do echo "===== $m ====="
    strace -f -e trace=clone,clone3,vfork,fork ./prims $m 2>&1 | grep -E "clone" | head -1; done'
```

```
===== fork =====
clone(child_stack=NULL,
      flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD

===== vfork =====
clone(child_stack=0xffffdfd38ad0,
      flags=CLONE_VM|CLONE_VFORK|SIGCHLD

===== pthread =====
clone(child_stack=0xffff993cea60,
      flags=CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD|
            CLONE_SYSVSEM|CLONE_SETTLS|CLONE_PARENT_SETTID|CLONE_CHILD_CLEARTID
```

**三者確實都是 `clone()`**，差別一目瞭然：

| 呼叫 | `CLONE_VM` | `CLONE_VFORK` | `CLONE_THREAD` | `child_stack` |
|------|-----------|--------------|---------------|--------------|
| `fork()` | ✗（複製 mm） | ✗ | ✗ | NULL（共用 COW 堆疊） |
| `vfork()` | ✓（共享 mm） | ✓（阻塞父行程） | ✗ | 有（父行程借給它） |
| `pthread_create()` | ✓ | ✗ | ✓（同一 tgid） | 有（自己配的） |

注意 `fork()` 和 `vfork()` 都帶 **`SIGCHLD`** 作為 exit_signal（結束時通知父行程），
而 `pthread` **沒有** ——因為 `CLONE_THREAD` 的 task 結束時不送 SIGCHLD，
它們同屬一個 thread group（呼應 [Q4](#q4)）。

### 實機驗證（二）：vfork 真的共享 mm 且阻塞父行程

實驗程式 `/tmp/vfork_order.c`：全域變數 `g=111`，子行程改成 222 後睡 1 秒才 `_exit`；
再用 `fork()` 做同樣的事（改成 333）。

```bash
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -o vfork_order vfork_order.c && ./vfork_order'
```

```
[parent] before vfork, g=111
[child ] pid=74548 set g=222, sleeping 1s then _exit      ← 子行程先跑
[parent] resumed after child exited, g=222  <-- vfork SHARES the mm
[parent] after fork()+child g=333, parent still sees g=111  <-- fork COPIES the mm
```

**兩個結論同時被證明**：
1. **`vfork`**：父行程的輸出出現在子行程之後（**被阻塞了 1 秒**），
   而且**看得到子行程改的 `g=222`**（共享同一個 `mm`）。
2. **`fork`**：子行程改成 333，父行程**仍然看到 111**（COW 各自一份）。

---

<a name="q9"></a>
## 9. 請簡述寫時複製技術的工作原理

### 結論

**COW（Copy-On-Write）= 「以唯讀方式共享，真正要寫的時候才複製」。**

**流程**：

```
1. fork() 時：
   copy_process() → copy_mm() → dup_mm() → copy_page_range()   (mm/memory.c:1278)
   ├─ 只複製「頁表」，不複製「頁面內容」
   ├─ 把父、子雙方的 PTE 都清掉可寫位（設為唯讀），並設 PTE 的 soft-dirty/write-protect
   └─ 對每個共享頁做 get_page()（refcount++）

2. 任一方嘗試寫入：
   MMU 發現 PTE 唯讀 → 觸發「寫保護缺頁異常」(Permission fault)
   → arch/arm64/mm/fault.c do_page_fault() → handle_mm_fault()
   → handle_pte_fault() → do_wp_page()          (mm/memory.c)

3. do_wp_page() 的判斷：
   ├─ 若 page 的 mapcount == 1（只剩我一個用）→ 直接把 PTE 改成可寫，「reuse」，不複製！
   └─ 否則 → wp_page_copy(): 配新頁 → 複製內容 → 建新 anon_rmap → 更新 PTE 為可寫
                             → 舊頁 put_page()（refcount--）
```

**COW 的價值**：`fork()` 的成本從 O(位址空間大小) 降到 O(頁表大小)。
配合 `execve()` 馬上把整個位址空間換掉的常見情境，**幾乎所有複製都被避免了**。

**兩個容易被追問的細節**：
1. **讀取不會觸發 COW**——PTE 是存在的（present）、只是唯讀，讀取直接命中，連缺頁都沒有。
2. **最後一個持有者不需要複製**——`do_wp_page()` 發現 `mapcount==1` 就地改成可寫（reuse）。
   這也是為什麼子行程退出後，父行程再寫這些頁不會有 COW 成本。

> **書目**：奔跑吧 §7.2.1「寫時複製技術」——
> 「寫時複製技術就是父行程在創建子行程時不需要複製行程地址空間的內容到子行程，
> 只需要複製父行程的行程地址空間的頁表到子行程…當父、子行程中有一方需要修改某個物理頁面的
> 內容時，觸發寫保護的缺頁異常，然後才複製共享頁面的內容。」（圖 7.7）
> 另見 §7.3.5「copy_mm() 函數分析」。

### 實機驗證（COW 缺頁次數精準命中）

實驗程式 `/tmp/cow.c`：父行程 `memset` 64 MB 匿名記憶體 → `fork()` →
子行程先做**唯讀掃描**、再做**寫入掃描**，全程觀察 `getrusage(RUSAGE_SELF).ru_minflt`
和 `/proc/self/smaps` 的 `Shared_Dirty`/`Private_Dirty`。

```bash
scp cow.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -o cow cow.c && ./cow'
```

```
                                    Rss     Shared_Dirty  Private_Dirty  | min_flt   VmRSS
[parent] after memset (pre-fork)   65536             0          65536    |   16456   66804
[child ] right after fork          65536         65536              0    |      23   66464
[child ] after READ-only sweep     65536         65536              0    |      25   66528
[child ] after WRITE sweep (COW)   65536             0          65536    |   16456   66804
[parent] after child exited        65536             0          65536    |   16464   66804
```

**這張表把 COW 的每一步都拍下來了**：

| 觀察點 | 數據 | 意義 |
|--------|------|------|
| **fork 瞬間** | `Private_Dirty 65536 → Shared_Dirty 65536` | 64 MB 全部從「私有」變成「**共享**」，**一個 byte 都沒複製** |
| **fork 瞬間** | 子行程 `min_flt = 23` | 只有 23 次缺頁（載入器造成的），**64MB 完全沒有缺頁** |
| **唯讀掃描 16384 頁** | `min_flt 23 → 25`，**只 +2** | ✅ **讀取完全不觸發 COW**（PTE 已存在，只是唯讀） |
| **寫入掃描 16384 頁** | `min_flt 25 → 16456`，**+16431** | ✅ **每寫一頁就一次 COW 缺頁**，64MB/4KB = **16384**，誤差 47 來自 printf/system() |
| **寫入後** | `Shared_Dirty 65536 → 0`，`Private_Dirty 0 → 65536` | 64 MB 全部被複製成子行程私有 |
| **子行程退出後** | 父行程 `Private_Dirty 65536` | 父行程的頁回歸私有（`mapcount` 降回 1） |

**`16384 = 64 MB / 4 KB` 這個數字精準命中**，是 COW「一頁一次缺頁異常」最直接的證據。

**用 perf 從核心側再看一次**：

```bash
ssh radxa@192.168.68.57 'sudo perf stat -e page-faults,minor-faults -- /tmp/cow 2>&1 | tail -6'
```

```
        33,530      minor-faults
   1.158761136 seconds time elapsed
   0.011003000 seconds user
   0.148196000 seconds sys      ← 幾乎全部時間都花在核心處理 COW 缺頁
```

**33,530 ≈ 2 × 16,384**：父行程 `memset` 時的 demand-paging 缺頁一次（16384），
子行程 COW 時再一次（16384），加上載入器與 `system()` 的零頭。
**COW 的代價一目瞭然——`sys` 時間是 `user` 時間的 13 倍。**

**COW 的全域統計**：

```bash
ssh radxa@192.168.68.57 'grep -E "^(pgfault|pgmajfault)" /proc/vmstat'
# pgfault    52275647    ← 開機至今 5227 萬次次缺頁（絕大多數是 COW + demand paging）
# pgmajfault    11642    ← 只有 1.1 萬次需要真的讀磁碟（major fault）
```

---

<a name="q10"></a>
## 10. 在 ARM64 的 Linux 內核中如何獲取當前進程的 task_struct 數據結構？

### 結論

**用 `SP_EL0` 暫存器。** `current` 巨集在 ARM64 上就是一條 `mrs` 指令：

```c
/* arch/arm64/include/asm/current.h —— 本機原始碼，一字不差 */
static __always_inline struct task_struct *get_current(void)
{
    unsigned long sp_el0;
    asm ("mrs %0, sp_el0" : "=r" (sp_el0));
    return (struct task_struct *)sp_el0;
}
#define current get_current()
```

**為什麼是 `SP_EL0`？** ARM64 的每個異常等級都有自己的堆疊指標。核心跑在 **EL1**，
使用 `SP_EL1`；而 **`SP_EL0` 在 EL1 的上下文中是閒置的**（使用者態的 SP 已經被存到 pt_regs 裡）。
於是 ARM64 就徵用這個閒置暫存器來存 `task_struct` 指標——**一條指令搞定，零記憶體存取**。

**演進史（面試常考）**：

| 版本 | 做法 | 問題 |
|------|------|------|
| **Linux 4.x（ARM32 至今）** | `thread_info` 放在**核心堆疊底部**。`current` = `SP & ~(THREAD_SIZE-1)` → `thread_info->task` | ① 堆疊溢位會**踩壞 `thread_info`**（含 `addr_limit`，可被利用提權）② 堆疊位址洩漏 = `task_struct` 位址洩漏 |
| **Linux 5.0+（ARM64）** | `CONFIG_THREAD_INFO_IN_TASK=y`：`thread_info` **內嵌進 `task_struct`**；`current` 從 **`SP_EL0`** 讀 | 解決上述兩個問題，且更快 |

**`SP_EL0` 是誰維護的？**

```bash
grep -n "sp_el0" arch/arm64/kernel/entry.S arch/arm64/kernel/head.S
```

```
arch/arm64/kernel/head.S:398:    msr  sp_el0, \tsk       ← 開機時 __primary_switched 設 init_task
arch/arm64/kernel/entry.S:221:   mrs  x21, sp_el0        ← kernel_entry: 從使用者態進來時保存
arch/arm64/kernel/entry.S:223:   msr  sp_el0, tsk        ← 並改成 current task
arch/arm64/kernel/entry.S:357:   msr  sp_el0, x23        ← kernel_exit: 返回使用者態時還原
arch/arm64/kernel/entry.S:1036:  mrs  x28, sp_el0        ← SDEI 處理：不能信任 sp_el0，先存起來
```

外加 **context switch 時**：`__switch_to()`（`arch/arm64/kernel/process.c:530` 附近）會
`write_sysreg(next, sp_el0)` 把新 task 寫進去。註解直白：
> `We store our current task in sp_el0, which is clobbered by userspace.`

> **書目**：奔跑吧 §7.1.6「獲取當前行程」，含圖 7.4（Linux 4.0 的 thread_info 方式）
> 與圖 7.5（Linux 5.0 的 SP_EL0 方式）。書中提到的兩個設計目的（防止 thread_info 被堆疊溢位
> 破壞、防止堆疊位址洩漏被利用）與本機原始碼完全一致。

### 實機驗證

```bash
# 確認組態
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep -E "THREAD_INFO_IN_TASK|VMAP_STACK"'
```

```
CONFIG_THREAD_INFO_IN_TASK=y      ✅ thread_info 已搬進 task_struct
CONFIG_VMAP_STACK=y               ✅ 核心堆疊用 vmalloc + guard page
```

看反組譯出來的 `current`（任何用到 `current` 的核心函式都是一條 `mrs`）：

```bash
ssh radxa@192.168.68.57 'sudo grep -E " (get_current|__switch_to)$" /proc/kallsyms'
# 若裝置上有 vmlinux 可再用 objdump 反組譯確認：
# aarch64-linux-gnu-objdump -d vmlinux | grep -A2 "<__switch_to>:"
```

`entry.S:865` 的 `get_current_task tsk` 巨集展開後也是 `mrs \rd, sp_el0`
（見 [Q15](#q15) 的 `ret_from_fork`）。

---

<a name="q11"></a>
## 11. 下面的程序會輸出幾個「_」？

```c
#include <stdio.h>
int main(void)
{
    int i;
    for(i=0; i<2; i++){
        fork();
        printf("_\n");
    }
    wait(NULL);
    wait(NULL);
    return 0;
}
```

### 結論

> ### **答案取決於 stdout 的緩衝模式：**
> - **stdout 是終端機（行緩衝）或無緩衝 → 6 個**
> - **stdout 被重導向到檔案/管線（全緩衝）→ 8 個**
>
> 書上的答案是 **6**（假設在終端機執行）。

**第一部分：為什麼是 6？（行程樹）**

`fork()` 呼叫了 3 次，產生 4 個行程；`printf` 被執行 **6 次**：

```
i=0:  P ──fork──► C1        P 印 1 次, C1 印 1 次           = 2
i=1:  P ──fork──► C2        P 印 1 次, C2 印 1 次           = 2
      C1──fork──► C3        C1 印 1 次, C3 印 1 次          = 2
                                                     總計    = 6
```

（C2、C3 是在 `i=1` 才被 fork 出來的，迴圈結束後直接退出，只印 1 次。）

**第二部分：為什麼會變成 8？（stdio 緩衝 + COW）**

`printf` 寫的是 **glibc 使用者空間的 `FILE` 緩衝區**，不是直接 `write()`。

- **全緩衝（重導向到檔案/管線，緩衝區 4096 bytes）**：
  `i=0` 印的 `"_\n"` **還躺在使用者空間的緩衝區裡沒有被 flush**。
  接著 `i=1` 呼叫 `fork()`，**整個位址空間（含這個未 flush 的緩衝區）被 COW 複製給子行程**。
  於是 P 和 C2 各自持有一份「已經有 1 個 `_`」的緩衝區，C1 和 C3 也是。
  4 個行程結束時 `exit()` 各自 flush 出 2 個 → **8 個**。
- **行緩衝（終端機）**：`"_\n"` 遇到 `\n` 立刻 flush 到核心，緩衝區在 fork 前就空了 → **6 個**。

> **書目**：奔跑吧 §7.3.6 末段明確給出「它最終輸出 6 個『_』」（圖 7.13 解題思路）。
> 書中是在終端機下執行的情境。

### 實機驗證（兩種答案都重現了）

```bash
scp q11.c q11_pid.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -w -o q11 q11.c
  echo "=== A) stdout 是 TTY（行緩衝）==="; script -qc ./q11 /dev/null | tr -d "\r" | grep -c "_"
  echo "=== B) stdout 是檔案（全緩衝）==="; ./q11 > q11.out; grep -c "_" q11.out
  echo "=== C) stdbuf -o0（無緩衝）==="; stdbuf -o0 ./q11 > q11b.out; grep -c "_" q11b.out'
```

```
=== A) stdout 是 TTY（行緩衝）===
6                              ✅ 與書上答案一致
=== B) stdout 是檔案（全緩衝）===
8                              ✅ COW 把未 flush 的緩衝區也複製了
=== C) stdbuf -o0（無緩衝）===
6                              ✅ 強制無緩衝後又回到 6
```

**B 和 C 的對比是決定性的**：同樣輸出到檔案，只是把緩衝模式從全緩衝改成無緩衝，
答案就從 8 變回 6 ——**證明那多出來的 2 個完全來自 stdio 緩衝區被 fork 複製**。

**用加了標記的版本看行程樹**（`q11_pid.c`，每次都 `fflush`）：

```bash
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -w -o q11_pid q11_pid.c && ./q11_pid | sort'
```

```
i=0 pid=74441 ppid=74416 fork_ret=74443     ← P，第 1 次 fork，生下 C1(74443)
i=0 pid=74443 ppid=74441 fork_ret=0         ← C1，fork 回傳 0
i=1 pid=74441 ppid=74416 fork_ret=74444     ← P，第 2 次 fork，生下 C2(74444)
i=1 pid=74443 ppid=74441 fork_ret=74445     ← C1 也 fork，生下 C3(74445)
i=1 pid=74444 ppid=74441 fork_ret=0         ← C2
i=1 pid=74445 ppid=74443 fork_ret=0         ← C3
```

**恰好 6 行**，行程樹是 `P{74441} → C1{74443} → C3{74445}` 加 `P → C2{74444}`，
4 個行程共 3 次 fork ✅。同時可以看到 `fork_ret` 在父行程是子 PID、在子行程是 0（呼應 [Q14](#q14)）。

> **面試回答建議**：先答「6」，然後**主動補充**「但如果重導向到檔案會是 8，因為 stdio 全緩衝
> 加上 COW」。這一句話能區分「背過答案」和「真的懂」。

---

<a name="q12"></a>
## 12. 用戶空間進程的頁表是什麼時候分配的？一級頁表？二級頁表？

### 結論

| 層級 | 何時分配 | 程式碼路徑 |
|------|---------|-----------|
| **PGD（一級/頂級）** | **建立 `mm_struct` 的當下**（`fork()` / `execve()`），一次配好一整頁 | `mm_init()` → `mm_alloc_pgd()` → `pgd_alloc(mm)`（`kernel/fork.c:731,1159`） |
| **PUD / PMD / PTE（下級）** | **第一次存取該位址、觸發缺頁異常時**，按需（on-demand）逐級配置 | `handle_mm_fault()` → `pud_alloc()`(`mm/memory.c:5085`) → `pmd_alloc()`(`:5115`) → `pte_alloc()`/`__pte_alloc()`(`:466`) |

**本機是 4 級頁表**（`CONFIG_PGTABLE_LEVELS=4`, `VA_BITS=48`, 4 KB 頁）：

```
虛擬位址 48 bits = [47:39] PGD | [38:30] PUD | [29:21] PMD | [20:12] PTE | [11:0] offset
                     512 項       512 項        512 項        512 項
每級各佔 1 個 4KB 頁（512 × 8 bytes）
  一個 PTE 表  覆蓋   2 MB
  一個 PMD 表  覆蓋   1 GB
  一個 PUD 表  覆蓋 512 GB
```

**兩個關鍵觀念**：
1. **`mmap()` 不會配置任何下級頁表**——它只建立 VMA。真正配頁表要等到**第一次觸碰**。
   這就是「demand paging」。
2. **`fork()` 會完整複製整棵頁表**（`copy_page_range()`），這是 fork 的主要成本。
   4 GB 的位址空間光頁表就要 8 MB（4G/4K × 8 bytes），加上上級表。

**頁表本身佔多少記憶體**：`mm->pgtables_bytes` → `/proc/PID/status` 的 `VmPTE`；
全系統則是 `/proc/meminfo` 的 `PageTables`（`NR_PAGETABLE`）。

> **書目**：奔跑吧 §7.3.5「copy_mm() 函數分析」、§7.3.2「copy_process() 函數分析」。
> 另見卷 1 第 2 章（ARM64 頁表格式）與第 4 章（缺頁異常處理）。

### 實機驗證（VmPTE 逐級變化，數字精準命中）

實驗程式 `/tmp/pgtbl.c`：分階段觸碰不同「距離」的位址，每步讀 `/proc/self/status:VmPTE`。

```bash
scp pgtbl.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -w -o pgtbl pgtbl.c && ./pgtbl'
```

```
at main() entry (pgd + 執行檔映像的各級表)        VmPTE=  40 kB (10 pages)
after mmap 1GB (no touch)                        VmPTE=  40 kB (10 pages)   ← 沒變！
after touching 8 pages inside one 2MB range      VmPTE=  48 kB (12 pages)   ← +2
after touching 8 pages in 8 different 2MB        VmPTE=  76 kB (19 pages)   ← +7
after touching 4 pages in 4 different 1GB        VmPTE= 108 kB (27 pages)   ← +8
```

**逐項解讀（每個數字都對得上）**：

| 步驟 | 增量 | 解釋 |
|------|------|------|
| `mmap()` 1 GB **不觸碰** | **+0** ✅ | **`mmap` 完全不配置下級頁表**，只建 VMA |
| 觸碰**同一個 2 MB 內**的 8 頁 | **+2** ✅ | 這 8 頁共用 1 個 PTE 表。因為 mmap 落在全新的 1 GB 範圍，所以要 **1 個 PMD 表 + 1 個 PTE 表 = 2 頁** |
| 觸碰 **8 個不同 2 MB** 的 8 頁 | **+7** ✅ | 每個 2 MB 要一個 PTE 表；第 1 個 2 MB 上一步已經配好 → **8 − 1 = 7 頁** |
| 觸碰 **4 個不同 1 GB** 的 4 頁 | **+8** ✅ | 每個 1 GB 要 **1 個 PMD 表 + 1 個 PTE 表** → **4 × 2 = 8 頁** |

**這組數字完美證明了「按需逐級配置」**：
存取的位址跨越的層級邊界越多，配的頁表就越多；同一個 2 MB 內再怎麼觸碰都只要 1 個 PTE 表。

**fork 會複製整棵頁表**：

```bash
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -w -o pgfork pgfork.c && ./pgfork'
```

```
[parent] VmPTE = 556 kB (before fork)
[child ] VmPTE = 556 kB (immediately after fork -> page tables were COPIED)
```

256 MB 的匿名映射需要 556 kB 頁表（≈ 256M/4K × 8B = 512 kB PTE + 上級表），
**子行程一 fork 出來就有一模一樣的 556 kB** ——這就是 [Q9](#q9) 說的「COW 只複製頁表」的成本。

**全系統的頁表開銷**：

```bash
ssh radxa@192.168.68.57 'grep -E "^(PageTables|SecPageTables):" /proc/meminfo; grep nr_page_table /proc/vmstat'
```

```
PageTables:  20244 kB       ← 全系統 295 個行程的頁表共 20 MB
nr_page_table_pages 5061    ← 5061 頁
```

---

<a name="q13"></a>
## 13. 什麼是進程調度器？早期 Linux 內核調度器（O(n) 和 O(1)）如何工作？

### 結論

### 調度器是什麼

**在多個就緒（runnable）行程中，決定「下一個跑誰、跑多久」的核心元件。**
存在的根本理由是**提高 CPU 使用率**：行程等磁碟、等鍵盤、等頁面時，
與其讓 CPU 陪著空轉，不如切去跑別人。

**通用 OS 的難處**是必須同時服侍三類行為迥異的行程：

| 類型 | 特徵 | 需求 |
|------|------|------|
| **交互式** | Vim、瀏覽器；睡著等使用者喚醒 | **響應時間越短越好** |
| **批處理** | 編譯、轉檔；悶頭吃 CPU | 吞吐量優先，不在乎延遲 |
| **實時** | VR（19 ms 內）、工控 | **延遲上界必須有保證** |

另一個切法是 **CPU-bound**（一直算，如 MATLAB）vs **I/O-bound**（一直等，如鍵盤輸入）。
麻煩的是 X-window 這種**兩者都是**的行程，所以調度器永遠是在
**吞吐量 vs 響應性**之間妥協（Linux 偏向響應性）。

### O(n) 調度器（Linux ≤ 2.4）

```
全域一條 runqueue（所有 CPU 共用一個鏈結串列）
每個行程建立時給一個固定時間片（epoch）

schedule():
    for each task in global runqueue:        ← ★ 遍歷全部！O(n)
        weight = goodness(task)              ← 算優先級 + 剩餘時間片
        pick the max
    當所有行程的時間片都用完 → 進入新 epoch，重新分配所有時間片
```

**三大缺點**：
1. **O(n)**：runqueue 上行程越多，選一次越慢——伺服器上跑幾千個行程直接崩潰。
2. **全域鎖**：所有 CPU 搶同一把 runqueue lock，SMP 擴展性極差。
3. **epoch 重算**：時間片用完時要**遍歷全系統所有行程**重新分配，是個 O(n) 的大停頓。

### O(1) 調度器（Linux 2.6，Ingo Molnar）

核心思想來自 Corbato 1962 年的**多級反饋佇列（MLFQ）**。

```
每顆 CPU 各自維護一個 runqueue        ← 解決全域鎖
每個 runqueue 有兩個優先級陣列：
   ┌─────────────────────────────────┐
   │ active[140]   ← 還有時間片的     │  140 = 100 實時 + 40 普通(nice -20..19)
   │ expired[140]  ← 時間片用完的     │
   └─────────────────────────────────┘
   每個陣列配一個 140-bit 的 bitmap

schedule():
    idx = find_first_bit(active->bitmap)     ← ★ 一條指令！與行程數無關 = O(1)
    next = first task in active->queue[idx]

當 active 陣列空了 → 直接 swap(active, expired) 兩個指標    ← O(1)，不用遍歷
```

**兩個關鍵設計**：
1. **bitmap + 優先級陣列** → 選下一個行程變成「找第一個 set bit」，時間複雜度與行程數無關。
2. **active/expired 對調** → 消除了 O(n) 調度器的 epoch 重算停頓。

**它的死穴——「交互性啟發式」**：O(1) 調度器靠一堆經驗公式猜測「這個行程是不是交互式的」
（依睡眠時間給 ±5 的優先級獎懲），猜對了很順、猜錯了就卡頓。
這堆 heuristic 越補越複雜、越來越難維護，最終在 **Linux 2.6.23 被 CFS 取代**。

### CFS（Linux 2.6.23+，本機使用中）

拋棄固定時間片和固定調度週期，改用**權重比例 + 虛擬時間（vruntime）**：

```
vruntime += delta_exec × (NICE_0_LOAD / se->load.weight)
```

- 優先級高（nice 小）→ 權重大 → vruntime **走得慢** → 能跑更久
- 調度器永遠挑 **vruntime 最小**的行程（用紅黑樹，選取 O(1)、插入 O(log n)）
- 比喻：「像一個多級變速箱，nice 為 0 的行程是基準齒輪，其他行程在不同變速比下相互追趕」

> **書目**：奔跑吧 §7.4「行程調度原語」、§7.4.1 行程分類、§7.4.5 經典調度算法（MLFQ）、
> §7.4.6「Linux 內核的 O(n) 調度算法」、§7.4.7「Linux 內核的 O(1) 調度算法」、
> §7.4.8「Linux 內核的 CFS」。第 8 章有 CFS 與負載均衡的完整分析。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'chrt -m'
```

```
SCHED_OTHER    min/max priority : 0/0        ← CFS（一般行程）
SCHED_FIFO     min/max priority : 1/99       ← 實時
SCHED_RR       min/max priority : 1/99       ← 實時
SCHED_BATCH    min/max priority : 0/0        ← CFS 的批處理變體
SCHED_IDLE     min/max priority : 0/0        ← 最低
SCHED_DEADLINE min/max priority : 0/0        ← EDF
```

對應核心的 5 個調度類（優先級由高到低串成鏈）：
`stop_sched_class` → `dl_sched_class` → `rt_sched_class` → `fair_sched_class` → `idle_sched_class`

**看 CFS 的 vruntime 實際在動**：

```bash
ssh radxa@192.168.68.57 'cat /proc/1/sched | head -8'
```

```
systemd (1, #threads: 1)
se.exec_start          : 172791089.431043
se.vruntime            :     41165.815840     ← CFS 的虛擬時間 ✅
se.sum_exec_runtime    :    167957.392551     ← 真實執行時間 168 秒
se.nr_migrations       :             4062     ← 被搬到別的 CPU 4062 次
nr_switches            :            60249
nr_voluntary_switches  :            33183     ← 主動讓出（等 I/O）
```

**`vruntime (41165) ≪ sum_exec_runtime (167957)`** 很有意思：
systemd 大多數時間在睡覺，每次被喚醒時 CFS 會把它的 vruntime 拉到接近
`min_vruntime`（`place_entity()` 的睡眠補償），所以 vruntime 遠落後於真實執行時間——
**這正是 CFS 照顧交互式行程的機制**（不需要 O(1) 那套 heuristic）。

**RK3588 特有的一點**：這是 big.LITTLE（4×A76 + 4×A55），
`nr_migrations=4062` 有相當比例是 EAS（Energy Aware Scheduling）在大小核之間搬移。

```bash
ssh radxa@192.168.68.57 'for c in 0 4 6; do printf "cpu%s: %s\n" $c "$(cat /sys/devices/system/cpu/cpu$c/cpu_capacity)"; done'
```

```
cpu0: 422      ← Cortex-A55（小核）
cpu4: 1024     ← Cortex-A76（大核）
cpu6: 1024     ← Cortex-A76（大核）
```

**大小核的 capacity 差 2.4 倍**，所以 CFS 的 `load_balance()` 必須做 capacity-aware 決策
（`select_task_rq_fair()` → EAS 的 `find_energy_efficient_cpu()`），
不能像同構 SMP 那樣只看 runqueue 長度。這是第 8 章負載均衡的重點。

---

<a name="q14"></a>
## 14. 以 fork() 為例，為什麼會返回兩次？父行程返回子 PID，子行程返回 0，子行程是如何返回 0 的？

### 結論

**「返回兩次」是個誤解——其實是「兩個行程各返回一次」。**

**父行程這一次**：`fork()` → `svc` 陷入核心 → `kernel_clone()` 建好子行程 →
```c
/* kernel/fork.c:2695 */
nr = pid_vnr(pid);
...
return nr;                    /* 回傳子行程的 PID */
```
這個回傳值透過 **X0** 傳回使用者空間，正常地從 `svc` 的下一條指令繼續。

**子行程這一次**：子行程是**被調度器排進 runqueue、稍後才被選中執行的**。
它從來沒有「呼叫」過 `fork()`——它是被**憑空造出來，並且被安排成看起來像剛從 `svc` 返回**。
關鍵在 `copy_thread()`：

```c
/* arch/arm64/kernel/process.c:411 —— 本機原始碼 */
int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
    struct pt_regs *childregs = task_pt_regs(p);   /* 子行程核心堆疊頂端的暫存器框 */
    memset(&p->thread.cpu_context, 0, sizeof(struct cpu_context));
    ...
    if (likely(!args->fn)) {                       /* 使用者行程（不是核心執行緒） */
        *childregs = *current_pt_regs();           /* ① 整框複製父行程的 pt_regs */
        childregs->regs[0] = 0;                    /* ② ★ 把 X0 改成 0 ★ */
        ...
    } else {                                       /* 核心執行緒 */
        memset(childregs, 0, sizeof(struct pt_regs));
        childregs->pstate = PSR_MODE_EL1h | PSR_IL_BIT;
        p->thread.cpu_context.x19 = (unsigned long)args->fn;      /* 回呼函式 */
        p->thread.cpu_context.x20 = (unsigned long)args->fn_arg;
    }
    p->thread.cpu_context.pc = (unsigned long)ret_from_fork;      /* ③ 入口 */
    p->thread.cpu_context.sp = (unsigned long)childregs;
    ...
}
```

**三個動作決定了一切**：

| 動作 | 效果 |
|------|------|
| ① `*childregs = *current_pt_regs()` | 子行程繼承父行程的**全部** X0–X30、SP、**PC**、PSTATE。因為 PC 是父行程 `svc` 的下一條指令，**子行程返回使用者空間時會停在跟父行程一模一樣的位置** |
| ② `childregs->regs[0] = 0` | **AArch64 PCS 規定 X0 是函式回傳值暫存器**。把它硬設成 0，子行程從核心返回使用者空間時 `fork()` 的回傳值就是 **0** |
| ③ `cpu_context.pc = ret_from_fork` | 子行程**第一次**被 `__switch_to()` 選中時，會從 `ret_from_fork` 開始跑，而不是從 `schedule()` 中間回來（見 [Q15](#q15)） |

**所以子行程的 0 不是「return 0」return 出來的，是核心在它的暫存器框裡「寫」進去的。**

> **書目**：奔跑吧 §7.3.6「行程創建後的返回」——
> 「`copy_thread()` 函數還會修改子行程的棧框中 X0 暫存器的值為 0，
> 因此在返回使用者空間時子行程的返回值就是 0，通過 X0 暫存器來傳遞返回值。」
> （書中用的是 Linux 5.x 的舊簽名 `copy_thread(...)`，6.1 已改為
> `copy_thread(struct task_struct *p, const struct kernel_clone_args *args)`，
> 判斷核心執行緒的條件也從 `p->flags & PF_KTHREAD` 改成 `args->fn`。）

### 實機驗證

```bash
ssh radxa@192.168.68.57 'cd /tmp && ./q11_pid | sort'
```

```
i=0 pid=74441 ... fork_ret=74443     ← 父行程拿到子 PID
i=0 pid=74443 ... fork_ret=0         ← 子行程拿到 0   ✅
i=1 pid=74441 ... fork_ret=74444     ← 父
i=1 pid=74443 ... fork_ret=74445     ← C1 當父
i=1 pid=74444 ... fork_ret=0         ← 子   ✅
i=1 pid=74445 ... fork_ret=0         ← 子   ✅
```

**同一行程式碼、同一個 `fork()` 呼叫點，父行程得到子 PID，子行程得到 0。**

**用 strace 看核心側只回一個值**：

```bash
ssh radxa@192.168.68.57 'cd /tmp && strace -f -e trace=clone ./prims fork 2>&1 | grep -E "clone|SIGCHLD" | head -4'
```

```
clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID|CLONE_CHILD_SETTID|SIGCHLD, ...) = 74533
strace: Process 74533 attached
```

`clone()` **只在父行程這邊回傳了 74533**——strace 看不到子行程「返回 0」這件事，
因為那不是一次系統呼叫返回，而是子行程被調度後從 `ret_from_fork` 走出來的結果。
`strace -f` 只能事後說 `Process 74533 attached`。

---

<a name="q15"></a>
## 15. 第一次返回用戶空間時，子進程返回哪裡？

### 結論

**返回到 `ret_from_fork` 這個組合語言函式，再由它走 `ret_to_user` 回到使用者空間——
落點與父行程完全相同（`fork()`/`clone()` 呼叫後的下一條指令）。**

**完整路徑**：

```
子行程第一次被調度：
  schedule() → context_switch() → switch_to() → __switch_to()
       └─ 還原 p->thread.cpu_context（x19–x28, fp, sp, pc）
       └─ pc = ret_from_fork    ← copy_thread() 在 ③ 設好的
                │
                ▼
     ┌──────────────────────────────────────────┐
     │ arch/arm64/kernel/entry.S:860             │
     │ SYM_CODE_START(ret_from_fork)             │
     │     bl   schedule_tail                    │ ← 放掉前一個 task 的 rq lock
     │     cbz  x19, 1f          // 不是核心執行緒？│ ← x19==0 ⇒ 使用者行程
     │     mov  x0, x20                          │   （核心執行緒才有回呼）
     │     blr  x19              // 呼叫 kthread fn│
     │ 1:  get_current_task tsk                  │ ← mrs tsk, sp_el0（見 Q10）
     │     mov  x0, sp                           │
     │     bl   asm_exit_to_user_mode            │ ← 處理 pending signal/resched
     │     b    ret_to_user                      │
     │ SYM_CODE_END(ret_from_fork)               │
     └──────────────────────────────────────────┘
                │
                ▼
     ret_to_user → kernel_exit 0 → 從 pt_regs 還原 X0–X30/SP_EL0/ELR_EL1/SPSR_EL1
                                 → eret
                │
                ▼
     使用者空間：ELR_EL1 = 父行程 svc 的下一條指令，X0 = 0
```

**兩條岔路**（由 `x19` 決定，`x19` 來自 `copy_thread()` 設的 `cpu_context.x19`）：

| `x19` | 身分 | 走向 |
|-------|------|------|
| `0` | **使用者行程**（`fork`/`clone`） | 跳到標籤 `1:` → `ret_to_user` → `eret` 回使用者空間 |
| `≠0` | **核心執行緒**（`kthread`） | `blr x19` 呼叫回呼函式（如 `kswapd`、`kthreadd`），**永遠不回使用者空間** |

**在使用者空間那一側**（以 glibc 的 `__clone` 為例，`sysdeps/unix/sysv/linux/aarch64/clone.S`）：

```asm
__clone:
    mov  x10, x0            /* 暫存子行程回呼函式位址 */
    ...
    mov  x8, #__NR_clone
    svc  0x0                /* ← 陷入核心 */
    /* ★ 父行程和子行程都從這裡「返回」★ */
    cmp  x0, #0
    beq  thread_start       /* X0 == 0 ⇒ 我是子行程 */
    ret                     /* X0 != 0 ⇒ 我是父行程，回傳子 PID */
thread_start:
    mov  x0, x12
    blr  x10                /* 執行子行程的回呼 */
```

**因為 `copy_thread()` 是整框複製 `pt_regs`，所以 `ELR_EL1`（返回位址）也被繼承了。
子行程 `eret` 之後，PC 落在跟父行程一模一樣的 `cmp x0, #0` 這一行——
唯一的差別就是 X0 被改成了 0。**

> **書目**：奔跑吧 §7.3.6「行程創建後的返回」，含 `ret_from_fork` 與 glibc `__clone` 的
> 完整組語分析。
> **📌 與書中的版本差異**：書上的 `ret_from_fork` 是
> ```asm
> ENTRY(ret_from_fork)
>     cbz x19, 1f
>     mov x0, x20
>     blr x19
> 1:  b   ret_to_user
> ```
> 本機 6.1 的版本（`entry.S:860`）多了 **`bl schedule_tail`**（開頭）和
> **`get_current_task tsk` / `bl asm_exit_to_user_mode`**（結尾），
> 後者是 Linux 5.11 之後把 entry/exit 邏輯從組語搬到 C（`entry-common.c`）的結果。
> 面試時能講出「新版把 exit-to-user 的工作交給 C 寫的 `exit_to_user_mode_prepare()`」會加分。

### 實機驗證

```bash
# 確認本機原始碼
grep -n -A10 "SYM_CODE_START(ret_from_fork)" arch/arm64/kernel/entry.S
```

```
860:SYM_CODE_START(ret_from_fork)
861-    bl      schedule_tail
862-    cbz     x19, 1f                 // not a kernel thread
863-    mov     x0, x20
864-    blr     x19
865-1:  get_current_task tsk
866-    mov     x0, sp
867-    bl      asm_exit_to_user_mode
868-    b       ret_to_user
869-SYM_CODE_END(ret_from_fork)
870-NOKPROBE(ret_from_fork)
```

```bash
# 在裝置上確認符號存在
ssh radxa@192.168.68.57 'sudo grep -E " (ret_from_fork|ret_to_user|schedule_tail)$" /proc/kallsyms'
```

```
ffff800008012094 t ret_to_user
ffff800008016018 T ret_from_fork
ffff8000080b19a4 T schedule_tail
```

**用 ftrace 直接抓到子行程從 `ret_from_fork` 走出來**（本題最有力的證據）：

```bash
ssh radxa@192.168.68.57 'echo radxa | sudo -S -p "" sh -c "cd /sys/kernel/debug/tracing
  echo function > current_tracer
  echo schedule_tail > set_ftrace_filter
  echo > trace; echo 1 > tracing_on
  /tmp/prims fork
  echo 0 > tracing_on
  grep -m5 schedule_tail trace
  echo nop > current_tracer; echo > set_ftrace_filter"'
```

```
prims-98677  [003] d.... 173303.492881: schedule_tail <-ret_from_fork
prims-98678  [005] d.... 173303.494444: schedule_tail <-ret_from_fork
```

**ftrace 直接印出了 `schedule_tail <-ret_from_fork`** ——
「`<-`」右邊就是呼叫者（return address），白紙黑字證明子行程的第一站是 `ret_from_fork`。
兩筆記錄分別是 shell fork 出的 `prims` 本身（98677）和 `prims` 自己 fork 的子行程（98678），
而且它們在**不同的 CPU（3 和 5）上**——正是「被調度器排進 runqueue、稍後才執行」的寫照。

> `schedule_tail` 全核心只有 `ret_from_fork` 一個呼叫點，所以它是這條路徑的完美探針。

---

## 附錄：本次用到的實驗程式

全部放在裝置 `/tmp/`，原始碼在本機
`/tmp/claude-1000/.../scratchpad/exp/`：

| 檔案 | 用途 | 對應題目 |
|------|------|---------|
| `life.c` | 僵屍態 / `wait()` 回收 / 孤兒託孤 | Q3, Q5 |
| `tid.c` | `getpid()` vs `gettid()`、`/proc/PID/task/` | Q4 |
| `prims.c` | fork / vfork / pthread 的 strace 對照 | Q8, Q14 |
| `vfork_order.c` | vfork 共享 mm 且阻塞父行程 vs fork COW | Q8 |
| `cow.c` | COW 缺頁計數 + smaps Shared/Private_Dirty | Q9 |
| `q11.c` / `q11_pid.c` | fork 迴圈輸出幾個 `_`（6 vs 8） | Q11 |
| `pgtbl.c` / `pgfork.c` | VmPTE 逐級變化、fork 複製頁表 | Q12 |

一次重跑全部：

```bash
scp life.c tid.c prims.c vfork_order.c cow.c q11.c q11_pid.c pgtbl.c pgfork.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp
  for s in life tid prims vfork_order cow q11 q11_pid pgtbl pgfork; do
      gcc -O0 -w -o $s $s.c -lpthread 2>/dev/null; done
  ./life; ./tid; ./vfork_order; ./cow; ./pgtbl; ./pgfork
  script -qc ./q11 /dev/null | grep -c _ ; ./q11 > o; grep -c _ o'
```

---

## 跨章節關聯

| 本章題目 | 關聯到第 6 章 |
|---------|--------------|
| [Q9](#q9) COW | 第 6 章 [Q2](./ch06_memory_management_case_studies.md#q2) `AnonPages`、`/proc/PID/smaps` 的 `Shared_Dirty`/`Private_Dirty` |
| [Q12](#q12) 頁表 | 第 6 章 [Q2](./ch06_memory_management_case_studies.md#q2) `PageTables`（`NR_PAGETABLE`）、[Q8](./ch06_memory_management_case_studies.md#q8) `VmPTE` |
| [Q2](#q2) 核心堆疊 | 第 6 章 [Q2](./ch06_memory_management_case_studies.md#q2) `KernelStack`（`NR_KERNEL_STACK_KB`） |
| [Q7](#q7) 核心執行緒無 mm | 第 6 章 [Q9](./ch06_memory_management_case_studies.md#q9) P_swap 統計時要跳過 `mm == NULL` 的 task |
