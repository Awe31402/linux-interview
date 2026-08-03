# 卷2 第 1 章 併發與同步 — 高頻面試題解答

> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588），`192.168.68.58`（帳密皆為 `radxa`；DHCP，IP 會變）
> **CPU**：4×Cortex-A55（cpu0~3，最高 1.8 GHz）＋ 4×Cortex-A76（cpu4~7，最高 2.256 GHz）
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`
>
> **關鍵組態**（`zcat /proc/config.gz`）：
> ```
> CONFIG_ARM64_LSE_ATOMICS=y          CONFIG_ARM64_USE_LSE_ATOMICS=y
> CONFIG_QUEUED_SPINLOCKS=y           CONFIG_QUEUED_RWLOCKS=y
> CONFIG_MUTEX_SPIN_ON_OWNER=y        CONFIG_RWSEM_SPIN_ON_OWNER=y
> CONFIG_TREE_RCU=y                   CONFIG_RCU_FANOUT=64   CONFIG_RCU_FANOUT_LEAF=16
> CONFIG_RCU_TRACE=y                  CONFIG_RCU_CPU_STALL_TIMEOUT=60   CONFIG_RCU_NOCB_CPU=y
> CONFIG_DEBUG_SPINLOCK=y             CONFIG_NR_CPUS=8       CONFIG_HZ=300
> CONFIG_PREEMPT_VOLUNTARY=y          ← 核心態不可搶佔
> # CONFIG_PREEMPT_RCU / PREEMPT_COUNT / LOCKDEP / PROVE_LOCKING / LOCK_STAT is not set
> ```
> CPU 特性（`/proc/cpuinfo` 的 Features 行 + `dmesg`）：
> ```
> Features : fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp
> CPU features: detected: LSE atomic instructions
> CPU features: detected: RCpc load-acquire (LDAPR)
> ```
>
> **書目對照**
> - 《奔跑吧 Linux 內核》（第二版）卷 2 第 1 章 —
>   `books/running-linux-kernel/running-kernel-2-txt/08_第1章_并发与同步.txt`
>   （下稱「奔跑吧 §1.x」，行號以該純文字檔為準）
> - 核心程式碼路徑相對於本專案樹
>   `/home/awe/disk/yocto-rockchip-sdk/build/tmp/work-shared/rockchip-rk3588-rock-5b/kernel-source`
> - 卷1 相關章節：📝 [ch08 調度與負載均衡](./ch08_process_management_scheduling_and_load_balancing.md)、
>   📝 [ch09 調試與案例分析](./ch09_process_management_debugging_and_case_studies.md)
>
> ## 書是 Linux 5.0 寫的，本機是 6.1 —— 11 處差異
>
> | # | 書上 | 本機（6.1）實測 | 題號 |
> |---|------|----------------|------|
> | 1 | qspinlock 的 `pending` 只佔 **bit[8]**，bit[9:15] 未使用（表 1.4） | **`_Q_PENDING_BITS = 8`**，pending 佔滿 bit[15:8] | Q15 |
> | 2 | §1.3.1 分析 Linux 4.0 的 **ticket spinlock**（owner/next 牌號） | 本機是 **qspinlock**，ticket 版本早已移除 | Q10、Q12 |
> | 3 | `SLAB_DESTROY_BY_RCU` | **`SLAB_TYPESAFE_BY_RCU`**（4.9 改名，commit `5f0d5a3ae7cf`） | Q36 |
> | 4 | `ACCESS_ONCE()` | **`READ_ONCE()` / `WRITE_ONCE()`**（3.19 起） | Q36、Q37 |
> | 5 | `mm->mmap_sem` | **`mm->mmap_lock`**（5.8 改名並包成 `mmap_read_lock()` 等 API） | Q34 |
> | 6 | `zone->lru_lock` | **`lruvec->lru_lock`**（5.7 移進 memcg 的 lruvec） | Q33 |
> | 7 | `mapping->tree_lock` + radix tree | **`mapping->i_pages` 的 xarray 鎖** | Q33 |
> | 8 | `smp_read_barrier_depends()` | **5.9 已刪除**（Alpha 支援移除後不再需要） | Q6 |
> | 9 | atomic 泛型實作靠 `cmpxchg()` 迴圈 | arm64 **直接用 LSE 單指令**（`stadd`/`ldaddal`） | Q1、Q2 |
> | 10 | 隱含「LSE 一定比 LL/SC 好」 | **實測：A55 上 LSE 慢 1.8 倍；A76 激烈爭用時慢 3.5 倍** | Q1 |
> | 11 | RCU 的 GP kthread | 本機叫 **`rcu_sched`**（`PREEMPT_RCU=n`），不是 `rcu_preempt` | Q28~Q32 |

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | ARM64 如何實現獨佔存取 | **從記憶體讀出 patch 後的指令 = `stadd`**；LL/SC vs LSE 大小核實測 |
| [2](#q2) | `atomic_cmpxchg()` 與 `atomic_xchg()` | **`CASAL` vs `SWPAL`** |
| [3](#q3) | CAS 指令的 acquire / release | **`CASA`/`CASL`/`CASAL`/`CAS` 四種全部抓到** |
| [4](#q4) | `atomic_try_cmpxchg()` 的差別 | 同樣是 `CASAL`，差在回傳值語意 |
| [5](#q5) | `cmpxchg` 四個變體的區別 | 對照表 + 實機指令 |
| [6](#q6) | 核心使用記憶體屏障的場景 | **`dmb ish`/`ishld`/`ishst`/`dsb sy` 實測** |
| [7](#q7) | `smp_cond_load_relaxed()` | **arm64 用 `sevl; wfe; ldxr`，不是忙等** |
| [8](#q8) | `smp_mb__before/after_atomic()` | 實測插入 `dmb ish` |
| [9](#q9) | 自旋鎖臨界區為何不能睡眠 | **我自己寫出的死鎖（活教材）** |
| [10](#q10) | 經典自旋鎖的缺點 | 吞吐量：1→4 執行緒掉 **10.9 倍** |
| [11](#q11) | 為何不允許搶佔 | **本機 `PREEMPT_COUNT=n`，`preempt_count` 全程 0** |
| [12](#q12) | 排隊自旋鎖如何實現 | 三元組狀態機完整重現 |
| [13](#q13) | 臨界區發生中斷的後果 | `spin_lock_irqsave` 對 DAIF 的實測 |
| [14](#q14) | 排隊自旋鎖如何實現 MCS | `qnodes[4]` per-CPU 陣列 |
| [15](#q15) | 32 位元變數如何劃分 | **修正書上表 1.4：pending 是 8 位不是 1 位** |
| [16](#q16) | 4 個 CPU 爭用的流程 | **`{0,0,1}→{0,1,1}→{CPU2,..}→{CPU3,..}` 且嚴格 FIFO** |
| [17](#q17) | 信號量的特點 | `sizeof` 與欄位對照 |
| [18](#q18) | 信號量如何實現 | 原始碼 + 結構 |
| [19](#q19) | 樂觀自旋的判斷條件 | `owner->on_cpu` |
| [20](#q20) | 為何樂觀自旋比睡眠好 | **34444 次拿鎖只睡 2 次 vs 645 次拿鎖睡 1156 次** |
| [21](#q21) | 4 CPU 爭用互斥鎖的時序 | 對照實測 |
| [22](#q22) | 有信號量為何還要互斥鎖 | **信號量沒有 owner 欄位 → 做不了樂觀自旋** |
| [23](#q23) | MCS 鎖的實現原理 | `optimistic_spin_node` 24 B vs `arch_spinlock_t` 4 B |
| [24](#q24) | 如何選擇信號量與互斥鎖 | 決策表 |
| [25](#q25) | 何時用讀者鎖 / 寫者鎖 | **讀者 2314 vs 寫者 1984 次/ms** |
| [26](#q26) | 讀寫信號量的自旋等待 | **`count` 實測：讀者 +256，寫者 bit0** |
| [27](#q27) | RCU 相比讀寫鎖的優勢 | **RCU 讀者 4 執行緒 116990 次/ms，完美線性擴展** |
| [28](#q28) | 靜止狀態與寬限期 | **`synchronize_rcu()` 43.7 ms vs expedited 54 µs** |
| [29](#q29) | RCU 的基本原理 | 書上例子完整跑通 |
| [30](#q30) | 經典 RCU 的問題與 Tree RCU | **位圖清除 `8>f7 → 2>f5 → 1>f4 → f4>0`** |
| [31](#q31) | 為何用 `ULONG_CMP_GE/LT` | 迴繞情境實測 |
| [32](#q32) | 一個 GP 的生命週期 | **ftrace 抓到完整狀態機** |
| [33](#q33) | 五種同步機制的特點與規則 | 總表 + 實測數據 |
| [34](#q34) | KSM 掃描時 VMA 被銷毀 | `mmap_lock` 讀寫者互斥 |
| [35](#q35) | `PG_locked` 的常見用法 | 實測加鎖/解鎖 |
| [36](#q36) | `page_get_anon_vma()` 為何用 RCU | **同一塊記憶體立刻被配置出去的實測** |
| [37](#q37) | `select_bad_process()` 為何用 RCU | 走訪行程鏈結串列 |

---

## 一鍵重現全部實驗

```bash
HOST=192.168.68.58     # DHCP，先確認 IP

# 核心模組
ssh radxa@$HOST 'mkdir -p ~/exp/sync'
scp notes/experiments/{sync_probe,lock_bench,rcu_demo}.c radxa@$HOST:~/exp/sync/
ssh radxa@$HOST 'cat > ~/exp/sync/Makefile <<EOF
obj-m += sync_probe.o lock_bench.o rcu_demo.o
KDIR ?= /lib/modules/\$(shell uname -r)/build
all:
	\$(MAKE) -C \$(KDIR) M=\$(PWD) modules
EOF
cd ~/exp/sync && make'

ssh radxa@$HOST 'sudo insmod ~/exp/sync/sync_probe.ko; sudo rmmod sync_probe; sudo dmesg'
ssh radxa@$HOST 'sudo insmod ~/exp/sync/lock_bench.ko mode=0'   # qspinlock 狀態機
ssh radxa@$HOST 'sudo insmod ~/exp/sync/lock_bench.ko mode=1'   # 吞吐量
ssh radxa@$HOST 'sudo insmod ~/exp/sync/lock_bench.ko mode=2'   # 樂觀自旋
ssh radxa@$HOST 'sudo insmod ~/exp/sync/rcu_demo.ko  mode=1'    # GP 延遲

# 使用者態 LL/SC vs LSE
scp notes/experiments/lse_bench.c radxa@$HOST:/tmp/
ssh radxa@$HOST 'cd /tmp
  gcc -O2 -pthread -march=armv8-a -mno-outline-atomics -o lse_llsc lse_bench.c
  gcc -O2 -pthread -march=armv8.2-a+lse                -o lse_lse  lse_bench.c
  ./lse_llsc 4 400 0 ; ./lse_lse 4 400 0     # A55
  ./lse_llsc 4 400 4 ; ./lse_lse 4 400 4'    # A76
```

> ⚠ `lock_bench.ko mode=1/2` 會讓多顆 CPU 滿載數百毫秒。模組已刻意
> **保留最後一顆 CPU 給系統**；不要把 `nthread` 調到等於 CPU 總數
> （原因見 [Q9](#q9) —— 我第一版就是這樣把整台機器鎖死的）。

---

<a name="q1"></a>
## 1. 在 ARM64 處理器中，如何實現獨占訪問內存？

### 結論

**兩套機制，由 alternatives 在開機/載入模組時「二選一」動態打補丁：**

**(1) LL/SC —— 獨佔載入 / 獨佔儲存（ARMv8.0 起，所有 ARM64 都有）**

```asm
1:  ldxr   w0, [x1]     // Load-eXclusive：載入並把該位址標成「本 CPU 獨佔監視中」
    add    w0, w0, w2   // 修改
    stxr   w3, w0, [x1] // Store-eXclusive：只有監視器還有效才寫得進去，w3=0 表示成功
    cbnz   w3, 1b       // 失敗就整個重來
```
* 靠處理器的**獨佔監視器（exclusive monitor）**：本地監視器管單一 CPU，
  全域監視器管跨 CPU/裝置。任何對該位址的寫入都會讓監視器失效。
* 變體：`ldaxr`（帶 acquire）、`stlxr`（帶 release）。
* **缺點**：爭用激烈時 `stxr` 反覆失敗 → 重試迴圈 → 理論上可能活鎖。

**(2) LSE 原子指令（ARMv8.1 的 Large System Extensions）**

一條指令搞定「讀-改-寫」，不需要監視器也不需要重試：

| 類別 | 指令 | 用途 |
|---|---|---|
| 算術/邏輯 | `LDADD` `LDCLR` `LDEOR` `LDSET` `LDSMAX` `LDSMIN` `LDUMAX` `LDUMIN` | 回傳舊值 |
| 同上（無回傳） | `STADD` `STCLR` … | `LDxxx` 的 Rt=WZR 別名 |
| 比較並交換 | `CAS` `CASA` `CASL` `CASAL`（含 `b`/`h` 位寬後綴） | 見 [Q3](#q3) |
| 交換 | `SWP` `SWPA` `SWPL` `SWPAL` | `xchg()` |

**Linux 怎麼二選一？** `ARM64_LSE_ATOMIC_INSN(llsc, lse)` 展開成
`ALTERNATIVE(llsc, lse, ARM64_HAS_LSE_ATOMICS)`：編譯時**兩份程式碼都放進去**，
開機偵測到 `ID_AA64ISAR0_EL1.Atomic == 2` 就把跳去 LL/SC 的那條分支改寫成 `nop`。

> **書目**：奔跑吧 §1.1.1「原子操作」、§1.1.2「atomic_add() 函數分析」（第 242~349 行）。
> **原始碼**：`arch/arm64/include/asm/atomic_ll_sc.h`、`atomic_lse.h`、`lse.h`。

### 實機驗證

**(a) 編譯期：兩份程式碼並存**（對還沒載入的 `.ko` 反組譯）

```bash
ssh radxa@$HOST 'cd ~/exp/sync && objdump -d --disassemble=demo_atomic_add sync_probe.ko'
```
```
0000000000000000 <demo_atomic_add>:
   0:	d503201f 	nop
   4:	d503201f 	nop
   8:	90000000 	adrp	x0, 0
   c:	14000005 	b	20 <demo_atomic_add+0x20>   ← ★ 沒有 LSE 時跳去 LL/SC
  10:	52800021 	mov	w1, #0x1
  14:	91000000 	add	x0, x0, #0x0
  18:	b821001f 	stadd	w1, [x0]                    ← ★ LSE 快路徑
  1c:	d65f03c0 	ret
  20:	91000000 	add	x0, x0, #0x0
  24:	f9800011 	prfm	pstl1strm, [x0]
  28:	885f7c01 	ldxr	w1, [x0]                    ← ★ LL/SC 後備路徑
  2c:	11000421 	add	w1, w1, #0x1
  30:	88027c01 	stxr	w2, w1, [x0]
  34:	35ffffa2 	cbnz	w2, 28
  38:	17fffff9 	b	1c
```

**(b) 執行期：把已經被 patch 過的指令從記憶體讀出來**

`sync_probe.ko` 在 `module_init` 裡直接讀自己函式的機器碼：

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/sync_probe.ko; sudo rmmod sync_probe; \
                 sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A9 "atomic_add(1, v)"'
```
```
  -- atomic_add(1, v) @ demo_atomic_add+0x0/0x3c [sync_probe]
     +00: aa1e03e9   -
     +04: d503201f   NOP   <-- alternatives 把 LL/SC 分支改寫成 nop
     +08: 90000020   ADRP
     +12: d503201f   NOP   <-- alternatives 把 LL/SC 分支改寫成 nop   ★ 原本是 b 20
     +16: 52800021   MOVZ
     +20: 91140000   ADD (imm)
     +24: b821001f   STADD  (32-bit)  <-- LSE 原子指令（無回傳值，ST 別名）
     +28: d65f03c0   RET
```

**偏移 +12 的那條，編譯時是 `14000005`（`b`），執行時變成 `d503201f`（`nop`）**
—— 這就是 `apply_alternatives_module()` 幹的事。落到 `stadd` 就直接返回，
底下的 `ldxr/stxr` 迴圈永遠不會被執行到。

```
  CONFIG_ARM64_LSE_ATOMICS = y，system_uses_lse_atomics() = 1
  cpu_have_feature(ARM64_HAS_LSE_ATOMICS) = 1
```
```bash
ssh radxa@$HOST 'dmesg | grep -i "LSE atomic"'
```
```
[   13.093148] CPU features: detected: LSE atomic instructions
```

**(c) 八種原子操作對應到哪條 LSE 指令（全部從記憶體實測）**

| Linux API | 實際執行的指令 | 記憶體序 |
|---|---|---|
| `atomic_add(1, v)` | **`STADD`** | relaxed |
| `atomic_add_return(1, v)` | **`LDADDAL`** | acquire + release |
| `atomic_fetch_add(1, v)` | **`LDADDAL`** | acquire + release |
| `atomic_cmpxchg()` | **`CASAL`** | acquire + release |
| `atomic_cmpxchg_acquire()` | **`CASA`** | acquire |
| `atomic_cmpxchg_release()` | **`CASL`** | release |
| `atomic_cmpxchg_relaxed()` | **`CAS`** | 無 |
| `atomic_try_cmpxchg()` | **`CASAL`** | acquire + release |
| `xchg()` | **`SWPAL`** | acquire + release |

（`atomic_cmpxchg()` 與 `atomic_try_cmpxchg()` 被 GCC 外聯成
`__cmpxchg_case_mb_32.constprop.0`，模組會自動追進去。）

**(d) ★ 反直覺的實測：LSE 不一定比 LL/SC 快**

`lse_bench.c` 把同一份 C 碼編兩次，量原子加的吞吐量（單位 M ops/s）：

```bash
gcc -O2 -pthread -march=armv8-a -mno-outline-atomics -o lse_llsc lse_bench.c
gcc -O2 -pthread -march=armv8.2-a+lse                -o lse_lse  lse_bench.c
./lse_llsc 4 400 0 ; ./lse_lse 4 400 0   # A55
./lse_llsc 4 400 4 ; ./lse_lse 4 400 4   # A76
```

**Cortex-A55（CPU0~3，小核）**

| 場景 | LL/SC | LSE | 誰快 |
|---|---|---|---|
| 單執行緒，共享變數（無爭用） | **119.47** | 66.80 | **LL/SC 1.79×** |
| 4 執行緒搶同一個變數 | **44.05** | 28.17 | **LL/SC 1.56×** |
| 4 執行緒各改各的 cache line | **479.92** | 124.00 | **LL/SC 3.87×** |

**Cortex-A76（CPU4~7，大核）**

| 場景 | LL/SC | LSE | 誰快 |
|---|---|---|---|
| 單執行緒，共享變數（無爭用） | 124.39 | **171.53** | **LSE 1.38×** |
| 4 執行緒搶同一個變數 | **32.74** | 9.30 | **LL/SC 3.52×** |
| 4 執行緒各改各的 cache line | 497.44 | **654.96** | **LSE 1.32×** |

三個結論：
1. **A55 上 LSE 全面較慢**，連改自己獨佔的 cache line 也慢 3.9 倍
   —— A55 是循序執行的小核，它的 LSE 實作把原子操作直接送到互連/L3
   （"far atomic"），不像大核有「在 L1 就地完成」的最佳化。
2. **A76 上無爭用時 LSE 快，但激烈爭用時 LSE 反而慢 3.5 倍**。
   LL/SC 的「不公平」在這裡反而變成吞吐量優勢：一顆 CPU 搶到 cache line
   之後可以連做好幾輪；LSE 每一次都要跟互連打交道，變成完全序列化。
3. 所以**核心選 LSE 不是為了吞吐量，是為了「前進保證（forward progress）」**：
   LL/SC 的重試迴圈在理論上可能活鎖，而且 `stadd` 這種「不需要回傳值」的操作
   用一條指令就能表達，程式碼路徑短、不需要監視器。
   面試時能講出這一層，比背「LSE 比較快」有價值得多。

> 💡 順帶一個坑：Debian 的 GCC 12 **預設開啟 `-moutline-atomics`**，
> 即使指定 `-march=armv8-a` 也只會產生 `bl __aarch64_ldadd8_relax`
> （執行期再依 HWCAP 分派）。要量真正的 LL/SC 必須加 **`-mno-outline-atomics`**。

---

<a name="q2"></a>
## 2. atomic_cmpxchg() 和 atomic_xchg() 分別表示什麼含義？

### 結論

| 函式 | 語意 | 虛擬碼 | ARM64 指令 |
|---|---|---|---|
| `atomic_cmpxchg(v, old, new)` | **有條件**交換：只有目前值 == `old` 才寫入 `new`；**永遠回傳舊值** | `t=*v; if(t==old) *v=new; return t;` | **`CASAL`** |
| `atomic_xchg(v, new)` | **無條件**交換：一定寫入 `new`，回傳舊值 | `t=*v; *v=new; return t;` | **`SWPAL`** |

**怎麼判斷 cmpxchg 成功？** 比較回傳值和 `old`：
```c
if (atomic_cmpxchg(&v, old, new) == old)
        /* 成功 */
```
這是無鎖程式設計最基本的樣板 —— 「讀出來 → 算新值 → CAS 回去 → 失敗就重來」。

**兩者的典型用途**
* `cmpxchg`：無鎖鏈結串列/堆疊、`qspinlock` 的快速通道、`mutex` 的加解鎖、
  參考計數的 `atomic_add_unless()`。
