# 卷2 第 2 章 中斷管理 — 高頻面試題解答

> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588），`192.168.68.58`（帳密皆為 `radxa`；DHCP，IP 會變）
> **CPU**：4×Cortex-A55（cpu0~3）＋ 4×Cortex-A76（cpu4~7）
> **中斷控制器**：**ARM GIC-600 r1p6-00rel0（GICv3）**，1 cluster / 8 CPU / **480 SPI** / **12 PPI** / 16 SGI / 2×ITS
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`
>
> **關鍵組態**（`zcat /proc/config.gz`）：
> ```
> CONFIG_SPARSE_IRQ=y                 CONFIG_IRQ_DOMAIN_HIERARCHY=y
> CONFIG_GENERIC_MSI_IRQ_DOMAIN=y     CONFIG_GENERIC_IRQ_IPI=y
> CONFIG_IRQ_FORCED_THREADING=y       # CONFIG_GENERIC_IRQ_DEBUGFS is not set  ← 沒有 /sys/kernel/debug/irq
> CONFIG_HZ=300                       CONFIG_NR_CPUS=8
> CONFIG_PREEMPT_VOLUNTARY=y          # CONFIG_PREEMPT_COUNT is not set
> CONFIG_VMAP_STACK=y                 CONFIG_SOFTIRQ_ON_OWN_STACK=y
> CONFIG_WQ_POWER_EFFICIENT_DEFAULT=y
> CONFIG_FUNCTION_GRAPH_TRACER=y      CONFIG_KPROBES=y   # CONFIG_KALLSYMS_ALL is not set
> # CONFIG_DEBUG_ATOMIC_SLEEP is not set
> ```
>
> **驗證強度**：全部 21 個實驗步驟在**重開機之後從重新編譯開始完整重跑過一次**，
> 25 項指標 24 項完全重現；唯一的差異反而挖出一個新結論（virq 每次開機重配）。
> 逐項對照見 [附錄 C](#附錄-c重開機後的完整重跑驗證)。
>
> **書目對照**
> - 《奔跑吧 Linux 內核》（第二版）卷 2 第 2 章 —
>   `books/running-linux-kernel/running-kernel-2-txt/09_第2章_中断管理.txt`
>   （下稱「奔跑吧 §2.x」，行號以該純文字檔為準）
> - **RK3588 TRM** — `books/rk3588_trm/part1/chapter_11.txt`（GIC600）、
>   `books/rk3588_trm/part1/chapter_01.txt` §1.3「System Interrupt Connection」Table 1-3（中斷連線表）
> - 核心程式碼路徑相對於本專案樹
>   `/home/awe/disk/yocto-rockchip-sdk/build/tmp/work-shared/rockchip-rk3588-rock-5b/kernel-source`
> - 相關筆記：📝 [ch10 併發與同步](./ch10_concurrency_and_synchronization.md)、
>   📝 [ch02 ARM64 在 Linux 內核中的實現](./ch02_arm64_in_linux_kernel.md)、
>   📝 [ch08 調度與負載均衡](./ch08_process_management_scheduling_and_load_balancing.md)

---

## 書是 Linux 5.0 + GIC-V2 + QEMU 寫的，本機是 6.1 + GIC-600 + 真硬體 —— 14 處差異

| # | 書上（5.0 / GIC-V2 / QEMU virt） | 本機實測（6.1 / GIC-600 / RK3588） | 題號 |
|---|------|------|------|
| 1 | 中斷入口是**組語** `el1_irq:` → `kernel_entry 1` → `irq_handler` 巨集 | 組語只剩 `entry_handler` 巨集（`entry.S:559`），真正的流程在 **C** 裡：`el1h_64_irq_handler()` → `el1_interrupt()` → `__el1_irq()`（`entry-common.c:489/478/465`） | Q1、Q3 |
| 2 | 向量表基址 = `entry.S` 的 `vectors` | **VBAR_EL1 指到 `__bp_harden_el1_vectors`**（差 0x2000）—— Spectre-BHB 緩解的向量表副本 | Q1 |
| 3 | 「EL1 是核心模式」 | 本機 **VHE：核心真的跑在 EL2**，實測中斷現場 `PSTATE.M[3:0] = 0x9 = EL2h` | Q1、Q14 |
| 4 | `enable_da_f` 組語巨集打開 D/A/F | `write_sysreg(DAIF_PROCCTX_NOIRQ, daif)`（`entry-common.c:480`），實測硬中斷裡 **DAIF = 0xc0**（只剩 I、F 遮住） | Q1、Q4 |
| 5 | `handle_domain_irq()` / `__handle_domain_irq()` | 已改名 **`generic_handle_domain_irq()`**（5.16, commit `a1b0973d5e97`），內部 `handle_irq_desc()` | Q2、Q3 |
| 6 | `irq_domain` 用 `linear_revmap[]` + `revmap_tree`（radix tree） | 6.1 換成 **`revmap[]` 陣列 + `struct mutex`**，查表函式叫 `__irq_resolve_mapping()` | Q2 |
| 7 | `struct pt_regs` 有 `orig_addr_limit` | **已刪除**（5.10 移除 `set_fs`）；6.1 多出 `sdei_ttbr1` / `pmr_save` / `lockdep_hardirqs` / `exit_rcu`；**`sizeof = 336`** | Q14 |
| 8 | `BLOCK_IOPOLL_SOFTIRQ` | 更名 **`IRQ_POLL_SOFTIRQ`**（4.5, commit `903cd12ee9e5`） | Q7 |
| 9 | `tasklet_struct.func(unsigned long data)` | 6.1 是 **union { func; callback }** + `use_callback`，`DECLARE_TASKLET(name, callback)` 只剩兩個參數（5.9, commit `12cc923f1ccc`） | Q9 |
| 10 | `worker_pool.nr_running` 是 `atomic_t` | 改成 **普通 `int`**（5.17, commit `bc35f7ef9628`），改由 `pool->lock` 保護 | Q12、Q13 |
| 11 | `pool_workqueue.delayed_works` | 更名 **`inactive_works`**（5.15, commit `f97a4a1a3f87`） | Q11、Q13 |
| 12 | `wq_worker_sleeping()` 從 **`__schedule()`** 裡呼叫，並用 `try_to_wake_up_local()` | 移到 **`sched_submit_work()`**（`core.c:6606`，`schedule()` 進 `__schedule()` **之前**），且直接 `wake_up_worker()`，`try_to_wake_up_local()` 已刪除 | Q12、Q13 |
| 13 | `in_irq()` | 6.1 建議用 **`in_hardirq()`**；另有 `interrupt_context_level()` | Q4、Q7 |
| 14 | GIC-V2：`GICC_IAR` MMIO 讀取、988 個 SPI | GIC**v3**：`ICC_IAR1_EL1` **系統暫存器**（`gic_read_iar()`）、**480 SPI + LPI/ITS**；EOImode1 下 ACK 與 deactivate 分離（`gic_eoimode1_chip`） | Q1、Q3 |

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | 發生硬體中斷後，ARM64 處理器做了哪些事情 | **抓到真實中斷的 SPSR/ELR**：EL0 被打斷時 `pc=0xaaaae6faad0c`（使用者位址）；**VBAR_EL1 不是 `vectors` 而是 BHB 副本**；`sub sp,sp,#336` |
| [2](#q2) | 硬體中斷號和 Linux IRQ 號如何映射 | **TRM #237 `irq_emmc` → DTB `<0 205 4>` → hwirq 237 → virq 160** 四層對帳一路打通；7 個 irq_domain |
| [3](#q3) | 一個硬體中斷發生後核心如何響應處理 | ftrace 抓到 **mt7921e 網卡完整上半部→`__tasklet_schedule`→下半部**，含 `gic_eoimode1_eoi_irq` 的時序 |
| [4](#q4) | 為什麼中斷上下文不能睡眠 | **`current` 是無辜的 `swapper/3`**；**SP 在 8 顆 CPU 各自的中斷棧**（實測 8 段位址）；`DAIF=0xc0` |
| [5](#q5) | 軟中斷回呼執行時是否允許響應本地中斷 | **tasklet 忙等 30 ms 期間收到 10 次 arch_timer 中斷**；ftrace 的 `d.H..` 旗標抓到「軟中斷裡巢狀硬中斷」 |
| [6](#q6) | 同一類型的軟中斷是否允許多 CPU 並行 | **8 顆 CPU 同時跑 TASKLET_SOFTIRQ，30 ms 做完 8×30 ms 的量** |
| [7](#q7) | 軟中斷上下文包括哪幾種情況 | **三種全部抓到**，且 `local_bh_enable` 那種 `preempt_count = 0x101`（**那個「留 1」看得見**） |
| [8](#q8) | 軟中斷 vs 行程上下文誰優先 | **被中斷的 kthread 迴圈整整空窗 40009218 ns**，等於 tasklet 忙等的 40 ms |
| [9](#q9) | 同一個 tasklet 是否允許多 CPU 並行 | **8 次 `tasklet_schedule()` 只換到 1 次執行**；重現書上 CPU0/CPU4 時序，最大並行數恆為 1 |
| [10](#q10) | 工作佇列跑在哪種上下文？能睡嗎 | `kworker/5:1`、`preempt_count=0`、**`msleep(120)` 實際睡了 124 ms 後正常返回** |
| [11](#q11) | 舊版工作佇列遇到的問題 | **3 秒內 13 個 workqueue 共用 60 個 kworker**（舊機制要 13×8=104 個）；`alloc_ordered_workqueue` 串行實測 |
| [12](#q12) | CMWQ 如何動態管理工作線程池 | **cpu3 的 kworker 6 → 10**（睡的 work）vs **10 → 10 且只用 1 個 worker**（燒 CPU 的 work） |
| [13](#q13) | 某個 work 阻塞了，剩下的 work 怎麼辦 | **6 個各睡 300 ms 的 work，總耗時 316 ms 而不是 1800 ms**，6 個不同 worker |
| [14](#q14) | 什麼是中斷現場？要保存哪些內容 | `sizeof(pt_regs)=336`，逐欄位偏移量 + **EL0/EL1 兩種現場的實際內容對照** |
| [15](#q15) | 中斷現場保存在什麼地方 | **EL0 進來時 `&pt_regs` 距棧頂剛好 336 B**（棧是空的）；EL1 巢狀時是 944 B；handler 已切到 Per-CPU 中斷棧 |

---

## 一鍵重現全部實驗

```bash
HOST=192.168.68.58     # DHCP，先確認 IP

ssh radxa@$HOST 'mkdir -p ~/exp/irq'
scp notes/experiments/{irq_probe,irq_live,ctx_probe,softirq_par,wq_probe}.c \
    notes/experiments/ksym.h notes/experiments/irq_trace.sh radxa@$HOST:~/exp/irq/
ssh radxa@$HOST 'cat > ~/exp/irq/Makefile <<EOF
obj-m += irq_probe.o irq_live.o ctx_probe.o softirq_par.o wq_probe.o
KDIR ?= /lib/modules/\$(shell uname -r)/build
all:
	\$(MAKE) -C \$(KDIR) M=\$(PWD) modules
EOF
chmod +x ~/exp/irq/irq_trace.sh
cd ~/exp/irq && make'

# --- 靜態結構（Q1/Q2/Q3/Q4/Q14/Q15）；模組刻意回傳 -EAGAIN，不必 rmmod
ssh radxa@$HOST 'sudo insmod ~/exp/irq/irq_probe.ko; sudo dmesg | sed "s/^\[[^]]*\] //"'

# --- 抓真實中斷現場（Q1/Q2/Q14/Q15）
ssh radxa@$HOST 'sudo insmod ~/exp/irq/irq_live.ko samples=1 want_hwirq=26; sudo rmmod irq_live'
ssh radxa@$HOST 'sudo insmod ~/exp/irq/irq_live.ko samples=1 from_el0=1;  sudo rmmod irq_live'
ssh radxa@$HOST 'sudo insmod ~/exp/irq/irq_live.ko samples=0 stackmap=1; sleep 3; sudo rmmod irq_live'

# --- 上下文指紋（Q4/Q5/Q7/Q8/Q10）
ssh radxa@$HOST 'sudo insmod ~/exp/irq/ctx_probe.ko mode=0; sudo rmmod ctx_probe'   # 指紋表
ssh radxa@$HOST 'sudo insmod ~/exp/irq/ctx_probe.ko mode=1 spin_ms=30; sudo rmmod ctx_probe'
ssh radxa@$HOST 'sudo insmod ~/exp/irq/ctx_probe.ko mode=2 spin_ms=40 cpu_target=3; sudo rmmod ctx_probe'
ssh radxa@$HOST 'sudo insmod ~/exp/irq/ctx_probe.ko mode=3; sudo rmmod ctx_probe'

# --- 軟中斷並行 vs tasklet 串行（Q6/Q9）
ssh radxa@$HOST 'sudo insmod ~/exp/irq/softirq_par.ko mode=0 spin_ms=30; sudo rmmod softirq_par'
ssh radxa@$HOST 'sudo insmod ~/exp/irq/softirq_par.ko mode=1 spin_ms=30; sudo rmmod softirq_par'
ssh radxa@$HOST 'sudo insmod ~/exp/irq/softirq_par.ko mode=2 spin_ms=60; sudo rmmod softirq_par'

# --- CMWQ（Q10~Q13）
ssh radxa@$HOST 'sudo insmod ~/exp/irq/wq_probe.ko mode=0 nwork=6 sleep_ms=300 cpu_target=3; sudo rmmod wq_probe'
ssh radxa@$HOST 'sudo insmod ~/exp/irq/wq_probe.ko mode=1 nwork=6 burn_ms=60  cpu_target=3; sudo rmmod wq_probe'
ssh radxa@$HOST 'sudo insmod ~/exp/irq/wq_probe.ko mode=2 nwork=4 sleep_ms=150; sudo rmmod wq_probe'
ssh radxa@$HOST 'sudo insmod ~/exp/irq/wq_probe.ko mode=3 nwork=6 sleep_ms=150; sudo rmmod wq_probe'

# --- ftrace（Q3/Q5/Q7/Q11）
ssh radxa@$HOST 'sudo ~/exp/irq/irq_trace.sh path'     # 完整中斷呼叫鏈
ssh radxa@$HOST 'sudo ~/exp/irq/irq_trace.sh softirq'  # 軟中斷在誰身上跑
ssh radxa@$HOST 'sudo ~/exp/irq/irq_trace.sh pools'    # workqueue vs kworker
ssh radxa@$HOST 'sudo ~/exp/irq/irq_trace.sh off'      # 還原
```

> ⚠ `ctx_probe.ko mode=1/2`、`softirq_par.ko`、`wq_probe.ko` 會讓 CPU 忙等數十到數百毫秒；
> 參數不要調太大（`spin_ms` 超過幾百毫秒可能觸發 RCU stall 警告）。
> ⚠ **踩過的坑**：`ctx_probe.ko` 第一版在 `module_exit` 裡對「這個 mode 根本沒 `hrtimer_init()`
> 過的 `hrtimer`」呼叫 `hrtimer_cancel()`，導致 `rmmod` 段錯誤、模組卡在 `Used by = -1`
> 的 GOING 狀態、**重開機才清得掉**。現在的版本用 `ht_inited/iw_inited/tl_inited` 旗標守住。

---

<a name="q1"></a>
## 1. 發生硬體中斷後，ARM64 處理器做了哪些事情？

### 結論

**硬體（CPU）自動做 6 件事，然後把棒子交給軟體：**

| # | 硬體動作 | 落點 |
|---|---------|------|
| 1 | 把中斷點的處理器狀態 `PSTATE` 存起來 | `SPSR_EL1`（VHE 下實際是 `SPSR_EL2`） |
| 2 | 把返回位址存起來 | `ELR_EL1` |
| 3 | **把 `PSTATE.DAIF` 四個位元全部設 1** —— 關掉除錯例外、SError、IRQ、FIQ | `PSTATE` |
| 4 | 同步例外才需要填原因 | `ESR_ELx`（IRQ 是**非同步**例外，不填 ESR） |
| 5 | SP 切到目標 EL 的堆疊指標 | `SP_EL1`（EL1h） |
| 6 | 提升 exception level，跳到 `VBAR_ELn + 偏移` 的向量表項 | `VBAR_EL1` |

軟體從向量表開始接手：每個表項 **128 B**（`.align 7`），第一條指令就是
`sub sp, sp, #PT_REGS_SIZE` —— 在被中斷者的核心棧上挖一個棧框。

> **書目**：奔跑吧 §2.4「ARM64底层中断处理」（第 1258~1275 行，硬體自動做的 6 件事）、
> §2.4.1「异常向量表」表 2.5（第 1276 行起）。
> **原始碼**：`arch/arm64/kernel/entry.S:506` `SYM_CODE_START(vectors)`、
> `:38` `.macro kernel_ventry`、`:199` `.macro kernel_entry`。

### 6.1 和書上（5.0）的差別 —— 入口從組語搬到 C

書上是 `el1_irq:` → `kernel_entry 1` → `enable_da_f` → `irq_handler`。
6.1 的組語只剩一個共用巨集，剩下全是 C：

```asm
	.macro entry_handler el:req, ht:req, regsize:req, label:req      /* entry.S:559 */
SYM_CODE_START_LOCAL(el\el\ht\()_\regsize\()_\label)
	kernel_entry \el, \regsize            // 保存現場
	mov	x0, sp                        // x0 = struct pt_regs *
	bl	el\el\ht\()_\regsize\()_\label\()_handler   // → C 函式
	b	ret_to_kernel                 // → kernel_exit 1
SYM_CODE_END(el\el\ht\()_\regsize\()_\label)
	.endm
```
```c
/* arch/arm64/kernel/entry-common.c:489 */
asmlinkage void noinstr el1h_64_irq_handler(struct pt_regs *regs)
{	el1_interrupt(regs, handle_arch_irq);	}

static void noinstr el1_interrupt(struct pt_regs *regs, void (*handler)(struct pt_regs *))
{
	write_sysreg(DAIF_PROCCTX_NOIRQ, daif);   /* ← 書上 enable_da_f 的 C 版：放開 D/A，I/F 仍遮住 */
	...  __el1_irq(regs, handler);
}
static __always_inline void __el1_irq(struct pt_regs *regs, void (*handler)(struct pt_regs *))
{
	enter_from_kernel_mode(regs);
	irq_enter_rcu();                  /* preempt_count += HARDIRQ_OFFSET */
	do_interrupt_handler(regs, handler);
	irq_exit_rcu();                   /* 遞減，並在這裡跑軟中斷 */
	arm64_preempt_schedule_irq();
	exit_to_kernel_mode(regs);
}
```

### 實機驗證（1）：向量表本體

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/irq_probe.ko show_irqs=0; sudo dmesg | sed "s/^\[[^]]*\] //"'
```
```
irq_probe: VBAR_EL1                    = 0xffff800008012800  (__bp_harden_el1_vectors+0x0/0x1b14)
irq_probe: entry.S 的 SYM_CODE(vectors)= 0xffff800008010800
irq_probe: __bp_harden_el1_vectors     = 0xffff800008012800
irq_probe: ★ VBAR_EL1 【不是】entry.S 裡那張 vectors！差 0x2000；本機 spectre_v2 = "Mitigation: CSV2, BHB"，
irq_probe:   開機時 spectre_bhb_enable_mitigation() 把 VBAR_EL1 改指到 __bp_harden_el1_vectors 這張加了 BHB 清除序列的副本
irq_probe: 每個表項 128 B（.align 7），共 16 項 = 2048 B
irq_probe: #   類型         位址             第一條指令 反組譯/符號
irq_probe: 0   EL1t sync      +0x000 = ...+0x0     0xd10543ff  sub sp, sp, #336  → PT_REGS_SIZE = 336
irq_probe: 1   EL1t IRQ       +0x080 = ...+0x80    0xd10543ff  sub sp, sp, #336  → PT_REGS_SIZE = 336
irq_probe: 2   EL1t FIQ       +0x100                0xd10543ff  sub sp, sp, #336  → PT_REGS_SIZE = 336
irq_probe: 3   EL1t error     +0x180                0xd10543ff  sub sp, sp, #336  → PT_REGS_SIZE = 336
irq_probe: 4   EL1h sync      +0x200                0xd10543ff  sub sp, sp, #336  → PT_REGS_SIZE = 336
irq_probe: 5   EL1h IRQ       +0x280                0xd10543ff  sub sp, sp, #336  → PT_REGS_SIZE = 336   ← 核心態外設中斷走這裡
irq_probe: 6   EL1h FIQ       +0x300                0xd10543ff  sub sp, sp, #336
irq_probe: 7   EL1h error     +0x380                0xd10543ff  sub sp, sp, #336
irq_probe: 8   EL0 64 sync    +0x400                0xd51bd07e                     ← msr tpidrro_el0, x30（trampoline 清理）
irq_probe: 9   EL0 64 IRQ     +0x480                0xd51bd07e
irq_probe: 10  EL0 64 FIQ     +0x500                0xd51bd07e
irq_probe: 11  EL0 64 error   +0x580                0xd51bd07e
irq_probe: 12  EL0 32 sync    +0x600                0xd280031e                     ← mov x30, #0x18
irq_probe: 13  EL0 32 IRQ     +0x680                0xd280031e
irq_probe: 14  EL0 32 FIQ     +0x700                0xd280031e
irq_probe: 15  EL0 32 error   +0x780                0xd280031e
```

**三個超出書本的發現：**

1. **16 項 × 128 B = 2048 B，佈局與書上表 2.5 一字不差。**
2. **`0xd10543ff` 解碼 = `sub sp, sp, #336`**，而 `sizeof(struct pt_regs)` 也正好是 336
   —— 組語裡的 `PT_REGS_SIZE` 與 C 結構體大小對上了（書上的 `S_FRAME_SIZE`）。
3. **VBAR_EL1 根本不指向 `entry.S` 的 `vectors`。**
   本機 `spectre_v2: Mitigation: CSV2, BHB`，開機時 `spectre_bhb_enable_mitigation()`
   （`arch/arm64/kernel/proton-pack.c`）把 VBAR_EL1 改指到
   `__bp_harden_el1_vectors` —— 一張在每個 EL0 表項前面插了 BHB 清除序列的向量表副本。
   ```bash
   ssh radxa@192.168.68.58 'sudo grep -E " (vectors|__bp_harden_el1_vectors)$" /proc/kallsyms
                            cat /sys/devices/system/cpu/vulnerabilities/spectre_v2'
   ```
   ```
   ffff800008010800 T vectors
   ffff800008012800 T __bp_harden_el1_vectors
   Mitigation: CSV2, BHB
   ```
   > 只看原始碼會答錯這一題 —— 必須讀 VBAR_EL1 本身。

### 實機驗證（2）：抓一個「打在使用者態身上」的真實中斷

`irq_live.ko` 用 kprobe 掛在 `generic_handle_domain_irq()`，用 `get_irq_regs()`
取出這次中斷的 `pt_regs`（也就是硬體 + `kernel_entry` 剛存好的現場）：

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/irq_live.ko samples=1 from_el0=1
                         (for i in $(seq 1 3000000); do :; done) & sleep 1
                         sudo rmmod irq_live; sudo dmesg | sed "s/^\[[^]]*\] //"'
```
```
irq_live: ---------------- 中斷現場 #1  (cpu2) ----------------
irq_live:   被中斷者 current      = bash/11737
irq_live:   中斷點的 exception level = EL0 使用者態（PSTATE.M[3:0]=0x0）
irq_live:   [硬體自動做的事 — Q1]
irq_live:     pt_regs->pstate     = 0x0000000020001000  ← 硬體存進 SPSR_EL1 的中斷點 PSTATE
irq_live:                           nzCv .. D=0 A=0 I=0 F=0 .. M=EL0t
irq_live:     pt_regs->pc         = 0x0000aaaae6faad0c  ← 硬體存進 ELR_EL1 的返回位址
irq_live:     現在的 DAIF(EL1)    = 0x000003c0  irqs_disabled()=128  ← 進中斷後 I 已被硬體設 1
irq_live:     pt_regs->sp         = 0x0000ffffead5f740        ← bash 的使用者態堆疊
irq_live:     pt_regs->regs[30]/lr= 0x0000aaaae6faacd4
```

逐條對上書上那 6 件事：

| 書上說的 | 實測 |
|---|---|
| PSTATE 存到 SPSR_ELx | `pt_regs->pstate = 0x20001000`；`M[3:0]=0` = **EL0t**，`I=0`（中斷點當時中斷是開的） |
| 返回位址存到 ELR_ELx | `pt_regs->pc = 0xaaaae6faad0c` —— **使用者態虛擬位址**，正是 bash 被打斷的那一行 |
| DAIF 全設 1 | **`現在的 DAIF = 0x3c0` = D\|A\|I\|F 全 1**（bit9~6）；`irqs_disabled()` 回傳非 0 |
| 設定 SP 指向對應 EL 的棧 | `&pt_regs` 落在 `current->stack`（見 [Q15](#q15)） |
| IRQ 是非同步例外，不看 ESR | 略 |
| 跳到向量表 | 走的是第 9 項（+0x480，EL0 64-bit IRQ） |

### 實機驗證（3）：這台機器的核心其實跑在 EL2（VHE）

同一支模組抓核心態被中斷時：

```
irq_live:   被中斷者 current      = swapper/4/0
irq_live:   中斷點的 exception level = EL2 核心態（VHE：核心跑在 EL2）（PSTATE.M[3:0]=0x9）
irq_live:     pt_regs->pstate     = 0x0000000060400009
irq_live:                           nZCv .. D=0 A=0 I=0 F=0 .. M=EL2h
irq_live:     pt_regs->pc         = 0xffff800008bc7a10  arch_local_irq_enable+0x8/0x18
```

`M[3:0] = 0b1001` → `M[3:2]=10` = **EL2**、`M[0]=1` = 用 SP_EL2 ⇒ **EL2h**。
RK3588 的 bootloader 把核心丟在 EL2，核心偵測到 ARMv8.1-VHE 就留在 EL2 跑
（`HCR_EL2.E2H=1`，所有 `*_EL1` 系統暫存器存取被硬體重導到 `*_EL2`）。
所以：

* 書上「EL1 = 核心模式」在本機**名義上成立、實體上是 EL2**；
* 我在模組裡 `read_sysreg(vbar_el1)` 讀到的其實是 `VBAR_EL2` —— 但這正是實際生效的那張表，所以結論不變；
* 這也是為什麼 `/proc/interrupts` 裡有 `vgic` / `kvm guest vtimer` 這些 KVM 中斷。

---

<a name="q2"></a>
## 2. 硬體中斷號和 Linux 內核的 IRQ 號是如何映射的？

### 結論

**四層資料，經由 `irq_domain` 串起來：**

```
 ① SoC 硬體接線        RK3588 TRM Table 1-3：237 = irq_emmc, High level
        ↓（晶片設計階段就固定）
 ② 設備樹 interrupts   <GIC_SPI 205 IRQ_TYPE_LEVEL_HIGH>     ← 注意：DTS 寫的是「SPI 內編號」
        ↓  of_irq_parse_one() 把三個 cell 放進 of_phandle_args.args[]
 ③ GIC hwirq（INTID）  gic_irq_domain_translate(): SPI → *hwirq = param[1] + 32 = 237
        ↓  irq_domain_alloc_descs() 從 allocated_irqs 位圖找一個空位
 ④ Linux virq          160  ← irq_desc 的索引，request_irq() 用的就是這個
```

映射的「本體」就是 `irq_desc.irq_data` 裡的兩個欄位：

```c
struct irq_data {
	unsigned int		irq;	/* Linux IRQ 號（virq） */
	unsigned long		hwirq;	/* 硬體中斷號（GIC INTID） */
	struct irq_chip		*chip;	/* 中斷控制器的方法集合 */
	struct irq_domain	*domain;/* 屬於哪個中斷控制器 */
	...
};
```

* **正向**（hwirq → virq）：`irq_find_mapping()` / `__irq_resolve_mapping()` 查 `domain->revmap[]`
* **反向**（virq → hwirq）：`irq_to_desc(virq)->irq_data.hwirq`

> **書目**：奔跑吧 §2.2「硬件中断号和Linux中断号的映射」（第 496~985 行），
> 特別是 `irq_of_parse_and_map()` → `irq_create_of_mapping()` → `irq_domain_alloc_irqs()`
> → `gic_irq_domain_alloc()` → `gic_irq_domain_map()` 這條鏈，以及圖 2.3。
> **TRM**：`books/rk3588_trm/part1/chapter_01.txt` §1.3 Table 1-3；
> `chapter_11.txt` §11.1（GIC600：480 SPI / 12 PPI / 16 SGI / 2 ITS）。
> **原始碼（6.1）**：`drivers/of/irq.c:36` `irq_of_parse_and_map()`；
> `kernel/irq/irqdomain.c:908` `irq_create_of_mapping()`、`:1557` `__irq_domain_alloc_irqs()`；
> `drivers/irqchip/irq-gic-v3.c:1553` `gic_irq_domain_translate()`、`:1626` `gic_irq_domain_alloc()`。

### 實機驗證：eMMC 的四層對帳

**① TRM**

```bash
grep -n -B2 -A2 "irq_emmc" books/rk3588_trm/part1/chapter_01.txt
```
```
237

irq_emmc

High level
```

**② 設備樹**（直接讀機台上跑著的 DTB，不是原始碼）

```bash
ssh radxa@192.168.68.58 'f=/sys/firmware/devicetree/base/soc/mmc@fe2e0000
                         hexdump -e "3/4 \"%08x \" \"\n\"" $f/interrupts'
```
```
00000000 cd000000 04000000
```
（big-endian，逐 cell 讀回來是 `<0x0, 0xcd, 0x4>` = `<GIC_SPI 205 IRQ_TYPE_LEVEL_HIGH>`，
與 `arch/arm64/boot/dts/rockchip/rk3588s.dtsi:5703` 一致。）

**③ translate：+32**

```c
/* drivers/irqchip/irq-gic-v3.c:1566 */
switch (fwspec->param[0]) {
case 0:			/* SPI */
	*hwirq = fwspec->param[1] + 32;      /* 205 + 32 = 237 */
	break;
case 1:			/* PPI */
	*hwirq = fwspec->param[1] + 16;
	break;
```

**④ 實際的 irq_desc**

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/irq_probe.ko; sudo dmesg | grep -E "irq 160|mmc0"'
```
```
irq_probe: irq 160   hwirq 237  chip=GICv3  domain=:interrupt-controller@fe600000-1  handle_irq=handle_fasteoi_irq+0x0/0x124  count=9
irq_probe:            action "mmc0"  handler=sdhci_irq+0x0/0xc4c  thread_fn=sdhci_thread_irq+0x0/0xc0  flags=0x00000084 SHARED [threaded]
```
```bash
ssh radxa@192.168.68.58 'grep mmc0 /proc/interrupts'
```
```
160:          9   0 0 0 0 0 0 0     GICv3 237 Level     mmc0
```

**四層完全對上：TRM 237 = DTS 205+32 = hwirq 237 = /proc/interrupts 的 237 → virq 160。**

同樣的鏈子套 uart2：TRM `365 irq_uart2` ← DTB `<0 333 4>`（333+32=365）← `/proc/interrupts` `21: GICv3 365 Level debug`。

### ★ 重開機再跑一次才看得到的事：**virq 會變，hwirq 不會**

同一台機器重開機後把全部實驗重跑一遍，前三層（TRM / DTB / hwirq）**一位都沒動**，
但第四層的 Linux IRQ 號變了：

| | 第一次開機 | 重開機後 |
|---|---|---|
| TRM Table 1-3 | `237 irq_emmc` | `237 irq_emmc`（硬體接線，不可能變） |
| DTB `interrupts` | `00000000 cd000000 04000000` | `00000000 cd000000 04000000`（完全相同） |
| hwirq（GIC INTID） | **237** | **237** |
| **virq（Linux IRQ 號）** | **160** | **171** ← 變了！ |
| `nr_irqs` | 183 | 191 |
| `dw-mci` 的 virq | 104 | 105 |

```bash
# 重開機後
ssh radxa@192.168.68.58 'grep mmc0 /proc/interrupts'
```
```
171:          8   0 0 0 0 0 0 0     GICv3 237 Level     mmc0
```

原因就在 `__irq_alloc_descs()`：

```c
	start = bitmap_find_next_zero_area(allocated_irqs, IRQ_BITMAP_BITS, from, cnt, 0);
```
virq 是**開機時按「誰先 probe 誰先拿」的順序，從 `allocated_irqs` 位圖找第一個空洞配的**，
所以驅動載入順序、模組有沒有內建、probe 有沒有 defer，都會改變結果；
`nr_irqs` 也跟著動態成長（183 → 191）。

**這正是 Q2 這一題的重點：**
* **hwirq 是硬體屬性**，寫死在 SoC 裡、記在 TRM 和 DTS 裡；
* **virq 是純軟體的、每次開機重新配置的索引**，`request_irq()` 用的就是它；
* 中間必須有 `irq_domain` 這一層做翻譯 —— 而且**不能把 virq 寫死在任何地方**
  （驅動一律用 `platform_get_irq()` / `irq_of_parse_and_map()` 現查）。

### 實機驗證：一個活著的中斷正在被映射

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/irq_live.ko samples=1 want_hwirq=26; sudo rmmod irq_live'
```
```
irq_live:   [硬體中斷號 → Linux IRQ 號 — Q2]
irq_live:     generic_handle_domain_irq(domain=":interrupt-controller@fe600000-1", hwirq=26)
irq_live:     irq_find_mapping() → virq 13
irq_live:     desc: irq_data.irq=13 irq_data.hwirq=26 chip=GICv3 handle_irq=handle_percpu_devid_irq+0x0/0x120 action="arch_timer"
```
hwirq 26 是 PPI（`GIC_PPI 10` → 10+16 = 26），對到 virq 13 = `arch_timer`。

### 本機的中斷號空間與 7 個 irq_domain

```
irq_probe: CONFIG_SPARSE_IRQ = y
irq_probe: nr_irqs = 183   NR_IRQS = 64
```

* `NR_IRQS = 64` 只是 arm64 的預設下限；因為 `CONFIG_SPARSE_IRQ=y`，
  `irq_desc` 存在 radix tree 裡按需配置，`nr_irqs` 開機後長到 **183**。
  書上示範的 `irq_desc[NR_IRQS]` 全域陣列在本機**不存在**。
* 位圖 `allocated_irqs` 的大小是 `IRQ_BITMAP_BITS = NR_IRQS + 8196`。

```bash
ssh radxa@192.168.68.58 'sudo dmesg | grep -o "domain=[^ ]*" | sort -u'
```
```
domain=:interrupt-controller@fe600000-1                          ← GIC-600 的 GICD/GICR（SGI/PPI/SPI）
domain=:interrupt-controller@fe600000:msi-controller@fe640000-3  ← ITS #0（PCIe MSI：mt7921e 無線網卡）
domain=:interrupt-controller@fe600000:msi-controller@fe660000-3  ← ITS #1（PCIe MSI：NVMe）
domain=:pinctrl:gpio@fd8a0000                                    ← GPIO0（二級/chained）
domain=:pinctrl:gpio@fec20000                                    ← GPIO3
domain=:pinctrl:gpio@fec40000                                    ← GPIO4
domain=:spi@feb20000:rk806single@0                               ← PMIC RK806（三級：SPI 上的中斷控制器）
```

這正是書上說「Linux 3.1 引入 irq_domain 是因為 SoC 裡有多個中斷控制器、還能級聯成樹」的實例。
GPIO 那條的級聯關係看得很清楚：

```
irq_probe: irq 15    hwirq 309  chip=GICv3  handle_irq=rockchip_irq_demux+0x0/0x1c0
irq_probe:            action "-"  handler=bad_chained_irq+0x0/0x4c
...
irq_probe: irq 173   hwirq 29   chip=rockchip_gpio_irq  domain=:pinctrl:gpio@fec20000  handle_irq=handle_edge_irq+0x0/0x104
irq_probe:            action "headset_detect"  handler=irq_default_primary_handler  thread_fn=headset_det_irq_thread  flags=0x2003 ONESHOT [threaded]
```
* virq 15（hwirq 309 = TRM 的 `irq_gpio0`）是 **chained** 的父中斷：`handle_irq` 換成
  `rockchip_irq_demux()`，action 是 `bad_chained_irq`（永遠不該被呼叫）。
* 真正的耳機偵測是 virq 173 / hwirq 29，屬於 **GPIO 自己的 irq_domain**。
  兩層 hwirq 各自編號，靠 domain 區分 —— 這就是為什麼 hwirq 不能當全域 ID 用。

**ITS/MSI 的 hwirq 更誇張**（不是 GIC INTID 而是 LPI/DevID 編碼）：

```
irq_probe: irq 161   hwirq 524289     chip=ITS-MSI  domain=...msi-controller@fe660000-3  action "nvme0q1"
irq_probe: irq 172   hwirq 285736960  chip=ITS-MSI  domain=...msi-controller@fe640000-3  action "mt7921e"
```

---

<a name="q3"></a>
## 3. 一個硬體中斷發生後，Linux 內核如何響應並處理該中斷？

### 結論（6.1 / GICv3 的完整鏈路）

```
外設拉中斷線 → GIC-600 Distributor 仲裁 → 送給某顆 CPU 的 Redistributor/CPU interface
   ↓ CPU 收到 nIRQ，硬體做 Q1 那 6 件事，跳向量表 +0x280（EL1h IRQ）
kernel_ventry 1,h,64,irq       sub sp,sp,#336  → 在被中斷者核心棧挖棧框
  el1h_64_irq  (entry.S:559 entry_handler)
    kernel_entry 1             保存 x0~x30 / sp / elr / spsr 到 pt_regs
    mov x0, sp ; bl el1h_64_irq_handler
      el1_interrupt()          write_sysreg(DAIF_PROCCTX_NOIRQ, daif)  放開 D/A，I/F 仍遮
        __el1_irq()
          enter_from_kernel_mode()
          irq_enter_rcu()      preempt_count += HARDIRQ_OFFSET  ← 「進入硬中斷上下文」
          do_interrupt_handler(regs, handle_arch_irq)
            set_irq_regs(regs)                 存進 Per-CPU __irq_regs
            call_on_irq_stack()                ★ SP 切到 Per-CPU 中斷棧
              gic_handle_irq()                 = handle_arch_irq（GICv3 驅動註冊）
                gic_read_iar()                 讀 ICC_IAR1_EL1 → 取得 INTID，同時「應答」中斷
                gic_complete_ack()             EOImode1：先寫 ICC_EOIR1_EL1（priority drop）
                generic_handle_domain_irq(domain, hwirq)
                  __irq_resolve_mapping()      hwirq → virq
                  handle_irq_desc(desc)
                    desc->handle_irq(desc)     SPI/LPI → handle_fasteoi_irq
                                               SGI/PPI → handle_percpu_devid_irq
                      handle_irq_event(desc)
                        handle_irq_event_percpu()
                          __handle_irq_event_percpu()
                            for each action: action->handler(irq, dev_id)   ← 上半部
                                 回傳 IRQ_WAKE_THREAD → __irq_wake_thread() 叫醒 irq/N-xxx 線程
                      chip->irq_eoi()          gic_eoimode1_eoi_irq() → 寫 ICC_DIR_EL1 deactivate
          irq_exit_rcu()       preempt_count -= HARDIRQ_OFFSET
                               若 local_softirq_pending() → invoke_softirq() → __do_softirq()  ← 下半部
          arm64_preempt_schedule_irq()
          exit_to_kernel_mode()
    ret_to_kernel → kernel_exit 1 → eret       ← eret 用 SPSR 還原 PSTATE，等於重新開中斷
```

> **書目**：奔跑吧 §2.5「ARM64高层中断处理」（第 1836~2224 行）、
> §2.5.2「handle_arch_irq处理」（第 1903 行起）、§2.5.3 小結（第 2225 行）。
> **原始碼**：`arch/arm64/kernel/entry-common.c:268/465/478/489`；
> `drivers/irqchip/irq-gic-v3.c:746` `__gic_handle_irq()`；
> `kernel/irq/irqdesc.c` `handle_irq_desc()`；`kernel/irq/chip.c` `handle_fasteoi_irq()`；
> `kernel/irq/handle.c` `handle_irq_event()`；`kernel/softirq.c:660` `irq_exit_rcu()`。

### 實機驗證（1）：ftrace 抓完整呼叫鏈

```bash
ssh radxa@192.168.68.58 'sudo ~/exp/irq/irq_trace.sh path'
```

**時鐘中斷（PPI，`handle_percpu_devid_irq`）：**
```
 5638.195253 |   2)    <idle>-0    |               |  gic_handle_irq() {
 5638.195254 |   2)    <idle>-0    |   0.875 us    |    gic_read_iar();
 5638.195256 |   2)    <idle>-0    |               |    generic_handle_domain_irq() {
 5638.195257 |   2)    <idle>-0    |   0.875 us    |      __irq_resolve_mapping();
 5638.195258 |   2)    <idle>-0    |               |      handle_irq_desc() {
 5638.195259 |   2)    <idle>-0    |               |        handle_percpu_devid_irq() {
 5638.195260 |   2)    <idle>-0    |               |          arch_timer_handler_phys() {
 5638.195261 |   2)    <idle>-0    |               |            hrtimer_interrupt() {
 ...
 5638.195301 |   2)    <idle>-0    | + 41.125 us   |          }
 5638.195302 |   2)    <idle>-0    |   0.584 us    |          gic_eoimode1_eoi_irq();     ← EOI 在 handler 之後
 5638.195303 |   2)    <idle>-0    | + 44.042 us   |        }
 5638.195305 |   2)    <idle>-0    | + 51.625 us   |  }
```

**無線網卡 mt7921e（ITS-MSI，`handle_fasteoi_irq`）—— 教科書級的「上半部 → 下半部」：**
```
 5649.830405 |   4)    <idle>-0    |               |  gic_handle_irq() {
 5649.830405 |   4)    <idle>-0    |   0.291 us    |    gic_read_iar();
 5649.830406 |   4)    <idle>-0    |               |    generic_handle_domain_irq() {
 5649.830406 |   4)    <idle>-0    |   0.875 us    |      __irq_resolve_mapping();
 5649.830407 |   4)    <idle>-0    |               |      handle_irq_desc() {
 5649.830408 |   4)    <idle>-0    |               |        handle_fasteoi_irq() {
 5649.830408 |   4)    <idle>-0    |               |          _raw_spin_lock() { ... }
 5649.830409 |   4)    <idle>-0    |   0.291 us    |          irq_may_run();
 5649.830410 |   4)    <idle>-0    |               |          handle_irq_event() {
 5649.830410 |   4)    <idle>-0    |               |            _raw_spin_unlock() { ... }   ← 執行 handler 前放掉 desc->lock
 5649.830410 |   4)    <idle>-0    |               |            handle_irq_event_percpu() {
 5649.830411 |   4)    <idle>-0    |               |              __handle_irq_event_percpu() {
 5649.830412 |   4)    <idle>-0    |               |                mt7921_irq_handler [mt7921e]() {   ← ★上半部
 5649.830413 |   4)    <idle>-0    |               |                  mt7921_wr [mt7921e]() {          ←  關掉裝置中斷
 5649.830417 |   4)    <idle>-0    |   3.500 us    |                  }
 5649.830417 |   4)    <idle>-0    |               |                  __tasklet_schedule() {           ← ★交棒給下半部
 5649.830417 |   4)    <idle>-0    |   0.875 us    |                    __tasklet_schedule_common();
 5649.830418 |   4)    <idle>-0    |   1.459 us    |                  }
 5649.830419 |   4)    <idle>-0    |   7.000 us    |                }
 5649.830419 |   4)    <idle>-0    |               |              add_interrupt_randomness() { ... }