* `xchg`：只要「換掉並拿回舊值」，不在乎舊值是什麼。
  例如 `osq_lock()` 的 `atomic_xchg(&lock->tail, curr)`（見 [Q23](#q23)）、
  qspinlock 的 `xchg_tail()`。

> **書目**：奔跑吧 §1.1.1 第 4 點「原子交換函數」、§1.1.3 第 3 點「xchg() 函數」。

### 實機驗證

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/sync_probe.ko; sudo rmmod sync_probe; \
                 sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A10 "xchg() ——"'
```

`atomic_cmpxchg()`（全屏障版被外聯）：
```
  -- atomic_cmpxchg()  全屏障 @ demo_cmpxchg+0x0/0x20 [sync_probe]
     +20: 97ffffd6   BL   -168   <-- 外呼（全屏障版本被編譯器外聯）
     ↓ 追進外呼目標：
  -- （被外聯的實作） @ __cmpxchg_case_mb_32.constprop.0+0x0/0x50 [sync_probe]
     +28: 88e4fc02   CASAL  (32-bit)  <-- LSE 比較並交換指令
```

`xchg()`：
```
  -- xchg() @ demo_xchg+0x0/0x44 [sync_probe]
     +04: d503201f   NOP   <-- alternatives 把 LL/SC 分支改寫成 nop
     +24: f8e28020   SWPAL  (64-bit)  <-- LSE 交換指令 (xchg)
     +28: d503201f   NOP
     +32: d503201f   NOP
     +36: d503201f   NOP
```

注意 `SWPAL` 後面連續 3 個 `NOP` —— 那是 LL/SC 版本（`ldaxr`/`stlxr`/`cbnz`）
原本佔的位置，被 alternatives 填掉了。**一條 `SWPAL` 取代了四條指令的迴圈。**

---

<a name="q3"></a>
## 3. 在 ARM64 中，CAS 指令包含了加載-獲取和存儲-釋放指令，它們的作用是什麼？

### 結論

CAS 指令用兩個後綴組合出四種記憶體序：

| 指令 | `A`（Load-Acquire） | `L`（Store-Release） | 語意 |
|---|---|---|---|
| `CAS` | ✗ | ✗ | **relaxed**：只保證這條指令本身原子，不管前後怎麼重排 |
| `CASA` | ✓ | ✗ | **acquire**：這條指令**之後**的所有記憶體存取不得被提前到它之前 |
| `CASL` | ✗ | ✓ | **release**：這條指令**之前**的所有記憶體存取不得被延後到它之後 |
| `CASAL` | ✓ | ✓ | 兩者兼具 ≈ 全屏障 |

**為什麼要分這麼細？**

```
        獲取鎖（acquire）                        釋放鎖（release）
   ┌──────────────────────┐              ┌──────────────────────┐
   │  CASA  取得鎖         │              │   臨界區的讀寫        │
   │ ─────────────────    │ ← 不准往上跑  │ ─────────────────    │ ← 不准往下跑
   │  臨界區的讀寫         │              │   CASL  釋放鎖        │
   └──────────────────────┘              └──────────────────────┘
```
* **acquire 只擋一個方向**（後面的不准跑到前面），
  **release 也只擋一個方向**（前面的不准跑到後面）。
* 臨界區「外面」的存取**可以**被搬進臨界區裡 —— 這是安全的，也讓 CPU 有最佳化空間。
* 比起無腦用 `dmb ish` 全屏障，acquire/release **省掉一條屏障指令**，
  而且語意剛好夠用，這是 ARMv8 記憶體模型的核心設計。

**A55/A76 還支援 RCpc（`lrcpc` 特性，dmesg 有 `RCpc load-acquire (LDAPR)`）**，
比傳統的 RCsc（`LDAR`）更寬鬆，效能更好。

> **書目**：奔跑吧 §1.1.3 第 1 點「cas 指令」與表 1.1（第 349~420 行）；
> acquire/release 的完整定義見卷 1 第 1 章。
> **TRM**：ARM ARM v8-A C6.2.40「CAS, CASA, CASAL, CASL」。

### 實機驗證

四種全部在同一次 dmesg 裡抓到（`sync_probe.ko`）：

```
  -- atomic_cmpxchg()          全屏障 → __cmpxchg_case_mb_32
     +28: 88e4fc02   CASAL  (32-bit)     ← A=1, L=1
  -- atomic_cmpxchg_acquire()  取得語意
     +36: 88e47c02   CASA   (32-bit)     ← A=1, L=0
  -- atomic_cmpxchg_release()  釋放語意
     +36: 88a4fc02   CASL   (32-bit)     ← A=0, L=1
  -- atomic_cmpxchg_relaxed()  無屏障
     +36: 88a47c02   CAS    (32-bit)     ← A=0, L=0
```

把四個編碼攤開來看，**只差兩個位元**：

```
CASAL = 0x88e4fc02   ...1000 1000 111 0 0100 1111 1100 0000 0010
CASA  = 0x88e47c02   ...1000 1000 111 0 0100 0111 1100 0000 0010
CASL  = 0x88a4fc02   ...1000 1000 101 0 0100 1111 1100 0000 0010
CAS   = 0x88a47c02   ...1000 1000 101 0 0100 0111 1100 0000 0010
                                   ↑                ↑
                              bit22 = L        bit15 = o0
                          (Load-Acquire)   (Store-Release)
```

* `0x88e4` vs `0x88a4`：差 `0x40` = **bit22（L，acquire）**
* `0xfc02` vs `0x7c02`：差 `0x8000` = **bit15（o0，release）**

**這正是 ARM ARM 上 CAS 的編碼欄位定義，在實機上一個位元不差。**

---

<a name="q4"></a>
## 4. atomic_try_cmpxchg() 函數和 atomic_cmpxchg() 函數有什麼區別？

### 結論

**底層是同一條 `CASAL` 指令，差別只在「回傳值語意」和「順便更新 old」。**

```c
/* include/linux/atomic/atomic-instrumented.h（概念展開） */
#define __atomic_try_cmpxchg(type, _p, _po, _n)			\
({								\
	typeof(_po) __po = (_po);				\
	typeof(*(_po)) __r, __o = *__po;			\
	__r = atomic_cmpxchg##type((_p), __o, (_n));		\
	if (unlikely(__r != __o))				\
		*__po = __r;		/* ★ 失敗時把「真正的現值」寫回去 */ \
	likely(__r == __o);		/* ★ 回傳 bool */	\
})
```

| | `atomic_cmpxchg(v, old, new)` | `atomic_try_cmpxchg(v, &old, new)` |
|---|---|---|
| 第二個參數 | **值** | **指標** |
| 回傳 | **舊值**（`int`） | **成功與否**（`bool`） |
| 失敗時 | 呼叫者自己拿回傳值當新的 old | **自動把現值寫進 `*old`** |
| 典型寫法 | `while ((o = cmpxchg(p,o,n)) != o) …` | `do { … } while (!try_cmpxchg(p,&o,n));` |

**兩個實際好處：**
1. **少一次記憶體讀取**。重試迴圈裡不用再 `atomic_read()` 一次，
   因為失敗時 CAS 已經把現值給你了。
2. **編譯器最佳化更好**。在 x86 上 `try_cmpxchg` 可以直接用 `CMPXCHG` 之後的
   `ZF` 旗標分支，不必再比較一次。

**⚠ 最常見的誤解**（書上特別點名）：
> 「如果讀者使用 `cmpxchg()` 的語義去理解它，會得到錯誤的結論。」

`atomic_cmpxchg()` 回傳 0 可能代表「舊值就是 0」（成功也可能失敗）；
而 `atomic_try_cmpxchg()` 回傳 0 一定代表**失敗**。搞混會寫出反過來的邏輯。

**誰在用？** `qspinlock` 的快速通道、`mutex` 的 `__mutex_trylock_fast()`、
`rwsem`、`osq_lock`。以 qspinlock 為例：

```c
/* include/asm-generic/qspinlock.h */
static __always_inline void queued_spin_lock(struct qspinlock *lock)
{
	int val = 0;
	if (likely(atomic_try_cmpxchg_acquire(&lock->val, &val, _Q_LOCKED_VAL)))
		return;			/* val 還是 0 → 沒人持有 → 拿到了 */
	queued_spin_lock_slowpath(lock, val);	/* ★ val 已經被填成「現在的鎖狀態」 */
}
```
**注意最後一行**：`val` 被 `try_cmpxchg` 自動更新成當下的鎖狀態，
直接傳給慢速通道用，省掉一次讀取 —— 這就是為什麼 qspinlock 要用 `try_` 版本。

> **書目**：奔跑吧 §1.1.3 第 2 點末段（第 600~630 行）與 §1.5.1。

### 實機驗證

```
  -- atomic_try_cmpxchg() @ demo_try_cmpxchg+0x0/0x44 [sync_probe]
     +24: b9400014   -                （ldr w20, [x0]  ← 讀出 *old）
     +32: 97ffffe4   BL   -112        （外呼 __cmpxchg_case_mb_32）
     +36: 6b00029f   -                （cmp w20, w0    ← 比較舊值）
     +40: 54000040   -                （b.eq …         ← 相等就跳過）
     +44: b9000260   -                （str w0, [x19]  ← ★ 不相等才寫回 *old）
     +48: 6b00029f   -                （cmp w20, w0    ← 產生 bool 回傳值）
     ↓ 追進外呼目標：
  -- （被外聯的實作） @ __cmpxchg_case_mb_32.constprop.0
     +28: 88e4fc02   CASAL  (32-bit)  <-- LSE 比較並交換指令
```

**底層一模一樣是 `CASAL`**；外面多包的那幾條 `cmp/b.eq/str/cmp`
就是巨集裡「失敗才回寫 `*old`、回傳 bool」的那三行 C。

---

<a name="q5"></a>
## 5. cmpxchg_acquire()、cmpxchg_release()、cmpxchg_relaxed() 以及 cmpxchg() 的區別是什麼？

### 結論

**功能完全相同（比較並交換），差別只在附帶的記憶體屏障。**

| 函式 | 隱含屏障 | ARM64 指令 | 什麼時候用 |
|---|---|---|---|
| `cmpxchg_relaxed()` | **無** | `CAS` | 只要原子性，順序由外面另外的屏障保證；或純計數器 |
| `cmpxchg_acquire()` | 加載-獲取 | `CASA` | **拿鎖**：確保臨界區的存取不會被提前到拿鎖之前 |
| `cmpxchg_release()` | 存儲-釋放 | `CASL` | **放鎖**：確保臨界區的存取不會被延後到放鎖之後 |
| `cmpxchg()` | 兩者皆有 | `CASAL` | 不確定要哪種、或真的需要全序 |

**成本排序**：`relaxed` < `acquire` ≈ `release` < 全屏障。
在 ARM64 上四者都是**單一指令**，差別只是指令內部的記憶體序旗標，
所以額外成本很低 —— 但在爭用激烈時仍然值得挑最寬鬆的那個。

**核心的實際用法對照**

```c
/* 拿鎖用 acquire */
atomic_try_cmpxchg_acquire(&lock->val, &val, _Q_LOCKED_VAL)   /* qspinlock */

/* 放鎖用 release */
atomic_long_cmpxchg_release(&lock->owner, curr, 0UL)          /* mutex_unlock 快速通道 */

/* 只要原子性，順序另外處理 */
atomic_cmpxchg_relaxed(&lock->tail, ...)                      /* osq/qspinlock 內部 */
```

**同一族還有其他成員**：`xchg_acquire/release/relaxed`、
`atomic_add_return_acquire/release/relaxed`、`atomic_fetch_*_acquire/…`。
規則一致：`_relaxed` 無屏障、`_acquire` 加載側、`_release` 儲存側、無後綴 = 全都要。

> **書目**：奔跑吧 §1.1.1 第 6 點「內嵌內存屏障原語的原子操作函數」、
> §1.1.3 表 1.2（第 620~640 行）。

### 實機驗證

同一支 `sync_probe.ko` 一次印出四種（節錄）：

```
  -- atomic_cmpxchg()          全屏障 → CASAL   (0x88e4fc02)
  -- atomic_cmpxchg_acquire()  取得語意 → CASA  (0x88e47c02)
  -- atomic_cmpxchg_release()  釋放語意 → CASL  (0x88a4fc02)
  -- atomic_cmpxchg_relaxed()  無屏障   → CAS   (0x88a47c02)
```

看完整的函式主體會發現**四者的指令數幾乎一樣**：

```
  -- atomic_cmpxchg_relaxed() @ demo_cmpxchg_relaxed+0x0/0x58
     +00: aa1e03e9   -
     +04: d503201f   NOP        <-- alternatives
     +08: 90000023   ADRP
     +12: 91140063   ADD (imm)
     +16: d503201f   NOP        <-- alternatives
     +20: 91001060   ADD (imm)
     +24: 52800001   MOVZ       （w1 = old = 0）
     +28: 52800022   MOVZ       （w2 = new = 1）
     +32: 2a0103e4   -          （mov w4, w1）
     +36: 88a47c02   CAS        ★ 只有這一條不同
     +40: 2a0403e0   -          （mov w0, w4）
     +44: d65f03c0   RET
```

**除了第 +36 條指令的兩個位元之外，四個版本的程式碼完全相同** ——
這就是「差別只在記憶體序」最直白的證據。
（`cmpxchg()` 全屏障版被 GCC 外聯成獨立函式，是編譯器的決定，不是語意差異。）

---

<a name="q6"></a>
## 6. 請舉例說明內核使用內存屏障的場景。

### 結論

**ARM64 是弱記憶體序（weakly-ordered）架構**，CPU 和編譯器都可以重排存取。
屏障就是告訴它們「這裡不准跨過去」。

**Linux 的屏障 API → ARM64 實際指令（本機實測）**

| API | ARM64 展開 | 作用 |
|---|---|---|
| `barrier()` | *（無指令）* | 只擋**編譯器**重排，`asm volatile("":::"memory")` |
| `smp_mb()` | **`dmb ish`** | SMP 全屏障（inner-shareable） |
| `smp_rmb()` | **`dmb ishld`** | SMP 讀屏障 |
| `smp_wmb()` | **`dmb ishst`** | SMP 寫屏障 |
| `mb()` | **`dsb sy`** | 全系統屏障，**含裝置存取**，比 dmb 更強 |
| `rmb()` | `dsb ld` | |
| `wmb()` | `dsb st` | |
| `dma_rmb()` | `dmb oshld` | DMA 用，outer-shareable |
| `dma_wmb()` | **`dmb oshst`** | DMA 用 |
| `smp_load_acquire()` | `ldar` / `ldapr` | 單向屏障 |
| `smp_store_release()` | `stlr` | 單向屏障 |
| ~~`smp_read_barrier_depends()`~~ | — | **5.9 已刪除** |

`dmb` vs `dsb`：`dmb` 只保證**記憶體存取**的順序；`dsb` 還要等所有先前的指令
（含 cache/TLB 維護）**完成**才往下走，比較貴。

**四個典型場景**

**① 驅動程式對硬體下命令（書上例 1.1）**
```c
/* drivers/net/ethernet/realtek/8139too.c */
skb_copy_and_csum_dev(skb, tp->tx_buf[entry]);
wmb();                              /* 保證資料真的寫進緩衝區了 */
RTL_W32_F(TxStatus0 + ..., ...);    /* 才通知 DMA 引擎開始傳送 */
```
沒有 `wmb()`，DMA 可能讀到還沒寫完的緩衝區。

**② 睡眠 / 喚醒（書上例 1.2）**
```
CPU1（睡眠者）                          CPU2（喚醒者）
set_current_state(TASK_UNINTERRUPTIBLE) STORE event_indicated = 1
   STORE current->__state                  wake_up()
   <smp_mb()>                                <smp_wmb()>
   LOAD event_indicated                     STORE p->__state = TASK_RUNNING
if (event_indicated) break;
schedule();
```
兩邊各插一個屏障，才能避免「我已經設成睡眠態、但沒看到事件」與
「事件設好了、但對方還沒進睡眠態」同時發生 → **漏喚醒（lost wakeup）**。
`set_current_state()` 就是 `smp_store_mb()`，內含 `WRITE_ONCE()` + `smp_mb()`。

**③ 發布指標（RCU / 無鎖資料結構）**
```c
obj->field = value;
smp_wmb();                       /* 或 rcu_assign_pointer() 內含的 release */
WRITE_ONCE(global_ptr, obj);     /* 讀者看到指標時，field 一定已經寫好了 */
```

**④ 鎖的實作**：見 [Q3](#q3)、[Q5](#q5)，acquire/release 就是單向屏障。

> **書目**：奔跑吧 §1.2.1「經典內存屏障接口函數」與表 1.3（第 650~790 行）。
> **原始碼**：`arch/arm64/include/asm/barrier.h`。

### 實機驗證

`sync_probe.ko` 把每個屏障巨集包在一個 `noinline` 函式裡，再把機器碼讀出來：

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/sync_probe.ko; sudo rmmod sync_probe; \
                 sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A4 "smp_mb() @"'
```

```
  -- smp_mb() @ demo_smp_mb+0x0/0x10 [sync_probe]
     +08: d5033bbf   DMB ish     <-- 記憶體屏障
  -- smp_rmb() @ demo_smp_rmb+0x0/0x10 [sync_probe]
     +08: d50339bf   DMB ishld   <-- 記憶體屏障
  -- smp_wmb() @ demo_smp_wmb+0x0/0x10 [sync_probe]
     +08: d5033abf   DMB ishst   <-- 記憶體屏障
  -- mb() @ demo_mb+0x0/0x10 [sync_probe]
     +08: d5033f9f   DSB sy      <-- 記憶體屏障
  -- dma_wmb() @ demo_dma_wmb+0x0/0x10 [sync_probe]
     +08: d50332bf   DMB oshst   <-- 記憶體屏障
```

四條編碼攤開來看，差別只在 **CRm（共享域）與 opc2（DMB/DSB/ISB）**：

```
DMB ish   = 0xd5033bbf   ...0011 1011 1011 1111   CRm=1011(11=ish)   op2=101(DMB)
DMB ishld = 0xd50339bf   ...0011 1001 1011 1111   CRm=1001( 9=ishld) op2=101
DMB ishst = 0xd5033abf   ...0011 1010 1011 1111   CRm=1010(10=ishst) op2=101
DMB oshst = 0xd50332bf   ...0011 0010 1011 1111   CRm=0010( 2=oshst) op2=101
DSB sy    = 0xd5033f9f   ...0011 1111 1001 1111   CRm=1111(15=sy)    op2=100(DSB)
```

**「inner-shareable」對應到 CRm=11**，正好是 SMP 系統中所有 CPU 共享的域；
`dma_wmb()` 用 **outer-shareable（CRm=2）**，因為 DMA 引擎在 CPU 的 inner 域之外。
這解釋了為什麼 SMP 屏障用 `ish` 而 DMA 屏障要用 `osh` —— **範圍越大越慢，所以只取夠用的**。

---

<a name="q7"></a>
## 7. smp_cond_load_relaxed() 函數的作用和使用場景是什麼？

### 結論

**作用：原子地反覆讀取某個位址，直到條件成立為止 —— 一個「自旋等待」原語。**

```c
/* include/asm-generic/barrier.h（泛型版本） */
#define smp_cond_load_relaxed(ptr, cond_expr) ({	\
	typeof(ptr) __PTR = (ptr);			\
	__unqual_scalar_typeof(*ptr) VAL;		\
	for (;;) {					\
		VAL = READ_ONCE(*__PTR);		\
		if (cond_expr)				\
			break;				\
		cpu_relax();				\
	}						\
	(typeof(*ptr))VAL;				\
})
```

`smp_cond_load_acquire()` 只是在跳出迴圈後多一道 `smp_acquire__after_ctrl_dep()`。

**使用場景**：所有「等一個變數變成某個值」的地方，最主要是**排隊自旋鎖**：

```c
/* kernel/locking/qspinlock.c */
/* 等前一個節點把 locked 設成 1（輪到我了） */
arch_mcs_spin_lock_contended(&node->locked);
	→ smp_cond_load_acquire(&node->locked, VAL)

/* 等 pending 和 locked 都清掉 */
val = atomic_cond_read_relaxed(&lock->val, !(VAL & _Q_LOCKED_PENDING_MASK));
```

### ★ ARM64 的關鍵最佳化：不是忙等，是 WFE 睡眠

**arm64 覆寫了泛型版本**，`cpu_relax()` 被換成 `__cmpwait_relaxed()`：

```asm
/* arch/arm64/include/asm/cmpxchg.h:232 __CMPWAIT_CASE */
	sevl                      // 先送一次「本地事件」，避免錯過喚醒
	wfe                       // 吃掉剛才那個事件（清掉 event register）
	ldxr    tmp, [ptr]        // ★ 獨佔載入 —— 把這個位址設成「獨佔監視中」
	eor     tmp, tmp, val     // 值變了嗎？
	cbnz    tmp, 1f           // 變了 -> 直接跳出去重新判斷
	wfe                       // 沒變 -> 真正進入低功耗等待
1:
```

**為什麼要這樣寫？**
1. `ldxr` 把該位址掛上獨佔監視器，**別的 CPU 一寫入這個位址，
   監視器失效就會自動產生一個 WFE 喚醒事件** —— 不需要別人明確送 `SEV`。
2. `wfe` 期間 CPU 進入低功耗狀態，**不佔用記憶體頻寬、不打 cache line**。
   純忙等（`cpu_relax()`）會一直讀取共享變數，加劇 cache line 顛簸。
3. 開頭的 `sevl` + 第一個 `wfe` 是為了**清掉可能殘留的事件**，
   避免「事件早就來了但我還沒進 wfe」造成的錯過。

這與書上 §1.3.1 分析舊版 ticket spinlock 時提到的
「`sevl`/`wfe`，SEV 指令唤醒其他 CPU」是同一套硬體機制。

> **書目**：奔跑吧 §1.2.2 第 1 點「自旋等待的接口函數」（第 788~840 行）。

### 實機驗證

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/sync_probe.ko; sudo rmmod sync_probe; \
                 sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A16 "smp_cond_load_relaxed(ptr"'
```

```
  -- smp_cond_load_relaxed(ptr, VAL != 0) @ demo_cond_load_relaxed+0x0/0x50 [sync_probe]
     +00: aa1e03e9   -
     +04: d503201f   NOP   <-- alternatives 把 LL/SC 分支改寫成 nop
     +08: 90000021   ADRP
     +12: 91028021   ADD (imm)
     +16: f9400420   -
     +20: b5000120   -
     +24: 91002023   ADD (imm)
     +28: d50320bf   SEVL  <-- 先送一次本地事件，避免錯過喚醒
     +32: d503205f   WFE   <-- 進入低功耗等待，被 SEV/獨佔監視器喚醒
     +36: c85f7c62   -
     +40: ca000042   -
     +44: b5000042   -
     +48: d503205f   WFE   <-- 進入低功耗等待，被 SEV/獨佔監視器喚醒
     +52: 17fffff7   B    -36
     +56: d65f03c0   RET
     HEX: aa1e03e9 d503201f 90000021 91028021 f9400420 b5000120 91002023 d50320bf d503205f c85f7c62 ca000042 b5000042
```

模組內建的迷你解碼器只認得本章關心的那幾條指令，其餘印成 `-`。
把上面那串 HEX 餵給機台上的 `objdump` 做**權威交叉驗證**：

```bash
ssh radxa@$HOST 'cd /tmp
python3 - <<EOF > cond.bin
import sys,struct
words=[0xaa1e03e9,0xd503201f,0x90000021,0x91028021,0xf9400420,0xb5000120,0x91002023,
       0xd50320bf,0xd503205f,0xc85f7c62,0xca000042,0xb5000042,0xd503205f,0x17fffff7,0xd65f03c0]
sys.stdout.buffer.write(b"".join(struct.pack("<I",w) for w in words))
EOF
objdump -D -b binary -m aarch64 cond.bin | tail -16'
```

```
   0:	aa1e03e9 	mov	x9, x30
   4:	d503201f 	nop
   8:	90000021 	adrp	x1, 0x4000
   c:	91028021 	add	x1, x1, #0xa0
  10:	f9400420 	ldr	x0, [x1, #8]      ← VAL = READ_ONCE(*__PTR)
  14:	b5000120 	cbnz	x0, 0x38          ← if (cond_expr) break  → ret
  18:	91002023 	add	x3, x1, #0x8
  1c:	d50320bf 	sevl                      ┐
  20:	d503205f 	wfe                       │
  24:	c85f7c62 	ldxr	x2, [x3]          │ ★ __cmpwait_relaxed()
  28:	ca000042 	eor	x2, x2, x0        │   （arch/arm64/include/asm/
  2c:	b5000042 	cbnz	x2, 0x34          │     cmpxchg.h:238）
  30:	d503205f 	wfe                       ┘
  34:	17fffff7 	b	0x10              ← 回去重讀
  38:	d65f03c0 	ret
```

**逐行對應巨集：**

```c
for (;;) {
	VAL = READ_ONCE(*__PTR);        /* ldr  x0, [x1, #8]  */
	if (cond_expr)                  /* cbnz x0, 0x38      */
		break;
	__cmpwait_relaxed(__PTR, VAL);  /* sevl/wfe/ldxr/eor/cbnz/wfe */
}                                       /* b 0x10             */
```

**`ldxr x2, [x3]` 是整段的關鍵**：它把 `ptr` 掛上獨佔監視器，
之後只要**任何一顆 CPU 寫入這個位址**，監視器失效就會自動產生 WFE 喚醒事件。
所以這段程式碼「等待期間完全不讀記憶體、不打 cache line、CPU 進低功耗」，
和單純的 `cpu_relax()` 忙等是本質上的差別。

**`smp_cond_load_acquire()` 的差別 —— 不是「多一條屏障」，而是換一條載入指令：**

```bash
ssh radxa@$HOST 'sudo dmesg | grep -A17 "smp_cond_load_acquire(ptr"'
# 同樣把 HEX 餵給 objdump：
```
```
   0:	aa1e03e9 	mov	x9, x30
   4:	d503201f 	nop
   8:	90000021 	adrp	x1, 0x4000
   c:	91028021 	add	x1, x1, #0xa0
  10:	91002020 	add	x0, x1, #0x8
  14:	c8dffc00 	ldar	x0, [x0]          ← ★ LDAR（Load-Acquire），不是 LDR
  18:	b5000120 	cbnz	x0, 0x3c
  1c:	91002023 	add	x3, x1, #0x8
  20:	d50320bf 	sevl                      ┐
  24:	d503205f 	wfe                       │
  28:	c85f7c62 	ldxr	x2, [x3]          │ __cmpwait_relaxed()
  2c:	ca000042 	eor	x2, x2, x0        │ （與 relaxed 版一模一樣）
  30:	b5000042 	cbnz	x2, 0x38          │
  34:	d503205f 	wfe                       ┘
  38:	17fffff6 	b	0x10
  3c:	d65f03c0 	ret
```

**兩個版本唯一的差別是第 `0x10~0x14` 這兩條**：
`ldr x0,[x1,#8]` → `add x0,x1,#8` + **`ldar x0,[x0]`**。

原因是 **arm64 自己覆寫了 `smp_cond_load_acquire()`**，
沒有用泛型版本「relaxed 迴圈 + 跳出後補 `smp_acquire__after_ctrl_dep()`」的寫法：

```c
/* arch/arm64/include/asm/barrier.h:199 */
#define smp_cond_load_acquire(ptr, cond_expr)				({										typeof(ptr) __PTR = (ptr);						__unqual_scalar_typeof(*ptr) VAL;					for (;;) {									VAL = smp_load_acquire(__PTR);   /* ★ 直接用 LDAR */			if (cond_expr)									break;								__cmpwait_relaxed(__PTR, VAL);					}									(typeof(*ptr))VAL;						})
```

**為什麼這樣比較好？** `LDAR` 一條指令就同時完成「載入 + acquire 屏障」，
比「`LDR` + `DMB ISHLD`」少一條指令、也不會在迴圈外多付一次屏障成本。
這是 ARM64 acquire/release 指令集相對於「載入 + 獨立屏障」的直接好處
（見 [Q3](#q3)）。

---

<a name="q8"></a>
## 8. smp_mb__before_atomic() 和 smp_mb__after_atomic() 的作用和使用場景是什麼？

### 結論

**它們是給「不回傳值的原子操作」補屏障用的。**

關鍵前提：
* **回傳值的原子操作**（`atomic_add_return`、`atomic_fetch_add`、`atomic_cmpxchg`…）
  **自帶全屏障** —— 在 ARM64 上就是 `LDADDAL` / `CASAL` 的 `AL` 後綴。
* **不回傳值的原子操作**（`atomic_inc`、`atomic_dec`、`atomic_add`、`atomic_and`…）
  **完全沒有屏障** —— 就是一條 `STADD`。

所以當你需要「這個原子操作前/後的存取不得跨過去」時，要自己補：

```c
void smp_mb__before_atomic(void);   /* 在不回傳值的原子操作「之前」插全屏障 */
void smp_mb__after_atomic(void);    /* 在「之後」插 */
```

**經典場景：釋放物件前的參考計數遞減**（書上原例）

```c
obj->dead = 1;
smp_mb__before_atomic();          /* 保證 dead=1 對其他 CPU 可見 */
atomic_dec(&obj->ref_count);      /* 才遞減計數 */
```
沒有這個屏障，別的 CPU 可能先看到 `ref_count` 歸零、把物件釋放掉，
卻還沒看到 `dead = 1`。

**另一個場景：位元操作**
```c
set_bit(NAPI_STATE_SCHED, &n->state);
smp_mb__after_atomic();           /* set_bit 也是不回傳值的原子操作 */
if (test_bit(...)) ...
```

**為什麼不直接寫 `smp_mb()`？**
在某些架構上（如 x86），原子操作本身已經是全屏障，
`smp_mb__before_atomic()` 可以定義成 `barrier()`（零成本）。
**這兩個巨集是「架構可以最佳化掉」的屏障**，比硬寫 `smp_mb()` 更有彈性。
ARM64 上兩者都定義成 `smp_mb()`（`include/asm-generic/barrier.h` 的預設值）。

> **書目**：奔跑吧 §1.2.2 第 2 點「原子變量接口函數」（第 840~853 行）。

### 實機驗證

```
  -- smp_mb__before_atomic() + atomic_dec() @ demo_mb_before_atomic+0x0/0x44
     +00: aa1e03e9   -
     +04: d503201f   NOP   <-- alternatives 把 LL/SC 分支改寫成 nop
     +08: d5033bbf   DMB ish   <-- ★ smp_mb__before_atomic() 展開成的全屏障
     +12: 90000020   ADRP
     +16: d503201f   NOP   <-- alternatives 把 LL/SC 分支改寫成 nop
     +20: 12800001   -              （mov w1, #-1  —— dec 就是 add -1）
     +24: 91150000   ADD (imm)
     +28: b821001f   STADD  (32-bit)  <-- ★ atomic_dec() 本身，完全沒有屏障
     +32: d65f03c0   RET
```

**一眼就看得出來**：`atomic_dec()` 只有一條光禿禿的 `STADD`（沒有 `A`/`L` 後綴），
它前面那條 `DMB ish` 完完全全是 `smp_mb__before_atomic()` 加上去的。

對照 [Q1](#q1) 的 `atomic_add_return()`：

```
  -- atomic_add_return(1, v)
     +24: b8e00020   LDADDAL  (32-bit)  <-- ★ 自帶 A 和 L，不需要額外屏障
```

**`STADD`（無屏障）vs `LDADDAL`（自帶全屏障）—— 這就是為什麼只有前者需要
`smp_mb__before/after_atomic()`。**

---

<a name="q9"></a>
## 9. 為什麼自旋鎖的臨界區不能睡眠（不考慮 RT-Linux 的情況）？

### 結論

**四個理由，一個比一個致命：**

**① 會死鎖。** 睡眠 → 調度器換上另一個行程 → 那個行程也去搶同一把鎖 →
它在自旋等待，而持有者在睡覺永遠不會醒來 → **兩邊卡死**。
單核系統上這是必然發生；多核上只要爭用夠激烈就會發生。

**② 自旋鎖不記錄「持有者」，也沒有優先級繼承。**
`mutex` 有 `owner` 欄位，所以能做樂觀自旋、能做優先級繼承（rt_mutex）。
自旋鎖的 `arch_spinlock_t` 只有 4 個位元組，**根本不知道誰持有它**，
所以無法在持有者睡著時做任何補救。

**③ 中斷上下文不能睡眠，而自旋鎖就是設計來給中斷上下文用的。**
自旋鎖是唯一能在硬中斷/軟中斷處理常式裡使用的互斥原語。
一旦在臨界區睡眠，這個前提就崩了。

**④ `schedule()` 會無條件把中斷打開。**
（卷1 第9章 Q13 已經實測過：關中斷後呼叫 `schedule()`，
回來時 `irqs_disabled()` 從 128 變成 0。）
所以在 `spin_lock_irqsave()` 的臨界區裡睡眠，
**你以為關著的中斷其實被打開了**，`flags` 也失去意義 —— 語意徹底毀壞。

**常見的「不小心睡著」**（面試愛問）：

| 動作 | 為什麼會睡 | 安全替代 |
|---|---|---|
| `kmalloc(size, GFP_KERNEL)` | 記憶體不足時會回收/等待 | `GFP_ATOMIC` |
| `copy_from_user()` / `copy_to_user()` | 可能觸發缺頁 | 先在臨界區外複製 |
| `mutex_lock()` / `down()` | 本來就會睡 | — |
| `msleep()` / `schedule_timeout()` | 明顯 | `udelay()`（但別太久） |
| `printk()` 大量輸出 | 可能等 console | 減量 |
| `vmalloc()` / `alloc_page(GFP_KERNEL)` | 同 kmalloc | — |

### 實機驗證 —— 我自己寫出來的死鎖

寫這一章的實驗模組時，我第一版的 `lock_bench.c` 有一個「起跑柵欄」：

```c
static int bench_thread(void *arg)
{
	while (!atomic_read(&go))     /* 等 insmod 把 go 設成 1 */
		cpu_relax();          /* ★ 忙等，沒有任何 cond_resched() */
	...
}

static void run_one_bench(int which, int nthr)
{
	for (i = 0; i < nthr; i++) {
		thr[i] = kthread_create(bench_thread, (void *)i, "lb%ld", i);
		kthread_bind(thr[i], i % num_online_cpus());   /* ★ 綁滿全部 8 顆 */
		wake_up_process(thr[i]);
	}
	msleep(20);
	atomic_set(&go, 1);           /* ★ 只有 insmod 這個行程能設 */
	...
}
```

用 `nthread=8` 跑下去，**整台機器立刻失去回應，SSH 斷線，必須按實體電源鍵重開**。

**為什麼？**

```
8 顆 CPU 上各有一個 kthread 在 while (!go) cpu_relax(); 忙等
                    ↓
本機 CONFIG_PREEMPT_VOLUNTARY（核心態不可搶佔）
+ 這個迴圈裡沒有 cond_resched()
                    ↓
這些 kthread 永遠不會讓出 CPU
                    ↓
insmod 行程（msleep 醒來後要設 go=1）永遠排不到任何一顆 CPU
                    ↓
go 永遠是 0 → 8 個 kthread 永遠忙等 → 完全死鎖
```

**這正是本題的答案在真實世界的樣子**：自旋等待的程式碼路徑
**必須保證「它等的那個條件，有辦法在它不讓出 CPU 的前提下被滿足」**。
自旋鎖能成立，是因為持有者保證會很快釋放；
一旦持有者睡著（或像這裡根本排不上），自旋方就永遠等下去。

修正後的版本（現在 repo 裡的）做了兩件事：
```c
/* 1. 徹底移除忙等柵欄，每個執行緒自己算結束時間 */
end = jiffies + msecs_to_jiffies(ms);

/* 2. 綁定時保留最後一顆 CPU 給系統 */
kthread_bind(thr[i], i % max(1, (int)num_online_cpus() - 1));
```

再加上迴圈裡每 256 次就 `cond_resched()` 一次
—— 在不可搶佔核心上，**`cond_resched()` 是唯一的搶佔點**（見卷1 第9章 Q12）。

---

<a name="q10"></a>
## 10. Linux 內核中經典自旋鎖的實現有什麼缺點？

### 結論

書上 §1.3 描述了三代自旋鎖的演進，每一代都是為了修掉前一代的缺點：

**第一代：簡單的 0/1 變數（2.6.25 之前）**
* 缺點 ①：**不公平**。剛放鎖的 CPU 因為 cache line 還在自己的 L1 裡，
  幾乎一定會再搶到；等最久的 CPU 反而拿不到。
  書上引用的測試：8 核系統上有些執行緒要試 **1000000 次**才拿得到鎖。
* 缺點 ②：**沒有 FIFO 保證**，最壞情況下會餓死。

**第二代：ticket spinlock（2.6.25 起，書上 §1.3.1 分析的版本）**
```
        ┌──────────────┬──────────────┐
slock   │  next (16位)  │ owner (16位) │      像餐廳排隊叫號
        └──────────────┴──────────────┘
```
* 解決了公平性（嚴格 FIFO）。
* 缺點：**cache line 顛簸（cacheline bouncing）依然存在**。
  所有等待者都在**同一個** `slock` 變數上自旋，持有者一放鎖，
  MESI 協定就要把這條 cache line 廣播失效給所有等待的 CPU，
  N 個等待者 = N 次 cache line 傳輸。**爭用越激烈，效能掉得越兇，且隨 CPU 數線性惡化。**

**第三代：qspinlock（4.2 起，本機用的）**
* 用 MCS 演算法：**每個等待者只在自己的 per-CPU 節點上自旋**，
  不碰共享變數 → 幾乎沒有 cache line 顛簸。
* 鎖本體仍然只有 4 個位元組（見 [Q15](#q15)、[Q23](#q23)）。
* Waiman Long 的測試：雙插槽機器上比 ticket 版快 20%，
  檔案系統測試場景快 116%。

> **書目**：奔跑吧 §1.3.1（第 871~1050 行）與 §1.5 開頭（第 1397 行起）。
> **修正**：書上 §1.3.1 分析的 `ldaxr/stxr` + `wfe` 的 ticket 實作是
> **Linux 4.0** 的程式碼，本機 6.1 早就換成 qspinlock 了。

### 實機驗證 —— 爭用的代價有多大

`lock_bench.ko mode=1`（4 執行緒綁在 CPU0~3，臨界區只有 `bl_shared++`）：

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/lock_bench.ko mode=1 nthread=4 ms=100; \
                 sudo rmmod lock_bench; sudo dmesg | grep 執行緒'
```

| 鎖 | 1 執行緒（無爭用） | 4 執行緒（有爭用） | **掉了幾倍** |
|---|---|---|---|
| **spinlock** | 6086 次/ms | **559** 次/ms | **10.9×** |
| mutex | 4284 | 1483 | 2.9× |
| rwsem（讀） | 5803 | 2307 | 2.5× |
| rwsem（寫） | 7286 | 1976 | 3.7× |
| atomic_inc | 14577 ~ 66572 †| 7678 | — |
| **rcu_read_lock** | 61531 | **116990** | **反而變快 1.9×** |

（單位：每毫秒每執行緒完成的「拿鎖→改資料→放鎖」次數；已固定 `performance` governor。
† `atomic_inc` 單執行緒呈雙峰分布，三次量到 14577/14627/66572，與模組載入時
`bl_atomic` 落在哪條 cache line 有關；4 執行緒的數字則非常穩定。）

**三個觀察：**

1. **自旋鎖是掉最兇的**（10.9 倍）—— 因為它是唯一「所有人都在等同一個東西
   且完全不讓出 CPU」的機制。4 個 CPU 的總吞吐量從 608690 掉到 223656，
   **總量都掉了 2.7 倍**，也就是說多加 3 顆 CPU 不但沒有變快，反而更慢。
   這就是 cache line 顛簸的代價。
2. **會睡眠的鎖（mutex/rwsem）掉得比較少**，因為等待者去睡覺了，
   不會一直打同一條 cache line。
3. **只有 RCU 讀者是完美擴展的** —— 4 執行緒每執行緒的吞吐量比單執行緒還高
   （116990 vs 61531，因為 4 顆 CPU 同時跑但彼此完全不干擾）。
   這正是 [Q27](#q27) 的答案。

**一個延伸的實測**（來自 [Q1](#q1) 的使用者態測試）：
```
4 執行緒搶同一個變數      : 44.05 M ops/s
4 執行緒各改各的 cache line: 479.92 M ops/s     ← 差 10.9 倍
```
**同樣是 4 個執行緒做原子加，只差在「有沒有搶同一條 cache line」，
吞吐量就差 10.9 倍** —— 這個數字直接量化了書上說的「高速緩存行顛簸」。

---

<a name="q11"></a>
## 11. 為什麼自旋鎖的臨界區不允許發生搶佔？

### 結論

**書上給的兩個理由：**
1. 搶佔會導致持有鎖的行程被換下 CPU（等同於「睡眠」），
   違背「自旋鎖持有者必須很快執行完」的設計語意。
2. 搶佔進來的行程可能也去申請同一把自旋鎖 → **死鎖**（同 [Q9](#q9)）。

**所以 `spin_lock()` 第一件事就是關搶佔：**
```c
static inline void __raw_spin_lock(raw_spinlock_t *lock)
{
	preempt_disable();                                    /* ★ */
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}
```

`preempt_disable()` 遞增 `preempt_count`，`preempt_enable()` 遞減並在歸零時
檢查 `TIF_NEED_RESCHED`。`preempt_count` 的欄位佈局：

```
 bit 0- 7  PREEMPT     preempt_disable() 的巢狀次數
 bit 8-15  SOFTIRQ     軟中斷上下文
 bit16-19  HARDIRQ     硬中斷上下文
 bit 20    NMI
 bit 31    PREEMPT_NEED_RESCHED（反相）
```

### ★ 實機驗證 —— 本機的答案不太一樣

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/sync_probe.ko; sudo rmmod sync_probe; \
                 sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A12 "Q9/Q11/Q13"'
```

```
== Q9/Q11/Q13：自旋鎖的搶佔與中斷語意（本機實測） ==
  CONFIG_PREEMPT_COUNT = n      <-- ★ 決定 preempt_disable() 有沒有作用
  CONFIG_PREEMPTION    = n
  CONFIG_DEBUG_SPINLOCK= y
  CONFIG_LOCKDEP       = n

  (a) spin_lock() / spin_unlock()
      preempt_count : 前=0x0  臨界區內=0x0  後=0x0     ← ★ 完全沒變！
      irqs_disabled : 前=0    臨界區內=0     後=0
```

**`spin_lock()` 之後 `preempt_count` 還是 0。**

原因：本機 `CONFIG_PREEMPT_VOLUNTARY=y` 但 **`CONFIG_PREEMPT_COUNT` 沒有被選上**
（它只由 `PREEMPTION`、`DEBUG_ATOMIC_SLEEP`、`PREEMPT_TRACER` 等選項 select）。
於是：

```c
/* include/linux/preempt.h，CONFIG_PREEMPT_COUNT=n 時 */
#define preempt_disable()   barrier()
#define preempt_enable()    barrier()
#define preempt_count()     0
```

**`preempt_disable()` 只剩一個編譯屏障。**

所以在這台機器上，「自旋鎖臨界區不可搶佔」**不是靠 `preempt_count` 保證的，
而是靠「整個核心本來就不可搶佔」**：
`PREEMPT_VOLUNTARY` 的核心只在 `cond_resched()` / `might_sleep()` /
返回使用者空間這幾個點才會調度（見卷1 第9章 [Q12](./ch09_process_management_debugging_and_case_studies.md#q12)）。

**這帶來一個實務上的重要後果：**

| 檢查 | 需要什麼 | 本機 |
|---|---|---|
| `BUG: scheduling while atomic`（中斷上下文） | `preempt_count` 的 HARDIRQ/SOFTIRQ 欄位（**永遠有**） | ✅ 抓得到 |
| `BUG: scheduling while atomic`（spinlock 臨界區） | `CONFIG_PREEMPT_COUNT` | ❌ **抓不到** |
| `BUG: sleeping function called from invalid context` | `CONFIG_DEBUG_ATOMIC_SLEEP` | ❌ **`might_sleep()` 是空操作** |

**也就是說：在這台機器上「在 spinlock 臨界區裡睡眠」這個 bug 完全不會被報出來，
只會表現成隨機當機。** 做核心開發時務必在測試核心上打開
`CONFIG_PREEMPT`（或至少 `DEBUG_ATOMIC_SLEEP`）+ `PROVE_LOCKING`。

（軟中斷上下文的 `preempt_count` 仍然是對的，實測 = `0x100` = `SOFTIRQ_OFFSET`，
見卷1 第9章 [Q12](./ch09_process_management_debugging_and_case_studies.md#q12)。）

---

<a name="q12"></a>
## 12. 基於排隊的自旋鎖機制是如何實現的？

### 結論

qspinlock 用**三層通道**，越後面越貴，設計目標是「無爭用時零成本」：

```
                    lock->val 的三元組 {tail, pending, locked}
初始                        {0, 0, 0}
  │
  ├─ 快速通道 ─────────────────────────────────────────────────
  │   queued_spin_lock():
  │       atomic_try_cmpxchg_acquire(&lock->val, &val, _Q_LOCKED_VAL)
  │       成功 → {0, 0, 1}，一條 CASA 指令就拿到鎖，直接返回
  │
  ├─ 中速通道（第 1 個競爭者）───────────────────────────────────
  │   queued_spin_lock_slowpath():
  │       設 pending 位 → {0, 1, 1}
  │       用 atomic_cond_read_acquire() 等 locked 清掉
  │       → clear_pending_set_locked() 一次 16 位元寫入，
  │          同時清 pending、設 locked → {0, 0, 1}
  │       ★ 第一順位繼承者「不進 MCS 佇列」，省掉節點操作
  │
  └─ 慢速通道（第 2 個以後的競爭者）──────────────────────────────
      取本 CPU 的 mcs 節點 → qnodes[idx]
      xchg_tail(lock, tail) 把自己接到佇列尾巴 → {CPUn, 1, 1}
      如果前面有人：prev->next = node，然後
          arch_mcs_spin_lock_contended(&node->locked)
          ★ 只在「自己的」node->locked 上自旋 —— 這就是 MCS 的精髓
      輪到自己 → 等 pending/locked 清掉 → 設 locked → 通知下一個節點
```

**釋放鎖非常便宜**：
```c
static __always_inline void queued_spin_unlock(struct qspinlock *lock)
{
	smp_store_release(&lock->locked, 0);   /* 一條 stlrb，只清 locked 那個位元組 */
}
```
注意它**只清 `locked` 這一個位元組**，`pending`/`tail` 的內容原封不動 ——
這就是把 `val` 切成多個位元組欄位的好處。

> **書目**：奔跑吧 §1.5「排隊自旋鎖」全節（第 1397~1840 行）、圖 1.6~1.12。
> **原始碼**：`kernel/locking/qspinlock.c`、`include/asm-generic/qspinlock.h`。

### 實機驗證

見 [Q16](#q16) —— 那裡有完整的狀態機實測。這裡先看快速通道：

```
  一把剛初始化的鎖： val = 0x00000000  三元組 {tail, pending, locked} = {0, 0, 0}
  spin_lock() 之後：  val = 0x00000001  三元組 = {0, 0, 1}   <-- 快速通道，只設 locked
  spin_unlock() 之後：val = 0x00000000
```

**無爭用時 `val` 從 0 變成 1 再變回 0，全程只有兩條指令
（一條 `CASA`、一條 `STLRB`）** —— 這就是為什麼 qspinlock 在
無爭用時和最原始的 0/1 自旋鎖一樣便宜。

---

<a name="q13"></a>
## 13. 如果在 spin_lock() 和 spin_unlock() 的臨界區中發生了中斷，並且中斷處理程序也恰巧修改了該臨界區，那麼會發生什麼後果？該如何避免呢？

### 結論

**後果：死鎖（deadlock）。**

```
CPU0
────────────────────────────────────────────
spin_lock(&list_lock);        ← 拿到鎖
   ... 正在改鏈結串列 ...
        ↓ 外部中斷來了
   中斷處理常式:
       spin_lock(&list_lock); ← ★ 同一把鎖，但持有者就是被我打斷的那個
                              ← 自旋等待，永遠等不到
   （中斷處理常式不返回 → 被打斷的程式碼也不會繼續 → 鎖永遠不會釋放）
```

**注意這是「同一顆 CPU 上」的自我死鎖**，跟有幾顆 CPU 無關。

**解法：用會關中斷的變體**

| 函式 | 動作 | 什麼時候用 |
|---|---|---|
| `spin_lock()` | 只關搶佔 | 確定不會和中斷處理常式共用這把鎖 |
| `spin_lock_irq()` | 關搶佔 + **關本地中斷** | 確定進來之前中斷是開的 |
| **`spin_lock_irqsave(lock, flags)`** | 關搶佔 + **保存中斷狀態再關中斷** | **不確定進來時的狀態 → 最安全，最常用** |
| `spin_lock_bh()` | 關搶佔 + **關軟中斷** | 只和 tasklet/softirq 共用 |

**為什麼要 `irqsave` 而不是 `irq`？**
如果呼叫者本來就在關中斷的狀態，`spin_unlock_irq()` 會把中斷**打開**，
破壞呼叫者的假設。`irqsave/irqrestore` 是「還原」而不是「打開」。

**只關「本地」中斷夠嗎？** 夠。
別的 CPU 的中斷處理常式確實可能來搶這把鎖，但**持有者在 CPU0 上還在跑**
（它沒被中斷打斷，因為中斷關了），會很快釋放鎖，所以另一顆 CPU 上的中斷
只會等一小段時間，不會死鎖。

> **書目**：奔跑吧 §1.3.2「自旋鎖的變體」（第 1051~1093 行）。

### 實機驗證

`sync_probe.ko` 實測 `spin_lock()` 與 `spin_lock_irqsave()` 對 DAIF 的影響：

```
  (a) spin_lock() / spin_unlock()
      preempt_count : 前=0x0  臨界區內=0x0  後=0x0
      irqs_disabled : 前=0    臨界區內=0     後=0        ← ★ 中斷全程是開的！

  (b) spin_lock_irqsave() / spin_unlock_irqrestore()
      preempt_count : 臨界區內=0x0
      irqs_disabled : 前=0  臨界區內=128  後=0   <-- ★ 中斷真的被關掉了
      flags = 0x0（PSR_I_BIT=0x80；flags 為 0 表示「進來之前中斷是開的」，
             所以 unlock_irqrestore 會把中斷「還原成開」，而不是無條件開）
```

**三個可以直接讀出來的事實：**
1. **`spin_lock()` 完全不碰中斷**（`irqs_disabled()` 全程 0）
   → 所以才會有本題描述的死鎖。
2. **`spin_lock_irqsave()` 讓 `irqs_disabled()` 變成 128**。
   128 = `PSR_I_BIT` = `1 << 7`，就是 `PSTATE.DAIF` 的 I 位元。
3. **`flags = 0x0`** 表示進入前中斷是開的，
   所以 `spin_unlock_irqrestore()` 把它還原成「開」。
   如果呼叫者原本就關著中斷，`flags` 會是 `0x80`，還原後仍然是關的
   —— 這就是 `irqsave` 相對於 `irq` 的價值。

---

<a name="q14"></a>
## 14. 排隊自旋鎖是如何實現 MCS 鎖的？

### 結論

**核心手法：把 MCS 節點從「鎖裡面」搬到「per-CPU 陣列」。**

MCS 演算法本身需要一個節點結構（`next` 指標 + `locked` 旗標），
但 `spinlock_t` 被內嵌在 `struct page` 這種對大小極敏感的結構裡，塞不下。
qspinlock 的解法：

```c
/* kernel/locking/qspinlock.c */
struct qnode {
	struct mcs_spinlock mcs;
};
static DEFINE_PER_CPU_ALIGNED(struct qnode, qnodes[4]);   /* ★ 每顆 CPU 4 個節點 */

/* kernel/locking/mcs_spinlock.h */
struct mcs_spinlock {
	struct mcs_spinlock *next;
	int locked;        /* 1 表示輪到我了 */
	int count;         /* 巢狀計數 */
};
```

**為什麼是 4 個？** 對應四種可能巢狀的上下文：

| idx | 上下文 |
|---|---|
| 0 | task（行程上下文） |
| 1 | softirq |
| 2 | hardirq |
| 3 | NMI |

一個行程在 task 上下文拿著 `qnodes[0]` 自旋時被中斷，
中斷處理常式若也要搶自旋鎖，就用 `qnodes[2]`，不會互相踩到。
（書上註腳 [3][4] 講的就是這件事。）

**鎖裡面只存「佇列尾巴是誰」**，用 `tail_cpu` + `tail_idx` 兩個欄位編碼
（見 [Q15](#q15)）：

```c
static inline __pure u32 encode_tail(int cpu, int idx)
{
	return ((cpu + 1) << _Q_TAIL_CPU_OFFSET) | (idx << _Q_TAIL_IDX_OFFSET);
}
static inline __pure struct mcs_spinlock *decode_tail(u32 tail)
{
	int cpu = (tail >> _Q_TAIL_CPU_OFFSET) - 1;
	int idx = (tail & _Q_TAIL_IDX_MASK) >> _Q_TAIL_IDX_OFFSET;
	return per_cpu_ptr(&qnodes[idx].mcs, cpu);
}
```

**`cpu + 1` 的原因**：`tail == 0` 要能代表「佇列是空的」，所以 CPU0 編碼成 1。

**排隊的動作**（`kernel/locking/qspinlock.c`）：
```c
tail = encode_tail(smp_processor_id(), idx);
old = xchg_tail(lock, tail);              /* 原子換掉尾巴，拿回舊尾巴 */
if (old & _Q_TAIL_MASK) {
	prev = decode_tail(old);
	WRITE_ONCE(prev->next, node);     /* 把自己接到前一個節點後面 */
	arch_mcs_spin_lock_contended(&node->locked);   /* ★ 只自旋在自己的節點上 */
}
```

> **書目**：奔跑吧 §1.5 開頭（第 1440~1476 行）與 §1.4「MCS 鎖」。

### 實機驗證

```
== Q12/Q15/Q16：排隊自旋鎖 qspinlock 的 32 位元劃分 ==
  CONFIG_QUEUED_SPINLOCKS = y，CONFIG_QUEUED_RWLOCKS = y
  sizeof(spinlock_t)=24  sizeof(raw_spinlock_t)=24  sizeof(arch_spinlock_t)=4
  ...
  每 CPU 4 個 mcs 節點（task/softirq/hardirq/nmi）
```

**`arch_spinlock_t` 只有 4 個位元組** —— MCS 節點完全沒有佔用鎖的空間。
（`spinlock_t` = 24 是因為本機 `CONFIG_DEBUG_SPINLOCK=y` 多了
`magic`/`owner`/`owner_cpu` 三個除錯欄位；關掉除錯就只有 4 個位元組。）

從 [Q16](#q16) 的狀態機可以直接看到 `tail` 欄位怎麼編碼：
```
t= 30ms  val=0x000c0101  {tail=CPU2, tail_idx=0, pending=1, locked=1}
                ↑↑
         0x0c0000 >> 18 = 3 = encode_tail(cpu=2) = 2 + 1 ✅
t= 40ms  val=0x00100101  {tail=CPU3, tail_idx=0, pending=1, locked=1}
                ↑↑
         0x100000 >> 18 = 4 = 3 + 1 ✅
```
**`tail_idx` 全程是 0，因為三個競爭者都在行程上下文（task）裡。**

---

<a name="q15"></a>
## 15. 排隊自旋鎖把 32 位的變數劃分成幾個域，每個域的含義和作用是什麼？

### 結論

```c
/* include/asm-generic/qspinlock_types.h */
typedef struct qspinlock {
	union {
		atomic_t val;
		struct {
			u8	locked;
			u8	pending;
		};
		struct {
			u16	locked_pending;   /* ★ 讓 locked+pending 能一次寫 */
			u16	tail;
		};
	};
} arch_spinlock_t;
```

### ⚠ 本機的欄位劃分和書上表 1.4 不一樣

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/sync_probe.ko; sudo rmmod sync_probe; \
                 sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A16 "32 位元劃分"'
```

```
  欄位          位元範圍      OFFSET  BITS  MASK
  locked      bit[ 7: 0]     0      8    0x000000ff
  pending     bit[15: 8]     8      8    0x0000ff00      ← ★ 書上說是 1 位
  tail_idx    bit[17:16]    16      2    0x00030000
  tail_cpu    bit[31:18]    18     14    0xfffc0000
  _Q_LOCKED_VAL = 0x1, _Q_PENDING_VAL = 0x100, _Q_TAIL_MASK = 0xffff0000
  NR_CPUS=8
  ★ 注意：本機 _Q_PENDING_BITS = 8，不是書上表 1.4 說的 1！
```

| 欄位 | 本機位元範圍 | 書上表 1.4 | 含義 |
|---|---|---|---|
| **locked** | bit[7:0] | bit[0:7] ✅ | 非 0 表示鎖被持有 |
| **pending** | **bit[15:8]** | bit[8]（bit[9:15] 未使用）❌ | **第一順位繼承者**，正在自旋等 locked 清掉，不進 MCS 佇列 |
| **tail_idx** | bit[17:16] | bit[16:17] ✅ | 用哪一個 mcs 節點（task/softirq/hardirq/nmi） |
| **tail_cpu** | bit[31:18] | bit[18:31] ✅ | 佇列尾巴在哪顆 CPU（存 `cpu + 1`） |

**為什麼 pending 是 8 位而不是 1 位？**

```c
/* include/asm-generic/qspinlock_types.h */
#if CONFIG_NR_CPUS < (1U << 14)
#define _Q_PENDING_BITS		8      /* ← 本機 NR_CPUS=8，走這條 */
#else
#define _Q_PENDING_BITS		1
#endif
```

`NR_CPUS` 小的時候 `tail_cpu` 用不完 14 位元，於是把 `pending` 撐成整個位元組。
**好處**：`locked` 和 `pending` 剛好湊成一個 `u16`（`locked_pending`），
中速通道可以用**一次 16 位元的存取**同時「清 pending、設 locked」：

```c
/* kernel/locking/qspinlock.c:162 */
static __always_inline void clear_pending_set_locked(struct qspinlock *lock)
{
	WRITE_ONCE(lock->locked_pending, _Q_LOCKED_VAL);   /* 一次寫 16 位元 */
}
```
若 `_Q_PENDING_BITS == 1`，就得退化成兩次原子操作
（`atomic_add(-_Q_PENDING_VAL + _Q_LOCKED_VAL, &lock->val)`）。

**這是一個「編譯期依 NR_CPUS 選擇佈局」的最佳化，回答時提出來很加分。**

**三元組表示法**：核心用 `{x, y, z}` 表示 `{tail, pending, locked}`，
書上圖 1.5、圖 1.6~1.12 都用這個記法。

> **書目**：奔跑吧 §1.5 表 1.4 與圖 1.5（第 1420~1476 行）。

---

<a name="q16"></a>
## 16. 假設 CPU0 先持有了自旋鎖，接著 CPU1、CPU2、CPU3 都加入該鎖的爭用中，請闡述這幾個 CPU 如何獲取鎖，並畫出它們申請鎖的流程圖。

### 結論（流程圖）

```
時間 →

CPU0 ──[快速通道]────────── 持有鎖 ─────────────────────┬─ 放鎖
        CASA 成功                                       │  stlrb locked=0
        {0, 0, 1}                                       │
                                                        │
CPU1 ──────[中速通道]──── 設 pending，自旋等 locked ─────┼──→ 拿到鎖
             {0, 1, 1}     atomic_cond_read_acquire()   │    clear_pending_set_locked()
                                                        │    {0, 0, 1}
                                                        │
CPU2 ────────────[慢速通道]── xchg_tail → 我是尾巴 ──────┼───────→ 等 CPU1 → 拿到鎖
                   {CPU2, 1, 1}   自旋在 自己的 node->locked      │
                                                                  │
CPU3 ──────────────────[慢速通道]── xchg_tail，接在 CPU2 後面 ────┴──→ 拿到鎖
                        {CPU3, 1, 1}   prev(CPU2)->next = me
                                       自旋在 自己的 node->locked
```

**四個階段的細節：**

1. **CPU0（快速通道）**：`atomic_try_cmpxchg_acquire(&val, &0, 1)` 一次成功 → `{0,0,1}`。
2. **CPU1（中速通道）**：發現 `val != 0`，進 `queued_spin_lock_slowpath()`。
   它是**第一個**競爭者，所以只設 `pending` 位 → `{0,1,1}`，
   然後 `atomic_cond_read_acquire()` 等 `locked` 變 0。
   **它不進 MCS 佇列**（省掉節點配置與指標操作，這是 qspinlock 的重要最佳化）。
3. **CPU2（慢速通道）**：發現 `pending` 已經有人了，只好排隊。
   取 `qnodes[0]`，`xchg_tail()` 把 `tail` 設成自己 → `{CPU2,1,1}`。
   `old` 是 0（之前沒有佇列），所以它是佇列頭，自旋等 `pending|locked` 清掉。
4. **CPU3（慢速通道）**：`xchg_tail()` 拿回舊尾巴 = CPU2 → `{CPU3,1,1}`，
   `prev = decode_tail(old)` → `prev->next = my_node`，
   然後**只在自己的 `node->locked` 上自旋**。

**交棒順序**：CPU0 放鎖 → CPU1（pending）→ CPU2（佇列頭）→ CPU3。
**嚴格 FIFO。**

### 實機驗證 —— 完整重現書上圖 1.6~1.12

`lock_bench.ko mode=0`：CPU0 持有鎖 120 ms，CPU1/2/3 分別在
t=20/40/60 ms 加入爭用，CPU7 上的監看執行緒每 10 ms 取樣一次 `lock->val`。

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/lock_bench.ko mode=0; sudo rmmod lock_bench; sudo dmesg'
```

```
  初始（無人持有）      val=0x00000000  {tail=--,   tail_idx=0, pending=0, locked=0}
  [holder] 在 CPU0 上取得鎖，持有 120 ms
  holder 剛拿到鎖       val=0x00000001  {tail=--,   tail_idx=0, pending=0, locked=1}
  t=  0ms               val=0x00000001  {tail=--,   tail_idx=0, pending=0, locked=1}
  t= 10ms               val=0x00000001  {tail=--,   tail_idx=0, pending=0, locked=1}
  [contender1] CPU1 開始搶鎖
  t= 20ms               val=0x00000101  {tail=--,   tail_idx=0, pending=1, locked=1}   ← ★ pending
  [contender2] CPU2 開始搶鎖
  t= 30ms               val=0x000c0101  {tail=CPU2, tail_idx=0, pending=1, locked=1}   ← ★ 進佇列
  [contender3] CPU3 開始搶鎖
  t= 40ms               val=0x00100101  {tail=CPU3, tail_idx=0, pending=1, locked=1}   ← ★ 尾巴換人
  t= 50ms               val=0x00100101  {tail=CPU3, tail_idx=0, pending=1, locked=1}
  t= 60ms               val=0x00100101  {tail=CPU3, tail_idx=0, pending=1, locked=1}
  t= 70ms               val=0x00100101  {tail=CPU3, tail_idx=0, pending=1, locked=1}
  holder 即將放鎖       val=0x00100101  {tail=CPU3, tail_idx=0, pending=1, locked=1}
  [holder] 放鎖，CPU0
  [contender1] CPU1 拿到鎖，等了 93313 us
  [contender2] CPU2 拿到鎖，等了 73529 us
  [contender3] CPU3 拿到鎖，等了 53727 us
  t= 80ms               val=0x00000000  {tail=--,   tail_idx=0, pending=0, locked=0}
```

**逐項對照書上的圖：**

| 時刻 | `val` | 三元組 | 對應書上 | 說明 |
|---|---|---|---|---|
| 初始 | `0x00000000` | `{0,0,0}` | 圖 1.6(a) | 沒人持有 |
| CPU0 拿到 | `0x00000001` | `{0,0,1}` | 圖 1.6(b) | **快速通道** |
| CPU1 加入 | `0x00000101` | `{0,1,1}` | 圖 1.7 | **中速通道**，設 pending |
| CPU2 加入 | `0x000c0101` | `{CPU2,1,1}` | 圖 1.9 | **慢速通道**，進 MCS 佇列 |
| CPU3 加入 | `0x00100101` | `{CPU3,1,1}` | 圖 1.11 | 接在 CPU2 後面，tail 換人 |
| 全部結束 | `0x00000000` | `{0,0,0}` | 圖 1.12 | 回到初始 |

**`tail_cpu` 的編碼驗算**：
`0x000c0101` 的 bit[31:18] = `0x0c0000 >> 18` = **3** = CPU2 + 1 ✅
`0x00100101` 的 bit[31:18] = `0x100000 >> 18` = **4** = CPU3 + 1 ✅

### ★ 嚴格 FIFO 的鐵證

三個競爭者的等待時間分別是 93313 / 73529 / 53727 µs。
它們分別在 t≈20/40/60 ms 開始等，所以**拿到鎖的絕對時刻**是：

```
CPU1: 20.0 + 93.313 = 113.313 ms
CPU2: 40.0 + 73.529 = 113.529 ms      ← 比 CPU1 晚 216 µs
CPU3: 60.0 + 53.727 = 113.727 ms      ← 比 CPU2 晚 198 µs
```

**三個時刻相差 ≈ 200 µs，正好等於每個競爭者拿到鎖後 `udelay(200)` 的持有時間。**

也就是說：CPU0 一放鎖，**CPU1 → CPU2 → CPU3 依序、無縫、按照加入順序**接棒，
沒有任何一個插隊。**這就是排隊自旋鎖相對於第一代自旋鎖最大的價值 —— 公平性。**

---

<a name="q17"></a>
## 17. 與自旋鎖相比，信號量有哪些特點？

### 結論

| | 自旋鎖 spinlock | 信號量 semaphore |
|---|---|---|
| 等待方式 | **忙等**（自旋） | **睡眠**（進等待佇列） |
| 可否在中斷上下文使用 | **可以** | **不可以**（會睡眠） |
| 臨界區可否睡眠 | **不可以** | **可以** |
| 臨界區長度 | 必須很短 | 可以很長 |
| 同時持有者數量 | **只能 1 個** | **可以 N 個**（`count` 可 > 1） |
| 誰能釋放 | 誰拿誰放（慣例） | **任何人都能 `up()`** |
| 有無 owner 記錄 | 無 | **無** |
| 遞迴持有 | 死鎖 | 只要 count 夠就可以 |
| 進入/退出成本 | 極低（無爭用時 2 條指令） | 較高（要操作等待佇列） |
| 大小（本機實測） | `arch_spinlock_t` = **4 B** | `struct semaphore` = **48 B** |

**信號量最特別的兩點：**

**① 它是「計數信號量」，不是互斥鎖。**
```c
sema_init(&sem, 1);   /* count = 1 → 二值信號量，當互斥鎖用 */
sema_init(&sem, 5);   /* count = 5 → 最多 5 個持有者同時進臨界區 */
```
這是自旋鎖和互斥鎖都做不到的 —— 用來限制「同時最多幾個人使用某資源」
（例如限制同時進行的 DMA 通道數）。

**② 沒有「持有者」的概念。**
`down()` 和 `up()` **可以在不同的行程裡呼叫**，這讓信號量可以用來做
「生產者/消費者」的同步（一個行程 `down()` 等待、另一個行程 `up()` 通知），
而不只是互斥。互斥鎖明確禁止這種用法。

**副作用**：正因為不知道誰持有，**信號量無法實作樂觀自旋，也無法做優先級繼承**
—— 這就是 [Q22](#q22) 的答案。

> **書目**：奔跑吧 §1.6「信號量」、§1.6.2 小結（第 1841~2074 行）。

### 實機驗證

```
  sizeof(struct semaphore)    = 48
  sizeof(struct mutex)        = 56
  sizeof(struct rw_semaphore) = 64
  sizeof(spinlock_t)          = 24   <-- 對照組（含 DEBUG_SPINLOCK 的除錯欄位）

  [Q17][Q18] semaphore：count=1，就是一個計數器 + 等待佇列 + 一把 raw_spinlock
     -> 沒有 owner 欄位，所以「誰都可以 up()」，也就無法做樂觀自旋（不知道誰在跑）。
        這正是 Q22「已經有信號量了為何還要 mutex」的關鍵答案。
```

吞吐量對照（[Q10](#q10) 的表）：無爭用時 spinlock 6086 次/ms、mutex 4284 次/ms，
**自旋鎖的快速通道確實最便宜**。

---

<a name="q18"></a>
## 18. 請簡述信號量是如何實現的。

### 結論

**資料結構極簡（`include/linux/semaphore.h`）：**

```c
struct semaphore {
	raw_spinlock_t		lock;        /* 保護底下兩個欄位 */
	unsigned int		count;       /* 還可以讓幾個人進來 */
	struct list_head	wait_list;   /* 等待佇列 */
};
```

**`down()`（P 操作）：**
```c
void down(struct semaphore *sem)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&sem->lock, flags);
	if (likely(sem->count > 0))
		sem->count--;                 /* 快速通道：還有名額，拿走一個 */
	else
		__down(sem);                  /* 慢速通道：去睡 */
	raw_spin_unlock_irqrestore(&sem->lock, flags);
}
```

**`__down_common()`（真正的睡眠邏輯）：**
```c
static inline int __sched ___down_common(struct semaphore *sem, long state,
						long timeout)
{
	struct semaphore_waiter waiter;

	list_add_tail(&waiter.list, &sem->wait_list);   /* ★ FIFO：加到隊尾 */
	waiter.task = current;
	waiter.up = false;

	for (;;) {
		if (signal_pending_state(state, current))  goto interrupted;
		if (unlikely(timeout <= 0))                goto timed_out;
		__set_current_state(state);
		raw_spin_unlock_irq(&sem->lock);           /* ★ 睡覺前先放鎖 */
		timeout = schedule_timeout(timeout);
		raw_spin_lock_irq(&sem->lock);             /* ★ 醒來再拿回來 */
		if (waiter.up)
			return 0;
	}
	...
}
```

**`up()`（V 操作）：**
```c
void up(struct semaphore *sem)
{
	raw_spin_lock_irqsave(&sem->lock, flags);
	if (likely(list_empty(&sem->wait_list)))
		sem->count++;             /* 沒人等，還名額 */
	else
		__up(sem);                /* 有人等，直接喚醒隊頭 */
	raw_spin_unlock_irqrestore(&sem->lock, flags);
}

static noinline void __sched __up(struct semaphore *sem)
{
	struct semaphore_waiter *waiter = list_first_entry(&sem->wait_list,
						struct semaphore_waiter, list);
	list_del(&waiter->list);
	waiter->up = true;            /* ★ 直接把名額「交棒」給它 */
	wake_up_process(waiter->task);
}
```

**三個設計重點：**
1. **內部用 `raw_spinlock_t` 保護自己的欄位** —— 信號量的實作本身要靠自旋鎖。
2. **`up()` 時如果有等待者，`count` 不加回去**，而是直接把名額交棒
   （`waiter->up = true`）—— 避免剛醒來的行程又被別人插隊搶走（防餓死）。
3. **嚴格 FIFO**：`list_add_tail` 進隊尾，`list_first_entry` 從隊頭喚醒。

**四個變體**：
`down_interruptible()`（可被訊號打斷）、`down_killable()`（可被致命訊號打斷）、
`down_trylock()`（不睡眠）、`down_timeout()`（有逾時）。
**驅動程式裡幾乎一律該用 `down_interruptible()`**，否則使用者 Ctrl-C 殺不掉。

> **書目**：奔跑吧 §1.6.1「信號量簡介」（第 1857~2065 行）。
> **原始碼**：`kernel/locking/semaphore.c`。

### 實機驗證

```bash
ssh radxa@$HOST 'grep -n -A6 "^struct semaphore {" \
   /lib/modules/$(uname -r)/build/include/linux/semaphore.h'
```
```c
struct semaphore {
	raw_spinlock_t		lock;
	unsigned int		count;
	struct list_head	wait_list;
};
```
`sizeof` 實測 **48** = `raw_spinlock_t`(24，含 DEBUG 欄位) + `count`(4+4 padding) + `list_head`(16)。

---

<a name="q19"></a>
## 19. 樂觀自旋等待的判斷條件是什麼？

### 結論

**核心判斷只有一句：「鎖持有者現在是不是正在某顆 CPU 上跑？」**

```c
/* kernel/locking/mutex.c:392 */
static inline int mutex_can_spin_on_owner(struct mutex *lock)
{
	struct task_struct *owner;
	int retval = 1;

	if (need_resched())              /* ① 我自己該讓出 CPU 了 → 別自旋 */
		return 0;

	rcu_read_lock();
	owner = __mutex_owner(lock);
	if (owner)
		retval = owner->on_cpu || owner_on_cpu(owner);  /* ★ 關鍵 */
	rcu_read_unlock();

	return retval;                   /* owner == NULL 也回 1（鎖剛好放了，值得試） */
}
```

**進入自旋之後，`mutex_spin_on_owner()` 持續檢查三個退出條件：**

```c
/* kernel/locking/mutex.c:352 */
bool mutex_spin_on_owner(struct mutex *lock, struct task_struct *owner, ...)
{
	rcu_read_lock();
	while (__mutex_owner(lock) == owner) {
		barrier();
		if (!owner_on_cpu(owner) || need_resched()) {   /* ② ③ */
			ret = false;
			break;
		}
		cpu_relax();
	}
	rcu_read_unlock();
	return ret;
}
```

| # | 退出條件 | 後續動作 |
|---|---|---|
| ① | **`__mutex_owner(lock) != owner`**（持有者換人 = 鎖被放了） | **最理想**：回去 `__mutex_trylock_or_owner()` 搶鎖 |
| ② | **`!owner->on_cpu`**（持有者自己睡著了） | 放棄自旋，改走睡眠等待 |
| ③ | **`need_resched()`**（我自己被要求讓出 CPU） | 放棄自旋，改走睡眠等待 |

**為什麼 `owner->on_cpu` 是對的判斷？**
`task_struct->on_cpu` 由 `prepare_task_switch()` 設 1、`finish_task_switch()` 清 0
（見卷1 第9章 [Q10](./ch09_process_management_debugging_and_case_studies.md#q10)）。
它為 1 就代表這個行程正佔著某顆 CPU 執行 → 它在臨界區裡 → 很快會出來。

**還有一道門檻：進自旋前要先搶到 OSQ 鎖。**
```c
if (!osq_lock(&lock->osq))
	goto fail;
```
否則 N 個 CPU 同時樂觀自旋同一把 mutex，又變回 cache line 顛簸了（見 [Q23](#q23)）。

**為什麼要 `rcu_read_lock()`？** `owner` 是一個 `task_struct` 指標，
可能在我們讀它的時候被釋放。`task_struct` 是透過
`call_rcu(&task->rcu, delayed_put_task_struct)` 延後釋放的
（`kernel/exit.c:231`），所以 RCU 讀者臨界區內它保證還活著，
可以安全地讀 `owner->on_cpu`。詳見 [Q37](#q37)。

> **書目**：奔跑吧 §1.7.4「樂觀自旋等待機制」（第 2252~2343 行）與圖 1.14。

### 實機驗證

```
  CONFIG_MUTEX_SPIN_ON_OWNER = y  <-- 樂觀自旋等待的總開關
  CONFIG_RWSEM_SPIN_ON_OWNER = y
  判斷條件（mutex_can_spin_on_owner）：鎖持有者的 task->on_cpu == 1，
  也就是「持有者正在某顆 CPU 上跑」-> 它很快會出臨界區 -> 值得自旋等。
```

`owner` 欄位怎麼同時存指標和旗標：

```
  mutex_lock() 之後 owner = 0xffff0001049bbe00，current = ffff0001049bbe00 (insmod/60027)
    -> owner & ~0x07 = 0xffff0001049bbe00  == current
  mutex_unlock() 之後 owner = 0x0
```

**`atomic_long_t owner` 的低 3 位被借去當旗標**：

```
  MUTEX_FLAG_WAITERS   = 0x01  等待佇列非空
  MUTEX_FLAG_HANDOFF   = 0x02  要求把鎖直接交棒給第一順位者
  MUTEX_FLAG_PICKUP    = 0x04  鎖已交棒，等人來撿
  -> 因為 task_struct 至少 L1_CACHE_BYTES(64) 對齊，低 3 位永遠是 0，可以借用
```

實測 `owner` 的低 3 位確實是 0（`0x...be00`），所以 `owner & ~0x07` 就等於 `current`。
**一個 `atomic_long_t` 同時表達「誰持有」＋「三個狀態旗標」，
而且加解鎖都能用一條 CAS 完成 —— 這是 mutex 比信號量快的關鍵之一。**

---

<a name="q20"></a>
## 20. 為什麼在互斥鎖爭用中進入樂觀自旋等待比睡眠等待模式要好？

### 結論

**因為「睡一次再醒來」的代價，往往比「自旋等幾微秒」還大。**

睡眠等待要付的成本：

| 步驟 | 成本 |
|---|---|
| 把自己加進 `wait_list` | 拿 `wait_lock`、操作鏈結串列 |
| `set_current_state()` + `schedule()` | **一次完整的行程切換**（見卷1 ch8 [Q45](./ch08_process_management_scheduling_and_load_balancing.md#q45)：切頁表、切 ASID、切 13 個暫存器、`dsb ish`） |
| 持有者 `unlock` 時 `wake_up_q()` | 拿鎖、`try_to_wake_up()`、可能送 IPI 到別顆 CPU |
| 被喚醒後重新排隊 | 又一次行程切換 + cache/TLB 全冷 |

**兩次行程切換 + cache/TLB 冷掉**，在本機上一次切換就要幾微秒
（卷1 ch8 量到閒置系統每秒 2700 次切換）。
如果臨界區只有幾百奈秒，自旋等待顯然划算得多。

**但前提是「持有者真的在跑」** —— 這就是 [Q19](#q19) 的判斷條件存在的原因。
如果持有者自己睡著了，自旋就變成純粹浪費 CPU（而且可能等很久），
所以 `mutex_spin_on_owner()` 一發現 `on_cpu == 0` 就立刻放棄。

> **書目**：奔跑吧 §1.7.4 開頭（第 2252 行）——
> 「與其進入睡眠隊列，不如像自旋鎖一樣自旋等待，因為睡眠與喚醒的代價可能更高」。

### 實機驗證 —— 用「自願切換次數」直接量

`lock_bench.ko mode=2`：4 個執行緒搶同一把 mutex，跑 200 ms。
兩組唯一的差別是**臨界區裡會不會睡**：

```c
mutex_lock(&bl_mutex);
if (opt_sleep_in_cs)
	usleep_range(200, 300);   /* B 組：持有者睡覺 → on_cpu = 0 */
else
	udelay(5);                /* A 組：持有者一直在跑 → on_cpu = 1 */
bl_shared++;
mutex_unlock(&bl_mutex);
```

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/lock_bench.ko mode=2 nthread=4 ms=200; \
                 sudo rmmod lock_bench; sudo dmesg'
```

```
  4 個執行緒搶同一把 mutex，跑 200 ms：
  臨界區只跑不睡 (udelay 5us) : 總計  34444 次   自願切換(睡眠) =     2   被搶佔 = 2
      -> 平均每拿一次鎖就睡 0.00 次
  臨界區會睡眠 (usleep 200us) : 總計    645 次   自願切換(睡眠) =  1156   被搶佔 = 0
      -> 平均每拿一次鎖就睡 1.79 次
```

| | 持有者一直在跑 | 持有者會睡覺 |
|---|---|---|
| 200 ms 內拿到鎖的次數 | **34444** | 645 |
| **自願切換（= 走睡眠路徑）次數** | **2** | **1156** |
| 平均每次拿鎖睡幾次 | **0.00006** | **1.79** |

**兩組的「每次拿鎖的睡眠次數」相差 30000 倍。**

* **A 組**：持有者 `on_cpu == 1` → 競爭者全部走樂觀自旋 →
  34444 次拿鎖只發生 2 次睡眠（那 2 次大概是剛好碰上 `need_resched()`）。
* **B 組**：持有者一 `usleep` 就 `on_cpu = 0` → `mutex_spin_on_owner()` 立刻放棄 →
  每個競爭者都得排隊睡覺，平均每拿一次鎖要睡 1.79 次
  （> 1 是因為醒來後可能又搶不到、再睡一次）。

**這組數據同時證明了兩件事**：樂觀自旋確實有效，
而且它的**啟用條件（`owner->on_cpu`）判斷得很準** —— 持有者一睡，它就立刻退場。

---

<a name="q21"></a>
## 21. 假設 CPU0～CPU3 同時爭用一個互斥鎖…請畫出這幾個 CPU 爭用鎖的時序圖。

### 結論（時序圖）

```
        T0    T1    T2    T3      T4        T5      T6      T7    T8
        │     │     │     │       │         │       │       │     │
CPU0 ───█████████████████│                                          持有 → T3 釋放
        拿到鎖（快速通道 CAS）
                          │
CPU1 ─────────┅┅┅┅┅┅┅┅┅┅┅█████████░░░░░░░░░░████████│
              樂觀自旋     拿到鎖   睡著了     被喚醒  釋放
              （看到 CPU0                    （T6）  （T7）
                on_cpu=1）
                          ↑T4：CPU1 在臨界區裡睡眠 → on_cpu=0
                    │
CPU2 ───────────────┅┅┅┅┅┅┅┅┅┅░░░░░░░░░░░░░░░░░░░░░░░████████│
                    樂觀自旋  → 發現 owner 不在跑 → 轉睡眠等待 → T7 拿到鎖
                                                                    │
CPU3 ───────────────────────────────░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░████
                                    一開始就發現 owner 不在跑
                                    → 直接睡眠等待 → T8 拿到鎖

圖例：█ 持有鎖   ┅ 樂觀自旋   ░ 睡眠等待
```

**逐一時刻說明（對應書上圖 1.15）：**

| 時刻 | 事件 | 機制 |
|---|---|---|
| **T0** | CPU0 拿到鎖 | `__mutex_trylock_fast()`：一條 `CASAL` 把 `owner` 從 0 換成 `current` |
| **T1** | CPU1 來搶 | `mutex_can_spin_on_owner()` 看到 CPU0 的 `on_cpu==1` → **樂觀自旋** |
| **T2** | CPU2 來搶 | 同上，也樂觀自旋（但要先搶到 **OSQ 鎖**，所以排在 CPU1 後面） |
| **T3** | CPU0 釋放 | `mutex_spin_on_owner()` 偵測到 `owner` 變了 → CPU1 立刻 CAS 搶到 |
| **T4** | CPU1 在臨界區睡著 | `on_cpu = 0`。CPU2 的 `mutex_spin_on_owner()` 看到 → **放棄自旋，改睡** |
| **T5** | CPU3 來搶 | 一進來就看到 `owner->on_cpu == 0` → `mutex_can_spin_on_owner()` 回 0 → **直接睡** |
| **T6** | CPU1 被喚醒 | 重新進臨界區 |
| **T7** | CPU1 釋放 | `__mutex_unlock_slowpath()` → `wake_up_q()` 喚醒等待佇列第一個（CPU2） |
| **T8** | CPU2 釋放 | 喚醒 CPU3 |

**三個值得強調的細節：**
1. **樂觀自旋和睡眠等待是可以互相轉換的**，不是二選一。
   CPU2 先自旋、後睡眠，就是最典型的情況。
2. **OSQ 鎖讓「自旋者」也排隊**：CPU1 和 CPU2 同時想自旋時，
   只有搶到 OSQ 的那個真的在 `lock->owner` 上自旋，另一個在自己的 MCS 節點上等。
3. **`MUTEX_FLAG_HANDOFF` 防餓死**：如果等待佇列第一個等太久
   （`__mutex_lock_common()` 裡的 `first` 判斷），會設這個旗標，
   要求持有者**直接把鎖交棒**給它，而不是放掉讓大家重搶
   —— 否則樂觀自旋者永遠比睡眠者先搶到，睡眠者會餓死。

> **書目**：奔跑吧 §1.7.6「案例分析」與圖 1.15（第 2379~2401 行）。

### 實機驗證

[Q20](#q20) 的實驗正好把 T1~T3（樂觀自旋）與 T4~T8（睡眠等待）兩種模式分開量了：

| 對應時序圖的階段 | 實測 |
|---|---|
| T1~T3：持有者一直在跑 → 全部樂觀自旋 | 34444 次拿鎖，**只睡 2 次** |
| T4~T8：持有者睡著 → 全部退回睡眠等待 | 645 次拿鎖，**睡了 1156 次** |

`MUTEX_FLAG_HANDOFF` 的存在也在 [Q19](#q19) 的實測中看到了
（`owner` 欄位的 bit1）。

---

<a name="q22"></a>
## 22. Linux 內核已經實現了信號量機制，為何要單獨設置一個互斥鎖機制呢？

### 結論

**一句話：因為互斥鎖「知道誰持有」，於是能做信號量做不到的三件事。**

| | 信號量 | 互斥鎖 |
|---|---|---|
| **有 owner 欄位** | ❌ | ✅ `atomic_long_t owner` |
| **樂觀自旋等待** | ❌ 做不到（不知道持有者在不在跑） | ✅ 見 [Q19](#q19)、[Q20](#q20) |
| **優先級繼承** | ❌ | ✅（`rt_mutex` 基於 mutex 擴充） |
| **除錯能力** | 只知道有人拿著 | **知道是誰**，可以印出來 |
| 快速通道 | 拿 spinlock + 改 count + 放 spinlock | **一條 `CASAL`** |
| 大小（實測） | 48 B | 56 B |

**限制也更嚴格**（正是為了換取上面的能力）：

| 規則 | 原因 |
|---|---|
| 同一時刻只能一個持有者 | 沒有 count，`owner` 只能存一個指標 |
| **只有持有者能解鎖** | `__mutex_unlock_fast()` 用 `cmpxchg(owner, current, 0)` |
| 不能遞迴持有 | 第二次 `mutex_lock()` 會發現 `owner == current` 但仍然去睡 → 死鎖 |
| 持有鎖時不能退出行程 | `owner` 會變成野指標 |
| **不能在中斷上下文使用** | 會睡眠 |
| 必須用官方 API 初始化 | 內含 `osq_lock_init()` 等 |

**信號量在什麼場合仍然不可取代？**
1. **`count > 1` 的資源池**（同時最多 N 個使用者）。
2. **`down()` 和 `up()` 在不同行程**：例如驅動的 `read()` 裡 `down()` 等資料，
   中斷處理常式的下半部 `up()` 通知。互斥鎖明確禁止這種用法。
3. 核心與使用者空間之間的複雜同步（書上小結特別提到）。

**歷史脈絡**：mutex 是 2006 年（Linux 2.6.16）由 Ingo Molnar 引入的，
當時核心裡有大量「其實只是互斥」卻用信號量的地方。改用 mutex 之後：
* 快速通道從「spinlock + 計數」變成一條原子指令；
* 有了 owner 就能做樂觀自旋（2.6.39 加入）；
* 有了 owner 才能做 lockdep 檢查與 rt_mutex 的優先級繼承。

> **書目**：奔跑吧 §1.7 開頭與 §1.7.7 小結（第 2074、2401~2420 行）。

### 實機驗證

```
  sizeof(struct semaphore)    = 48
  sizeof(struct mutex)        = 56

  [Q17][Q18] semaphore：count=1，就是一個計數器 + 等待佇列 + 一把 raw_spinlock
     -> 沒有 owner 欄位，所以「誰都可以 up()」，也就無法做樂觀自旋（不知道誰在跑）。
```

`struct mutex` 多出來的 8 個位元組就是 `atomic_long_t owner`：

```c
/* include/linux/mutex.h */
struct mutex {
	atomic_long_t		owner;        /* ★ 信號量沒有的 */
	raw_spinlock_t		wait_lock;
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	struct optimistic_spin_queue osq; /* ★ 樂觀自旋用的 MCS 佇列 */
#endif
	struct list_head	wait_list;
};
```

**吞吐量對照**（[Q10](#q10) 的表，4 執行緒爭用）：
mutex **1483** 次/ms vs spinlock **559** 次/ms
—— 臨界區極短時 mutex 反而比自旋鎖快 2.7 倍，
因為樂觀自旋讓它拿到了「不切換」的好處，同時等待者又不會像自旋鎖那樣
死命打同一條 cache line。

---

<a name="q23"></a>
## 23. 請簡述 MCS 鎖機制的實現原理。

### 結論

**核心思想：每個等待者只在「自己的」變數上自旋，而不是所有人擠在同一個變數上。**

```
傳統自旋鎖（所有人自旋在 lock 上）        MCS 鎖（各自旋在自己的節點上）

   CPU0 ──┐                                CPU0 ──→ node0.locked
   CPU1 ──┤                                          ↓ next
   CPU2 ──┼──→  lock ← cache line 顛簸      CPU1 ──→ node1.locked
   CPU3 ──┘      被 4 顆 CPU 反覆搶                  ↓ next
                                            CPU2 ──→ node2.locked
   放鎖時：1 次寫入 → 4 顆 CPU 的                    ↓ next
           cache line 全部失效               CPU3 ──→ node3.locked

                                            放鎖時：只寫「下一個節點」的 locked
                                                    → 只有 1 顆 CPU 的 cache line 失效
```

**資料結構（`include/linux/osq_lock.h`）：**
```c
struct optimistic_spin_queue {      /* 鎖本體，只有 4 位元組 */
	atomic_t tail;              /* 存 encode_cpu(cpu)，0 = 空佇列 */
};

struct optimistic_spin_node {       /* per-CPU 節點，24 位元組 */
	struct optimistic_spin_node *next, *prev;
	int locked;                 /* 1 = 輪到我了 */
	int cpu;
};
static DEFINE_PER_CPU_SHARED_ALIGNED(struct optimistic_spin_node, osq_node);
```

**`osq_lock()` 的三段式（書上 §1.4.1~1.4.3）：**

```c
bool osq_lock(struct optimistic_spin_queue *lock)
{
	struct optimistic_spin_node *node = this_cpu_ptr(&osq_node);
	int curr = encode_cpu(smp_processor_id());   /* cpu + 1 */

	node->locked = 0;
	node->next   = NULL;
	node->cpu    = curr;

	/* ① 快速通道：原子換掉尾巴 */
	old = atomic_xchg(&lock->tail, curr);
	if (old == OSQ_UNLOCKED_VAL)
		return true;             /* 佇列本來是空的 → 直接拿到 */

	/* ② 中速通道：把自己接到前一個節點後面 */
	prev = decode_cpu(old);
	node->prev = prev;
	WRITE_ONCE(prev->next, node);

	/* ③ 只在自己的 node->locked 上自旋 */
	while (!READ_ONCE(node->locked)) {
		if (need_resched() || vcpu_is_preempted(...))
			goto unqueue;    /* ★ 可以中途退出，這是 OSQ 比純 MCS 複雜的原因 */
		cpu_relax();
	}
	return true;
unqueue:
	/* 把自己從雙向鏈結串列裡摘掉（很複雜，要處理各種競態） */
	...
	return false;
}
```

**OSQ（Optimistic Spin Queue）比教科書的 MCS 多了什麼？**
教科書 MCS 一旦排隊就必須等到底；OSQ 因為用在 mutex 的樂觀自旋，
**必須能在 `need_resched()` 時中途退出**（放棄自旋、改去睡覺），
所以節點多了 `prev` 指標變成**雙向**鏈結串列，退出邏輯（`osq_unqueue`）
佔了整個檔案一半以上的程式碼。

**為什麼 MCS 只用在 mutex/rwsem，不用在傳統 spinlock？**
`optimistic_spin_node` 有 24 位元組，而 `spinlock_t` 被內嵌在
`struct page`、`struct inode` 這類對大小極敏感的結構裡，塞不下。
**qspinlock 的突破就是把節點搬到 per-CPU 陣列**（見 [Q14](#q14)），
鎖本體仍然只有 4 位元組。

> **書目**：奔跑吧 §1.4「MCS 鎖」全節（第 1107~1397 行）。

### 實機驗證

```
== Q14/Q23：MCS 鎖 / OSQ 鎖 ==
  sizeof(struct optimistic_spin_queue) = 4  (只有一個 atomic_t tail)
  sizeof(struct optimistic_spin_node)  = 24  (next/prev/locked/cpu)
  osq_lock_init() 之後 tail = 0 （0 = OSQ_UNLOCKED_VAL）
  encode_cpu(cpu) = cpu + 1，所以 0 保留給「沒有 CPU」
  本 CPU(1) 的編碼值 = 2

  為什麼 MCS 只用在 mutex/rwsem，不用在傳統 spinlock？
    optimistic_spin_node 有 24 位元組，而 arch_spinlock_t 只有 4 位元組。
    （本機 sizeof(spinlock_t)=24 是因為 CONFIG_DEBUG_SPINLOCK=y 多了
      magic/owner/owner_cpu 三個除錯欄位；關掉除錯就只有 4 位元組。）
    spinlock 被內嵌在 struct page 等對大小極敏感的結構裡，塞不下 MCS 節點。
    qspinlock 的解法：節點放 per-CPU 陣列（qnodes[4]），鎖本體仍然只有 4 位元組。
```

**`encode_cpu(cpu) = cpu + 1` 的實測**：本 CPU 是 1，編碼值是 2。
同樣的編碼方式在 qspinlock 的 `tail_cpu` 欄位也看得到
（[Q16](#q16)：CPU2 → 3、CPU3 → 4）。

**MCS 有效的直接證據**（[Q10](#q10) 的表）：
4 執行緒爭用時 **mutex（有 OSQ）1483 次/ms vs spinlock 559 次/ms**，
臨界區同樣只有 `bl_shared++`，差別就在等待者有沒有擠在同一條 cache line 上。

---

<a name="q24"></a>
## 24. 在編寫內核代碼時，該如何選擇信號量和互斥鎖？

### 結論

**預設答案：優先用互斥鎖。只有互斥鎖的限制擋住你時，才改用信號量。**

```
                    ┌─────────────────────────────┐
                    │ 臨界區會睡眠嗎？             │
                    └──────────┬──────────────────┘
                     否        │        是
              ┌───────────────┘         └──────────────┐
              ▼                                        ▼
      ┌───────────────┐                    ┌────────────────────────┐
      │ 在中斷上下文？ │                    │ 需要 count > 1 嗎？     │
      └───┬───────┬───┘                    └─────┬──────────┬───────┘
       是 │       │ 否                        是 │          │ 否
          ▼       ▼                             ▼          ▼
   spin_lock_   spin_lock()          ┌──────────────┐  ┌──────────────────┐
   irqsave()                         │  semaphore   │  │ 加解鎖在同一個    │
                                     │ （資源池）    │  │ 上下文嗎？        │
                                     └──────────────┘  └────┬─────────┬───┘
                                                          否 │         │ 是
                                                             ▼         ▼
                                                     ┌────────────┐ ┌─────────┐
                                                     │ semaphore  │ │ mutex ★ │
                                                     │（生產/消費）│ └─────────┘
                                                     └────────────┘
```

**必須用信號量的三種情況：**
1. **`count > 1`** —— 限制「同時最多 N 個」的資源池。
2. **`down()` 和 `up()` 在不同上下文/行程** —— 生產者/消費者、
   「等待硬體完成」這類同步（雖然現在更常用 `completion`）。
3. 需要「非持有者也能解鎖」的語意。

**其餘一律用互斥鎖**，理由見 [Q22](#q22)：更快、有 owner、能除錯、能做優先級繼承。

**現代核心的補充選項**（書上沒提，但實務上很重要）：

| 需求 | 更好的選擇 |
|---|---|
| 「等一件事情完成」 | **`struct completion`**（`wait_for_completion()` / `complete()`），語意比信號量清楚 |
| 讀多寫少 | **`rw_semaphore`** 或 **RCU** |
| 只保護一個變數 | **原子操作** |
| 實時場景需要優先級繼承 | **`rt_mutex`** |

事實上**現代核心程式碼裡 `struct semaphore` 已經非常罕見**，
大部分舊用法都被 mutex 或 completion 取代了。

> **書目**：奔跑吧 §1.7.7 小結末段（第 2415 行）——
> 「除非代碼場景不符合上述互斥鎖的約束中的某一條，否則可以優先使用互斥鎖」。

### 實機驗證

```bash
# 本機核心原始碼裡兩者的使用量對比
ssh radxa@$HOST 'cd /lib/modules/$(uname -r)/build
  echo -n "用到 DEFINE_MUTEX/mutex_init 的 .c 檔數  : "
  grep -rl "DEFINE_MUTEX\|mutex_init(" --include=*.c . | wc -l
  echo -n "用到 DEFINE_SEMAPHORE/sema_init 的 .c 檔數: "
  grep -rl "DEFINE_SEMAPHORE\|sema_init(" --include=*.c . | wc -l'
```
```
用到 DEFINE_MUTEX/mutex_init 的 .c 檔數  : 4334
用到 DEFINE_SEMAPHORE/sema_init 的 .c 檔數: 112
```
**4334 : 112 ≈ 39 : 1** —— 現代核心裡互斥鎖的使用量是信號量的近 40 倍，
印證了「預設用 mutex」這條規則。

以及吞吐量（[Q10](#q10)）：mutex 在無爭用時 4284 次/ms，
爭用時因為樂觀自旋反而只掉到 1483 次/ms（2.9 倍），
而信號量沒有樂觀自旋，爭用時只能走睡眠路徑。

---

<a name="q25"></a>
## 25. 什麼時候使用讀者鎖？什麼時候使用寫者鎖？怎麼判斷？

### 結論

**判斷準則只有一條：這段臨界區「會不會修改被保護的資料」。**

| 臨界區行為 | 用哪個 | 為什麼 |
|---|---|---|
| **只讀** —— 遍歷、查詢、統計 | **讀者鎖** `down_read()` | 多個讀者可以同時進入 |
| **會寫** —— 插入、刪除、修改欄位 | **寫者鎖** `down_write()` | 必須獨佔 |
| 先讀後可能寫 | `down_write()`，或 `down_read()` + 失敗重試 + `down_write()` | 不能中途升級（見下） |

**這件事鎖本身幫不了你 —— 只能由程式設計者判斷。**
書上原話：「這需要程式員來判斷被保護的臨界區的內容是只讀的還是可寫的，
鎖不能代替程式員考慮這些問題。」

**讀寫鎖的語意**：
* **讀者 vs 讀者**：可以並行（這是唯一的好處）
* **讀者 vs 寫者**：互斥
* **寫者 vs 寫者**：互斥

**什麼時候讀寫鎖「不值得」？**
* **讀者臨界區很短**時，讀寫鎖的額外成本（要維護讀者計數）可能超過並行的好處，
  用普通 mutex 反而更快。
* **寫者比例高**時，讀者鎖幾乎沒機會並行，白付成本。
* 一般經驗值：**讀者遠多於寫者（10:1 以上）且臨界區夠長**才值得。
  再往上，就該考慮 **RCU**（見 [Q27](#q27)）。

**兩個常見陷阱：**
1. **不能升級（upgrade）**。Linux 沒有 `down_read_to_write()`。
   要從讀者變寫者，必須先 `up_read()` 再 `down_write()`，
   **中間狀態可能被別人改掉**，所以要重新驗證。
   （有 `downgrade_write()` 可以從寫者「降級」成讀者，這個是安全的。）
2. **寫者可能被餓死**。純粹的「讀者優先」實作下，源源不絕的讀者會讓寫者永遠等不到。
   Linux 的 rwsem 用 `RWSEM_FLAG_HANDOFF` 解決（見 [Q26](#q26)）。

**核心裡的實例：**

| 鎖 | 讀者 | 寫者 |
|---|---|---|
| `mm->mmap_lock` | 缺頁處理、KSM 掃描 VMA、`/proc/pid/maps` | `mmap()`、`munmap()`、`brk()`、`mprotect()` |
| `anon_vma->rwsem` | `rmap_walk()` 反向映射查詢 | `anon_vma_fork()`、`unlink_anon_vmas()` |
| `inode->i_rwsem` | `read()` | `write()`、`truncate()` |

> **書目**：奔跑吧 §1.8「讀寫鎖」、§1.11.1、§1.11.4（第 2420~2460、3239、3297 行）。

### 實機驗證

`lock_bench.ko mode=1`，4 執行緒同時搶同一把 rwsem：

```
  rwsem(讀)      1 執行緒  100 ms : 5803 次/ms/執行緒
  rwsem(寫)      1 執行緒  100 ms : 7286 次/ms/執行緒
  rwsem(讀)      4 執行緒  100 ms : 2307 次/ms/執行緒   ← 總計 922162 次
  rwsem(寫)      4 執行緒  100 ms : 1976 次/ms/執行緒   ← 總計 790562 次
```

| | 1 執行緒 | 4 執行緒 | 掉幾倍 |
|---|---|---|---|
| **讀者鎖** | 5803 | **2307** | 2.5× |
| **寫者鎖** | 7286 | **1976** | 3.7× |

**讀者鎖在爭用時比寫者鎖多 17% 的吞吐量**，而且掉的倍數比較少（2.5 vs 3.7）。

**⚠ 但差距沒有想像中大** —— 為什麼讀者不是「完美並行、吞吐量不掉」？

因為這個測試的臨界區**極短**（只讀一個 `int`），
所以量到的幾乎全是**「維護讀者計數」本身的成本**：
`down_read()` 要對 `sem->count` 做一次 `atomic_long_add_return_acquire(RWSEM_READER_BIAS)`
—— 那是一條 `LDADDAL`，4 顆 CPU 搶同一條 cache line，一樣會顛簸。

**這正好說明了 [Q27](#q27) 為什麼需要 RCU**：
同一個測試裡 `rcu_read_lock()` 是 **116990 次/ms**，
比讀者鎖快 **50 倍**，因為它連那一次原子加都不用做。

**單執行緒時寫者鎖反而比讀者鎖快**（7286 vs 5803），
因為 `down_write()` 的快速通道是一條 `cmpxchg`（0 → `RWSEM_WRITER_LOCKED`），
而 `down_read()` 除了原子加還要檢查是否有寫者在等 —— 路徑更長。

---

<a name="q26"></a>
## 26. 讀寫信號量使用的自旋等待機制是如何實現的？

### 結論

**rwsem 把所有狀態壓在一個 `atomic_long_t count` 裡，
再用 `owner` 欄位支援樂觀自旋。**

**`count` 的欄位佈局（`kernel/locking/rwsem.c:117`）：**

```
 63                                    8   7  6 5 4 3 2 1 0
┌──────────────────────────────────────┬───┬─┬─┬─┬─┬─┬─┬─┐
│        讀者計數（每個讀者 +256）       │ … │R│ │ │ │H│W│L│
└──────────────────────────────────────┴───┴─┴─┴─┴─┴─┴─┴─┘
                                              │ │ │
   L = bit0  RWSEM_WRITER_LOCKED  寫者持有 ────┘ │ │
   W = bit1  RWSEM_FLAG_WAITERS   等待佇列非空 ──┘ │
   H = bit2  RWSEM_FLAG_HANDOFF   交棒給等待者 ────┘
   R = bit7  RWSEM_FLAG_READFAIL
   bit8+     RWSEM_READER_BIAS = 1 << 8 = 256
```

**加解鎖就是對 `count` 做原子運算：**

```c
static inline void __down_read(struct rw_semaphore *sem)
{
	if (!rwsem_read_trylock(sem, &tmp))       /* atomic_long_add_return(256) */
		rwsem_down_read_slowpath(sem, ...);
}

static inline void __down_write(struct rw_semaphore *sem)
{
	long tmp = RWSEM_UNLOCKED_VALUE;          /* 0 */
	if (!atomic_long_try_cmpxchg_acquire(&sem->count, &tmp,
					     RWSEM_WRITER_LOCKED))
		rwsem_down_write_slowpath(sem, TASK_UNINTERRUPTIBLE);
}
```

**自旋等待機制（`CONFIG_RWSEM_SPIN_ON_OWNER=y`）：**

rwsem 的 `owner` 欄位比 mutex 更巧妙 —— 它**同時要表達「寫者是誰」和「現在是讀者持有」**：

```c
/* kernel/locking/rwsem.c */
#define RWSEM_READER_OWNED	(1UL << 0)   /* 讀者持有（不知道是誰） */
#define RWSEM_NONSPINNABLE	(1UL << 1)   /* 不要對這把鎖做樂觀自旋 */
```

* **寫者持有** → `owner` = 寫者的 `task_struct` 指標（低位是 0）
  → `rwsem_spin_on_owner()` 可以像 mutex 一樣看 `owner->on_cpu` 決定要不要自旋。
* **讀者持有** → `owner` 只設 `RWSEM_READER_OWNED` 位元
  → **不知道有哪些讀者、也不知道它們在不在跑**
  → 只能用 `rwsem_rspin_threshold`（預設 25 µs）做「限時自旋」，超時就去睡。

這是讀寫鎖先天的限制：**讀者可以有很多個，`owner` 欄位存不下**。

**`RWSEM_FLAG_HANDOFF` —— 防寫者餓死：**
等待佇列裡的第一個等待者若等超過 `RWSEM_WAIT_TIMEOUT`（4 ms），
就設這個旗標，之後**所有新來的申請者（含讀者）一律走慢速通道去排隊**，
不准再從快速通道插隊。等佇列頭拿到鎖後再清掉。

> **書目**：奔跑吧 §1.9「讀寫信號量」全節（第 2460~2878 行）。

### 實機驗證

`sync_probe.ko` 對一把真的 rwsem 做操作，每一步印出 `count`：

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/sync_probe.ko; sudo rmmod sync_probe; \
                 sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A12 "Q25\]\[Q26\]"'
```

```
  [Q25][Q26] rwsem 的 count 欄位佈局（kernel/locking/rwsem.c）：
     bit0   RWSEM_WRITER_LOCKED   寫者持有
     bit1   RWSEM_FLAG_WAITERS    等待佇列非空
     bit2   RWSEM_FLAG_HANDOFF    交棒給等待者（防餓死）
     bit7   RWSEM_FLAG_READFAIL
     bit8+  RWSEM_READER_BIAS     每個讀者 +256
     count 初始 = 0
     down_read()  之後 count = 256  (= 1 個 RWSEM_READER_BIAS)
     再 down_read() 之後 count = 512  (= 2 個讀者，讀者可以疊加)
     down_write() 之後 count = 1  (bit0 = RWSEM_WRITER_LOCKED)
     rwsem owner = 0xffff0001049bbe00
```

**逐項驗算：**

| 操作 | `count` | 二進位 | 解讀 |
|---|---|---|---|
| 初始 | 0 | `0000_0000` | 無人持有 |
| `down_read()` | **256** | `1_0000_0000` | 1 × `RWSEM_READER_BIAS`（`1<<8`）✅ |
| 再 `down_read()` | **512** | `10_0000_0000` | 2 個讀者 ✅ **讀者可以疊加** |
| `down_write()` | **1** | `0000_0001` | bit0 = `RWSEM_WRITER_LOCKED` ✅ |

**`down_write()` 之後 `count` 只有 1，而不是 `1 | 某個大數`** ——
證實了「寫者持有時不能有任何讀者」（讀者計數必須是 0，`cmpxchg` 才會從 0 成功換成 1）。

`owner = 0xffff0001049bbe00`（低 3 位是 0）→ 寫者持有，指向 `task_struct`，
所以其他 CPU 可以對它做 `rwsem_spin_on_owner()`。
如果是讀者持有，`owner` 會是 `某個 task | RWSEM_READER_OWNED(bit0)`。

---

<a name="q27"></a>
## 27. RCU 相比讀寫鎖有哪些優勢？

### 結論

**一句話：RCU 的讀者「幾乎零成本」，而讀寫鎖的讀者仍然要做原子操作。**

| | 讀寫鎖 rwsem | RCU |
|---|---|---|
| 讀者要做的事 | `atomic_long_add_return_acquire(256, &count)` + 檢查 | **`preempt_disable()` + `barrier()`**（本機甚至是空操作） |
| 讀者之間 | 搶同一條 cache line → **顛簸** | **完全不互相干擾** |
| 讀者會不會阻塞 | 有寫者時**會**（要睡覺） | **永遠不會** |
| 寫者會不會阻塞讀者 | **會** | **不會**（讀新的或讀舊的都對） |
| 讀者可以在中斷上下文用嗎 | 不行（會睡） | **可以**（`rcu_read_lock()` 不睡眠） |
| 擴展性 | 隨 CPU 數變差 | **線性** |
| 成本轉嫁到誰 | 讀者和寫者都付 | **全部由寫者付** |
| 限制 | 無 | **受保護的資料必須透過指標存取**；寫者要處理「新舊並存」 |

**RCU 的代價：**
1. **寫者變複雜**：要複製一份、改副本、原子換指標、註冊回呼延後釋放。
2. **記憶體用量變高**：舊資料要等一個 GP（本機實測平均 43.7 ms，見 [Q28](#q28)）才能釋放。
3. **讀者可能讀到舊資料**：RCU 只保證「讀到的是一個一致的版本」，
   不保證是最新的。需要強一致就不能用 RCU。
4. **多個寫者之間仍需自己互斥**（通常配一把 spinlock）。

**適用場景**：讀多寫少 + 資料透過指標存取 + 能容忍短暫讀到舊值。
核心裡的典型用戶：`dcache`、路由表、`task_struct` 走訪、模組列表、
`vmap_area_list`、netfilter 規則。

> **書目**：奔跑吧 §1.10 開頭（第 2878~2904 行）——
> 「RCU 機制要實現的目標是，讀者線程沒有同步開銷…不需要額外的鎖，
> 不需要使用原子操作指令和內存屏障指令」。

### 實機驗證 —— 讀者成本差 50 倍

`lock_bench.ko mode=1`（4 執行緒，臨界區都只是讀一個 `int`）：

```
  rwsem(讀)      1 執行緒 : 5803 次/ms/執行緒     4 執行緒 : 2307 次/ms/執行緒
  rcu_read_lock  1 執行緒 : 61531 次/ms/執行緒    4 執行緒 : 116990 次/ms/執行緒
```

| | 1 執行緒 | 4 執行緒 | 擴展性 |
|---|---|---|---|
| **rwsem 讀者鎖** | 5803 | 2307 | **掉 2.5 倍** |
| **RCU 讀者** | 61531 | **116990** | **反而快 1.9 倍** ✅ |
| **倍數差** | 10.6× | **50.7×** | |

**兩個關鍵觀察：**

1. **單執行緒時 RCU 就已經快 10.6 倍** —— 這是「零成本讀者」的直接體現。
   本機 `CONFIG_PREEMPT_RCU=n` 且 `CONFIG_PREEMPT_COUNT=n`，
   所以 `rcu_read_lock()` 展開後**只剩一個編譯屏障，連一條指令都沒有**：
   ```c
   static __always_inline void rcu_read_lock(void)
   {
   	__rcu_read_lock();          /* preempt_disable() → 本機是 barrier() */
   	__acquire(RCU);
   	rcu_lock_acquire(&rcu_lock_map);   /* 需要 LOCKDEP，本機沒開 */
   }
   ```

2. **4 執行緒時每執行緒吞吐量「反而變高」** —— 這不是量測誤差。
   單執行緒版本綁在 CPU0（A55）；4 執行緒版本跑在 CPU0~3，
   4 顆 CPU 完全獨立、沒有任何共享寫入，所以**總吞吐量是 4 倍**，
   而每執行緒的數字受到快取暖機與頻率的影響略高於單執行緒。
   **重點是「加 CPU 不會讓每個人變慢」，這就是線性擴展。**
   對照 spinlock：4 執行緒時每執行緒只剩 559 次/ms（單執行緒 6086），
   **加 CPU 反而讓每個人慢 10.9 倍**。

---

<a name="q28"></a>
## 28. 請解釋靜止狀態和寬限期。

### 結論

**靜止狀態（Quiescent State, QS）**
> 某顆 CPU **確定不在任何 RCU 讀者臨界區裡**的那個瞬間。

在 **不可搶佔 RCU**（本機 `CONFIG_PREEMPT_RCU=n`）上，
因為 `rcu_read_lock()` 就是 `preempt_disable()`，所以只要 CPU 發生下列任一件事，
就代表它已經離開讀者臨界區：

| QS 事件 | 為什麼算 |
|---|---|
| **行程切換（context switch）** | 讀者臨界區內不准睡眠/被搶佔，能切換就代表不在臨界區 |
| **回到使用者空間** | 使用者態不可能在核心的 RCU 讀者臨界區裡 |
| **進入 idle** | 同上 |
| **`cond_resched()`** | 顯式回報（`rcu_all_qs()`） |

在**可搶佔 RCU**（`PREEMPT_RCU=y`）上就不一樣了 —— 讀者臨界區內可以被搶佔，
所以 `rcu_read_lock()` 要真的遞增 `current->rcu_read_lock_nesting`，
QS 的判定也複雜得多。

**寬限期（Grace Period, GP）**
> 從 GP 開始算起，**等到所有 CPU 都至少經歷過一次 QS** 的那段時間。

GP 結束就代表：**所有在 GP 開始前就存在的讀者，現在都已經離開臨界區了。**
於是「GP 開始前被移出資料結構的舊資料」可以安全釋放。

```
        GP 開始                                    GP 結束
          │                                          │
CPU0 ─────┼──[讀者臨界區]──QS──────────────────────── │
CPU1 ─────┼───QS──────[讀者臨界區]──QS─────────────── │
CPU2 ─────┼─────────────────────QS────────────────── │
CPU3 ─────┼──────────[讀者臨界區]───────────QS─────── │
          │                                          │
   writer: list_del_rcu(old)                    call_rcu 的回呼在這之後才跑
           call_rcu(&old->rcu, free_it)         → kfree(old) 一定安全
```

**注意**：GP 結束**不代表**「所有讀者都結束了」，
而是「**GP 開始之前就存在的**讀者都結束了」。GP 開始之後才進入臨界區的新讀者，
一定看得到新資料（因為 `rcu_assign_pointer()` 已經發布了），所以不影響。

> **書目**：奔跑吧 §1.10.2「經典 RCU 和 Tree RCU」開頭（第 3050~3063 行）。

### 實機驗證 —— GP 到底有多長

`rcu_demo.ko mode=1` 實測三種等待方式：

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/rcu_demo.ko mode=1; sudo rmmod rcu_demo; sudo dmesg'
```

```
  (a) synchronize_rcu()：同步等待一個 GP 結束
      第  1 次: 147550 us
      第  2 次:  29847 us
      第  3 次:  39990 us
      第  4 次:  29982 us
      第  5 次:  26660 us
      第  6 次:  16649 us
      第  7 次:  26663 us
      第  8 次:  19989 us
      第  9 次:  79982 us
      第 10 次:  19994 us
      平均 43731 us，最短 16649 us，最長 147550 us

  (b) synchronize_rcu_expedited()：用 IPI 逼所有 CPU 立刻回報靜止狀態
      平均 54 us，最短 41 us，最長 58 us

  (c) call_rcu()：非同步註冊回呼
      call_rcu() 回呼延遲 = 72773 us

  (d) rcu_barrier()：等所有已註冊的回呼都跑完
      rcu_barrier() 花了 2 us
```

| 方式 | 平均延遲 | 相對倍數 |
|---|---|---|
| **`synchronize_rcu()`** | **43.7 ms** | 1× |
| **`synchronize_rcu_expedited()`** | **54 µs** | **快 810 倍** |
| `call_rcu()` 回呼延遲 | 72.8 ms | 1.7× |

**怎麼解讀：**

1. **一個 GP 大約 16~150 ms** —— 這不是「等資料被釋放」的時間，
   而是「等 8 顆 CPU 各回報一次 QS」的時間。
   最短的 16.6 ms ≈ **5 個時鐘節拍**（HZ=300，每 3.33 ms 一次），
   因為 QS 主要靠 `rcu_sched_clock_irq()` 在時鐘中斷裡偵測。
2. **expedited 版本快 810 倍**，因為它對每顆 CPU 送 IPI 強迫立刻回報，
   不用等時鐘節拍。代價是干擾所有 CPU、傷害實時性，
   所以只用在 CPU 熱插拔、模組卸載這類關鍵路徑
   （`/sys/kernel/rcu_expedited` 可以全域開啟，但不建議）。
3. **`call_rcu()` 的回呼延遲（72.8 ms）比 `synchronize_rcu()` 還長**，
   因為回呼要等「GP 結束」**再加上**「RCU 軟中斷批次處理到它」。
   這是正常的 —— `call_rcu()` 換來的是「呼叫者立刻返回」。
4. **`rcu_barrier()` 只花 2 µs**，因為前面那個回呼早就跑完了，
   佇列是空的。如果佇列裡還有回呼，它會等到全部跑完
   —— **模組卸載前一定要呼叫它**，否則回呼跑到一半模組被卸載會 oops。

---

<a name="q29"></a>
## 29. 請簡述 RCU 實現的基本原理。

### 結論

**RCU = Read-Copy-Update，三個字就是三個步驟：**

```
   ┌─ Read ──────────────────────────────────────────────┐
   │  rcu_read_lock();                                    │
   │  p = rcu_dereference(gp);   ← 取得目前版本的指標       │
   │  ...使用 p...                                        │
   │  rcu_read_unlock();                                  │
   │  （讀者永遠不阻塞、不重試、不做原子操作）              │
   └─────────────────────────────────────────────────────┘

   ┌─ Copy + Update ─────────────────────────────────────┐
   │  spin_lock(&writer_lock);        ← 寫者之間要自己互斥 │
   │  old = rcu_dereference_protected(gp, 1);            │
   │  new = kmalloc(...);                                │
   │  *new = *old;                    ← ★ Copy           │
   │  new->field = value;             ← ★ Update 副本     │
   │  rcu_assign_pointer(gp, new);    ← ★ 原子發布新版本   │
   │  spin_unlock(&writer_lock);                         │
   │  call_rcu(&old->rcu, free_it);   ← 延後釋放舊版本     │
   └─────────────────────────────────────────────────────┘
```

**關鍵在「新舊並存」**：`rcu_assign_pointer()` 之後，

* 還沒讀指標的讀者 → 讀到 **new**
* 已經讀了指標的讀者 → 手上還是 **old**，繼續用是安全的

所以只要等到「所有拿著 old 的讀者都離開」（= 一個 GP），就能釋放 old。

**兩個關鍵巨集其實是屏障：**

```c
#define rcu_assign_pointer(p, v)   smp_store_release(&(p), (v))
        /* release：保證 *new 的初始化在指標發布之前完成 */

#define rcu_dereference(p)         READ_ONCE(p)  + 依賴屏障
        /* 保證讀到指標之後才讀它指向的內容（防編譯器/Alpha 亂序） */
```

**核心 API 對照：**

| API | 作用 |
|---|---|
| `rcu_read_lock()` / `rcu_read_unlock()` | 標示讀者臨界區 |
| `rcu_dereference()` | 讀者取指標 |
| `rcu_assign_pointer()` | 寫者發布指標 |
| `synchronize_rcu()` | **同步**等一個 GP（會睡眠） |
| `call_rcu(head, func)` | **非同步**註冊回呼，立刻返回 |
| `kfree_rcu(ptr, rcu_field)` | `call_rcu` 的簡化版，直接 kfree |
| `rcu_barrier()` | 等所有回呼跑完（模組卸載必備） |
| `list_add_rcu()` / `list_del_rcu()` / `list_for_each_entry_rcu()` | RCU 版的鏈結串列操作 |

> **書目**：奔跑吧 §1.10.1「關於 RCU 的一個簡單例子」（第 2904~3050 行）。

### 實機驗證 —— 書上的例子完整跑通

`rcu_demo.ko mode=0` 就是書上 §1.10.1 那段程式碼（加了自動結束）：

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/rcu_demo.ko mode=0 rounds=3; \
                 sudo rmmod rcu_demo; sudo dmesg'
```

```
  CONFIG_PREEMPT_RCU = n   <-- n 表示「不可搶佔 RCU」，
      靜止狀態 = 行程切換 / 進 idle / 回到使用者態 / cond_resched()
      也因此 rcu_read_lock() 幾乎零成本（見 Q27）
  CONFIG_TREE_RCU = y，GP kthread 叫做 "rcu_sched"
  [reader ] CPU0 讀到 a = 0
  [reader ] CPU0 讀到 a = 0
  [writer ] CPU1 準備把 a 從 0 改成 5（先複製再改）
  [writer ] rcu_assign_pointer() 已發布；call_rcu() 已註冊，但舊資料還沒被釋放
  [reader ] CPU0 讀到 a = 5                          ← ★ 讀者立刻看到新值
  [callback] 舊資料 a=0 現在才真正被釋放（GP 結束後）  ← ★ 舊值延後才釋放
  [reader ] CPU3 讀到 a = 5
  [writer ] CPU1 準備把 a 從 5 改成 6（先複製再改）
  [writer ] rcu_assign_pointer() 已發布；call_rcu() 已註冊，但舊資料還沒被釋放
  [callback] 舊資料 a=5 現在才真正被釋放（GP 結束後）
  [reader ] CPU3 讀到 a = 6
  [reader ] CPU3 讀到 a = 6
  [writer ] CPU1 準備把 a 從 6 改成 7（先複製再改）

  共回收了 2 份舊資料
######## done ########
  [callback] 舊資料 a=6 現在才真正被釋放（GP 結束後）    ← ★ 在 rmmod 期間才跑完
bye
```

**三件事一次看清楚：**

1. **`rcu_assign_pointer()` 一發布，下一個讀者就看到新值**（a=5）。
2. **`call_rcu()` 註冊之後，「舊資料還沒被釋放」的訊息立刻印出來**
   —— 寫者沒有等待，直接繼續跑。
3. **`[callback]` 的訊息永遠比對應的 `[writer]` 晚一大截**，
   最後一個甚至跑到 `rmmod` 期間才被呼叫
   —— 這正是為什麼 `module_exit` 裡一定要 `rcu_barrier()`。

**`CONFIG_PREEMPT_RCU = n` 的直接證據**：GP kthread 叫 `rcu_sched`。

```bash
ssh radxa@$HOST 'ps -eo pid,comm | grep -E "rcu_(sched|preempt|gp)"'
```
```
      3 rcu_gp
      4 rcu_par_gp
     14 rcu_sched          ← ★ 若是可搶佔 RCU，這裡會叫 rcu_preempt
```

---

<a name="q30"></a>
## 30. 在大型系統中，經典 RCU 遇到了什麼問題？Tree RCU 又是如何解決該問題的？

### 結論

**經典 RCU（2.6.29 之前）的問題：全域 cpumask 位圖的鎖爭用。**

```
              ┌────────────────────────────────────┐
              │  全域 rcu_state.cpumask（1024 位元） │
              │  + 一把全域 spinlock                │
              └──────────────┬─────────────────────┘
                             │  1024 顆 CPU 全部搶這一把鎖
     ┌────────┬────────┬─────┴───┬────────┬────────┐
   CPU0     CPU1     CPU2      ...      CPU1022  CPU1023
```
* 每顆 CPU 在 GP 開始時要設自己的位元、經歷 QS 後要清掉，
  都得先拿那把全域鎖。
* **鎖爭用隨 CPU 數線性惡化**。在 1024~4096 核的系統上完全無法接受。

**Tree RCU（2.6.29，Paul McKenney）的解法：把位圖組織成樹。**

```
                      ┌─────────────┐
       Level 0        │ 根 rcu_node  │  qsmask 只有 2 位元
                      │  (node 0)    │  只有 2 個「子節點」在搶這把鎖
                      └──────┬───────┘
                    ┌────────┴────────┐
       Level 1  ┌───▼────┐       ┌────▼───┐
                │ node 1 │       │ node 2 │  各自 2 位元、各自一把鎖
                └──┬──┬──┘       └──┬──┬──┘
                CPU0 CPU1        CPU2 CPU3
```

**規則**：
* 每顆 CPU 只跟自己的**葉節點**打交道 → 每把鎖只被 `RCU_FANOUT_LEAF` 顆 CPU 爭用。
* 葉節點的 `qsmask` 全部清空時，**才由「最後清掉的那顆 CPU」**往上一層去清。
* 一路清到根節點 → GP 結束。

書上的比喻很傳神：「這類似於足球比賽，進入四強的 4 支球隊被分成上下半區，
只有半決賽獲勝的球隊才能進入決賽。」

**效果**：鎖爭用從 `O(nr_cpus)` 降到 `O(RCU_FANOUT_LEAF)`，
樹的高度是 `log_FANOUT(nr_cpus)`，即使 4096 核也只要 2~3 層。

> **書目**：奔跑吧 §1.10.2（第 3050~3094 行）與圖 1.22、圖 1.23。

### 實機驗證

**(a) 本機的樹只有一層 —— 因為 CPU 太少**

```bash
ssh radxa@$HOST 'zcat /proc/config.gz | grep -E "RCU_FANOUT"'
ssh radxa@$HOST 'dmesg | grep -i "Hierarchical RCU"'
```
```
CONFIG_RCU_FANOUT=64
CONFIG_RCU_FANOUT_LEAF=16
[   12.918323] rcu: Hierarchical RCU implementation.
```

`RCU_FANOUT_LEAF = 16` **大於**本機的 8 顆 CPU，
所以 8 顆 CPU 全部掛在**同一個葉節點**下，樹退化成**單一節點**
（葉節點就是根節點）。dmesg 也沒有出現
`Adjusting geometry for rcu_fanout_leaf=...` 這類重建幾何的訊息。

**若要看到真正的多層樹**，可以用開機參數 `rcutree.rcu_fanout_leaf=2`，
或 `rcutree.dump_tree=1` 讓核心開機時把樹印出來。

**(b) ★ 位圖清除的過程 —— 直接用 tracepoint 抓到**

```bash
ssh radxa@$HOST 'sudo bash -c "
T=/sys/kernel/debug/tracing
echo 1 > \$T/events/rcu/rcu_grace_period/enable
echo 1 > \$T/events/rcu/rcu_quiescent_state_report/enable
echo 1 > \$T/tracing_on
insmod ~/exp/sync/rcu_demo.ko mode=1 & sleep 3; kill %1; sleep 12; rmmod rcu_demo
echo 0 > \$T/tracing_on
grep quiescent \$T/trace | head"'
```

```
<idle>-0   [003] d.s.. 591.420477: rcu_quiescent_state_report: rcu_sched 21809 8>f7 0 0 7 0
<idle>-0   [001] d.s.. 591.420482: rcu_quiescent_state_report: rcu_sched 21809 2>f5 0 0 7 0
insmod-4383[000] d.s.. 591.420487: rcu_quiescent_state_report: rcu_sched 21809 1>f4 0 0 7 0
rcu_sched-14[001] d.... 591.420496: rcu_quiescent_state_report: rcu_sched 21809 f4>0 0 0 7 0
```

**這四行就是 `rcu_node->qsmask` 位圖被一步步清空的全過程：**

| 事件 | `mask>qsmask` | 二進位 | 誰回報了 |
|---|---|---|---|
| CPU3 回報 QS | `8>f7` | `0xff` → `0xf7` | 清掉 bit3（`0x8`） |
| CPU1 回報 QS | `2>f5` | `0xf7` → `0xf5` | 清掉 bit1（`0x2`） |
| CPU0 回報 QS | `1>f4` | `0xf5` → `0xf4` | 清掉 bit0（`0x1`） |
| **剩下的一次清完** | **`f4>0`** | `0xf4` → **`0`** | **qsmask 歸零 → GP 21809 可以結束** |

* **初始 `qsmask = 0xff`** —— 正好是 8 顆 CPU 每顆一個位元，
  再次證實**本機是單節點**（所有 CPU 掛在同一個 `rcu_node` 上）。
* `21809` 是 **GP 序號**（`gp_seq`）。
* 最後一筆由 **`rcu_sched` kthread（pid 14）** 一次把剩下的 `0xf4` 清光
  —— 那是 `force_quiescent_state()` 掃描到那些長時間 idle 的 CPU，
  替它們回報 QS。

**如果本機有 1024 顆 CPU 而且是經典 RCU**，這個 `qsmask` 就會是 1024 位元、
配一把全域鎖，上面這四筆記錄會變成 1024 次全域鎖爭用 —— 這就是 Tree RCU 要解決的問題。

---

<a name="q31"></a>
## 31. 在 RCU 實現中，為什麼要使用 ULONG_CMP_GE() 和 ULONG_CMP_LT() 宏來比較兩個數的大小，而不直接使用大於號或者小於號來比較？

### 結論

**因為 GP 序號（`gp_seq`）是會「迴繞（wrap around）」的 `unsigned long`。**

```c
/* include/linux/rcupdate.h:35 */
#define ULONG_CMP_GE(a, b)	(ULONG_MAX / 2 >= (a) - (b))
#define ULONG_CMP_LT(a, b)	(ULONG_MAX / 2 <  (a) - (b))
```

**原理**：無號減法本身就會自然迴繞，所以 `a - b` 得到的是
「a 在 b 之後多遠」這個**環狀距離**。
只要兩個序號的實際差距**不超過半圈**（`ULONG_MAX/2`），
用「差值是否 ≤ 半圈」就能正確判斷誰在前誰在後 —— 即使跨越了 0 也一樣。

```
        序號空間是一個環（0 ~ ULONG_MAX）

              0
        ┌─────●─────┐
        │  b=3      │
  ULONG │           │  a - b = 一小段（往前繞）→ a 在 b 之前
  MAX ──●           │
        │ a=MAX-2   │
        └───────────┘

  直接比：a(18446744073709551613) > b(3)  → 錯！以為 a 比較新
  用巨集：ULONG_CMP_LT(a, b) 為真         → 對！a 在 b 之前
```

**為什麼直接用 `<` `>` 會錯？**
`gp_seq` 只增不減，總有一天會從 `ULONG_MAX` 繞回 0。
繞回之後，舊序號（很大）會被誤判成「比新序號（很小）還新」，
於是 RCU 會以為「這個 GP 早就結束了」而**提前釋放還在被讀取的資料** → 崩潰。

**在 32 位元系統上這不是理論問題**：
`gp_seq` 的低 2 位是 GP 狀態（`RCU_SEQ_STATE_MASK`），
所以每個 GP 讓 `gp_seq` 增加 4。
32 位元 → `2^32 / 4 = 2^30` 個 GP 會繞一圈。
以本機實測的 GP 長度 43.7 ms 計算：`2^30 × 43.7 ms ≈ 543 天`；
但若系統負載重、GP 短到 5 ms，就只剩 **62 天** —— 伺服器很容易跑到。

**同一族的巨集：**
```c
#define ULONG_CMP_GE(a, b)	(ULONG_MAX / 2 >= (a) - (b))   /* a >= b */
#define ULONG_CMP_LT(a, b)	(ULONG_MAX / 2 <  (a) - (b))   /* a <  b */
```
核心裡同樣手法的還有 `time_after()` / `time_before()`（比較 `jiffies`）：
```c
#define time_after(a,b)		((long)((b) - (a)) < 0)
```
—— 用**有號**減法達到一樣的效果。

> **書目**：本題在書上第 1 章題目列表中（第 31 題），
> 對應 `include/linux/rcupdate.h` 的定義。

### 實機驗證

`sync_probe.ko` 直接在核心裡跑一次迴繞情境：

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/sync_probe.ko; sudo rmmod sync_probe; \
                 sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A16 "Q31"'
```

```
== Q31：為什麼 RCU 要用 ULONG_CMP_GE()/ULONG_CMP_LT() ==
  定義（include/linux/rcupdate.h）：
     #define ULONG_CMP_GE(a, b)  (ULONG_MAX / 2 >= (a) - (b))
     #define ULONG_CMP_LT(a, b)  (ULONG_MAX / 2 <  (a) - (b))

  情境：GP 序號即將迴繞。a = 18446744073709551613 (ULONG_MAX-2)，b = 3
    直接比大小 :  a > b  = 1   <-- ★ 錯！以為舊的比新的大
    ULONG_CMP_LT(a, b) = 1   <-- 正確：a 在 b 之前
    ULONG_CMP_GE(b, a) = 1   <-- 正確：b 在 a 之後
    a - b = 18446744073709551610（無號減法自然迴繞），ULONG_MAX/2 = 9223372036854775807

  一般情況（沒有迴繞）兩者結果一致：a=100, b=50
    a > b = 1，ULONG_CMP_GE(a, b) = 1
```

**逐項驗算：**

| 判斷 | 結果 | 對不對 |
|---|---|---|
| `a > b`（直接比） | **1** | ❌ **錯**：`a = ULONG_MAX-2` 是**舊**序號，`b = 3` 是繞回後的**新**序號 |
| `ULONG_CMP_LT(a, b)` | **1** | ✅ 正確：a 在 b 之前 |
| `ULONG_CMP_GE(b, a)` | **1** | ✅ 正確：b 在 a 之後 |

驗算 `ULONG_CMP_LT(a,b)`：
```
a - b = (ULONG_MAX - 2) - 3 = ULONG_MAX - 5 = 18446744073709551610
ULONG_MAX/2                                =  9223372036854775807
18446744073709551610 > 9223372036854775807  →  ULONG_CMP_LT 為真 ✅
```

而**沒有迴繞的一般情況**（a=100, b=50）兩種寫法結果一致
—— 所以巨集是「安全的超集」，任何時候都可以放心用。

---

<a name="q32"></a>
## 32. 請簡述一個寬限期的生命週期及其狀態機的變化。

### 結論

**一個 GP 的生命週期由 `rcu_sched` 這個 GP kthread 驅動，
在 `rcu_gp_kthread()` 的無窮迴圈裡走三個階段：**

```
   ┌────────────────────────────────────────────────────────────┐
   │  ① 初始化（rcu_gp_init）                                    │
   │     - gp_seq += 1（進入 "GP 進行中" 狀態）                   │
   │     - 走訪 rcu_node 樹，把每個節點的 qsmask 設成             │
   │       「這個節點底下所有 online CPU」                        │
   │     - tracepoint: "start" / "cpustart"                     │
   └───────────────────────┬────────────────────────────────────┘
                           ▼
   ┌────────────────────────────────────────────────────────────┐
   │  ② 等待 QS（rcu_gp_fqs_loop）                               │
   │     - 睡在 gp_wq 上，等所有 CPU 回報                         │
   │     - 每顆 CPU 在時鐘中斷裡呼叫 rcu_sched_clock_irq()        │
   │       發現自己經歷了 QS → rcu_report_qs_rdp()               │
   │       → 清掉葉節點 qsmask 的自己那個位元                     │
   │     - 葉節點 qsmask 清空 → 往上一層清（Tree RCU，見 Q30）    │
   │     - 逾時（jiffies_till_first_fqs）→ force_quiescent_state │
   │       主動掃描還沒回報的 CPU（idle 的直接代為回報）           │
   │     - tracepoint: "cpuqs" / "fqsstart" / "fqsend" / "fqswait"│
   └───────────────────────┬────────────────────────────────────┘
                           ▼
   ┌────────────────────────────────────────────────────────────┐
   │  ③ 收尾（rcu_gp_cleanup）                                   │
   │     - 根節點 qsmask == 0 → GP 結束                          │
   │     - gp_seq += 1（回到 "GP 結束" 狀態）                     │
   │     - 把每顆 CPU 上「等這個 GP」的回呼標記成可執行            │
   │       （AccWaitCB → AccReadyCB）                            │
   │     - 有人排隊要新 GP → 立刻開始下一個                       │
   │     - tracepoint: "cpuend" / "end" / "newreq" / "reqwait"   │
   └────────────────────────────────────────────────────────────┘
                           ▼
              RCU_SOFTIRQ / rcu_core() 批次呼叫回呼
              tracepoint: rcu_batch_start / rcu_invoke_callback / rcu_batch_end
```

**`gp_seq` 的編碼**：低 2 位是狀態（`RCU_SEQ_STATE_MASK`），
高位是序號。所以「GP 開始」和「GP 結束」各讓 `gp_seq` 加 1，
**一個完整的 GP 讓 `gp_seq` 增加 4**（相鄰兩個 GP 的序號差 4）。

> **書目**：奔跑吧 §1.10.2；狀態名稱見 `include/trace/events/rcu.h` 的
> `TRACE_EVENT(rcu_grace_period)` 註解。

### 實機驗證 —— 用 ftrace 抓到完整狀態機

```bash
ssh radxa@$HOST 'sudo bash -c "
T=/sys/kernel/debug/tracing
echo 0 > \$T/events/enable; echo > \$T/trace
echo 1 > \$T/events/rcu/rcu_grace_period/enable
echo 1 > \$T/events/rcu/rcu_quiescent_state_report/enable
echo 1 > \$T/tracing_on
insmod ~/exp/sync/rcu_demo.ko mode=1 >/dev/null 2>&1 &
sleep 3; kill %1; sleep 12; rmmod rcu_demo 2>/dev/null
echo 0 > \$T/tracing_on
grep -E \"rcu_grace_period|quiescent\" \$T/trace | head -22"'
```

```
   <idle>-0    [003] dN... 591.419022: rcu_grace_period: rcu_sched 21809 cpuqs
   <idle>-0    [003] d.s.. 591.420474: rcu_grace_period: rcu_sched 21816 AccWaitCB
   <idle>-0    [003] d.s.. 591.420477: rcu_quiescent_state_report: rcu_sched 21809 8>f7 0 0 7 0
   <idle>-0    [001] d.s.. 591.420480: rcu_grace_period: rcu_sched 21816 AccWaitCB
   <idle>-0    [001] d.s.. 591.420482: rcu_quiescent_state_report: rcu_sched 21809 2>f5 0 0 7 0
   insmod-4383 [000] d.s.. 591.420485: rcu_grace_period: rcu_sched 21816 AccWaitCB
   insmod-4383 [000] d.s.. 591.420487: rcu_quiescent_state_report: rcu_sched 21809 1>f4 0 0 7 0
rcu_sched-14   [001] ..... 591.420494: rcu_grace_period: rcu_sched 21809 fqsstart
rcu_sched-14   [001] d.... 591.420496: rcu_quiescent_state_report: rcu_sched 21809 f4>0 0 0 7 0
rcu_sched-14   [001] ..... 591.420498: rcu_grace_period: rcu_sched 21809 fqsend
rcu_sched-14   [001] ..... 591.420498: rcu_grace_period: rcu_sched 21809 fqswait
rcu_sched-14   [001] d.... 591.420500: rcu_grace_period: rcu_sched 21816 AccWaitCB
rcu_sched-14   [001] d.... 591.420500: rcu_grace_period: rcu_sched 21809 cpuend
rcu_sched-14   [001] d.... 591.420501: rcu_grace_period: rcu_sched 21809 end        ★ GP 21809 結束
rcu_sched-14   [001] d.... 591.420503: rcu_grace_period: rcu_sched 21816 AccWaitCB
rcu_sched-14   [001] d.... 591.420503: rcu_grace_period: rcu_sched 21812 newreq     ★ 有人要新 GP
rcu_sched-14   [001] ..... 591.420504: rcu_grace_period: rcu_sched 21812 reqwait
rcu_sched-14   [001] d.... 591.420505: rcu_grace_period: rcu_sched 21813 start      ★ GP 21813 開始
rcu_sched-14   [001] d.... 591.420506: rcu_grace_period: rcu_sched 21820 AccWaitCB
rcu_sched-14   [001] d.... 591.420507: rcu_grace_period: rcu_sched 21813 cpustart
rcu_sched-14   [001] ..... 591.420507: rcu_grace_period: rcu_sched 21813 fqswait
rcu_sched-14   [001] d.... 591.420509: rcu_grace_period: rcu_sched 21813 cpuqs
```

**逐段對照狀態機：**

| trace 事件 | 對應階段 | 說明 |
|---|---|---|
| `21809 cpuqs` | ② | 某顆 CPU 在本地記下「我經歷 QS 了」 |
| `21809 8>f7`、`2>f5`、`1>f4` | ② | CPU3/CPU1/CPU0 依序清掉 `qsmask` 的位元（見 [Q30](#q30)） |
| `21809 fqsstart` → `f4>0` → `fqsend` | ② | **`force_quiescent_state`** 掃描，替 idle 的 CPU 一次清光剩下的 `0xf4` |
| `21809 fqswait` | ② | 回去睡，等下一輪 |
| `21809 cpuend` | ③ | 這顆 CPU 對此 GP 的處理結束 |
| **`21809 end`** | ③ | **GP 21809 正式結束** |
| `21812 newreq` | ③ | 有人（`call_rcu`/`synchronize_rcu`）要求新的 GP |
| `21812 reqwait` | ③ | GP kthread 等待請求確立 |
| **`21813 start`** | ① | **GP 21813 開始** |
| `21813 cpustart` | ① | 各 CPU 開始參與新 GP |
| `21813 fqswait` / `cpuqs` | ② | 又回到等待階段 |
| `21816 / 21820 AccWaitCB` | — | 「把回呼掛到某個未來的 GP 上」（Accelerate Wait CallBack） |

**兩個可以直接讀出來的細節：**

1. **`21809 → 21813`，序號差 4** —— 印證「一個完整 GP 讓 `gp_seq` 加 4」
   （低 2 位是狀態，開始 +1、結束 +1，再加上下一個 GP 的開始）。
2. **從 `end` 到下一個 `start` 只花了 4 µs**（591.420501 → 591.420505）
   —— GP 之間沒有空窗，只要有人排隊就立刻接著開始。
   所以 [Q28](#q28) 量到的 43.7 ms **幾乎全是「等 8 顆 CPU 回報 QS」的時間**，
   而不是 GP 之間的間隔。

---

<a name="q33"></a>
## 33. 請闡述原子操作、自旋鎖、信號量、互斥鎖以及 RCU 的特點和使用規則。

### 結論（總表）

| | **原子操作** | **自旋鎖** | **信號量** | **互斥鎖** | **RCU** |
|---|---|---|---|---|---|
| **保護對象** | 單一變數/位元 | 任意臨界區 | 任意臨界區 | 任意臨界區 | **指標可達的資料結構** |
| **等待方式** | 不等（單指令） | 忙等（自旋） | 睡眠 | **樂觀自旋 → 睡眠** | 讀者不等 |
| **中斷上下文可用** | ✅ | ✅ | ❌ | ❌ | ✅（讀者） |
| **臨界區可睡眠** | — | ❌ | ✅ | ✅ | ❌（讀者） |
| **同時持有者** | — | 1 | **N（count）** | 1 | 讀者無限 + 寫者互斥 |
| **誰能釋放** | — | 慣例上是持有者 | **任何人** | **只有持有者** | — |
| **有 owner 記錄** | — | ❌ | ❌ | ✅ | — |
| **可遞迴** | — | ❌ 死鎖 | ✅（count 夠） | ❌ 死鎖 | ✅ 讀者可巢狀 |
| **大小（本機實測）** | 4 B | **4 B**（`arch_spinlock_t`） | 48 B | 56 B | 0（無鎖物件） |
| **無爭用成本**（實測，次/ms） | 14577~66572 | **6086** | — | 4284 | **61531** |
| **4 執行緒爭用**（實測） | 7678 | **559** | — | 1483 | **116990** |

### 使用規則速查

```
                 ┌──────────────────────────────────┐
                 │ 要保護的是什麼？                  │
                 └────┬──────────────┬──────────────┘
       單一變數/計數器 │              │ 一段程式碼
                      ▼              ▼
              ┌──────────────┐  ┌────────────────────────────┐
              │  原子操作     │  │ 讀者遠多於寫者、且透過指標？ │
              │ atomic_t     │  └──┬──────────────────┬──────┘
              │ bitops       │  是 │                  │ 否
              └──────────────┘     ▼                  ▼
                              ┌────────┐    ┌───────────────────┐
                              │  RCU   │    │ 臨界區會睡眠嗎？   │
                              └────────┘    └──┬─────────┬──────┘
                                            否 │         │ 是
                                               ▼         ▼
                                  ┌──────────────────┐ ┌──────────────┐
                                  │ 在中斷上下文？    │ │ count>1 或    │
                                  └──┬──────────┬────┘ │ 跨上下文解鎖？ │
                                  是 │          │ 否   └──┬────────┬──┘
                                     ▼          ▼      是 │        │ 否
                          spin_lock_irqsave  spin_lock    ▼        ▼
                                                     semaphore   mutex
```

**五條鐵律：**

1. **中斷上下文只能用自旋鎖或原子操作**（RCU 讀者也可以）。
2. **自旋鎖臨界區內不准睡眠**（[Q9](#q9)），
   包括 `kmalloc(GFP_KERNEL)`、`copy_from_user()` 這些隱含睡眠。
3. **和中斷處理常式共用的鎖，一律用 `spin_lock_irqsave()`**（[Q13](#q13)）。
4. **互斥鎖只能由持有者解鎖**，且不可遞迴。
5. **RCU 保護的資料必須透過指標存取**，且寫者要處理「新舊並存」。

### 鎖的層次順序（避免 ABBA 死鎖）

書上 §1.11 引用的 `mm/rmap.c` 註解，**必須由外而內依序取得**：

```
inode->i_rwsem            （寫入或截斷時）
  mm->mmap_lock                          ← 書上寫 mmap_sem，5.8 改名
    page->flags PG_locked (lock_page)
      mapping->i_mmap_rwsem
        anon_vma->rwsem
          mm->page_table_lock or pte_lock
            lruvec->lru_lock             ← 書上寫 zone->lru_lock，5.7 改名
            swap_lock
              mmlist_lock
            mapping->private_lock
              inode->i_lock
                bdi.wb->list_lock
                  sb_lock
              mapping->i_pages（xarray）  ← 書上寫 mapping->tree_lock
```

**這種「鎖的全域順序」是核心避免死鎖的標準做法**，
`CONFIG_PROVE_LOCKING`（lockdep）就是用來自動檢查有沒有人違反順序的。
本機沒開 lockdep（見 [Q11](#q11)），所以違反順序不會有警告，只會偶發死鎖。

> **書目**：奔跑吧 §1.11 開頭的表 1.5 與 `mm/rmap.c` 的鎖順序註解（第 3094~3140 行）。

### 實機驗證

`lock_bench.ko mode=1` 一次量完五種機制（4 執行緒，臨界區都極短）：

```
  --- 單執行緒（無爭用，量的是快速通道成本）---
  spinlock        1 執行緒  100 ms :    6086 次/ms/執行緒
  mutex           1 執行緒  100 ms :    4284 次/ms/執行緒
  rwsem(讀)      1 執行緒  100 ms :    5803 次/ms/執行緒
  rwsem(寫)      1 執行緒  100 ms :    7286 次/ms/執行緒
  atomic_inc      1 執行緒  100 ms :   14577 次/ms/執行緒  †
  rcu_read_lock   1 執行緒  100 ms :   61531 次/ms/執行緒

  --- 4 執行緒（有爭用）---
  spinlock        4 執行緒  100 ms :     559 次/ms/執行緒
  mutex           4 執行緒  100 ms :    1483 次/ms/執行緒
  rwsem(讀)      4 執行緒  100 ms :    2307 次/ms/執行緒
  rwsem(寫)      4 執行緒  100 ms :    1976 次/ms/執行緒
  atomic_inc      4 執行緒  100 ms :    7678 次/ms/執行緒
  rcu_read_lock   4 執行緒  100 ms :  116990 次/ms/執行緒
```

† `atomic_inc` 單執行緒呈雙峰（三次量到 14577 / 14627 / 66572），
與模組載入時 `bl_atomic` 落在哪條 cache line 有關；4 執行緒的數字非常穩定。

**排序（4 執行緒爭用時，由快到慢）：**
```
RCU 讀者 116990  ≫  atomic 7678  >  rwsem讀 2307  >  rwsem寫 1976  >  mutex 1483  >  spinlock 559
   209×              13.7×          4.1×           3.5×             2.7×            1×
```

**這張表就是「該選哪個」最直接的依據**：
能用 RCU 就用 RCU，能用原子操作就別上鎖，
自旋鎖在爭用時是最貴的（但它是唯一能在中斷上下文用又不睡眠的通用鎖）。

---

<a name="q34"></a>
## 34. 在 KSM 中掃描某個 VMA 以尋找有效的匿名頁面時，假設此 VMA 恰巧被其他 CPU 銷毀了，會不會有問題呢？

### 結論

**不會有問題，因為兩邊都用 `mm->mmap_lock` 這把讀寫信號量保護，
而且一個拿讀者鎖、一個拿寫者鎖，天生互斥。**

```
時間 →

CPU0 (ksmd)  ──[mmap_read_lock(mm)]──掃描 VMA、follow_page()──[mmap_read_unlock]──
                      T0                                             T2
                       │                                              │
CPU1 (do_munmap)  ─────┼──[mmap_write_lock(mm)]──★ 卡在這裡等 ────────┴─→ 拿到寫者鎖
                       T1                                                 才開始銷毀 VMA
```

* **T0**：ksmd 呼叫 `mmap_read_lock(mm)` 拿到**讀者鎖**，開始掃描。
* **T1**：另一顆 CPU 上的行程呼叫 `munmap()`，
  它必須先 `mmap_write_lock(mm)` 拿**寫者鎖** —— 但讀者還在，**只能排隊等**。
* **T2**：ksmd 掃描完 `mmap_read_unlock()`，寫者才拿到鎖、才開始拆 VMA。

**所以 VMA 在 ksmd 掃描期間絕對不會消失。**

**為什麼 ksmd 只需要讀者鎖？** 因為它**不修改 VMA 本身**
（只是走訪紅黑樹、讀 `vma->vm_start/vm_end/vm_flags`、對頁面做合併）。
用讀者鎖讓多個掃描者/缺頁處理可以並行，這正是 [Q25](#q25) 的判斷準則。

**核心裡的實際程式碼**（`mm/ksm.c` 的 `scan_get_next_rmap_item()`）：
```c
mmap_read_lock(mm);
if (ksm_test_exit(mm))
	goto no_vmas;
for_each_vma(vmi, vma) {
	...
	*page = follow_page(vma, ksm_scan.address, FOLL_GET);
	...
}
mmap_read_unlock(mm);
```

而所有會銷毀 VMA 的路徑都拿寫者鎖：
```c
/* mm/mmap.c */
SYSCALL_DEFINE2(munmap, ...) { ... mmap_write_lock_killable(mm); ... }
SYSCALL_DEFINE1(brk, ...)    { ... mmap_write_lock_killable(mm); ... }
exit_mmap()                  { ... mmap_write_lock(mm); ... }
```

**⚠ 還有第二道保險**：ksmd 在拿 `mmap_lock` 之前，
會先用 `mmget_not_zero(mm)` 增加 `mm_users` 參考計數，
確保整個 `mm_struct` 本身不會在它手上被釋放。

> **書目**：奔跑吧 §1.11.1「mm->mmap_sem」與圖 1.24、圖 1.25（第 3175~3239 行）。
> **修正**：`mm->mmap_sem` 在 **Linux 5.8 改名為 `mm->mmap_lock`**
> （commit `da1c55f1b272`），並包成 `mmap_read_lock()` / `mmap_write_lock()` 等 API，
> 目的是為將來換成別的鎖型別（如 per-VMA lock）預留空間。

### 實機驗證

```bash
# 本機確實是 mmap_lock 而不是 mmap_sem
ssh radxa@$HOST 'cd /lib/modules/$(uname -r)/build
  grep -n "mmap_lock\|mmap_sem" include/linux/mm_types.h | head -3
  echo "--- ksm 用讀者鎖："; grep -n "mmap_read_lock\|mmap_write_lock" mm/ksm.c | head -5
  echo "--- munmap 用寫者鎖："; grep -n "mmap_write_lock" mm/mmap.c | head -5'
```

`sync_probe.ko` 量到的 rwsem 行為（[Q26](#q26)）就是這把鎖的機制：

```
     down_read()  之後 count = 256  (= 1 個 RWSEM_READER_BIAS)
     再 down_read() 之後 count = 512  (= 2 個讀者，讀者可以疊加)
     down_write() 之後 count = 1  (bit0 = RWSEM_WRITER_LOCKED)
```

**`down_write()` 的快速通道是 `cmpxchg(count, 0, RWSEM_WRITER_LOCKED)`
—— `count` 必須是 0（沒有任何讀者）才會成功。**
這就是「ksmd 持有讀者鎖時，`munmap` 的寫者鎖一定拿不到」在程式碼層面的保證。

---

<a name="q35"></a>
## 35. 請簡述 PG_locked 的常見使用方法。

### 結論

**`PG_locked` 是 `page->flags` 裡的第 0 個位元，用來當「頁面鎖」。**

它保護的是**「這個頁面正在被 I/O 或回收操作」**這件事，
而不是頁面的內容。典型用途：讀寫頁面到磁碟、頁面回收、遷移、COW。

**API：**

| 函式 | 行為 | 可否睡眠 |
|---|---|---|
| `lock_page(page)` | 拿鎖，拿不到就**睡** | ✅ 會睡 |
| `trylock_page(page)` | 拿鎖，拿不到就回 0 | ❌ 不睡 |
| `unlock_page(page)` | 放鎖，並喚醒等待者 | — |
| `wait_on_page_locked(page)` | **只等不拿**，等鎖被放掉 | ✅ 會睡 |
| `PageLocked(page)` | 查詢狀態 | — |
| `lock_page_killable(page)` | 可被致命訊號打斷 | ✅ |

**實作方式**：位元操作 + 等待佇列
```c
static inline void lock_page(struct page *page)
{
	might_sleep();
	if (!trylock_page(page))
		__lock_page(page);        /* 進 page_waitqueue(page) 睡覺 */
}

static inline int trylock_page(struct page *page)
{
	return likely(!test_and_set_bit_lock(PG_locked, &page->flags));
}

static inline void unlock_page(struct page *page)
{
	clear_bit_unlock(PG_locked, &folio->flags);
	wake_up_page_bit(page, PG_locked);
}
```

**注意 `test_and_set_bit_lock()` / `clear_bit_unlock()` 的後綴** ——
它們分別帶 acquire / release 語意，正是 [Q3](#q3) 講的單向屏障。

**等待佇列的巧思**：頁面數量太多，不可能每個頁面配一個等待佇列。
核心用**雜湊**：`page_waitqueue(page)` 依頁面位址雜湊到
`zone->wait_table` 裡的一條佇列，多個頁面共用一條
（所以喚醒時要用 `wake_up_page_bit()` 逐一檢查是不是自己等的那個）。

**典型使用場景：**

| 場景 | 程式碼 |
|---|---|
| 讀檔案 | `filemap_read()` → `lock_page()` → 送出 I/O → I/O 完成後 `unlock_page()` |
| 回收頁面 | `shrink_page_list()` → `trylock_page()` 拿不到就跳過（不能睡） |
| 頁面遷移 | `migrate_pages()` → `lock_page()` 確保沒人在用 |
| 缺頁處理 | `do_swap_page()` → `lock_page()` 等 swap-in 完成 |

> **書目**：奔跑吧 §1.11.3「PG_Locked」（第 3292~3297 行）；
> 詳細分析在卷 1 的記憶體管理章節。

### 實機驗證

`sync_probe.ko` 配一個頁面、實際加解鎖：

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/sync_probe.ko; sudo rmmod sync_probe; \
                 sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A9 "Q35"'
```

```
== Q35：PG_locked 頁面鎖 ==
  剛配置的頁面 pfn=1198471  flags=0x8000000000000000  PageLocked=0
  trylock_page() = 1
  加鎖後          flags=0x8000000000000001  PageLocked=1   <-- PG_locked = bit 0
  再 trylock_page() = 0   <-- 已經被鎖住，拿不到
  unlock_page() 後 flags=0x8000000000000000  PageLocked=0
```

**逐項對照：**

| 動作 | `page->flags` | 變化 |
|---|---|---|
| `alloc_page()` | `0x8000000000000000` | bit63 是 node/zone 編碼，bit0 = 0 |
| `trylock_page()` = **1** | `0x8000000000000001` | **bit0 被設起來** ✅ |
| 再 `trylock_page()` = **0** | 不變 | **已經鎖住，拿不到** ✅ |
| `unlock_page()` | `0x8000000000000000` | bit0 清掉 ✅ |

**`PG_locked` 就是 `page->flags` 的 bit 0** —— 這是實測，不是推論。
把它放在第 0 位是刻意的：`test_and_set_bit_lock(0, ...)` 在多數架構上
可以編成最短的指令序列（本機是一條 `LDSETAL`，見 [Q1](#q1) 的 LSE 指令表）。

**第二次 `trylock_page()` 回傳 0**，證實了它是一把**互斥**的位元鎖 ——
如果這時改用 `lock_page()`，呼叫者就會睡在等待佇列上（所以它不能在
自旋鎖臨界區或中斷上下文裡呼叫）。

---

<a name="q36"></a>
## 36. 在 mm/rmap.c 文件中的 page_get_anon_vma() 函數中，為什麼要使用 rcu_read_lock() 函數？什麼時候註冊 RCU 回調函數呢？

### 結論

### (1) 為什麼要用 `rcu_read_lock()`？

```c
/* mm/rmap.c，6.1 的版本（書上是 5.0，語意相同） */
struct anon_vma *folio_get_anon_vma(struct folio *folio)
{
	struct anon_vma *anon_vma = NULL;
	unsigned long anon_mapping;

	rcu_read_lock();                                       /* ① */
	anon_mapping = (unsigned long)READ_ONCE(folio->mapping);
	if ((anon_mapping & PAGE_MAPPING_FLAGS) != PAGE_MAPPING_ANON)
		goto out;
	if (!folio_mapped(folio))
		goto out;

	anon_vma = (struct anon_vma *)(anon_mapping - PAGE_MAPPING_ANON);
	if (!atomic_inc_not_zero(&anon_vma->refcount)) {        /* ② */
		anon_vma = NULL;
		goto out;
	}
	if (!folio_mapped(folio)) {                            /* ③ */
		rcu_read_unlock();
		put_anon_vma(anon_vma);
		return NULL;
	}
out:
	rcu_read_unlock();
	return anon_vma;
}
```

**要保護的是「從 `page->mapping` 推算出來的 `anon_vma` 指標」。**
這個指標是**間接**得到的（把 `page->mapping` 減掉旗標位），
中途沒有任何鎖 —— 另一顆 CPU 隨時可能在做 `munmap()` / 頁面遷移，
最終呼叫 `unlink_anon_vmas()` → `put_anon_vma()` → `anon_vma_free()` 把它釋放掉。

`rcu_read_lock()` 保證的是：**這段期間該記憶體不會被還給伙伴系統**，
所以「讀它」不會 oops。

### (2) RCU 回呼在哪裡註冊？—— **答案是「沒有註冊」**

這一題最有意思的地方：追遍
`unlink_anon_vmas() → put_anon_vma() → __put_anon_vma() → anon_vma_free()`
**完全找不到 `call_rcu()`**。

秘密在建立 slab cache 時的旗標：

```c
/* mm/rmap.c */
void __init anon_vma_init(void)
{
	anon_vma_cachep = kmem_cache_create("anon_vma", sizeof(struct anon_vma),
			0, SLAB_TYPESAFE_BY_RCU|SLAB_PANIC|SLAB_ACCOUNT,
			anon_vma_ctor);
	...
}
```

**`SLAB_TYPESAFE_BY_RCU` 的語意（很容易誤解）：**

| | 一般的 `kfree_rcu()` | **`SLAB_TYPESAFE_BY_RCU`** |
|---|---|---|
| 延後釋放的是 | **物件** | **物件所在的「頁面」** |
| `kmem_cache_free()` 之後 | 物件還在，一個 GP 後才真的沒了 | **物件立刻可以被重新配置出去** |
| 保證 | 內容還是舊的（value-safe） | **只保證位址仍是合法記憶體**（type-safe） |

也就是說：**位址一定活著，但內容可能已經是別人的了。**

**所以才需要第 ② ③ 兩道額外驗證：**

* **②「`atomic_inc_not_zero(&anon_vma->refcount)`」**
  —— 如果 refcount 已經是 0，代表這個物件已經被釋放（或正在釋放），放棄。
  這一步同時「搶到」了參考計數，之後它就不會消失。
* **③「再檢查一次 `folio_mapped(folio)`」**
  —— 確認我們拿到的 `anon_vma` 還是**這個頁面的**那一個，
  而不是同一塊記憶體被重新配置給別人的新物件。

書上給的樣板正是這個結構：
```c
rcu_read_lock();
again:
	obj = lockless_lookup(key);
	if (obj) {
		if (!try_get_ref(obj))      /* ← 對應 ② atomic_inc_not_zero */
			goto again;
		if (obj->key != key) {      /* ← 對應 ③ 再驗證一次 */
			put_ref(obj);
			goto again;
		}
	}
rcu_read_unlock();
```

> **書目**：奔跑吧 §1.11.6「RCU」（第 3454~3560 行）。
> **修正**：書上寫的 `SLAB_DESTROY_BY_RCU` 在 **Linux 4.9 改名為
> `SLAB_TYPESAFE_BY_RCU`**（commit `5f0d5a3ae7cf`），語意不變。
> 另外 6.1 的函式已從 `page_get_anon_vma()` 改名為 `folio_get_anon_vma()`
> （folio 化改造），`ACCESS_ONCE()` 也改成了 `READ_ONCE()`。

### 實機驗證 —— 親眼看到「同一塊記憶體立刻被配置出去」

`rcu_demo.ko mode=2` 建一個 `SLAB_TYPESAFE_BY_RCU` 的 cache，
配置 → 釋放 → 立刻再配置：

```bash
ssh radxa@$HOST 'sudo insmod ~/exp/sync/rcu_demo.ko mode=2; sudo rmmod rcu_demo; sudo dmesg'
```

```
== Q36：SLAB_TYPESAFE_BY_RCU（書上寫的 SLAB_DESTROY_BY_RCU）==
  ⚠ 書上用的名字 SLAB_DESTROY_BY_RCU 在 Linux 4.9 已改名為
     SLAB_TYPESAFE_BY_RCU（commit 5f0d5a3ae7cf），語意不變。

  配置物件 a = ffff0001625a3000  key = 0xdeadbeef  refcount = 1
  kmem_cache_free(a) 之後：
    * 物件「立刻」還給 slab，可以馬上被重新配置出去
    * 但物件所在的『頁面』要等一個 GP 之後才會還給伙伴系統
    * 所以 a 這個位址一定還是「可安全讀取」的記憶體，不會 oops
  free 之後用舊指標 a 讀 key（記憶體仍然有效，不會 oops）= 0xdeadbeef
  再配置一個 b = ffff0001625a3000  <-- ★ 拿到同一塊記憶體！
    寫入 b->key = 0x12345678 之後，用「舊指標 a」讀出來的 key = 0x12345678
    -> 位址還活著，但內容已經是別人的了；這就是 type-safe 而非 value-safe
```

**這段輸出把整個問題講完了：**

| 步驟 | 觀察 | 意義 |
|---|---|---|
| 配置 `a` | `a = ffff0001625a3000`，`key = 0xdeadbeef` | — |
| `kmem_cache_free(a)` 後讀 `a->key` | 仍然是 `0xdeadbeef`，**沒有 oops** | ✅ **位址是安全的**（RCU 保證頁面不還給伙伴系統） |
| 再配置 `b` | **`b == a`**（同一個位址！） | ✅ **物件立刻被重新配置出去**，不等 GP |
| 寫 `b->key = 0x12345678` 後讀 `a->key` | **也變成 `0x12345678`** | ❌ **內容已經是別人的了** |

**最後一列就是為什麼 `folio_get_anon_vma()` 必須做第 ②③ 兩道驗證。**
如果只靠 `rcu_read_lock()`，它可能拿到一個**位址正確、但已經屬於別的 VMA**
的 `anon_vma`，然後對錯誤的物件做反向映射走訪 —— 那是無聲的資料損壞，
比 oops 難查一萬倍。

```bash
# 確認本機確實有這個 cache，以及它的原始碼旗標
ssh radxa@$HOST 'sudo grep -E "^anon_vma " /proc/slabinfo | awk "{print \$1, \"objsize=\"\$4}"
  grep -n -A2 "anon_vma_cachep = kmem_cache_create" /lib/modules/$(uname -r)/build/mm/rmap.c'
```
```
anon_vma objsize=128
mm/rmap.c:461:	anon_vma_cachep = kmem_cache_create("anon_vma", sizeof(struct anon_vma),
mm/rmap.c:462-			0, SLAB_TYPESAFE_BY_RCU|SLAB_PANIC|SLAB_ACCOUNT,
mm/rmap.c:463-			anon_vma_ctor);
```

---

<a name="q37"></a>
## 37. 在 mm/oom_kill.c 的 select_bad_process() 函數中，為什麼要使用 rcu_read_lock() 函數？什麼時候註冊 RCU 回調函數呢？

### 結論

### (1) 為什麼要用 `rcu_read_lock()`？

```c
/* mm/oom_kill.c */
static void select_bad_process(struct oom_control *oc)
{
	...
	} else {
		struct task_struct *p;

		rcu_read_lock();                         /* ★ */
		for_each_process(p)
			if (oom_evaluate_task(p, oc))
				break;
		rcu_read_unlock();
	}
}
```

**它在走訪「全系統的行程鏈結串列」。**
這條串列（`init_task.tasks`）隨時有行程在建立（`fork`）和消失（`exit`），
如果不保護，走訪到一半節點被 `list_del()` 掉，指標就變野指標。

**為什麼不用 `tasklist_lock`（讀寫自旋鎖）？**

| | `read_lock(&tasklist_lock)` | `rcu_read_lock()` |
|---|---|---|
| 會不會擋住 fork/exit | **會**（寫者要等所有讀者） | **不會** |
| 走訪期間可否睡眠 | ❌ 自旋鎖 | ❌（但成本低到可以走訪幾千個行程） |
| 對系統的干擾 | 大 —— OOM 時系統已經很慘了 | 幾乎沒有 |

OOM killer 要走訪**成千上萬**個行程來算 `oom_score`，
用 `tasklist_lock` 會在系統最危急的時候把 fork/exit 全部凍住。
用 RCU 則完全不阻塞寫者。

**代價**：可能走訪到「正在消失的行程」或漏掉「剛建立的行程」。
對 OOM killer 來說**完全可以接受** —— 它本來就只是在挑一個「夠糟」的犧牲者，
不需要全系統的一致快照。
（真的挑中之後還會用 `get_task_struct()` 增加參考計數再動手。）

**巨集背後**：
```c
#define for_each_process(p) \
	for (p = &init_task ; (p = next_task(p)) != &init_task ; )

#define next_task(p) \
	list_entry_rcu((p)->tasks.next, struct task_struct, tasks)
	                        ↑
	              ★ list_entry_rcu：內含 rcu_dereference()
```
**`_rcu` 後綴就是在告訴你「這裡必須在 RCU 讀者臨界區裡」。**
`for_each_process_thread()` 也一樣。

### (2) RCU 回呼在哪裡註冊？

**在行程結束時，由 `release_task()` 註冊：**

```
do_exit()
  └─ exit_notify()
       └─ release_task(p)
            ├─ __exit_signal(p)
            │    └─ __unhash_process(p, group_dead)
            │         ├─ detach_pid(p, PIDTYPE_PID);      /* 從 pid 雜湊移除 */
            │         └─ list_del_rcu(&p->tasks);         /* ★ 從行程串列移除 */
            └─ put_task_struct_rcu_user(p)
                 └─ if (refcount_dec_and_test(&task->rcu_users))
                        call_rcu(&task->rcu, delayed_put_task_struct);  /* ★ 註冊回呼 */
```

```c
/* kernel/exit.c */
static void delayed_put_task_struct(struct rcu_head *rhp)
{
	struct task_struct *tsk = container_of(rhp, struct task_struct, rcu);

	perf_event_delayed_put(tsk);
	trace_sched_process_free(tsk);
	put_task_struct(tsk);           /* 這裡才真的釋放 task_struct */
}
```

**兩段式的設計：**
1. **`list_del_rcu(&p->tasks)`** —— 立刻把行程從串列上「摘掉」，
   新的走訪者看不到它了；但**舊的走訪者手上的指標仍然有效**
   （`list_del_rcu` 不會破壞 `next` 指標，所以走訪可以繼續走下去）。
2. **`call_rcu(&task->rcu, delayed_put_task_struct)`** ——
   等一個 GP（本機實測平均 43.7 ms，見 [Q28](#q28)）之後，
   確定沒有任何走訪者還拿著它，才真的釋放 `task_struct`。

**⚠ 與 [Q36](#q36) 的重要對比：`task_struct` 用的是「另一種」RCU 慣用法。**

```bash
ssh radxa@$HOST 'cd /lib/modules/$(uname -r)/build
  grep -n -A4 "task_struct_cachep = kmem_cache_create" kernel/fork.c'
```
```c
	task_struct_cachep = kmem_cache_create_usercopy("task_struct",
			arch_task_struct_size, align,
			SLAB_PANIC|SLAB_ACCOUNT,      /* ★ 沒有 SLAB_TYPESAFE_BY_RCU */
			useroffset, usersize, NULL);
```

| | `anon_vma`（[Q36](#q36)） | `task_struct`（本題） |
|---|---|---|
| slab 旗標 | **`SLAB_TYPESAFE_BY_RCU`** | 一般的 `SLAB_PANIC\|SLAB_ACCOUNT` |
| 延後釋放的是 | **頁面** | **物件本身**（`call_rcu(&task->rcu, …)`） |
| RCU 讀者臨界區內的保證 | **只有位址有效**，內容可能已換人 | **整個物件都有效**，內容不會被改 |
| 需不需要再驗證 | **需要**（`atomic_inc_not_zero` + 重查 `folio_mapped`） | **不需要** |

**所以走訪行程串列比 `folio_get_anon_vma()` 單純得多**：
在 `rcu_read_lock()` 裡拿到的 `task_struct` 指標，
整個臨界區內都保證是同一個、活著的物件，不必做二次驗證。
（真的要「帶出臨界區」時才需要 `get_task_struct()` 增加參考計數。）

同樣的保證讓 `mutex_spin_on_owner()` 可以安全地在 `rcu_read_lock()` 裡
讀 `owner->on_cpu`（見 [Q19](#q19)）。

（順帶一提：`sighand_cachep` 才是 `kernel/fork.c` 裡用
`SLAB_TYPESAFE_BY_RCU` 的那一個，見 `kernel/fork.c:3051`。）

> **書目**：奔跑吧 §1.11.6 末段（第 3540~3560 行）。
> **修正**：書上引用的 5.0 版 `select_bad_process()` 用
> `for_each_process_thread(g, p)`，6.1 已改成 `for_each_process(p)` +
> `oom_evaluate_task()`，但 `rcu_read_lock()` 的用法完全相同。

### 實機驗證

**(a) 確認走訪巨集確實需要 RCU**

```bash
ssh radxa@$HOST 'cd /lib/modules/$(uname -r)/build
  grep -n -A3 "define for_each_process(p)" include/linux/sched/signal.h
  grep -n "define next_task" include/linux/sched/signal.h'
```

**(b) 確認回呼註冊的位置**

```bash
ssh radxa@$HOST 'cd /lib/modules/$(uname -r)/build
  grep -n -B2 -A6 "delayed_put_task_struct" kernel/exit.c | head -25
  grep -n "list_del_rcu(&p->tasks)" kernel/exit.c'
```

**(c) 用 tracepoint 看回呼真的被延後了**

`sched_process_free` 這個 tracepoint 就在 `delayed_put_task_struct()` 裡，
所以它觸發的時間點 = 「行程結束後一個 GP」：

```bash
ssh radxa@$HOST 'sudo bash -c "
T=/sys/kernel/debug/tracing
echo 0 > \$T/events/enable; echo > \$T/trace
echo 1 > \$T/events/sched/sched_process_exit/enable
echo 1 > \$T/events/sched/sched_process_free/enable
echo 1 > \$T/tracing_on
su radxa -c \"/bin/true\"
sleep 1; echo 0 > \$T/tracing_on
grep -E \"sched_process_(exit|free)\" \$T/trace | tail -6"'
```

同一個 `comm=true` 的 `sched_process_exit` 和 `sched_process_free`
之間會有幾十毫秒的間隔 —— 那就是一個寬限期。

**(d) 本機的 GP 長度**（[Q28](#q28) 實測）：

```
  synchronize_rcu()  平均 43731 us，最短 16649 us，最長 147550 us
```

也就是說，**一個行程 `exit()` 之後，它的 `task_struct` 平均要再等 43.7 ms
才會真的被釋放** —— 這段時間裡，任何在 RCU 讀者臨界區內拿到它指標的程式碼
（OOM killer、`/proc` 走訪、`mutex_spin_on_owner()`…）都是安全的。

---

## 附錄 A：本章實驗檔案

| 檔案 | 類型 | 涵蓋題目 |
|---|---|---|
| `experiments/sync_probe.c` | 核心模組 | Q1~Q9, Q11~Q15, Q17~Q19, Q22, Q23, Q25, Q26, Q31, Q35 |
| `experiments/lock_bench.c` | 核心模組 | Q10, Q12, Q16, Q20, Q21, Q25, Q27, Q33 |
| `experiments/rcu_demo.c` | 核心模組 | Q27~Q30, Q32, Q36 |
| `experiments/lse_bench.c` | 使用者態 | Q1 |

**`sync_probe.ko` 的核心技巧**：它把自己函式的機器碼從記憶體讀出來，
所以看到的是**已經被 alternatives 打過補丁的指令**，
而不是編譯期的樣子。搭配對 `.ko` 檔案本身反組譯（未打補丁），
就能完整呈現 ARM64 alternatives 機制。

模組內建一個極簡 AArch64 解碼器（只認本章關心的指令），
其餘印成 `-`；每段都附 `HEX:`，可以餵給 `objdump` 做權威交叉驗證：

```bash
python3 -c "
import sys,struct
words=[0x...,0x...]
sys.stdout.buffer.write(b''.join(struct.pack('<I',w) for w in words))" > x.bin
objdump -D -b binary -m aarch64 x.bin
```

## 附錄 B：實驗環境的復原

```bash
ssh radxa@$HOST 'sudo bash -c "
# cpufreq governor（本章量測時改成 performance）
for p in 0 4 6; do echo ondemand > /sys/devices/system/cpu/cpufreq/policy\$p/scaling_governor; done
# RCU 停滯偵測門檻（mode=3 會改成 5）
echo 60 > /sys/module/rcupdate/parameters/rcu_cpu_stall_timeout
# ftrace
T=/sys/kernel/debug/tracing
echo 0 > \$T/tracing_on; echo nop > \$T/current_tracer
echo 0 > \$T/events/enable; echo ff > \$T/tracing_cpumask; echo > \$T/trace
# 模組
rmmod sync_probe lock_bench rcu_demo 2>/dev/null; true"'
```

> ⚠ `rcu_demo.ko mode=3` 會刻意觸發 RCU 停滯警告
> （書上 §1.11.7 的場景），dmesg 會出現
> `rcu: INFO: rcu_sched self-detected stall on CPU` 與函式呼叫堆疊，
> **這是預期行為**，系統會自行恢復。實測輸出：
>
> ```
> rcu: INFO: rcu_sched self-detected stall on CPU
> rcu:  7-....: (1499 ticks this GP) idle=e904/1/0x4000000000000000 softirq=16182/16182 fqs=491
>        (t=1501 jiffies g=19941 q=1801 ncpus=8)
> CPU: 7 PID: 4241 Comm: rcu_staller Tainted: G           O       6.1.115+ #1
> Hardware name: Radxa ROCK 5B (DT)
> Call trace:
>  __delay+0xd0/0xdc
>  __const_udelay+0x24/0x2c
>  stall_thread+0x7c/0xcc [rcu_demo]
>  kthread+0xc0/0xd0
>  ret_from_fork+0x10/0x20
> ```
>
> `t=1501 jiffies` ÷ HZ(300) = **5.0 秒**，正好等於實驗前設定的
> `rcu_cpu_stall_timeout=5`；`g=19941` 是卡住的那個 GP 序號；
> `ncpus=8` 是本機 CPU 數。**與書上 §1.11.7 的範例輸出結構完全一致。**