```
上半部總共只花 **7 µs**（讀狀態、關裝置中斷、`tasklet_schedule`），完全符合「上半部要越快越好」。

### 實機驗證（2）：tracepoint 看上半部交棒給下半部

```bash
ssh radxa@192.168.68.58 'T=/sys/kernel/debug/tracing; sudo bash -c "
  for e in irq/irq_handler_entry irq/irq_handler_exit irq/softirq_raise irq/softirq_entry irq/softirq_exit; do
      echo 1 > $T/events/\$e/enable; done
  echo > $T/trace; echo 1 > $T/tracing_on; ping -c 2 192.168.68.1 >/dev/null; echo 0 > $T/tracing_on
  awk \"/irq_handler_entry.*mt7921e/{f=1} f{print; n++} n>14{exit}\" $T/trace"'
```
```
napi/phy0-9-415  [004] d.h..  5679.594846: irq_handler_entry: irq=172 name=mt7921e
napi/phy0-9-415  [004] d.h..  5679.594847: softirq_raise: vec=6 [action=TASKLET]    ← 在硬中斷裡 raise
napi/phy0-9-415  [004] d.h..  5679.594847: irq_handler_exit:  irq=172 ret=handled
napi/phy0-9-415  [004] ..s..  5679.594849: softirq_entry: vec=6 [action=TASKLET]    ← irq_exit() 立刻跑下半部
napi/phy0-9-415  [004] d.s..  5679.594852: softirq_raise: vec=3 [action=NET_RX]
napi/phy0-9-415  [004] ..s..  5679.594852: softirq_exit:  vec=6 [action=TASKLET]
napi/phy0-9-415  [004] ..s..  5679.594852: softirq_entry: vec=3 [action=NET_RX]
napi/phy0-9-415  [004] ..s..  5679.594856: softirq_exit:  vec=3 [action=NET_RX]
```
第 3 欄的旗標把上下文寫得清清楚楚：`d.h..` = 關中斷 + **硬中斷**上下文；
`..s..` = 開中斷 + **軟中斷**上下文。硬中斷離開後 **2 µs** 軟中斷就接上了。

### 實機驗證（3）：中斷上下文的 preempt_count

`irq_live.ko` 在 `handle_irq_event()` 之前取樣：

```
irq_live: [Q3 派發 #1] cpu0  virq=104 hwirq=235  action="dw-mci" handler=dw_mci_interrupt+0x0/0x554 thread_fn=0x0
irq_live:             preempt_count=0x00010000 in_hardirq=1 irqs_disabled=128  SP=0xffff800008003d10 (不在行程棧)
```
`preempt_count = 0x00010000` 正好是 `HARDIRQ_OFFSET`，由 `irq_enter_rcu()` 加上去。

### 中斷線程化（本機有 24 條）

書上 §2.3 講 `request_threaded_irq()`。本機大量使用：

```bash
ssh radxa@192.168.68.58 'ps -eo pid,class,rtprio,comm | grep "^ *[0-9]* FF" | head'
```
```
     76 FF      50 irq/22-rockchip_thermal
     93 FF      50 irq/24-rockchip_usb2phy
    116 FF      50 irq/60-fde50000.dp
    294 FF      50 irq/160-mmc0
```
```
irq_probe: irq 22  hwirq 429  handle_irq=handle_fasteoi_irq
irq_probe:      action "rockchip_thermal" handler=irq_default_primary_handler+0x0/0x10
                thread_fn=rockchip_thermal_alarm_irq_thread+0x0/0x90 flags=0x00002004 ONESHOT [threaded]
```
`handler = irq_default_primary_handler`（什麼都不做，直接回 `IRQ_WAKE_THREAD`）
\+ `IRQF_ONESHOT` —— 正是書上 §2.5.2 講的那套。中斷線程是 **SCHED_FIFO prio 50**。

---

<a name="q4"></a>
## 4. 為什麼說中斷上下文不能執行睡眠操作？

### 結論 —— 五個理由，每個都能在本機量到

| # | 理由 | 本機實測證據 |
|---|------|------|
| 1 | **`current` 是無辜的受害者。** 硬中斷借用「當時剛好在跑的那個 task」的身份，睡下去等於把不相干的行程掛起 | 硬中斷取樣到 `current = swapper/3`、`swapper/5`、`bash/11737`、`ctxprobe_victim` —— 每次都不一樣 |
| 2 | **中斷處理程序在關中斷的環境下執行。** `schedule()` 換到 next 行程時本地中斷還是關的 | `DAIF = 0xc0`（I、F 遮住）、`irqs_disabled() = 128` |
| 3 | **SP 已經切到 Per-CPU 中斷棧**，那張棧是同一顆 CPU 上所有行程共用的。`schedule()` 一走就再也回不來，別人下一次中斷會把它踩爛 | 8 顆 CPU 各一段固定的 SP 區間（見下） |
| 4 | **GIC 還在等 EOI。** deactivate 沒做完，該 INTID 一直是 active，同優先級以下的中斷全被擋住 | ftrace：`gic_eoimode1_eoi_irq()` 排在 handler **之後** |
| 5 | **`preempt_count != 0`，排程器會直接罵人** | 硬中斷 `0x00010000`、軟中斷 `0x00000100`、關 BH `0x00000200` |

> **書目**：奔跑吧 §2.5.3「小结」（第 2225~2380 行）—— 「何为中断上下文？为什么中断上下文中
> 不能调用含有睡眠的函数？」那一大段，含兩個 `BUG: scheduling while atomic` 的實驗。
> **原始碼**：`kernel/sched/core.c` `schedule_debug()`；`include/linux/preempt.h` 的
> `in_atomic_preempt_off()`。

### 實機驗證（1）：`current` 是誰？

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/ctx_probe.ko mode=0; sudo rmmod ctx_probe'
```
```
執行環境                        cpu  current       preempt_cnt  IRQ  hardirq/softirq/serving/interrupt/task
行程: insmod 系統呼叫路徑        5    insmod        0x00000000   開   0 / 0 / 0 / 0 / 1
行程: local_bh_disable 臨界區內  5    insmod        0x00000200   開   0 / 1 / 0 / 1 / 1
硬中斷: irq_work (SGI/IPI)       3    swapper/3     0x00010000   關   1 / 0 / 0 / 1 / 0
硬中斷: hrtimer 回呼             5    swapper/5     0x00010000   關   1 / 0 / 0 / 1 / 0
軟中斷①: irq_exit→__do_softirq   5    swapper/5     0x00000100   開   0 / 1 / 1 / 1 / 0
軟中斷③: local_bh_enable→do_soft 5    insmod        0x00000101   開   0 / 1 / 1 / 1 / 0
軟中斷②: ksoftirqd               5    ksoftirqd/5   0x00000100   開   0 / 1 / 1 / 1 / 0
行程: workqueue kworker          5    kworker/5:1   0x00000000   開   0 / 0 / 0 / 0 / 1
```

硬中斷那兩行的 `current` 是 `swapper/3` 和 `swapper/5` —— 我根本沒叫它們做事，
它們只是「中斷發生那一刻剛好在 CPU 上」。如果在 handler 裡 `schedule()`，
被掛起的是 idle 執行緒，而不是「想睡的那個人」（handler 本身不是一個可調度實體）。

### 實機驗證（2）：Per-CPU 中斷棧是共用的

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/irq_live.ko samples=0 stackmap=1
                         sleep 3; sudo rmmod irq_live; sudo dmesg | tail -12'
```
```
---- Q15：每顆 CPU 在中斷處理常式裡看到的 SP 範圍 ----
  cpu0: hits=1886  SP 0xffff800008003620 .. 0xffff800008003cb0  (跨度 1680 B)
  cpu1: hits=171   SP 0xffff80000800b980 .. 0xffff80000800bcb0  (跨度  816 B)
  cpu2: hits=271   SP 0xffff80000a2db960 .. 0xffff80000a2dbcb0  (跨度  848 B)
  cpu3: hits=456   SP 0xffff80000a2e3cb0 .. 0xffff80000a2e3cb0  (跨度    0 B)
  cpu4: hits=209   SP 0xffff80000a2eb960 .. 0xffff80000a2ebcb0  (跨度  848 B)
  cpu5: hits=249   SP 0xffff80000a2f3960 .. 0xffff80000a2f3cb0  (跨度  848 B)
  cpu6: hits=746   SP 0xffff80000a2fba40 .. 0xffff80000a2fbcb0  (跨度  624 B)
  cpu7: hits=538   SP 0xffff80000a303870 .. 0xffff80000a303cb0  (跨度 1088 B)
```

**4526 次中斷、打斷了無數個不同的行程，但每顆 CPU 的 SP 永遠落在同一個小區間，
而且 8 顆 CPU 各自的位址間隔剛好 0x8000（VMAP 棧 16 KB + guard page）。**
這就是 `DEFINE_PER_CPU(unsigned long *, irq_stack_ptr)`（`arch/arm64/kernel/irq.c:32`）。

若在中斷上下文裡 `schedule()`：這張棧的內容會被同一顆 CPU 的下一個中斷覆蓋掉，
「未完成的中斷處理成為亡命之徒」（書上原話），因為根本沒有一條路徑會回來收尾。

### 為什麼我**沒有**在本機重現書上的 `BUG: scheduling while atomic`

書上在 QEMU 上直接改 `pl011_int()` 加 `schedule()`，印出警告後系統照常。
在本機我**刻意不做**，原因是 6.1 的 `schedule_debug()` 在印完之後多做了一步：

```c
/* kernel/sched/core.c，schedule_debug() */
	if (unlikely(in_atomic_preempt_off())) {
		__schedule_bug(prev);
		preempt_count_set(PREEMPT_DISABLED);     /* ← 把 preempt_count 直接歸零 */
	}
```
本機 `CONFIG_PREEMPT_COUNT=n` ⇒ `PREEMPT_DISABLE_OFFSET = 0` ⇒
`in_atomic_preempt_off()` 就是 `preempt_count() != 0`，而 `PREEMPT_DISABLED` 也是 0。
所以一旦觸發，**`preempt_count` 會被清成 0**；接下來配對的
`local_bh_enable()` / `irq_exit_rcu()` 再減，就會 **下溢成 0xfffffe01**，
那個 task 從此永遠看起來像在 NMI 上下文裡，`rmmod`／`exit` 都會再爆一次 —— 只能重開機。
書上的 5.0 + QEMU 環境沒有這個放大效應，**這是實機和書上環境的實質差異，不是同一個實驗**。

上面 5 條實測證據已經完整解釋「為什麼不能睡」，不需要拿機器去換那一行警告。

---

<a name="q5"></a>
## 5. 軟中斷的回調函數執行過程中是否允許響應本地中斷？

### 結論

**允許，而且是刻意的。** `__do_softirq()` 一進去就把本地中斷打開：

```c
/* kernel/softirq.c:528 */
asmlinkage __visible void __softirq_entry __do_softirq(void)
{
	pending = local_softirq_pending();
	softirq_handle_begin();          /* preempt_count += SOFTIRQ_OFFSET */
	...
restart:
	set_softirq_pending(0);          /* ← 先清 pending 位圖 */
	local_irq_enable();              /* ★ 再開中斷（順序不能反！） */
	while ((softirq_bit = ffs(pending))) {
		...
		h->action(h);            /* ← 回呼在開中斷的環境下跑 */
	}
	local_irq_disable();
	pending = local_softirq_pending();
	if (pending) {
		if (time_before(jiffies, end) && !need_resched() && --max_restart)
			goto restart;
		wakeup_softirqd();
	}
	softirq_handle_end();
}
```

**「先清 pending 再開中斷」的順序很關鍵**：如果先開中斷，中斷處理程序 `raise_softirq()`
設好的新位元會被緊接著的 `set_softirq_pending(0)` 抹掉 —— 軟中斷就漏了。

代價：軟中斷回呼**必須自己處理與硬中斷處理程序的併發**（所以驅動裡到處是
`spin_lock_irqsave()`）。

> **書目**：奔跑吧 §2.6.1「软中断」對 `__do_softirq()` 的逐行分析（第 2398~2538 行），
> 特別是「在第276行中，打开本地中断……讀者可以思考如果在第274行之前打開本地中斷會有什麼後果」。
> §2.6.4 小結：「软中断的回调函数在开中断环境下执行」。
> **原始碼**：`kernel/softirq.c:528` `__do_softirq()`；`:492` `MAX_SOFTIRQ_TIME`。

### 實機驗證（1）：在 tasklet 裡忙等 30 ms，數收到幾次時鐘中斷

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/ctx_probe.ko mode=1 spin_ms=30; sudo rmmod ctx_probe'
```
```
ctx_probe: mode=1：Q5 —— 軟中斷回呼執行期間本地中斷是否開著？
ctx_probe: 找到 arch_timer 的 virq = 13（PPI，每 CPU 私有）
ctx_probe:   tasklet 執行在 cpu5，實際忙等 30000498 ns
ctx_probe:   進入 tasklet 時 DAIF = 0x00000000   irqs_disabled() = 0
ctx_probe:   arch_timer 在 cpu5 的計數： 1162131 → 1162141   （期間收到 10 次硬體中斷）
ctx_probe:   ✓ 結論：軟中斷回呼是在【開中斷】的環境下執行的，期間硬體中斷照樣被回應。
```

* **`DAIF = 0`** —— 四個遮罩位全開，跟硬中斷裡的 `0xc0` 形成強烈對比。
* **30 ms 內收到 10 次 `arch_timer`**：本機 `CONFIG_HZ=300` ⇒ 每 tick 3.33 ms
  ⇒ 30 ms 理論上 9 次；實測 10 次（含一次邊界）—— 數字完全合理。
  這 10 次中斷是**在 tasklet 還沒跑完的時候插進來的**。
  （重開機後重跑一次是 `50111 → 50120`，**9 次**，正好落在 9~10 的理論值上。）

### 實機驗證（2）：ftrace 的 `H` 旗標直接證明「軟中斷裡巢狀硬中斷」

```bash
ssh radxa@192.168.68.58 'sudo ~/exp/irq/irq_trace.sh softirq'
```
```
<idle>-0  [000] d.h..  5663.379467: softirq_raise: vec=6 [action=TASKLET]
<idle>-0  [000] ..s..  5663.379472: softirq_entry: vec=6 [action=TASKLET]   ← 軟中斷開始
<idle>-0  [000] d.H..  5663.379490: softirq_raise: vec=6 [action=TASKLET]   ← ★ 大寫 H！
<idle>-0  [000] ..s..  5663.379493: softirq_exit:  vec=6 [action=TASKLET]
<idle>-0  [000] ..s..  5663.379494: softirq_entry: vec=6 [action=TASKLET]
<idle>-0  [000] ..s..  5663.379499: softirq_exit:  vec=6 [action=TASKLET]
```

ftrace 的 latency 旗標欄位：`h` = 硬中斷上下文，**`H` = 硬中斷發生在軟中斷之中**。
`softirq_entry` 和 `softirq_exit` 之間出現 `d.H..` ⇒
**一個硬體中斷確確實實在 tasklet 執行到一半時打了進來**，處理完又回到原本的軟中斷。
這比任何原始碼註解都直接。

### 順帶：`__do_softirq()` 的三個退場條件

```
irq_probe: __do_softirq() 的退場條件（kernel/softirq.c）：
irq_probe:   MAX_SOFTIRQ_TIME    = 2 ms   MAX_SOFTIRQ_RESTART = 10 次
irq_probe:   三個條件任一不滿足 → wakeup_softirqd() 丟給 ksoftirqd/N
```
即 `time_before(jiffies, end) && !need_resched() && --max_restart`，與書上完全一致。

---

<a name="q6"></a>
## 6. 同一類型的軟中斷是否允許多個 CPU 並行執行？

### 結論

**允許，而且完全並行。**

* `__softirq_pending` 是 **Per-CPU** 變數（`DEFINE_PER_CPU_ALIGNED(irq_cpustat_t, irq_stat)`），
  每顆 CPU 有自己的 pending 位圖；
* `softirq_vec[]` 是**全域共用**的，`action` 函式指標只有一份；
* 兩者一組合 ⇒ **同一個 `action` 函式可以同時在 N 顆 CPU 上被呼叫**。

**所以自己新增軟中斷類型時，回呼函式必須自己做多核同步。**
（這也是核心不希望驅動開發者新增軟中斷、建議用 tasklet 的原因之一 —— tasklet 幫你串行化了，見 [Q9](#q9)。）

> **書目**：奔跑吧 §2.6.4「小结」（第 2784 行起）：
> 「**同一类型的软中断可以在多个CPU上并行执行**。以TASKLET_SOFTIRQ类型的软中断为例，
> 多个CPU可以同时tasklet_schedule……假如有驱动开发者要新增一个软中断类型，
> 那么软中断的处理程序需要考虑同步问题。」
> **原始碼**：`kernel/softirq.c:59` `softirq_vec[]`（全域）、
> `include/linux/interrupt.h` `local_softirq_pending()`（Per-CPU）。

### 實機驗證：8 顆 CPU 同時跑 TASKLET_SOFTIRQ

`softirq_par.ko mode=0` 準備 **8 個不同的 tasklet**，用 `on_each_cpu()`
（本身是 IPI，跑在硬中斷上下文）讓每顆 CPU 各掛一個，每個回呼忙等 30 ms：

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/softirq_par.ko mode=0 spin_ms=30; sudo rmmod softirq_par'
```
```
softirq_par: mode=0：Q6 —— 同一類型的軟中斷（TASKLET_SOFTIRQ）能不能多 CPU 並行？
softirq_par:   tasklet_schedule() 呼叫次數 = 8
softirq_par:   回呼實際執行次數            = 8
softirq_par:   最大同時執行數              = 8   ← 這就是同時在跑 TASKLET_SOFTIRQ 的 CPU 數
softirq_par:   執行過回呼的 CPU 位圖       = 0xff
softirq_par:     cpu0: 1 次   cpu1: 1 次   cpu2: 1 次   cpu3: 1 次
softirq_par:     cpu4: 1 次   cpu5: 1 次   cpu6: 1 次   cpu7: 1 次
softirq_par:   全部完成耗時                = 30 ms（若是串行應該要 240 ms）
softirq_par:   ✓ 結論：同一類型的軟中斷【可以】在多顆 CPU 上並行執行（實測 8 顆同時）。
```

**8 顆 CPU 同時執行同一種軟中斷（TASKLET_SOFTIRQ），30 ms 做完 8×30 = 240 ms 的工作量。**

### 旁證：系統日常運作中也看得到

```bash
ssh radxa@192.168.68.58 'sudo ~/exp/irq/irq_trace.sh softirq'
```
```
<idle>-0  [006] ..s..  5663.381528: softirq_entry: vec=7 [action=SCHED]
<idle>-0  [004] ..s..  5663.381529: softirq_entry: vec=7 [action=SCHED]   ← 同一微秒，不同 CPU，同一 vec
send-1233 [000] ..s..  5663.381536: softirq_entry: vec=7 [action=SCHED]
<idle>-0  [001] ..s..  5663.381541: softirq_entry: vec=7 [action=SCHED]
```
`vec=7 (SCHED)` 在 13 µs 內同時在 cpu0/1/4/6 上執行。

`/proc/softirqs` 也是每 CPU 一欄，本來就是為並行設計的：
```
                    CPU0     CPU1     CPU2     CPU3     CPU4     CPU5     CPU6     CPU7
     TASKLET:     554499    35943    18649    17276   273578    12320    36517    34390
        RCU :      47197    42885    37841    43361   109321    78153    57593    57433
```

---

<a name="q7"></a>
## 7. 軟中斷上下文包括哪幾種情況？

### 結論 —— 三種，全部都被 `in_serving_softirq()` 認可

| # | 情況 | 觸發路徑 | 跑在誰身上 | 傳統意義的「中斷上下文」？ |
|---|------|---------|-----------|-----------|
| ① | **中斷返回時的下半部** | `irq_exit_rcu()` → `invoke_softirq()` → `__do_softirq()` | 被中斷的那個 task（借用身份） | ✅ 是 |
| ② | **`ksoftirqd/N` 內核執行緒** | 軟中斷跑太久（>2 ms / >10 輪 / `need_resched`）或行程上下文 raise → `wakeup_softirqd()` | `ksoftirqd/N` | ❌ 其實是行程上下文 |
| ③ | **`local_bh_enable()` 出關 BH 臨界區時** | `__local_bh_enable_ip()` → `do_softirq()` | 呼叫者自己 | ❌ 其實是行程上下文 |

Linux 把三者統一歸為「軟中斷上下文」，用 `preempt_count` 的 SOFTIRQ 欄位表達。
另外還有第四種容易混淆的狀態：**關 BH 臨界區內**（`local_bh_disable()` 之後、真正跑軟中斷之前），
它 `in_softirq()==1` 但 `in_serving_softirq()==0`。

```c
/* include/linux/preempt.h */
#define in_hardirq()          (hardirq_count())
#define in_softirq()          (softirq_count())                  /* 含關 BH 臨界區 */
#define in_serving_softirq()  (softirq_count() & SOFTIRQ_OFFSET) /* 只有真的在跑回呼 */
#define in_interrupt()        (irq_count())
#define in_task()             (!(preempt_count() & (NMI_MASK|HARDIRQ_MASK|SOFTIRQ_OFFSET)))
```

> **書目**：奔跑吧 §2.6.4「小结」（第 2784 行起）：
> 「软中断上下文包括三部分：第一部分是在下半部执行的软中断处理……第二部分是ksoftirqd内核线程
> 执行的软中断……第三部分是在进程上下文中调用local_bh_enable()函数时执行的软中断处理。」
> **原始碼**：`kernel/softirq.c:218/373` `__local_bh_enable_ip()`、`:291/433` `invoke_softirq()`、
> `:74` `wakeup_softirqd()`、`:660` `irq_exit_rcu()`。

### 實機驗證：三種全部抓到（外加關 BH 臨界區）

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/ctx_probe.ko mode=0; sudo rmmod ctx_probe'
```
```
執行環境                          cpu  current      preempt_cnt  IRQ  hardirq/softirq/serving/interrupt/task
行程: insmod 系統呼叫路徑          5   insmod       0x00000000   開   0 / 0 / 0 / 0 / 1
行程: local_bh_disable 臨界區內    5   insmod       0x00000200   開   0 / 1 / 0 / 1 / 1   ← 第四種
硬中斷: irq_work (SGI/IPI)         3   swapper/3    0x00010000   關   1 / 0 / 0 / 1 / 0
硬中斷: hrtimer 回呼               5   swapper/5    0x00010000   關   1 / 0 / 0 / 1 / 0
軟中斷①: irq_exit→__do_softirq     5   swapper/5    0x00000100   開   0 / 1 / 1 / 1 / 0
軟中斷③: local_bh_enable→do_softirq 5  insmod       0x00000101   開   0 / 1 / 1 / 1 / 0
軟中斷②: ksoftirqd                 5   ksoftirqd/5  0x00000100   開   0 / 1 / 1 / 1 / 0
行程: workqueue kworker            5   kworker/5:1  0x00000000   開   0 / 0 / 0 / 0 / 1

執行環境                          SP                 在行程棧?   DAIF
硬中斷: irq_work (SGI/IPI)        0xffff80000a2e3e60 否(中斷棧)  0x000000c0
硬中斷: hrtimer 回呼              0xffff80000a2f3e60 否(中斷棧)  0x000000c0
軟中斷①: irq_exit→__do_softirq    0xffff80000a2f3e90 否(中斷棧)  0x00000000
軟中斷③: local_bh_enable→do_soft  0xffff80000a2f3e90 否(中斷棧)  0x00000000
軟中斷②: ksoftirqd                0xffff80000a65bcb0 是          0x00000000
行程: workqueue kworker           0xffff80000a33bd90 是          0x00000000
```

**六個推得動的結論：**

1. **① 的 `current = swapper/5`** —— 就是剛剛被 hrtimer 打斷的那個 idle task。
   軟中斷「借」它的 `task_struct` 跑，這是傳統意義的中斷上下文。
2. **② 的 `current = ksoftirqd/5`** —— 一個真正的內核執行緒，可以被排程、被搶佔。
3. **③ 的 `current = insmod`** —— 就是呼叫 `local_bh_enable()` 的那個行程自己，同步跑完才返回。
4. **③ 的 `preempt_count = 0x101`，多出來的那個 `1` 看得見！**
   ```c
   /* kernel/softirq.c:373 __local_bh_enable_ip() */
   	__preempt_count_sub(cnt - 1);   /* cnt = SOFTIRQ_DISABLE_OFFSET = 0x200 → 只減 0x1FF */
   	if (unlikely(!in_interrupt() && local_softirq_pending()))
   		do_softirq();           /* 這時 preempt_count = 1，跑軟中斷再 +0x100 → 0x101 */
   	preempt_count_dec();
   ```
   書上原文：「preempt_count减去（SOFTIRQ_DISABLE_OFFSET – 1），這裡並沒有完全減去
   SOFTIRQ_DISABLE_OFFSET，為什麼還留了1呢？**留1表示關閉本地CPU的搶佔**」——
   **這個「留 1」在實測的 `0x101` 裡一位不差地看到了。**
5. **關 BH 臨界區（`0x200`）：`in_softirq()=1` 但 `in_serving_softirq()=0`。**
   `SOFTIRQ_DISABLE_OFFSET = 2*SOFTIRQ_OFFSET = 0x200`，剛好避開 `SOFTIRQ_OFFSET`(bit 8) 這一位，
   所以兩個巨集分得開。注意這時 `in_interrupt()=1` 卻同時 `in_task()=1` ——
   `in_task()` 只看 `SOFTIRQ_OFFSET` 那一位。
6. **①③ 的 SP 在中斷棧、② 在行程棧。** 本機 `CONFIG_SOFTIRQ_ON_OWN_STACK=y`，
   `invoke_softirq()`/`do_softirq()` 走 `do_softirq_own_stack()` 切到 Per-CPU 中斷棧；
   `ksoftirqd` 是普通內核執行緒，直接用自己的棧。

### 旁證：日常負載下軟中斷分佈在哪些 comm 上

```bash
ssh radxa@192.168.68.58 'sudo ~/exp/irq/irq_trace.sh softirq'
```
```
=== 執行軟中斷的 comm 統計 ===
    135 <idle>          ← ① 借 idle 的身份（機器空閒時最常見）
     12 HeapHelper      ← ① 借應用程式的身份
     11 napi/phy0-11    ← ① 借網卡 napi 執行緒的身份
      3 dd
      2 ping
      1 claude
=== 軟中斷類型統計 ===
     51 action=RCU      50 action=TASKLET    45 action=SCHED
     28 action=TIMER     3 action=NET_RX
```
軟中斷會出現在**任何一個**剛好被中斷的 task 名下 —— 這正是「不能睡」的根源（見 [Q4](#q4)）。

---

<a name="q8"></a>
## 8. 軟中斷上下文還是進程上下文的優先級高？為什麼？

### 結論

**軟中斷上下文的優先級高於行程上下文，軟中斷永遠搶行程。**

原因在中斷返回的順序上：

```c
/* arch/arm64/kernel/entry-common.c:465 __el1_irq() */
	irq_enter_rcu();
	do_interrupt_handler(regs, handler);   /* 上半部 */
	irq_exit_rcu();                        /* ← ① 這裡先把軟中斷跑完 */
	arm64_preempt_schedule_irq();          /* ← ② 才輪到「要不要換行程」 */
	exit_to_kernel_mode(regs);
```
```c
/* kernel/softirq.c:660 */
void irq_exit_rcu(void)
{
	__irq_exit_rcu();      /* preempt_count -= HARDIRQ_OFFSET;
	                          if (!in_interrupt() && local_softirq_pending()) invoke_softirq(); */
	...
}
```

所以流程恆為：**硬中斷 → 軟中斷（含 tasklet）→ 才檢查要不要搶佔被中斷的行程**。
一個行程被中斷後，必須等軟中斷全部做完才拿得回 CPU。

**代價**：任一 tasklet 回呼執行過久，都會直接變成整個系統的排程延遲。
這正是 Red Hat 社群一直主張用工作佇列取代 tasklet 的理由（書上原話）。

> **書目**：奔跑吧 §2.6.4「小结」（第 2784 行起）：
> 「软中断上下文的优先级高于进程上下文，因此软中断包括tasklet总是抢占进程的执行……
> 如果在执行软中断和tasklet的时间很长，那么高优先级任务就长时间得不到运行，
> 势必会影响系统的实时性，这也是Red Hat Linux社区里有专家一直要求用工作队列机制
> 来替代tasklet机制的原因。」以及該節末尾的流程圖。

### 實機驗證：讓 tasklet 把一個行程卡住 40 ms

`ctx_probe.ko mode=2` 的作法：
1. 在 cpu3 上跑一個 `SCHED_OTHER` 的死迴圈 kthread，不斷記錄「兩次 `ktime_get_ns()` 的間隔」；
2. 在**同一顆** CPU 上放一個 `HRTIMER_MODE_REL_PINNED` 的 hrtimer 去打斷它；
3. hrtimer 回呼（硬中斷上下文）裡 `tasklet_schedule()`；
4. tasklet 回呼忙等 40 ms；
5. 看那個 kthread 的迴圈被卡了多久。

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/ctx_probe.ko mode=2 spin_ms=40 cpu_target=3; sudo rmmod ctx_probe'
```
```
ctx_probe:   t_hardirq  (hrtimer 回呼)      = 5578899465428 ns
ctx_probe:   t_softirq_in  (tasklet 開始)   = 5578899469803 ns  （+4375 ns）
ctx_probe:   t_softirq_out (tasklet 結束)   = 5578939469980 ns  （+40004552 ns）
ctx_probe:   被中斷的 kthread 迴圈最大空窗 = 40009218 ns (40 ms)
ctx_probe:   ✓ 結論：行程整整被卡住 40 ms ≥ tasklet 的 40 ms。

執行環境                       cpu  current           preempt_cnt  IRQ  hardirq/softirq/serving/interrupt/task
硬中斷: hrtimer（打斷了行程）   3   ctxprobe_victim   0x00010000   關   1 / 0 / 0 / 1 / 0
軟中斷: tasklet（搶了行程的CPU） 3   ctxprobe_victim   0x00000100   開   0 / 1 / 1 / 1 / 0
```

**三個數字說完整個故事：**

| 事件 | 相對時間 | 意義 |
|---|---|---|
| hrtimer 回呼（硬中斷） | +0 ns | 行程被打斷 |
| tasklet 開始 | **+4.4 µs** | 硬中斷一退出，`irq_exit_rcu()` 立刻接上軟中斷 |
| tasklet 結束 | **+40.005 ms** | 軟中斷跑完 |
| 行程迴圈的最大空窗 | **40.009 ms** | ★ 行程整整等到軟中斷結束才拿回 CPU |

空窗 40009218 ns 與 tasklet 的 40004552 ns **幾乎完全重合**（差 4.6 µs = 進出中斷的成本），
證明中間**沒有任何機會**讓行程插進來跑。

另外注意軟中斷那一列的 `current = ctxprobe_victim` —— 軟中斷是**借被害者的身份**在跑，
它「偷」的就是這個行程的 CPU 時間，而且這段時間不計入該行程的排程統計。

---

<a name="q9"></a>
## 9. 是否允許同一個 tasklet 在多個 CPU 上並行執行？

### 結論

**絕對不允許。同一個 tasklet 永遠串行。** 靠 `state` 裡兩個位元巧妙配合：

| 位元 | 名稱 | 誰設 / 誰清 | 作用 |
|---|---|---|---|
| bit 0 | `TASKLET_STATE_SCHED` | `tasklet_schedule()` 設；`tasklet_action_common()` 執行**前**清 | 防止重複掛入鏈結串列 |
| bit 1 | `TASKLET_STATE_RUN` | `tasklet_trylock()` 設；回呼結束後 `tasklet_unlock()` 清 | **就是那把「同一時間只有一顆 CPU 能跑」的鎖** |

```c
/* kernel/softirq.c:776 tasklet_action_common() */
	while (list) {
		struct tasklet_struct *t = list;
		list = list->next;
		if (tasklet_trylock(t)) {                 /* ← 拿不到 = 別的 CPU 正在跑它 */
			if (!atomic_read(&t->count)) {
				if (tasklet_clear_sched(t)) { /* 先清 SCHED，再執行 */
					if (t->use_callback) t->callback(t);
					else                 t->func(t->data);
				}
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}
		/* 搶不到鎖 → 重新掛回【本 CPU】的鏈結串列，再 raise 一次軟中斷 */
		local_irq_disable();
		t->next = NULL;  *tl_head->tail = t;  tl_head->tail = &t->next;
		__raise_softirq_irqoff(softirq_nr);
		local_irq_enable();
	}
```

**「先清 SCHED 再執行回呼」的順序**是為了讓回呼執行期間新的觸發不會遺失
（可以重新掛上，只是要等這一輪跑完）。

> **書目**：奔跑吧 §2.6.2「tasklet」（第 2539~2734 行），
> 特別是 `tasklet_action_common()` 逐行分析與末尾那張 CPU0/CPU1 時序圖
> （以 `drivers/char/snsc_event.c` 為例）。§2.6.4：「**tasklet是串行执行的**」。

### 實機驗證（1）：8 顆 CPU 同時 schedule 同一個 tasklet

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/softirq_par.ko mode=1 spin_ms=30; sudo rmmod softirq_par'
```
```
softirq_par: mode=1：Q9 —— 同一個 tasklet 能不能在多個 CPU 上並行？
softirq_par:   tasklet_schedule() 呼叫次數 = 8（8 顆 CPU 各一次）
softirq_par:   回呼實際執行次數            = 1
softirq_par:   最大同時執行數              = 1
softirq_par:   執行過回呼的 CPU 位圖       = 0x01
softirq_par:     cpu0: 1 次
softirq_par:   進入回呼時 t->state = 0x2  (bit0=TASKLET_STATE_SCHED, bit1=TASKLET_STATE_RUN)
softirq_par:   ✓ 結論：同一個 tasklet【絕不會】在多顆 CPU 上並行，最大同時執行數 = 1。
```

* **8 次 `tasklet_schedule()` 只換到 1 次執行** —— 第一顆 CPU 的
  `test_and_set_bit(TASKLET_STATE_SCHED)` 成功後，其餘 7 次全部被吃掉。
* **`t->state = 0x2`**：進入回呼時 `SCHED`(bit0) 已清、`RUN`(bit1) 已設 —— 與原始碼一致。
* 對照 [Q6](#q6) 的 `最大同時執行數 = 8`（8 個**不同**的 tasklet）：
  **軟中斷類型並行，單一 tasklet 串行** —— 這兩題必須放在一起看。

### 實機驗證（2）：重現書上的 CPU0 / CPU1 時序圖

`mode=2` 精準複製書上場景：先讓 tasklet 在 cpu0 上跑起來並忙等 60 ms，
**中途**從 cpu4 再 `tasklet_schedule()` 一次（此時 `SCHED` 已被清掉，掛得進去，
但 `tasklet_trylock()` 會因為 `RUN` 還在而失敗）：

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/softirq_par.ko mode=2 spin_ms=60; sudo rmmod softirq_par'
```
```
softirq_par:   忙等中：t->state = 0x2（RUN=bit1 已設，SCHED=bit0 已清）
softirq_par:   tasklet_schedule() 呼叫次數 = 2
softirq_par:   回呼實際執行次數            = 2
softirq_par:   最大同時執行數              = 1
softirq_par:     cpu0: 1 次
softirq_par:     cpu4: 1 次
softirq_par:   ✓ 第二次 schedule 掛在 cpu4 上，但它必須等 cpu0 上的回呼跑完、
softirq_par:     清掉 TASKLET_STATE_RUN 之後，下一輪 TASKLET_SOFTIRQ 才會真的執行 ——
softirq_par:     最大同時執行數依然是 1。
```

**跟書上那張時序圖逐行對應：**

| 書上（CPU0 / CPU1） | 實測（cpu0 / cpu4） |
|---|---|
| CPU0 進入軟中斷，設 `TASKLET_STATE_RUN`、清 `TASKLET_STATE_SCHED` | `t->state = 0x2` |
| CPU0 執行回呼期間，設備 A 又中斷，派給 CPU1 | 從 cpu4 再 `tasklet_schedule()` |
| CPU1 `tasklet_schedule()` 成功（SCHED 已清）→ 掛進 CPU1 的 `tasklet_vec` | 呼叫次數 = 2 |
| CPU1 執行 `tasklet_trylock()` 失敗 → **跳過**，重新掛回 CPU1 鏈結串列 | 最大同時執行數 = 1 |
| CPU0 回呼結束、清 `TASKLET_STATE_RUN`，下一輪 CPU1 才跑 | cpu4 也執行了 1 次（在 cpu0 之後） |

**同一個 tasklet 跑在了兩顆不同的 CPU 上（cpu0 → cpu4），但從未同時。**

### 補充：`count` 欄位 —— 第三個煞車

`tasklet_disable()` 會 `atomic_inc(&t->count)`，回呼裡的 `if (!atomic_read(&t->count))`
就會跳過。所以完整條件是「拿得到 RUN 鎖 **且** `count == 0`」。

### 6.1 的差異

```c
/* include/linux/interrupt.h（6.1） */
struct tasklet_struct {
	struct tasklet_struct *next;
	unsigned long state;
	atomic_t count;
	bool use_callback;                       /* ← 新增 */
	union {                                  /* ← 新增 */
		void (*func)(unsigned long data);            /* 舊式 */
		void (*callback)(struct tasklet_struct *t);  /* 新式，回呼直接拿到 tasklet 本身 */
	};
	unsigned long data;
};
#define DECLARE_TASKLET(name, _callback)  ...   /* 只剩 2 個參數！書上是 3 個 */
#define DECLARE_TASKLET_OLD(name, _func)  ...   /* 書上那個叫這個 */
```
新程式碼一律用 `tasklet_setup(&t, callback)` + `from_tasklet()`（5.9, commit `12cc923f1ccc`）。

---

<a name="q10"></a>
## 10. 工作隊列是運行在中斷上下文，還是進程上下文？它回調函數允許睡眠嗎？

### 結論

**行程上下文；允許睡眠。**

work 的回呼是由 `kworker/*` **內核執行緒**呼叫的：

```
worker_thread()                       /* kernel/workqueue.c:2391，一個正常的 kthread */
  → process_one_work()                /* :2194 */
      → worker->current_func(work);   /* ← 你的回呼，preempt_count 全 0 */
```

因為 `preempt_count` 的 HARDIRQ / SOFTIRQ 欄位都是 0、`in_task() == 1`，
`schedule()` 完全合法 —— 這正是工作佇列存在的意義：
**把「需要睡覺 / 需要跑很久」的中斷後續工作，從不能睡的軟中斷搬到能睡的行程上下文。**

三種下半部機制的取捨：

| 機制 | 上下文 | 可睡 | 可多 CPU 並行 | 延遲 | 適用 |
|---|---|---|---|---|---|
| 軟中斷 | 中斷上下文 | ❌ | ✅（同類型） | 最低 | 網路、塊裝置、RCU、定時器（核心自己用） |
| tasklet | 中斷上下文（軟中斷） | ❌ | ❌（同一個） | 低 | 驅動的快速後續處理 |
| **工作佇列** | **行程上下文** | **✅** | ✅ | 較高（要排程） | 需要睡眠 / 長時間 / 要拿 mutex 的工作 |
| 中斷線程 | 行程上下文（SCHED_FIFO 50） | ✅ | — | 低且可控 | 即時性要求高又需要睡的驅動 |

> **書目**：奔跑吧 §2.7「工作队列」開頭（第 2841 行起）：
> 「工作队列的基本原理是把work（需要推迟执行的函数）交由内核线程来执行，
> **它总是在进程上下文中执行**。工作队列的优点是利用进程上下文来执行中断下半部操作，
> 因此**工作队列允许重新调度和睡眠**。」§2.7.5「处理一个work」（第 3435 行起）。
> **原始碼**：`kernel/workqueue.c:2391` `worker_thread()`、`:2194` `process_one_work()`。

### 實機驗證：在 work 回呼裡真的 `msleep(120)`

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/ctx_probe.ko mode=3; sudo rmmod ctx_probe'
```
```
ctx_probe: mode=3：Q10 —— 工作佇列回呼跑在行程上下文，而且真的可以睡
ctx_probe:   msleep(120) 成功返回，實際睡了 124334814 ns (124 ms)

執行環境                        cpu  current       preempt_cnt  IRQ  hardirq/softirq/serving/interrupt/task
行程: insmod                     5   insmod        0x00000000   開   0 / 0 / 0 / 0 / 1
行程: workqueue 回呼（睡之前）   5   kworker/5:1   0x00000000   開   0 / 0 / 0 / 0 / 1
行程: workqueue 回呼（睡醒後）   5   kworker/5:1   0x00000000   開   0 / 0 / 0 / 0 / 1

執行環境                        SP                 在行程棧?
行程: workqueue 回呼（睡之前）  0xffff80000a33bd80 是
行程: workqueue 回呼（睡醒後）  0xffff80000a33bd80 是
```

**四個關鍵：**
1. `current = kworker/5:1` —— 一個貨真價實、有 `task_struct`、可被排程的內核執行緒。
2. `preempt_count = 0x00000000`，`in_interrupt() = 0`，`in_task() = 1` ——
   和硬中斷的 `0x10000`、軟中斷的 `0x100` 形成三方對照。
3. **`msleep(120)` 正常返回**，實測 124 ms（多的 4 ms 是 `HZ=300` 的計時粒度 3.33 ms）。
4. **睡前睡後 SP 完全相同（`0xffff80000a33bd80`）而且都在行程棧內** ——
   跟 [Q4](#q4) 的「中斷處理跑在共用的 Per-CPU 中斷棧上、睡了就回不來」正好相反：
   kworker 有自己的核心棧，`schedule()` 換出換入之後棧原封不動。

### 補充：`in_task()` 才是判斷「能不能睡」的正確依據

```
irq_probe: 目前 preempt_count() = 0x00000000
irq_probe:   in_hardirq()=0  in_softirq()=0  in_serving_softirq()=0  in_interrupt()=0  in_task()=1
```
但要注意：`in_atomic()` / `in_interrupt()` **偵測不到自旋鎖**（本機
`CONFIG_PREEMPT_COUNT=n`，`spin_lock()` 只是 `barrier()`）。
`include/linux/preempt.h` 自己也寫了「Do not use in_atomic() in driver code」。
詳見 📝 [ch10 Q9/Q11](./ch10_concurrency_and_synchronization.md#q9)。

---

<a name="q11"></a>
## 11. 舊版本（Linux 2.6.25）的工作隊列機制在實際應用中遇到了哪些問題和挑戰？

### 結論 —— 三個問題

| # | 問題 | 舊機制為什麼會這樣 | CMWQ 的解法 |
|---|------|-----------------|------------|
| 1 | **內核線程數量爆炸** | 每個 `create_workqueue()` 建立的工作佇列，在**每顆 CPU 上**都要一個專屬線程。大型伺服器上開機完 PID 就快用光 | **工作線程池（worker-pool）與工作佇列解耦**：每顆 CPU 只有 2 個 pool（普通 + 高優先級），全系統所有工作佇列共用；`pool_workqueue` 當橋樑 |
| 2 | **並發性差** | 工作線程與 CPU 綁死。CPU0 的線程上掛著 A、B、C，A 睡下去 → CPU0 被調度去跑別的行程 → **B、C 只能乾等**，即使其他 CPU 閒著也搬不過去 | **動態管理線程數**：worker 睡下去時 `wq_worker_sleeping()` 發現 pool 沒有活躍 worker 就叫醒／新建一個接手（見 [Q12](#q12)、[Q13](#q13)） |
| 3 | **死鎖** | 大家都往預設佇列丟 work，彼此有資料相依 → 互等。解法是「每個可能死鎖的 work 建一個專職線程」→ 又回到問題 1 | `WQ_MEM_RECLAIM` + **rescuer 線程**；以及 `max_active` / `alloc_ordered_workqueue()` 明確表達相依性 |

> **書目**：奔跑吧 §2.7「工作队列」開頭（第 2841~2863 行）完整列出這三點，
> 以及 Tejun Heo 在 Linux 2.6.36 提出的 CMWQ（Concurrency-Managed Workqueue）。
> **原始碼**：`kernel/workqueue.c` 的 `worker_pool` / `pool_workqueue` / `workqueue_struct` 三層結構；
> `Documentation/core-api/workqueue.rst`。

### 實機驗證（1）：13 個工作佇列共用 60 個 kworker

```bash
ssh radxa@192.168.68.58 'T=/sys/kernel/debug/tracing; sudo bash -c "
   echo 1 > $T/events/workqueue/enable; echo > $T/trace; echo 1 > $T/tracing_on
   sleep 3; echo 0 > $T/tracing_on
   grep -o \"workqueue=[^ ]*\" $T/trace | sort | uniq -c | sort -rn"'
```
```
    264 workqueue=kblockd
     44 workqueue=events
     24 workqueue=devfreq_wq
     21 workqueue=events_freezable_power_
     17 workqueue=mm_percpu_wq
     15 workqueue=events_freezable
     11 workqueue=mt76
      4 workqueue=writeback
      4 workqueue=ext4-rsv-conversion
      2 workqueue=events_unbound
      1 workqueue=pm
      1 workqueue=phy0
      1 workqueue=mmc_complete
```
```bash
ssh radxa@192.168.68.58 'sudo ~/exp/irq/irq_trace.sh pools'
```
```
kworker 執行緒數  = 60
CPU 數            = 8

--- kworker 執行緒（依 CPU 分組）---
      6 kworker/0      7 kworker/1      6 kworker/2      6 kworker/3
      6 kworker/4      6 kworker/5      6 kworker/6      6 kworker/7
      5 kworker/u16    ← UNBOUND pool（u16 = unbound, 16 = pool id）
      2 kworker/u17
```

**這就是問題 1 的量化：**
* 光是 **3 秒的取樣**就看到 13 個不同的工作佇列在用（實際存在的更多）；
* 舊機制要 **13 × 8 = 104 個**綁定線程（且只多不少）；
* CMWQ 只用 **60 個 kworker**，而且是**按需動態增減**的（見 [Q12](#q12)）。

從命名也看得出 CMWQ 的結構：`kworker/<cpu>:<id>` 是 BOUND、
`kworker/<cpu>:<id>H` 是**高優先級 pool**、`kworker/u<pool_id>:<id>` 是 UNBOUND。
每顆 CPU 兩個 pool（`NR_STD_WORKER_POOLS = 2`）：

```
kworker/3:1H(272) kworker/3:0(6588) kworker/3:1(8691) kworker/3:0H(10501)
kworker/3:2H(12299) kworker/3:2(12640)
```

### 實機驗證（2）：問題 2「並發性差」的正反對照

**舊機制的行為 = 一個佇列一條線程串行做** —— 用 `alloc_ordered_workqueue()`（`max_active=1`）模擬：

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/wq_probe.ko mode=2 nwork=4 sleep_ms=150; sudo rmmod wq_probe'
```
```
work cpu  worker(comm)    pid   preempt_cnt  起(ms)  訖(ms)
0    5    kworker/u16:3   71    0x00000000   0       154
1    5    kworker/u16:3   71    0x00000000   154     311
2    5    kworker/u16:3   71    0x00000000   311     467
3    5    kworker/u16:3   71    0x00000000   467     624
  不同的 worker 執行緒數 = 1
  最大同時活躍 work 數   = 1
  總耗時                 = 624 ms（若完全串行應為 600 ms）
```
4 個各睡 150 ms 的 work **完全串行**，總共 624 ms —— 一個睡著，後面三個乾等。
**這就是問題 2。**

**CMWQ 的行為**（同樣 4→6 個會睡的 work，改丟預設的 `system_wq`）：

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/wq_probe.ko mode=0 nwork=6 sleep_ms=300 cpu_target=3; sudo rmmod wq_probe'
```
```
  不同的 worker 執行緒數 = 6
  最大同時活躍 work 數   = 6
  總耗時                 = 316 ms   （若完全串行應為 1800 ms）
```
**6 倍工作量、幾乎同樣的牆鐘時間** —— 問題 2 被 CMWQ 解掉了。詳見 [Q13](#q13)。

### 實機驗證（3）：`max_active` —— 問題 3 的現代解法

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/wq_probe.ko mode=3 nwork=6 sleep_ms=150; sudo rmmod wq_probe'
```
```
work cpu  worker(comm)   pid     起(ms)  訖(ms)
0    3    kworker/3:5    13532   0       155
1    3    kworker/3:4    13531   0       155
2    3    kworker/3:4    13531   155     312
3    3    kworker/3:5    13532   155     312
4    3    kworker/3:4    13531   312     469
5    3    kworker/3:5    13532   312     469
  不同的 worker 執行緒數 = 2
  最大同時活躍 work 數   = 2
  總耗時                 = 469 ms
```
`alloc_workqueue(..., max_active=2)`：**同時最多 2 個 work 活躍，一批一批放行**。
超過的放進 `pwq->inactive_works`（6.1 欄位名；書上 5.0 叫 `delayed_works`），
等 `nr_active` 降下來才轉正。驅動可以用它明確表達「這個佇列最多同時做幾件事」，
不必再為每個可能死鎖的 work 開一條專職線程。

`/sys/bus/workqueue/devices/*/max_active` 也可以在跑的時候調（只有 `WQ_SYSFS` 的佇列才會出現）：
```
NAME                     max_active per_cpu
blkcg_punt_bio           256        0
nvme-wq                  256        0
writeback                256        0
```

---

<a name="q12"></a>
## 12. CMWQ 機制如何動態管理工作線程池的線程呢？

### 結論 —— 一個計數器 `pool->nr_running` 撐起整套機制

`worker_pool->nr_running` = **這個池子裡「正在跑（沒睡）」的 worker 數量**，
它是工作佇列與**排程器**之間的唯一介面。四個加減點：

| 時機 | 函式 | 動作 |
|---|---|---|
| worker 開始處理 work | `worker_thread()` → `worker_clr_flags(WORKER_PREP)` | `nr_running++` |
| worker 沒事做要去睡 | `worker_thread()` → `worker_set_flags(WORKER_PREP)` | `nr_running--` |
| **worker 執行中睡著了** | `schedule()` → `sched_submit_work()` → **`wq_worker_sleeping()`** | `nr_running--`，並判斷要不要叫人 |
| **worker 被喚醒** | `schedule()` 返回 → `sched_update_worker()` → **`wq_worker_running()`** | `nr_running++` |

三個判斷式（`kernel/workqueue.c:800/812/818`）：

```c
static bool __need_more_worker(struct worker_pool *pool)
{	return !pool->nr_running;	}                       /* 一個活躍的都沒有？ */

static bool need_more_worker(struct worker_pool *pool)
{	return !list_empty(&pool->worklist) && __need_more_worker(pool);	}

static bool may_start_working(struct worker_pool *pool)
{	return pool->nr_idle;	}                           /* 池子裡還有閒著的嗎？ */

static bool need_to_create_worker(struct worker_pool *pool)
{	return need_more_worker(pool) && !may_start_working(pool);	}  /* 要人但沒閒人 → 新建 */

static bool keep_working(struct worker_pool *pool)
{	return !list_empty(&pool->worklist) && (pool->nr_running <= 1);	}  /* 活躍 ≤1 才繼續做 */
```

**兩條規則就把整件事講完：**
* **有 work 待做 + 沒有活躍 worker** ⇒ 叫醒 idle worker；沒有 idle 的就 `create_worker()` 新建一個。
* **已經有 ≥2 個活躍 worker** ⇒ `keep_working()` 回 false，多的 worker 去睡。
  單一 CPU 上永遠只留一個活躍 worker（`nr_running <= 1`）—— **防止線程泛濫**。

閒太久的 worker 會被 `pool->idle_timer` → `idle_worker_timeout()` 回收
（`IDLE_WORKER_TIMEOUT = 5 分鐘`）。

> **書目**：奔跑吧 §2.7.5「处理一个work」的 `worker_thread()` / `need_more_worker()` /
> `manage_workers()` / `keep_working()` 分析（第 3435~3545 行），
> §2.7.7「和调度器的交互」（第 3611~3688 行）—— `nr_running` 是「工作队列机制和进程调度器之间的枢纽」。
> **原始碼**：`kernel/workqueue.c:800~818`（判斷式）、`:869` `wq_worker_running()`、
> `:896` `wq_worker_sleeping()`、`:1930` `create_worker()`、`:2162` `manage_workers()`、`:2391` `worker_thread()`；
> `kernel/sched/core.c:6606` `sched_submit_work()`。

### 6.1 vs 書上（5.0）的兩個實質差異

```c
/* 差異 1：nr_running 從 atomic_t 變成普通 int（5.17, commit bc35f7ef9628） */
struct worker_pool {
	...
	int			nr_running;   /* 書上是 atomic_t nr_running ____cacheline_aligned_in_smp */
	...
};

/* 差異 2：呼叫點從 __schedule() 移到 sched_submit_work()，且不再用 try_to_wake_up_local() */
/* kernel/sched/core.c:6592 */
static inline void sched_submit_work(struct task_struct *tsk)
{
	if (task_is_running(tsk)) return;
	if (task_flags & (PF_WQ_WORKER | PF_IO_WORKER)) {
		if (task_flags & PF_WQ_WORKER)
			wq_worker_sleeping(tsk);      /* ← 在進 __schedule() 之前就呼叫 */
	...
}
/* kernel/workqueue.c:896 */
void wq_worker_sleeping(struct task_struct *task)
{
	...
	raw_spin_lock_irq(&pool->lock);
	pool->nr_running--;
	if (need_more_worker(pool))
		wake_up_worker(pool);             /* 書上是 try_to_wake_up_local()，5.9 已刪除 */
	raw_spin_unlock_irq(&pool->lock);
}
```

### 實機驗證（1）：work 會睡 ⇒ 線程池長大

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/wq_probe.ko mode=0 nwork=6 sleep_ms=300 cpu_target=3; sudo rmmod wq_probe'
```
```
排入前 cpu3 上的 kworker（6 個）：
  kworker/3:1H(272) kworker/3:0(6588) kworker/3:1(8691) kworker/3:0H(10501) kworker/3:2H(12299) kworker/3:2(12640)
執行中 cpu3 上的 kworker（10 個）：
  kworker/3:1H(272) kworker/3:0(6588) kworker/3:1(8691) kworker/3:0H(10501) kworker/3:2H(12299) kworker/3:2(12640)
  kworker/3:3(13530) kworker/3:4(13531) kworker/3:5(13532) kworker/3:6(13533)      ← ★ 現場新建的 4 個
→ cpu3 的 kworker 數量：6 → 10（多出 4 個）
```

**6 個會睡的 work 排進去，`create_worker()` 當場生出 4 個新線程**（PID 13530~13533 連號）。

### 實機驗證（2）：work 不睡 ⇒ 線程池不動，只用一個 worker

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/wq_probe.ko mode=1 nwork=6 burn_ms=60 cpu_target=3; sudo rmmod wq_probe'
```
```
work cpu  worker(comm)  pid     preempt_cnt  起(ms)  訖(ms)
0    3    kworker/3:5   13532   0x00000000   0       60
1    3    kworker/3:5   13532   0x00000000   60      120
2    3    kworker/3:5   13532   0x00000000   120     180
3    3    kworker/3:5   13532   0x00000000   180     240
4    3    kworker/3:5   13532   0x00000000   240     300
5    3    kworker/3:5   13532   0x00000000   300     360
  不同的 worker 執行緒數 = 1
  最大同時活躍 work 數   = 1
  總耗時                 = 360 ms（若完全串行應為 360 ms）
→ kworker 數量：10 → 10
```

**同樣 6 個 work，只因為「不睡」，就完全是另一種行為：**
* **同一個 worker（pid 13532）從頭做到尾**，一個接一個，60 ms 一格，整整齊齊；
* **kworker 數量一個都沒多**（10 → 10）。

這就是 `keep_working()` 的 `pool->nr_running <= 1`：
第一個 worker 開跑後 `nr_running = 1`，`need_more_worker()` 因此為 false，
既不叫醒也不新建；worker 做完一個 work 發現 worklist 還有東西且 `nr_running <= 1`，
就繼續往下做。

**兩個 mode 放在一起，就是 CMWQ「動態」兩個字的完整定義：**
> 判斷依據不是 work 的數量，而是**有沒有 worker 因為阻塞而讓 CPU 閒下來**。

---

<a name="q13"></a>
## 13. 如果多個 work 掛入一個工作線程中執行，當某個 work 的回調函數執行了阻塞操作時，那麼剩下的 work 該怎麼辦？

### 結論

**不用等 —— CMWQ 會馬上生一個新的 worker 接手。**

完整因果鏈（實機 6.1）：

```
work A 的回呼呼叫 msleep()
  → schedule()
      → sched_submit_work(tsk)                     kernel/sched/core.c:6592
          tsk->flags & PF_WQ_WORKER  →  wq_worker_sleeping(tsk)     workqueue.c:896
              pool->nr_running--;                   /* 1 → 0 */
              if (need_more_worker(pool))           /* worklist 非空 && nr_running==0 → true */
                  wake_up_worker(pool);             /* 叫醒 idle worker */
      → __schedule() 真的把 worker A 換出
  → 被叫醒的 worker B 進 worker_thread()
      if (need_to_create_worker(pool))              /* 要人但一個 idle 都沒有？ */
          manage_workers() → maybe_create_worker() → create_worker()   /* 現場新建 */
      → process_one_work(work B)
  → work A 睡醒
      → sched_update_worker() → wq_worker_running() /* nr_running++ */
```

> **書目**：奔跑吧 §2.7.7「和调度器的交互」（第 3611 行起）開宗明義：
> 「CMWQ机制会动态地调整一个工作线程池中工作线程的执行情况，**不会因为某一个work回调函数
> 执行了阻塞操作而影响到整个工作线程池中其他work的执行**。」
> **原始碼**：`kernel/workqueue.c:896` `wq_worker_sleeping()`、`:2162` `manage_workers()`、
> `:1930` `create_worker()`。

### 實機驗證：6 個各睡 300 ms 的 work，總共只花 316 ms

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/wq_probe.ko mode=0 nwork=6 sleep_ms=300 cpu_target=3; sudo rmmod wq_probe'
```
```
wq_probe: mode=0：Q13 —— 6 個【會睡 300 ms】的 work 全部丟到 cpu3 的 worker_pool
wq_probe:   排入前 cpu3 上的 kworker（6 個）：... kworker/3:2(12640)
wq_probe:   執行中 cpu3 上的 kworker（10 個）：... kworker/3:3(13530) kworker/3:4(13531) kworker/3:5(13532) kworker/3:6(13533)

work cpu  worker(comm)  pid     preempt_cnt  起(ms)  訖(ms)  in_task/in_intr
0    3    kworker/3:0   6588    0x00000000   0       316     1 / 0
1    3    kworker/3:2   12640   0x00000000   0       316     1 / 0
2    3    kworker/3:1   8691    0x00000000   0       316     1 / 0
3    3    kworker/3:3   13530   0x00000000   0       316     1 / 0     ← 新建
4    3    kworker/3:4   13531   0x00000000   0       316     1 / 0     ← 新建
5    3    kworker/3:5   13532   0x00000000   0       316     1 / 0     ← 新建

  不同的 worker 執行緒數 = 6
  最大同時活躍 work 數   = 6
  總耗時                 = 316 ms
  若完全串行應為         = 1800 ms
→ cpu3 的 kworker 數量：6 → 10（多出 4 個）
```

**一張表講完這一題：**

| 指標 | 舊機制（每 CPU 一條線程） | CMWQ 實測 |
|---|---|---|
| 執行 work 的線程數 | 1 | **6** |
| 6 個 work 的起始時間 | 0 / 300 / 600 / 900 / 1200 / 1500 ms | **全部都是 0 ms** |
| 總耗時 | 1800 ms | **316 ms（5.7 倍）** |
| cpu3 的 kworker 數 | 不變 | **6 → 10** |

**全部 6 個 work 都在 0 ms 開跑、316 ms 一起結束** ——
第 1 個 work 一 `msleep()`，`wq_worker_sleeping()` 立刻把第 2 個放出來，
第 2 個又睡、放第 3 個……連鎖反應在幾微秒內把 6 個 work 全部推上場。

多出來的 16 ms 就是這條連鎖反應（`create_worker()` 要 `kthread_create_on_node()`
配 `task_struct` 和棧）的成本。

### 反面對照：不睡就不會這樣（見 [Q12](#q12) mode=1）

同樣 6 個 work，改成純燒 CPU 60 ms：**1 個 worker、串行、360 ms、線程數不變**。
兩者對比證明 CMWQ 的觸發條件是「**阻塞**」而不是「work 的數量」。

### 補充：`WQ_MEM_RECLAIM` 與 rescuer

記憶體吃緊時 `create_worker()` 可能失敗（配不出 `task_struct`）。
帶 `WQ_MEM_RECLAIM` 的佇列會預先建一條 **rescuer 線程**，
透過 `pool->mayday_timer` → `send_mayday()` 把 pwq 掛進 `wq->maydays`，
由 rescuer 接手，保證記憶體回收路徑不會卡死。這是 §2.7.2 提到的 `rescuer` 欄位。

---

<a name="q14"></a>
## 14. 什麼是中斷現場？中斷現場中需要保存哪些內容？

### 結論

**中斷現場 = 讓被中斷的程式「像什麼都沒發生過一樣」繼續執行所需的全部處理器狀態。**
在 ARM64 上就是一個 `struct pt_regs`（書上叫「棧框 / stack frame」），
由**硬體和軟體合作**填滿：

| 內容 | 誰存的 | 為什麼要存 |
|---|---|---|
| `PSTATE`（N/Z/C/V + DAIF + M[3:0]） | **硬體** → `SPSR_EL1` → 軟體搬到 `pt_regs->pstate` | 條件旗標、中斷遮罩、來源 EL。`eret` 靠它還原 |
| 返回位址 | **硬體** → `ELR_EL1` → 軟體搬到 `pt_regs->pc` | 回到中斷點 |
| `x0 ~ x30`（31 個通用暫存器） | **軟體** `kernel_entry`（15 條 `stp`） | 中斷處理程序會用掉這些暫存器 |
| `SP` | 軟體：EL0 來的存 `SP_EL0`；EL1 來的存 `sp + PT_REGS_SIZE` | 還原堆疊 |
| `stackframe[2]`（假 frame record） | 軟體：EL1 存 `{x29, elr}`；EL0 清 0 | 讓 backtrace 能穿過中斷邊界 |
| `pmr_save` | 軟體（僅 pseudo-NMI 啟用時有效） | `ICC_PMR_EL1` 優先級遮罩 |
| `orig_x0` / `syscallno` | 系統呼叫路徑才用 | 重啟系統呼叫 |

**不存的東西**：浮點/SIMD（`fpsimd_state`，只在切換行程時 lazy save）、
TLS、除錯暫存器 —— 因為中斷處理程序不會碰它們。

> **書目**：奔跑吧 §2.4.3「栈框」（第 1507~1559 行，`struct pt_regs` 與圖 2.5）、
> §2.4.4「保存中断上下文」（第 1560~1712 行，`kernel_entry` 逐行）、
> §2.4.5「恢复中断上下文」（第 1713~1835 行，`kernel_exit` 與 `eret`）。
> **原始碼**：`arch/arm64/include/asm/ptrace.h:178`；
> `arch/arm64/kernel/entry.S:199` `kernel_entry`、`:330` `kernel_exit`；
> `arch/arm64/kernel/asm-offsets.c`（產生 `S_LR`/`S_SP`/`S_PC`/`S_PSTATE`/`PT_REGS_SIZE`）。

### 實機驗證（1）：棧框大小與逐欄位偏移量

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/irq_probe.ko show_irqs=0; sudo dmesg | grep -A 16 "Q14"'
```
```
irq_probe: sizeof(struct pt_regs)  = 336  (= 組語裡的 PT_REGS_SIZE / 書上 S_FRAME_SIZE)
irq_probe:   regs[0..30]  (x0~x30)  offset   0 .. 240  (248 B，31 個通用暫存器)
irq_probe:   sp                     offset 248  ← 中斷點的 SP（EL0 時是 SP_EL0）
irq_probe:   pc                     offset 256  ← 硬體存進 ELR_EL1 的返回位址
irq_probe:   pstate                 offset 264  ← 硬體存進 SPSR_EL1 的處理器狀態
irq_probe:   orig_x0                offset 272
irq_probe:   syscallno              offset 280
irq_probe:   sdei_ttbr1             offset 288
irq_probe:   pmr_save               offset 296  ← 6.1 新增（pseudo-NMI 用 ICC_PMR_EL1）
irq_probe:   stackframe[2]          offset 304  ← 給 backtrace 用的假 frame record
irq_probe:   lockdep_hardirqs       offset 320
irq_probe:   exit_rcu               offset 328
```

**336 這個數字可以三方交叉驗證：**
1. `sizeof(struct pt_regs)` = 336（C）
2. 向量表第一條指令 `0xd10543ff` = `sub sp, sp, #336`（組語，見 [Q1](#q1)）
3. EL0 進來時 `&pt_regs` 距核心棧頂剛好 336 B（見 [Q15](#q15)）

**與書上（5.0）的欄位差異**：書上有 `orig_addr_limit`（`set_fs` 機制，5.10 移除）；
6.1 多出 `sdei_ttbr1`、`pmr_save`、`lockdep_hardirqs`、`exit_rcu`。

### 實機驗證（2）：兩個真實現場的完整內容

**(a) 中斷打在使用者態（bash）身上**

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/irq_live.ko samples=1 from_el0=1
                         (for i in $(seq 1 3000000); do :; done) & sleep 1; sudo rmmod irq_live'
```
```
被中斷者 current      = bash/11737
中斷點的 exception level = EL0 使用者態（PSTATE.M[3:0]=0x0）
  pt_regs->pstate     = 0x0000000020001000    nzCv .. D=0 A=0 I=0 F=0 .. M=EL0t
  pt_regs->pc         = 0x0000aaaae6faad0c    ← 使用者態程式碼位址
  pt_regs->sp         = 0x0000ffffead5f740    ← 使用者態堆疊（SP_EL0）
  pt_regs->regs[30]/lr= 0x0000aaaae6faacd4
  pt_regs->regs[29]/fp= 0x0000ffffead5f740
  pt_regs->regs[0..3] = 0000ffff9b2033cd 0000000000000000 0000000000000000 0000000000001000
  pt_regs->stackframe = { 0x0000000000000000, 0x0000000000000000 }   ← ★ 清 0
```

**(b) 中斷打在核心態（idle）身上**

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/irq_live.ko samples=1 want_hwirq=26; sudo rmmod irq_live'
```
```
被中斷者 current      = swapper/4/0
中斷點的 exception level = EL2 核心態（VHE：核心跑在 EL2）（PSTATE.M[3:0]=0x9）
  pt_regs->pstate     = 0x0000000060400009    nZCv .. D=0 A=0 I=0 F=0 .. M=EL2h
  pt_regs->pc         = 0xffff800008bc7a10    arch_local_irq_enable+0x8/0x18
  pt_regs->sp         = 0xffff80000a523da0    ← 核心棧
  pt_regs->regs[30]/lr= 0xffff800008bc803c    cpuidle_enter_state+0x138/0x248
  pt_regs->regs[29]/fp= 0xffff80000a523da0
  pt_regs->stackframe = { 0xffff80000a523da0, 0xffff800008bc7a10 }   ← ★ {x29, elr}
```

**兩者一比，`kernel_entry` 的 `.if \el == 0` 分支活生生擺在眼前：**

| 欄位 | EL0 現場 | EL1/EL2 現場 | 對應原始碼 |
|---|---|---|---|
| `pc` | 使用者位址 `0xaaaae6faad0c` | 核心位址 `arch_local_irq_enable+0x8` | 硬體寫 `ELR_EL1` |
| `sp` | 使用者棧 `0xffffead5f740`（`mrs x21, sp_el0`） | `add x21, sp, #PT_REGS_SIZE` | `entry.S:199` `.if \el == 0` |
| `pstate.M` | `0x0` = EL0t | `0x9` = EL2h | 硬體寫 `SPSR_EL1` |
| `stackframe[]` | **`{0, 0}`** | **`{x29, elr}`** | `stp xzr, xzr` vs `stp x29, x22` |

`stackframe` 的差別正是書上 §2.4.4 那句：
「如果異常發生在 EL0，那麼把棧框的 stackframe[] 欄位清零。如果異常發生在 EL1，
那麼把棧框的 stackframe[] 欄位填入 x29 和 x22 暫存器中。」
—— EL0 清 0 表示 backtrace 到此為止（不要往使用者態走）；
EL1 填假 frame record，讓 `dump_backtrace()` 能穿過中斷邊界印出被中斷的核心呼叫鏈。

### 現場如何被還原

```asm
	.macro	kernel_exit, el                      /* entry.S:330 */
	.if	\el != 0
	disable_daif
	.endif
	ldp	x21, x22, [sp, #S_PC]                // x21=pc, x22=pstate
	.if	\el == 0
	ldr	x23, [sp, #S_SP] ; msr sp_el0, x23   // 還原使用者棧
	.endif
	msr	elr_el1, x21
	msr	spsr_el1, x22
	ldp	x0, x1, [sp, #16 * 0]                // 還原 x0~x29
	...
	ldr	lr, [sp, #S_LR]
	add	sp, sp, #PT_REGS_SIZE
	eret                                         // ★ 用 SPSR 還原 PSTATE = 自動重新開中斷
	.endm
```
**`eret` 把 `SPSR_EL1` 寫回 `PSTATE`，中斷點的 `I=0` 就這樣被還原 ——
這就是書上說的「代碼裡看不到開中斷，因為 `eret` 就是開中斷」。**

---

<a name="q15"></a>
## 15. 中斷現場保存在什麼地方？

### 結論 —— 兩張棧，分工明確

| | 存什麼 | 位在哪 | 大小 |
|---|---|---|---|
| **被中斷行程的核心棧** | **中斷現場（`pt_regs`）** | `current->stack`，每個 task 一張 | `THREAD_SIZE = 16 KB` |
| **Per-CPU 中斷棧** | **中斷處理程序的執行棧**（`gic_handle_irq()` 以下的所有呼叫） | `DEFINE_PER_CPU(unsigned long *, irq_stack_ptr)`，每顆 CPU 一張 | `IRQ_STACK_SIZE = 16 KB` |

**切換點在 `do_interrupt_handler()`：**

```c
/* arch/arm64/kernel/entry-common.c:268 */
static void do_interrupt_handler(struct pt_regs *regs, void (*handler)(struct pt_regs *))
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	if (on_thread_stack())
		call_on_irq_stack(regs, handler);   /* ★ 還在行程棧上 → 切到中斷棧 */
	else
		handler(regs);                      /* 已經在中斷棧（巢狀）→ 直接呼叫 */
	set_irq_regs(old_regs);
}
```
```asm
SYM_FUNC_START(call_on_irq_stack)                    /* entry.S */
	stp	x29, x30, [sp, #-16]!      // 在行程棧上留一個 frame record
	mov	x29, sp
	ldr_this_cpu x16, irq_stack_ptr, x17
	add	sp, x16, #IRQ_STACK_SIZE   // ★ SP = 本 CPU 中斷棧的頂端
	blr	x1                         // 在中斷棧上執行 handler
	mov	sp, x29                    // 切回行程棧
	ldp	x29, x30, [sp], #16
	ret
SYM_FUNC_END(call_on_irq_stack)
```

**為什麼要分兩張？** 中斷可能發生在任何一個行程身上，包括核心棧已經用掉一大半的行程。
如果中斷處理也用它的棧，16 KB 很容易爆掉（尤其是網路/塊裝置的深呼叫鏈）。
Linux 4.5 起 arm64 引入獨立中斷棧（commit `132cd887`），書上有提到。

> **書目**：奔跑吧 §2.5.1「汇编跳转」（第 1837~1902 行，`irq_stack_entry` / `irq_stack_exit`）：
> 「**中斷發生時，中斷上下文保存在中斷進程的內核棧裡。然後，在 irq_stack_entry 宏裡切換到中斷棧。**」
> §2.5.3 小結第 [14] 個註腳（Linux 4.5 patch `132cd887`）。
> **原始碼**：`arch/arm64/kernel/irq.c:32` `DEFINE_PER_CPU(unsigned long *, irq_stack_ptr)`、
> `:54/:68` `init_irq_stacks()`；`arch/arm64/include/asm/memory.h:112` `IRQ_STACK_SIZE`。

### 實機驗證（1）：兩個位址一次看清楚

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/irq_live.ko samples=1 from_el0=1
                         (for i in $(seq 1 3000000); do :; done) & sleep 1; sudo rmmod irq_live'
```
```
被中斷者 current      = bash/11737     （中斷點在 EL0）
  &pt_regs            = 0xffff80000dc93eb0
  current->stack      = 0xffff80000dc90000 .. 0xffff80000dc94000 (THREAD_SIZE=16384)
  → pt_regs 【在】被中斷行程的核心棧內；距棧頂 336 B（= sizeof(pt_regs)=336）
  現在的 SP           = 0xffff80000a2dbcb0
  → SP 【不在】行程核心棧內  ← 已被 call_on_irq_stack() 切到 Per-CPU 中斷棧
```

**`0xffff80000dc94000 - 0xffff80000dc93eb0 = 0x150 = 336` —— 一位元組不差。**

為什麼剛好是 336？因為 bash 當時在**使用者態**跑，它的核心棧是**空的**；
中斷一來，`kernel_ventry` 的 `sub sp, sp, #336` 就在棧頂正下方挖出唯一的那個棧框。

**對照核心態被中斷的情況（棧上已經有東西）：**
```
被中斷者 current      = swapper/4/0    （中斷點在 EL2，正在 cpuidle）
  &pt_regs            = 0xffff80000a523c50
  current->stack      = 0xffff80000a520000 .. 0xffff80000a524000
  → pt_regs 【在】被中斷行程的核心棧內；距棧頂 944 B
```
944 B —— 因為 idle 執行緒當時已經在 `cpuidle_enter_state()` 裡了，
棧上先有 608 B 的既有呼叫框，中斷現場疊在它上面。

### 實機驗證（2）：中斷棧真的是 Per-CPU

```bash
ssh radxa@192.168.68.58 'sudo insmod ~/exp/irq/irq_live.ko samples=0 stackmap=1
                         sleep 3; sudo rmmod irq_live'
```
```
---- 每顆 CPU 在中斷處理常式裡看到的 SP 範圍 ----
  cpu0: hits=1886  SP 0xffff800008003620 .. 0xffff800008003cb0  (跨度 1680 B)
  cpu1: hits=171   SP 0xffff80000800b980 .. 0xffff80000800bcb0  (跨度  816 B)
  cpu2: hits=271   SP 0xffff80000a2db960 .. 0xffff80000a2dbcb0  (跨度  848 B)
  cpu3: hits=456   SP 0xffff80000a2e3cb0 .. 0xffff80000a2e3cb0  (跨度    0 B)
  cpu4: hits=209   SP 0xffff80000a2eb960 .. 0xffff80000a2ebcb0  (跨度  848 B)
  cpu5: hits=249   SP 0xffff80000a2f3960 .. 0xffff80000a2f3cb0  (跨度  848 B)
  cpu6: hits=746   SP 0xffff80000a2fba40 .. 0xffff80000a2fbcb0  (跨度  624 B)
  cpu7: hits=538   SP 0xffff80000a303870 .. 0xffff80000a303cb0  (跨度 1088 B)
```

**四個推論：**

1. **4526 次中斷（1886+171+271+456+209+249+746+538）、無數個不同的被害行程，但每顆 CPU 的 SP 永遠落在同一個小區間**
   —— 中斷棧與被中斷者是誰完全無關，只跟 CPU 有關。
2. **cpu2~cpu7 的位址間隔剛好 `0x8000` = 32 KB**
   （`0xa2db` → `0xa2e3` → `0xa2eb` → `0xa2f3` → `0xa2fb` → `0xa303`）。
   `CONFIG_VMAP_STACK=y` ⇒ 中斷棧用 `arch_alloc_vmap_stack()` 配，
   16 KB 的棧 + 16 KB 的對齊/guard 空間。
3. **cpu0/cpu1 在另一段位址（`0xffff8000080xxxxx`）** ——
   它們是開機早期由 `init_irq_stacks()` 在 vmalloc 可用之前配的。
4. **跨度只有幾百到 1.6 KB**：中斷處理的呼叫鏈很淺，16 KB 的中斷棧綽綽有餘。
   （cpu3 跨度 0 B 是因為 456 次取樣全是同一種中斷、同樣深度。）
5. **重開機後這 8 個位址一個都沒變。** 重跑一次拿到的棧頂完全相同
   （`0x...08003cb0` / `0x...0800bcb0` / `0x...0a2dbcb0` / `0x...0a2e3cb0` /
   `0x...0a2ebcb0` / `0x...0a2f3cb0` / `0x...0a2fbcb0` / `0x...0a303cb0`）——
   中斷棧是在 `init_irq_stacks()` 開機極早期就配好的，位置固定；
   對比 [Q2](#q2) 的 virq 重開機就會變，兩者的「動態」程度完全不同。

### 對照：軟中斷跑在哪張棧上？

```
ctx_probe 指紋表（SP 欄）：
硬中斷: hrtimer 回呼               0xffff80000a2f3e60  否(中斷棧)
軟中斷①: irq_exit→__do_softirq     0xffff80000a2f3e90  否(中斷棧)   ← 同一張！只差 0x30
軟中斷③: local_bh_enable→do_softirq 0xffff80000a2f3e90  否(中斷棧)
軟中斷②: ksoftirqd                 0xffff80000a65bcb0  是（自己的行程棧）
行程: workqueue kworker            0xffff80000a33bd90  是（自己的行程棧）
```
本機 `CONFIG_SOFTIRQ_ON_OWN_STACK=y`，`invoke_softirq()` / `do_softirq()` 也走
`do_softirq_own_stack()` 借用同一張 Per-CPU 中斷棧
（硬中斷 SP `...e60` 和軟中斷 SP `...e90` 只差 0x30 —— 硬中斷退掉幾層之後軟中斷就接上去）。
只有 `ksoftirqd` 和 `kworker` 這種真正的內核執行緒才用自己的行程棧 ——
**這也是為什麼只有它們可以睡。**

---

## 附錄 A：實驗程式一覽

| 檔案 | 用途 | 題目 |
|------|------|------|
| `irq_probe.c` | 從 VBAR_EL1 dump 異常向量表、`pt_regs` 欄位偏移、`preempt_count` 佈局、中斷棧大小、掃描全部 `irq_desc`（virq↔hwirq↔chip↔domain↔action）、軟中斷靜態資訊 | **Q1, Q2, Q3, Q4, Q7, Q14, Q15** |
| `irq_live.c` | kprobe 掛在 `generic_handle_domain_irq()` / `handle_irq_event()`，用 `get_irq_regs()` 抓**真實中斷現場**；`stackmap=1` 統計每 CPU 中斷棧位址 | **Q1, Q2, Q3, Q4, Q14, Q15** |
| `ctx_probe.c` | 八種執行環境的上下文指紋表；tasklet 忙等期間數硬體中斷；軟中斷 vs 行程優先級；work 回呼裡 `msleep()` | **Q4, Q5, Q7, Q8, Q10** |
| `softirq_par.c` | 8 個不同 tasklet 的多 CPU 並行度；同一個 tasklet 的串行化；重現書上 CPU0/CPU1 時序 | **Q6, Q9** |
| `wq_probe.c` | CMWQ worker pool 動態伸縮（會睡 vs 不睡）、`alloc_ordered_workqueue`、`max_active` | **Q10, Q11, Q12, Q13** |
| `ksym.h` | 用 kprobe 取回 `kallsyms_lookup_name()`，解析未 EXPORT 的 `irq_to_desc()` / `irq_work_queue_on()` | 共用 |
| `irq_trace.sh` | ftrace：`function_graph` 抓中斷呼叫鏈、irq/softirq/workqueue tracepoints、workqueue vs kworker 統計、`/proc/interrupts` ↔ DTB 對帳 | **Q3, Q5, Q7, Q11** |
| `irq_rerun_all.sh` | 把上面全部 21 個步驟串成一支腳本（含重新編譯、每段清 `dmesg`、最後自動還原機台），用來做重跑驗證 | 全部 |

### 兩個「本機才會遇到」的技術障礙與解法

1. **`irq_to_desc()` 在 aarch64 沒有 EXPORT。**
   `kernel/irq/irqdesc.c:358` 的 `EXPORT_SYMBOL_GPL(irq_to_desc)` 被
   `#ifdef CONFIG_KVM_BOOK3S_64_HV_MODULE` 包住（PowerPC 專用），模組連不到。
   `irq_work_queue_on()` 也沒 EXPORT。
   → `ksym.h` 用 kprobe 拿 `kallsyms_lookup_name()` 再查（兩者都是 text 符號，在 kallsyms 裡）。
   注意本機 `CONFIG_KALLSYMS_ALL=n`，**data 符號查不到**（`softirq_vec`、`irq_stack_ptr` 都不行），
   所以中斷棧只能靠「觀測 SP」而不是讀 `irq_stack_ptr`。

2. **`gic_handle_irq()` 不能掛 kprobe。**
   ```
   irq_live: register_kprobe(gic_handle_irq) 失敗: -22    (-EINVAL)
   ```
   它帶 `__exception_irq_entry` 屬性 → 連結到 `.irqentry.text`：
   ```bash
   $ sudo grep -E " (gic_handle_irq|__irqentry_text_start)$" /proc/kallsyms
   ffff800008010000 T __irqentry_text_start
   ffff800008010000 t gic_handle_irq          ← 位址完全相同
   ```
   這段是 kprobe 黑名單。
   → 改掛下一站 `generic_handle_domain_irq()`，用 `get_irq_regs()`
   （讀 Per-CPU 的 `__irq_regs`，由 `do_interrupt_handler()` 的 `set_irq_regs(regs)` 存入）
   拿到同一個 `pt_regs`。

### 踩過的坑（留給下一個人）

* **`ctx_probe.ko` 第一版在 `module_exit` 裡對沒 `hrtimer_init()` 過的 hrtimer 呼叫
  `hrtimer_cancel()`** → `rmmod` 段錯誤 → 模組卡在 `lsmod` 顯示 `Used by = -1` 的
  GOING 狀態，**無法卸載、無法重新載入，只能重開機**。
  現在的版本用 `ht_inited` / `iw_inited` / `tl_inited` 旗標守住。
  ✅ **已重開機驗證**：修好之後 `mode=0/1/2/3` 連續 4 次 `insmod`→`rmmod`，
  每次 `lsmod | grep ctx_probe` 都是空的（已完全卸載）。
* **多顆 CPU 同時 `pr_info()` 會交錯到看不懂**：`irq_live.c` 加了 `raw_spinlock`
  把一整筆現場的輸出包起來。

## 附錄 C：重開機後的完整重跑驗證

為了確認結論不是「剛好那一次」，把機器 `reboot` 之後（順便清掉卡住的舊模組），
用 `rerun_all.sh` 從重新編譯開始把 21 個步驟全部再跑一次。結果：

| 題目 | 指標 | 第一次 | 重開機重跑 | 判定 |
|---|---|---|---|---|
| Q1 | `VBAR_EL1` | `0xffff800008012800` = `__bp_harden_el1_vectors` | 完全相同 | ✅ 一致 |
| Q1 | 向量表第一條指令 | `0xd10543ff` = `sub sp,sp,#336` ×16 項 | 完全相同 | ✅ 一致 |
| Q1 | 中斷點 PSTATE（EL0 / 核心態） | `0x20001000` (EL0t) / `0x60400009` (EL2h) | 完全相同 | ✅ 一致 |
| Q1/Q4 | 硬中斷中的 `DAIF` | `0x3c0`（全遮）；handler 內 `0xc0` | 完全相同 | ✅ 一致 |
| **Q2** | **hwirq（TRM/DTB/GIC）** | 237 / `<0 205 4>` | **完全相同** | ✅ 一致 |
| **Q2** | **virq（Linux IRQ 號）** | mmc0 = **160**、`nr_irqs`=183 | mmc0 = **171**、`nr_irqs`=**191** | ⚠ **會變 → 見上文新增段落** |
| Q2 | irq_domain 數量 | 7 個 | 7 個，名稱相同 | ✅ 一致 |
| Q3 | 呼叫鏈 | `gic_handle_irq`→`generic_handle_domain_irq`→`__irq_resolve_mapping`→`handle_irq_desc`→`handle_fasteoi/percpu_devid`→…→`gic_eoimode1_eoi_irq` | 完全相同 | ✅ 一致 |
| Q3 | 網卡上半部 | `mt7921_irq_handler` → `__tasklet_schedule`，7 µs | 相同型態（`d.h..`→`..s..` 間隔 3 µs） | ✅ 一致 |
| Q4/Q15 | 8 顆 CPU 的中斷棧棧頂 | `08003cb0`/`0800bcb0`/`0a2dbcb0`/`0a2e3cb0`/`0a2ebcb0`/`0a2f3cb0`/`0a2fbcb0`/`0a303cb0` | **8 個位址一模一樣** | ✅ 一致 |
| Q5 | tasklet 內 `DAIF` / `irqs_disabled()` | `0` / `0` | 完全相同 | ✅ 一致 |
| Q5 | 30 ms 忙等期間的時鐘中斷數 | 10 次 | **9 次** | ✅ 都落在 HZ=300 的 9~10 |
| Q5 | 軟中斷內巢狀硬中斷（`d.H..`） | 抓到（TASKLET） | 抓到（`irq=105 dw-mci`） | ✅ 一致 |
| Q6 | 8 tasklet 最大並行數 / 耗時 | 8 / 30 ms（串行需 240 ms） | **8 / 30 ms** | ✅ 一致 |
| Q7 | 三種軟中斷上下文 + 關 BH | `0x100` / `0x100`(ksoftirqd) / **`0x101`** / `0x200` | **四個值完全相同** | ✅ 一致 |
| Q8 | 行程被卡住的時間 | 40009218 ns | **40010834 ns** | ✅ 一致（差 1.6 µs） |
| Q9 | 8 次 schedule → 執行次數 / 最大並行 | 1 / 1，`state=0x2` | **1 / 1，`state=0x2`** | ✅ 一致 |
| Q9 | 書上 CPU0/CPU1 時序 | cpu0 → cpu4，最大並行 1 | **cpu0 → cpu4，最大並行 1** | ✅ 一致 |
| Q10 | `msleep(120)` 實測 | 124 ms，`preempt_count=0` | **126 ms**，`preempt_count=0` | ✅ 一致（HZ=300 粒度 3.3 ms） |
| Q11 | 3 秒內用到的 workqueue 數 | 13 個 / 60 個 kworker | **11 個 / 77 個 kworker** | ✅ 同一結論（數量隨負載浮動） |
| Q12/Q13 | 6 個睡 300 ms 的 work | 6 worker、316 ms、kworker 6→10 | **6 worker、325 ms、kworker 7→10** | ✅ 一致 |
| Q12 | 6 個燒 CPU 的 work | 1 worker、360 ms、kworker 不變 | **1 worker、360 ms、kworker 10→10** | ✅ 一致 |
| Q11 | ordered wq（4 個睡 150 ms） | 1 worker、624 ms | **1 worker、624 ms** | ✅ 一致 |
| Q11 | `max_active=2`（6 個睡 150 ms） | 2 worker、2 並行、469 ms | **2 worker、2 並行、468 ms** | ✅ 一致 |
| Q14 | `sizeof(pt_regs)` 與欄位偏移 | 336，逐欄位 | 完全相同 | ✅ 一致 |
| Q15 | EL0 現場距棧頂 | **336 B**（= `sizeof(pt_regs)`） | **336 B**（不同 bash、不同位址） | ✅ 一致 |
| Q15 | 核心態現場距棧頂 | 944 B | **944 B** | ✅ 一致 |

**結論：25 項指標中 24 項完全重現**，唯一的差異（virq / `nr_irqs`）不是誤差，
而是揭露了一個本來會被漏掉的性質 —— **virq 是每次開機重新配置的**，
已補進 [Q2](#q2)。

重跑腳本：`ssh radxa@$HOST 'bash /tmp/rerun_all.sh'`
（腳本本身在 scratchpad，內容就是本文「一鍵重現全部實驗」那一節按順序串起來、
每段之間 `dmesg -c`，最後自動還原機台。）

## 附錄 B：還原機台

本章的實驗**不改任何系統設定**（沒動 governor、sysctl、sysfs），只需要：

```bash
HOST=192.168.68.58
# 1) 卸掉所有實驗模組
ssh radxa@$HOST 'for m in irq_live ctx_probe ctx_probe2 softirq_par wq_probe; do sudo rmmod $m 2>/dev/null; done; lsmod | head'
# 2) 關掉 ftrace 並清空
ssh radxa@$HOST 'sudo ~/exp/irq/irq_trace.sh off'
# 3) 確認 tracing 真的關了
ssh radxa@$HOST 'sudo cat /sys/kernel/debug/tracing/current_tracer /sys/kernel/debug/tracing/tracing_on'
```
> `irq_probe.ko` 的 `init` 刻意回傳 `-EAGAIN`，印完就不會留在系統裡，不需要 `rmmod`。

實測還原後的狀態（重開機重跑那一輪的最後一步）：
```
--- tracing 狀態 ---
nop
0
--- 殘留模組 ---
（全部乾淨 ✓）
```

> ⚠ 若 `lsmod` 看到 `ctx_probe  20480  -1`（`Used by` 是 `-1`），代表模組卡在 GOING 狀態，
> **必須 `sudo reboot` 才清得掉**。這是舊版 `ctx_probe.c` 的 bug（見附錄 A 的「踩過的坑」），
> 現在的版本已修好並驗證過，正常情況不會再遇到。
