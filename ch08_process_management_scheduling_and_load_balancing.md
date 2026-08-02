# 第 8 章 進程管理之調度與負載均衡 — 高頻面試題解答

> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588），`192.168.68.57`（帳密皆為 `radxa`）
> **CPU**：4×Cortex-A55（cpu0~3，`0xd05`，最高 1.8 GHz）＋ 4×Cortex-A76（cpu4~7，`0xd0b`，最高 2.256 GHz）
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`
>
> **關鍵組態**（`zcat /proc/config.gz`）：
> ```
> CONFIG_HZ=300                       CONFIG_PREEMPT_VOLUNTARY=y   （沒有 CONFIG_PREEMPT_COUNT！）
> CONFIG_NO_HZ_IDLE=y                 CONFIG_HIGH_RES_TIMERS=y
> CONFIG_SCHED_DEBUG=y                CONFIG_SCHEDSTATS=y
> CONFIG_SCHED_MC=y                   # CONFIG_SCHED_SMT / CONFIG_SCHED_CLUSTER is not set
> CONFIG_FAIR_GROUP_SCHED=y           CONFIG_CFS_BANDWIDTH=y        CONFIG_RT_GROUP_SCHED=y
> CONFIG_SCHED_AUTOGROUP=y            CONFIG_UCLAMP_TASK=y          CONFIG_SCHED_THERMAL_PRESSURE=y
> CONFIG_ENERGY_MODEL=y               CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y
> ```
>
> **書目對照**
> - 《奔跑吧 Linux 內核》（第二版）卷 1 第 8 章 —
>   `books/running-linux-kernel/running-kernel-1-txt/16_第8章_进程管理之调度与负载均衡.txt`
>   （下稱「奔跑吧 §8.x」，行號以該純文字檔為準）
> - 核心程式碼路徑相對於本專案樹
>   `/home/awe/disk/yocto-rockchip-sdk/build/tmp/work-shared/rockchip-rk3588-rock-5b/kernel-source`
>   （本機樹為 v6.1.84，機台為 6.1.115+，本章引用的排程器程式碼兩者一致）
> - RK3588 裝置樹：`arch/arm64/boot/dts/rockchip/rk3588s.dtsi`
>
> **書是 Linux 5.0 寫的，本機是 6.1**，以下 12 處已經改變，答題時要注意（詳見各題）：
>
> | # | 書上（5.0） | 本機（6.1）實測 | 題號 |
> |---|-------------|----------------|------|
> | 1 | `/proc/sys/kernel/sched_latency_ns` 等 | 搬到 **`/sys/kernel/debug/sched/`** | Q14、Ch9 Q4 |
> | 2 | `/proc/sched_debug` | 搬到 **`/sys/kernel/debug/sched/debug`** | Ch9 Q2 |
> | 3 | `runnable_load_avg` | **5.7 已刪除**，改成語意不同的 `runnable_avg` | Q22 |
> | 4 | `cpu_load[5]` 平滑陣列 | **5.7 已刪除** | Q15、Ch9 Q2 |
> | 5 | `imbalance_pct` 預設 125 | 本機 MC 域是 **117** | Q27 |
> | 6 | `SD_LOAD_BALANCE` | **5.9 已刪除** | Q24 |
> | 7 | `runnable_avg_yN_sum[]` | **4.12 已刪除**（改用 c2 技巧） | Q20 |
> | 8 | 頻率/算力不變性在 `accumulate_sum()` 裡乘 | **5.3 起改在 `update_rq_clock_pelt()`** | Q17、Q23 |
> | 9 | `find_busiest_group()` 用 avg_load | **5.7 重寫**成 `group_type` + `migration_type` | Q26、Q27 |
> | 10 | `em_cap_state` / `em_pd_energy()` | **`em_perf_state` / `em_cpu_energy()`** | Q33~Q35 |
> | 11 | `sd_flags` 只有 `SD_SHARE_PKG_RESOURCES` | 本機 MC 還帶 `SD_ASYM_CPUCAPACITY(_FULL)`、`SD_PREFER_SIBLING` | Q24 |
> | 12 | 書上假設 `CONFIG_PREEMPT=y` | 本機 **`PREEMPT_VOLUNTARY`，連 `PREEMPT_COUNT` 都沒開** | Q36~Q40 |

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | 優先級、nice 與權重的關係 | **nice 5 → weight 335，CPU 佔比 24.72% vs 理論 24.65%** |
| [2](#q2) | CFS 如何工作 | 紅黑樹 / `min_vruntime` 實測 |
| [3](#q3) | vruntime 怎麼算 | **Δvruntime/Δexec = 3.0567 = 1024/335，一位不差** |
| [4](#q4) | vruntime 何時更新 | ftrace 抓到 `update_curr → update_min_vruntime` |
| [5](#q5) | `min_vruntime` 的作用 | 睡 2 秒後 vruntime **自己跳了 675 ms** |
| [6](#q6) | 新建 / 剛喚醒行程的特殊處理 | **START_DEBIT 讓子行程一出生就欠 12 ms** |
| [7](#q7) | 核心裡那幾張表 | 模組把 4 張表全部重算並對帳 |
| [8](#q8) | 在就緒佇列等很久的行程，量化負載怎麼算 | **8 個行程：`load_avg`=1023 但 `util_avg`=115** |
| [9](#q9) | `switch_to()` 為什麼要 3 個參數 | arm64 `__switch_to` 回傳值＝x0 |
| [10](#q10) | `switch_to()` 後面的程式碼誰跑 | `finish_task_switch(prev)` 由 next 跑 |
| [11](#q11) | 兩個使用者行程之間怎麼切換 | ftrace 完整切換序列 |
| [12](#q12) | next 行程從哪裡開始跑第一條指令 | **`cpu_context.pc = ret_from_fork`** |
| [13](#q13) | ARM64 如何提高 TLB 效能 | **ASIDBits=2 → 16 bit ASID；`mm->context.id=0x37fe0`** |
| [14](#q14) | CFS 什麼時候檢查要不要調度 | ftrace `scheduler_tick` 完整呼叫圖 |
| [15](#q15) | 兩顆 CPU 各一個行程，負載一樣嗎 | **50% duty：util 496 vs 100% duty：util 1024** |
| [16](#q16) | 負載衰減的意義 | `y^32 = 0.5`，2016 ms 忘光 |
| [17](#q17) | PELT 量化負載計算方法 | **頻率不變性：2256/1200/600 MHz → util 243/131/64** |
| [18](#q18) | 額定算力 vs 實際算力 | **A55 = 422 由 DT 的 530 dmips 算出** |
| [19](#q19) | `LOAD_AVG_MAX` 是什麼 | **模組算出 47742，與核心一模一樣** |
| [20](#q20) | 第 n 個週期的衰減 | `decay_load()` 實作對帳 |
| [21](#q21) | 行程的 `load_avg` | 100% 佔用 → `load_avg = weight` |
| [22](#q22) | 就緒佇列的 `runnable_load_avg` | **本機已改名 `runnable_avg`，語意也變了** |
| [23](#q23) | 行程的 `util_avg` | **A55 200 vs A76 496，比值 0.403 ≈ 422/1024** |
| [24](#q24) | 畫出調度域 / 調度組拓撲 | **RK3588 只有 1 層 MC 域、8 個單 CPU 組** |
| [25](#q25) | 同域兩顆都不空閒還能均衡嗎 | `should_we_balance()` 原始碼 + 實測 |
| [26](#q26) | 怎麼找最繁忙的調度組 | 6.1 已改用 `group_type` |
| [27](#q27) | 不均衡時要遷移多少負載 | **5 個行程 100 ms 內收斂成 3/2** |
| [28](#q28) | 喚醒的行程該在哪顆 CPU 跑 | **滿載時 85.6% 落在 waker 的 CPU** |
| [29](#q29) | EAS 如何衡量行程的計算能力 | `util_avg` + `util_est` |
| [30](#q30) | EAS 喚醒時如何選 CPU | **輕載 → A55；重載 → A76，開關 EAS 前後對照** |
| [31](#q31) | EAS 會不會做 CPU 間負載均衡 | `overutilized` 條件 |
| [32](#q32) | CPUFreq 與調度器如何協同 | **schedutil 實測 util → 頻率對應** |
| [33](#q33) | 什麼是能效模型 | **RK3588 的 3 個性能域全部 dump 出來** |
| [34](#q34) | EAS 如何讀能效模型 | `em_cpu_get()` / debugfs |
| [35](#q35) | EAS 如何算一顆 CPU 的功耗 | **cost = power × fmax / f，實機 8 個 OPP 全部驗證** |
| [36](#q36) | 什麼是硬實時和軟實時 | 本機是軟實時 |
| [37](#q37) | 如何計算實時系統的延時 | **FIFO 空閒 p50=15.5 µs / 滿載 max=9.6 ms** |
| [38](#q38) | 中斷延時的場景 | cpuidle `cpu-sleep` 退出延時 **220 µs** |
| [39](#q39) | 中斷處理延時的場景 | 軟中斷 / threadirq |
| [40](#q40) | 調度延時的場景 | **ftrace 抓到 `kworker/u16` 卡住 RT 行程 10 ms** |
| [41](#q41) | 調度的時機 | ftrace 三種時機全部抓到 |
| [42](#q42) | 如何合理選擇下一個行程 | `pick_next_task()` 五個調度類 |
| [43](#q43) | 什麼是行程上下文 | `pt_regs` 336 B + `cpu_context` 104 B |
| [44](#q44) | 行程上下文保存到哪裡 | **實測偏移 `offsetof(task_struct, thread)=2944`** |
| [45](#q45) | 行程切換要切換哪些東西 | `switch_mm` + `switch_to` 兩步 |

---

## 一鍵重現全部實驗

```bash
# 使用者態程式
scp notes/experiments/{sched_weight,pelt_duty,wake_cpu,lb_case,rt_latency,vruntime_place}.c \
    radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp; for s in sched_weight pelt_duty wake_cpu lb_case \
    rt_latency vruntime_place; do gcc -O2 -w -o $s $s.c; done'

# 核心模組
ssh radxa@192.168.68.57 'mkdir -p ~/exp/sched'
scp notes/experiments/sched_probe.c radxa@192.168.68.57:~/exp/sched/
scp notes/experiments/Makefile.mod  radxa@192.168.68.57:~/exp/sched/Makefile
ssh radxa@192.168.68.57 'cd ~/exp/sched && make && sudo insmod sched_probe.ko && \
                         sudo rmmod sched_probe && sudo dmesg | tail -60'

# ftrace 腳本
scp notes/experiments/sched_trace.sh radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'sudo /tmp/sched_trace.sh switch'
```

---

<a name="q1"></a>
## 1. 請簡述進程優先級、nice 值和權重之間的關係。

### 結論

三者是**同一件事的三種表示法**，靠兩張表串起來：

```
     使用者看到的        核心裡的                 CFS 真正用的
        nice      →   static_prio        →        weight
      −20 ~ 19        100 ~ 139            88761 ~ 15   （nice 0 = 1024）
                      prio = static_prio            ↑
                      （普通行程）          sched_prio_to_weight[nice+20]
```

| 概念 | 範圍 | 說明 |
|------|------|------|
| **nice** | −20 ~ 19 | 使用者介面（`nice`/`renice`/`setpriority`），數字越大越「客氣」 |
| **static_prio** | 100 ~ 139 | `static_prio = nice + 120 = nice + MAX_RT_PRIO(100) + 20` |
| **normal_prio** | 0 ~ 139 | 依 policy 算出來的「應有」優先級；普通行程 = `static_prio`，RT 行程 = `99 - rt_priority` |
| **prio** | 0 ~ 139 | **動態**優先級，是調度器真正比較的值；平常 = `normal_prio`，但 RT-mutex 優先級繼承會臨時把它拉高 |
| **weight** | 15 ~ 88761 | `sched_prio_to_weight[static_prio - 100]`；存進 `p->se.load.weight` 時還會 `scale_load()`（左移 10 位） |

**核心規則：nice 每差 1 級，CPU 時間相差約 10%，用 1.25 這個係數實現**
（`weight(n) = 1024 / 1.25^n`）。因為 CFS 分配的是**比例**：

```
行程能拿到的 CPU 時間 = 調度週期 × (自己的 weight / 就緒佇列總 weight)
```

> **書目**：奔跑吧 §8.1.1「vruntime 的計算」（第 93~281 行）——
> 「內核約定 nice 值為 0 的權重值為 1024」「使用一個係數 1.25 來計算」。
> **原始碼**：`kernel/sched/core.c:11245` `sched_prio_to_weight[40]`、
> `kernel/sched/core.c:11266` `sched_prio_to_wmult[40]`、`set_load_weight()`。
> 注意書上寫成 `prio_to_weight[]`，5.x 之後正式名稱是 **`sched_prio_to_weight[]`**。

### 實機驗證

**(a) 權重表就是 `1024/1.25^n`**（`sched_probe.ko`）

```bash
ssh radxa@192.168.68.57 'cd ~/exp/sched && make'
ssh radxa@192.168.68.57 'sudo insmod ~/exp/sched/sched_probe.ko; sudo rmmod sched_probe; \
                         sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A12 "Q1/Q7"'
```

```
== Ch8 Q1/Q7  nice -> weight -> inv_weight ==
   NICE_0_LOAD = 1048576  (= 1024 << SCHED_FIXEDPOINT_SHIFT(10) = scale_load(1024))
   nice  weight    wmult(表)   2^32/weight  1024/1.25^n
   -20   88761     48388        48388        88817
   -15   29154     147320       147320       29103
   -10   9548      449829       449829       9536
   -5    3121      1376151      1376151      3125
   0     1024      4194304      4194303      1024
   5     335       12820798     12820797     335
   10    110       39045157     39045157     109
   15    36        119304647    119304647    36
```

最後兩欄一模一樣 → **`sched_prio_to_weight[]` 確實是 `1024 / 1.25^nice` 四捨五入**，
`sched_prio_to_wmult[]` 確實是 `2^32 / weight`（`0xffffffff/weight`）。

**(b) 權重比例 == CPU 時間比例**（`sched_weight.c`，把 nice 0 與 nice 5 一起釘在 CPU3）

```bash
ssh radxa@192.168.68.57 'cd /tmp && gcc -O2 -w -o sched_weight sched_weight.c && \
                         ./sched_weight 3 10 0 5'
```

```
CPU3 上 2 個 CPU-bound 行程，取樣 10 秒
pid    nice   weight    CPU%      Δvrt/Δex load_avg util_avg invol_sw
94444  0      1048576   75.28     1.0000    1023     761      387
94445  5      343040    24.72     3.0567    334      278      381
```

| | 理論值 | 實測 |
|---|---|---|
| nice 0 的 `se.load.weight` | 1024 × 1024 = 1048576 | **1048576** ✅ |
| nice 5 的 `se.load.weight` | 335 × 1024 = 343040 | **343040** ✅ |
| nice 0 的 CPU 佔比 | 1024/(1024+335) = **75.35%** | **75.28%** ✅ |
| nice 5 的 CPU 佔比 | 335/(1024+335) = **24.65%** | **24.72%** ✅ |

誤差 0.07 個百分點。**「nice 差 5 級 ≈ 1.25⁵ ≈ 3.05 倍 CPU 時間」在實機上完全成立**。

---

<a name="q2"></a>
## 2. 請簡述 CFS 是如何工作的。

### 結論

CFS 的核心思想：**理想的多工處理器上，n 個行程應該各拿到 1/n 的 CPU。**
現實只有一顆 CPU，所以 CFS 記帳，讓「虛擬時間」看起來公平。

一句話總結：**CFS = 用權重換算出的虛擬時鐘 + 一棵按 vruntime 排序的紅黑樹，永遠挑最左邊那個跑。**

五個要件：

1. **記帳（`update_curr()`）**：每次時鐘節拍 / 入出佇列 / 切換，把當前行程真正跑掉的
   `delta_exec` 換算成 `vruntime += delta_exec × 1024 / weight`。
   權重大 → 虛擬時間走得慢 → 在紅黑樹裡待得久 → 拿到更多實際時間。
2. **排序（紅黑樹）**：`cfs_rq->tasks_timeline`，key 是 `se->vruntime`。
   `rb_leftmost` 快取最左節點，`pick_next_entity()` 是 **O(1)**。
3. **選擇（`pick_next_task_fair()`）**：挑最左邊（vruntime 最小 = 最虧欠）的實體。
   組調度時要**逐層往下**挑（`for (;;) { se = pick_next_entity(cfs_rq); cfs_rq = group_cfs_rq(se); }`）。
4. **調度週期與粒度**：一個「調度週期」內每個行程至少跑一次。
   `__sched_period(nr) = nr > sched_nr_latency ? nr × min_granularity : sched_latency`
   （`kernel/sched/fair.c:710`）。本機 `sched_latency=24 ms`、`min_granularity=3 ms`
   → 門檻 `sched_nr_latency = 24/3 = 8`。
5. **搶佔（`check_preempt_tick()` / `check_preempt_wakeup()`）**：跑滿自己的
   `sched_slice()` 或被更虧欠的行程喚醒，就設 `TIF_NEED_RESCHED`。

`min_vruntime` 是這棵樹的「基準線」，見 [Q5](#q5)。

> **書目**：奔跑吧 §8.1「CFS」（第 71 行起）與 §8.1.5「進程調度」（第 1394 行起）。
> **原始碼**：`kernel/sched/fair.c` — `update_curr():882`、`pick_next_entity()`、
> `__sched_period():710`、`sched_slice():726`、`check_preempt_tick():4908`。

### 實機驗證

**(a) 一棵活的 CFS 紅黑樹長什麼樣**

```bash
ssh radxa@192.168.68.57 'sudo grep -A22 "^cfs_rq\[3\]:/$" /sys/kernel/debug/sched/debug'
```

```
cfs_rq[3]:/
  .exec_clock                    : 0.000000
  .MIN_vruntime                  : 0.000001        ← 紅黑樹最左節點（空佇列時印 1ns）
  .min_vruntime                  : 3864604.880899  ← 佇列的基準線（單調遞增）
  .max_vruntime                  : 0.000001        ← 最右節點
  .spread                        : 0.000000        ← max - MIN
  .spread0                       : -737063.074821  ← 與 cpu0 的 min_vruntime 差
  .nr_running                    : 0
  .load                          : 0               ← 佇列總權重
  .load_avg                      : 0
  .runnable_avg                  : 6
  .util_avg                      : 6
```

**(b) 「輪流跑」是真的**——兩個同權重行程釘在 CPU2，用 ftrace 看 `sched_switch`：

```bash
ssh radxa@192.168.68.57 'sudo /tmp/sched_trace.sh switch'
```

```
sh-100767 [002] ... 156307.978571: sched_switch: prev_comm=sh prev_pid=100767 prev_state=R ==> next_comm=sh next_pid=100768
sh-100768 [002] ... 156307.991901: sched_switch: prev_comm=sh prev_pid=100768 prev_state=R ==> next_comm=sh next_pid=100767
sh-100767 [002] ... 156308.005234: sched_switch: prev_comm=sh prev_pid=100767 prev_state=R ==> next_comm=sh next_pid=100768
sh-100768 [002] ... 156308.018567: sched_switch: prev_comm=sh prev_pid=100768 prev_state=R ==> next_comm=sh next_pid=100767
sh-100767 [002] ... 156308.031900: sched_switch: prev_comm=sh prev_pid=100767 prev_state=R ==> next_comm=sh next_pid=100768
```

**間隔精準地是 13.333 ms**（= 4 個 tick，HZ=300）。理論值：
`sched_slice = sched_latency × (1024/2048) = 12 ms`，但 `check_preempt_tick()` 只在節拍上檢查，
所以要湊滿 4 個節拍（13.33 ms）才會真的切走 → **完全吻合**。
`prev_state=R` 表示「被搶佔」（還在就緒佇列裡），這正是 `nivcsw` 會加 1 的情況。

---

<a name="q3"></a>
## 3. CFS 中 vruntime 是如何計算的？

### 結論

```
                        NICE_0_LOAD          delta_exec × 1024 × inv_weight
vruntime += delta_exec × ───────────  ≡  ────────────────────────────────────
                           weight                      2^32
```

`kernel/sched/fair.c:694`：

```c
static inline u64 calc_delta_fair(u64 delta, struct sched_entity *se)
{
	if (unlikely(se->load.weight != NICE_0_LOAD))
		delta = __calc_delta(delta, NICE_0_LOAD, &se->load);
	return delta;
}
```

三個重點：

1. **nice 0 走捷徑**：`weight == NICE_0_LOAD` 時直接回傳 `delta`，不做任何乘除
   → 絕大多數行程的 `vruntime` 就等於真實執行時間。
2. **沒有除法、沒有浮點**：`__calc_delta()` 用預先算好的
   `inv_weight = 2^32 / weight`（`sched_prio_to_wmult[]`）把除法變成「乘法 + 右移 32」。
3. **防溢位**：`while (fact >> 32) { fact >>= 1; shift--; }` 先把
   `fact = weight × inv_weight` 壓回 32 bit 以內，再一起移位。

> **書目**：奔跑吧 §8.1.1 式 (8.2)(8.3)(8.4) 與 `__calc_delta()` 程式碼（第 240~270 行）。

### 實機驗證

**(a) 用核心模組把 `__calc_delta()` 重跑一遍**（`sched_probe.ko`）

```
== Ch8 Q3  calc_delta_fair(10ms, nice) —— 乘法＋移位，無浮點 ==
   nice=-5  weight=3121   inv=1376151     vruntime = 3280999 ns   (= 10ms * 1024/3121)
   nice=0   weight=1024   inv=4194304     vruntime = 10000000 ns  (= 10ms * 1024/1024)
   nice=1   weight=820    inv=5237765     vruntime = 12487804 ns  (= 10ms * 1024/820)
   nice=5   weight=335    inv=12820798    vruntime = 30567164 ns  (= 10ms * 1024/335)
   nice=19  weight=15     inv=286331153   vruntime = 682666666 ns (= 10ms * 1024/15)
```

驗算 nice=5：`10 ms × 1024 / 335 = 30.5672 ms`，模組算出 `30567164 ns` → **只差 8 ns（純整數捨入）**。

**(b) 在真實行程上量 Δvruntime / Δsum_exec_runtime**

`sched_weight.c` 每隔 10 秒讀兩次 `/proc/<pid>/sched`，算出兩者的比值：

```
pid    nice   weight    CPU%      Δvrt/Δex
94444  0      1048576   75.28     1.0000     ← nice 0：虛擬時間 = 真實時間
94445  5      343040    24.72     3.0567     ← nice 5：1024/335 = 3.05672
```

**實測 3.0567，理論 1024/335 = 3.05672 —— 小數點後 4 位完全一致。**

---

<a name="q4"></a>
## 4. vruntime 是何時更新的？

### 結論

統一入口只有一個：**`update_curr(cfs_rq)`**（`kernel/sched/fair.c:882`）。
它做三件事：算 `delta_exec` → 累加 `curr->sum_exec_runtime` → `curr->vruntime += calc_delta_fair(...)`
→ 最後呼叫 `update_min_vruntime(cfs_rq)`。

被呼叫的時機（全部在 `fair.c` 裡 grep `update_curr` 即可看到）：

| 時機 | 呼叫路徑 |
|------|----------|
| **時鐘節拍** | `scheduler_tick() → task_tick_fair() → entity_tick() → update_curr()` |
| **行程入佇列** | `enqueue_entity() → update_curr()`（先把舊帳結清） |
| **行程出佇列** | `dequeue_entity() → update_curr()` |
| **主動讓出** | `yield_task_fair() → update_curr()` |
| **喚醒搶佔檢查** | `check_preempt_wakeup() → update_curr()` |
| **切換出去** | `put_prev_entity() → update_curr()`（`curr->on_rq` 時） |
| **設定 / 查詢** | `set_next_entity()`、`task_sched_runtime()`（`/proc/pid/stat` 讀 utime 時） |
| **組調度頻寬** | `throttle/unthrottle_cfs_rq()`、`update_cfs_group()` |

**關鍵觀念：vruntime 不是「連續」更新的，而是「事件驅動」的。**
一個行程在 CPU 上跑的時候，它的 `se.vruntime` 其實是**停在上一次事件的值**，
要等下一次 `update_curr()` 才會補記。所以 `/proc/pid/sched` 讀到的 `se.vruntime`
對「正在跑」的行程來說是滯後的。

> **書目**：奔跑吧 §8.1.7「調度節拍」（第 1947 行起）——
> 「在第 4184 行中，update_curr() 函数更新当前进程的 vruntime 和就绪队列的 min_vruntime」。

### 實機驗證

用 `function_graph` 把 CPU2 上一次完整的 `scheduler_tick()` 抓下來：

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
T=/sys/kernel/debug/tracing
echo 4 > \$T/tracing_cpumask          # 只追 CPU2
echo function_graph > \$T/current_tracer
echo scheduler_tick > \$T/set_graph_function
echo 1 > \$T/tracing_on
taskset -c 2 sh -c \"while :; do :; done\" & A=\$!
sleep 0.05; echo 0 > \$T/tracing_on; kill \$A
grep -A25 \"scheduler_tick() {\" \$T/trace | head -30
echo ff > \$T/tracing_cpumask; echo nop > \$T/current_tracer"'
```

```
 2)               |  scheduler_tick() {
 2)   0.875 us    |    topology_scale_freq_tick();      ← 頻率不變性（見 Q17）
 2)               |    raw_spin_rq_lock_nested() { ... }
 2)   0.875 us    |    update_rq_clock();               ← 先更新 rq->clock / clock_task
 2)               |    update_thermal_load_avg() { decay_load(); ×3 }
 2)               |    task_tick_fair() {
 2)               |      update_curr() {                ← ★ vruntime 在這裡更新
 2)   0.875 us    |        update_min_vruntime();       ← ★ 接著更新 min_vruntime
 2)   0.583 us    |        cpuacct_charge();
 2)               |        __cgroup_account_cputime() { ... }
 2)   7.000 us    |      }
 2)               |      __update_load_avg_se() {       ← PELT：行程的負載
 2)   0.583 us    |        decay_load(); ×3
 2)               |        __accumulate_pelt_segments() { decay_load(); ×2 }
 2)   8.167 us    |      }
 2)               |      __update_load_avg_cfs_rq() {   ← PELT：佇列的負載
 2)               |        ... }
 2)   0.584 us    |      update_cfs_group();
 2)               |      update_curr() {                ← ★ 組調度：上一層 se 再算一次
 2)   0.583 us    |        __calc_delta();              ← 這一層 weight != 1024，走乘法路徑
 2)   0.875 us    |        update_min_vruntime();
 2)   3.500 us    |      }
```

三個從 trace 直接讀出來的事實：
1. **`update_curr()` 緊接著 `update_min_vruntime()`** —— 兩者永遠成對。
2. **`update_curr()` 出現兩次**，因為 `CONFIG_FAIR_GROUP_SCHED=y`，
   `task_tick_fair()` 的 `for_each_sched_entity(se)` 會沿 cgroup 階層往上走。
3. 第一次沒有 `__calc_delta()`（行程 nice=0 走捷徑），第二次有
   （group se 的 shares 權重 ≠ 1024）—— 正好印證 [Q3](#q3) 的「nice 0 走捷徑」。

---

<a name="q5"></a>
## 5. CFS 中的 min_vruntime 有什麼作用？

### 結論

`cfs_rq->min_vruntime` 是**這條就緒佇列的「虛擬時間水位線」**，有三個作用：

1. **當基準點**：`vruntime` 是 u64 且一直增長，直接比大小會溢位。
   核心比較時用 `entity_before(a,b) → (s64)(a->vruntime - b->vruntime) < 0`，
   而紅黑樹的 key 用 `se->vruntime - cfs_rq->min_vruntime`，把絕對值變成相對值。
2. **安置新來的實體**：`place_entity()` 一律以 `min_vruntime` 為起點（見 [Q6](#q6)）。
   沒有它，新建行程 `vruntime=0` 會霸佔 CPU 直到追上別人；
   長睡行程醒來也會因為 `vruntime` 太小而餓死別人。
3. **單調遞增的保證**：`update_min_vruntime()` 用
   `cfs_rq->min_vruntime = max_vruntime(cfs_rq->min_vruntime, vruntime)`，
   **只增不減**，即使佇列空了也不會退回去。

```c
/* kernel/sched/fair.c:592 */
static void update_min_vruntime(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	struct rb_node *leftmost = rb_first_cached(&cfs_rq->tasks_timeline);
	u64 vruntime = cfs_rq->min_vruntime;

	if (curr) {
		if (curr->on_rq) vruntime = curr->vruntime;
		else             curr = NULL;
	}
	if (leftmost) {
		struct sched_entity *se = __node_2_se(leftmost);
		vruntime = curr ? min_vruntime(vruntime, se->vruntime) : se->vruntime;
	}
	u64_u32_store(cfs_rq->min_vruntime, max_vruntime(cfs_rq->min_vruntime, vruntime));
}
```

也就是說 `min_vruntime = max(舊值, min(正在跑的 curr, 樹上最左節點))`。

> **書目**：奔跑吧 §8.1.1、§8.1.4；Ch9 §9.1.2 對 `MIN_vruntime` / `min_vruntime` / `max_vruntime`
> 三個欄位的說明（第 150 行附近）。

### 實機驗證

`vruntime_place.c`（需 root，因為要讀 `/sys/kernel/debug/sched/debug`）。
為了讓行程直接掛在 root cfs_rq 上（否則 autogroup 會把它塞進 `/user.slice/...` 的子佇列，
`/proc/pid/sched` 的 `se.vruntime` 就跟 `cfs_rq[N]:/` 的 `min_vruntime` 不同基準）：

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
echo 0 > /proc/sys/kernel/sched_autogroup_enabled
echo \$\$ > /sys/fs/cgroup/cgroup.procs
cd /tmp && ./vruntime_place 3
echo 1 > /proc/sys/kernel/sched_autogroup_enabled"'
```

```
== CPU3 上已經有 3 個 busy 行程 ==
父行程(pid=102781) 自己的 se.vruntime = 6167433.921980 ms
cfs_rq[3]:/ 的 min_vruntime      = 6167438.102221 ms
```

`min_vruntime`（6167438.10）**大於**正在跑的父行程 vruntime（6167433.92）約 4.2 ms
——因為父行程正在 CPU 上跑，它的 `se.vruntime` 停在上一次 `update_curr()`（見 [Q4](#q4)），
而 `min_vruntime` 已經被別的 tick 推上去了；`max_vruntime()` 的單調性保證它不會退回來。

睡眠測試（同一支程式的第二段）：

```
== 睡 2 秒前 ==
   自己 vruntime = 6167460.512609 ms, cfs_rq min_vruntime = 6167478.343938 ms
== 睡 2 秒後被喚醒 ==
   自己 vruntime = 6168135.464892 ms, cfs_rq min_vruntime = 6168166.550757 ms
   睡眠期間 min_vruntime 前進了 688.207 ms，我的 vruntime 前進了 674.952 ms
```

**我睡了 2 秒、幾乎沒跑，`se.vruntime` 卻自己往前跳了 675 ms** ——
這 675 ms 不是我跑出來的，是 `place_entity()` 用 `min_vruntime` 幫我「補記」的。
如果沒有這個機制，我醒來後 vruntime 會落後 688 ms，會霸佔 CPU3 好幾百毫秒。

---

<a name="q6"></a>
## 6. CFS 對新創建的進程和剛喚醒的進程有何特殊處理？

### 結論

兩者都走 `place_entity()`（`kernel/sched/fair.c:4647`），但方向**相反**：

```c
static void place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int initial)
{
	u64 vruntime = cfs_rq->min_vruntime;

	/* (1) 新建行程：往後推一個時間片 —— 罰它 */
	if (initial && sched_feat(START_DEBIT))
		vruntime += sched_vslice(cfs_rq, se);

	/* (2) 喚醒行程：往前拉半個調度週期 —— 補償它 */
	if (!initial) {
		unsigned long thresh = se_is_idle(se) ? sysctl_sched_min_granularity
						      : sysctl_sched_latency;
		if (sched_feat(GENTLE_FAIR_SLEEPERS))
			thresh >>= 1;
		vruntime -= thresh;
	}
	...
	se->vruntime = max_vruntime(se->vruntime, vruntime);   /* 只會往前，不會倒退 */
}
```

| | 新建（`initial=1`，`wake_up_new_task()`） | 喚醒（`initial=0`，`enqueue_entity(ENQUEUE_WAKEUP)`） |
|---|---|---|
| 特性開關 | **START_DEBIT** | **GENTLE_FAIR_SLEEPERS** |
| 動作 | `vruntime = min_vruntime + sched_vslice(se)` | `vruntime = max(自己的, min_vruntime − sched_latency/2)` |
| 目的 | 新行程先「欠一個時間片」，**擋 fork 炸彈**、避免搶走現有行程這一輪的份 | 睡久的行程給一點補償以提升互動性，但**最多只能欠半個調度週期**，避免餓死 CPU-bound 行程 |
| 本機數值 | `sched_vslice` ≈ 24 ms/nr | `thresh = 24/2 = 12 ms` |

另外兩個相關處理：

* `sysctl_sched_child_runs_first`（本機 = **0**）：fork 之後**父行程先跑**。
  設成 1 才會 `swap(curr->vruntime, se->vruntime)` 讓子行程先跑。
* 喚醒之後還要 `check_preempt_wakeup()` 決定要不要立刻搶佔當前行程，
  門檻是 `sysctl_sched_wakeup_granularity`（本機 **4 ms**）：
  被喚醒者的 vruntime 要比 curr 小超過這個值（換算成加權後）才搶得動。

> **書目**：奔跑吧 §8.1.3/§8.1.4「對進程加入調度器的代碼的分析」（第 1316 行起）；
> Ch9 §9.1.4 對 `sched_child_runs_first`、`sched_wakeup_granularity_ns` 的說明。

### 實機驗證

**(a) START_DEBIT：新生兒一出生就欠債**

```
父行程(pid=102781) 自己的 se.vruntime  = 6167433.921980 ms
cfs_rq[3]:/ 的 min_vruntime            = 6167438.102221 ms
[新建子行程 pid=102795] 一出生的 se.vruntime = 6167450.721975 ms     ← 比 min_vruntime 大 12.6 ms
```
第二次跑：
```
父行程 se.vruntime            = 6169203.129258
cfs_rq[3]:/ min_vruntime      = 6169208.361425
[新建子行程] 一出生 se.vruntime = 6169219.929253                      ← 比 min_vruntime 大 11.6 ms
```

子行程**一出生的 vruntime 就比 `min_vruntime` 大 11.6~12.6 ms**，也比父行程大 16.8 ms
→ 它在紅黑樹上被排到後面，父行程繼續跑。這正是 `vruntime += sched_vslice()`。
（`min_vruntime` 是透過 `popen(awk ...)` 讀 4600 行的 debugfs 取得的，不是原子快照，
所以只能取到 ~ms 級精度；量級與方向完全符合。）

**(b) GENTLE_FAIR_SLEEPERS：睡再久也只能欠半個週期**

```
睡眠期間 min_vruntime 前進了 688.207 ms，我的 vruntime 前進了 674.952 ms
   -> 殘留落後量 = Δmin - Δ我 = 13.255 ms（GENTLE_FAIR_SLEEPERS 上限 sysctl_sched_latency/2 = 12 ms）
```
第二次：`Δmin = 690.507`、`Δ我 = 670.391` → 殘留 **20.1 ms**。

殘留落後量落在 12~20 ms（上限 12 ms + 取樣不同步造成的誤差），
**而不是 690 ms** —— 證明喚醒時 vruntime 被硬拉到 `min_vruntime − 12 ms`。

**(c) 確認兩個特性都開著**

```bash
ssh radxa@192.168.68.57 'sudo cat /sys/kernel/debug/sched/features'
```
```
GENTLE_FAIR_SLEEPERS START_DEBIT NO_NEXT_BUDDY LAST_BUDDY CACHE_HOT_BUDDY
WAKEUP_PREEMPTION NO_HRTICK ... UTIL_EST UTIL_EST_FASTUP NO_LATENCY_WARN ALT_PERIOD BASE_SLICE
```

---

<a name="q7"></a>
## 7. 內核代碼中定義了若干個表，請分別說出它們的含義。

### 結論

| 表 | 位置 | 大小 | 含義 |
|---|---|---|---|
| **`sched_prio_to_weight[40]`** | `kernel/sched/core.c:11245` | nice −20~19 | 權重表，`= round(1024 / 1.25^nice)`。`set_load_weight()` 取出後 `scale_load()`（<<10）存進 `se.load.weight` |
| **`sched_prio_to_wmult[40]`** | `kernel/sched/core.c:11266` | 同上 | 反權重表，`= 2^32 / weight`。把 `__calc_delta()` 的除法變成乘法＋右移 |
| **`runnable_avg_yN_inv[32]`** | `kernel/sched/sched-pelt.h` | 32 項 | PELT 衰減係數 `y^n × 2^32`，`n = 0..31`；`y = 0.5^(1/32)`，所以 `y^32 = 0.5` |
| **`LOAD_AVG_MAX`** | `kernel/sched/sched-pelt.h:14` | 常數 47742 | 無窮級數 `1024 × Σ y^n` 的收斂值（見 [Q19](#q19)） |
| ~~`runnable_avg_yN_sum[33]`~~ | — | — | **書上有、本機沒有**：4.12 的 commit「sched/fair: Rewrite PELT」用 `c2 = LOAD_AVG_MAX − decay_load(LOAD_AVG_MAX, periods) − 1024` 這個技巧取代了整張表 |

這三張活著的表都是為了**把浮點與除法變成整數乘法與移位**——排程器在每個 tick、每次
enqueue/dequeue 都要跑，不能有除法。

> **書目**：奔跑吧 §8.1.1（`prio_to_weight`/`prio_to_wmult`，式 8.1）與
> §8.2.7「PELT 代碼分析」（`runnable_avg_yN_inv`、`runnable_avg_yN_sum`、`LOAD_AVG_MAX`，第 2620 行起）。
> 書中「`runnable_avg_yN_sum[]` 表」那一段（第 2700 行附近）在 6.1 已經不適用。

### 實機驗證

`sched_probe.ko` 把四樣東西全部重算：

```
== Ch8 Q1/Q7 ==（見 Q1，wmult 完全 = 2^32/weight）
== Ch8 Q19/Q20  PELT 衰減 ==
   y = 0.5^(1/32)；y^32 應為 0.5：decay_load(1024, 32) = 511 （= 1024/2）
   decay_load(1024, 1)=1002  (10)=824  (32)=511  (64)=255  (2016)=0  (2017)=0
   收斂的 sum 1024*y^n = 47742（346 次迭代收斂），核心 LOAD_AVG_MAX = 47742
```

* `decay_load(1024,1) = 1002` → `y^1 = 0.9786`，對得上書上 `runnable_avg_yN_org[1] = 0.978`。
* `decay_load(1024,32) = 511 ≈ 1024/2` → **`y^32 = 0.5` 成立**。
* `decay_load(1024,64) = 255 ≈ 1024/4`。
* n=2016（= 32×63）之後直接回 0 —— 對應 `if (n > LOAD_AVG_PERIOD * 63) return 0;`。
* 用核心 `Documentation/scheduler/sched-pelt.c` 的迭代法算出 **47742，與 `LOAD_AVG_MAX` 一模一樣**。

---

<a name="q8"></a>
## 8. 如果一個普通進程在就緒隊列裡等待了很長時間才被調度，那麼它的量化負載該如何計算？

### 結論

**照算不誤——因為 PELT 的 `load_avg` 統計的是「可運行（runnable）」時間，
而不是「正在運行（running）」時間。** 等待也算數。

| 統計量 | 計的是什麼 | 一直在等的行程 |
|--------|-----------|----------------|
| `se->avg.load_sum` / `load_avg` | **runnable** 時間（在就緒佇列裡就算，含等待）× 權重 | **會逼近 weight** |
| `se->avg.runnable_avg` | 對 task 而言等於 `load_avg`（6.1 對 cfs_rq 才有不同語意，見 [Q22](#q22)） | 同上 |
| `se->avg.util_sum` / `util_avg` | 只計 **running** 時間（`cfs_rq->curr == se`） | **只會逼近實際 CPU 佔比** |

所以答案是：

```
load_avg  = weight × (衰減後的 runnable 時間 / LOAD_AVG_MAX)  →  等很久 ⇒ 仍然 ≈ weight
util_avg  = 1024   × (衰減後的 running  時間 / LOAD_AVG_MAX)  →  等很久 ⇒ 遠小於 1024
```

這個設計是刻意的：**負載均衡要回答「這顆 CPU 上有多少人在排隊」，不是「CPU 有多忙」。**
一顆 CPU 上塞 8 個 CPU-bound 行程和塞 1 個，`util_avg` 都是滿的（1024），
但 `cfs_rq->avg.load_avg` 會差 8 倍——這樣 `load_balance()` 才知道要搬人。

判斷「正在運行」的方式：`___update_load_sum(now, sa, load, runnable, running)` 的第三個參數
`running = (cfs_rq->curr == se)`（見 `__update_load_avg_se()`）。

> **書目**：奔跑吧 §8.2.6「sched_avg 數據結構」表 8.6 與後面的「需要特別說明幾點」
> （第 2426~2611 行）——「可運行狀態在就緒隊列中的時間包括兩部分：一是正在運行的時間…
> 二是在就緒隊列中等待的時間」。

### 實機驗證

在**同一顆 CPU3** 上放 1、2、4、8、16 個完全相同的 CPU-bound 行程，
每個行程都是「100% runnable」，但「running」的比例是 1/n：

```bash
ssh radxa@192.168.68.57 'cd /tmp && for n in 1 2 4 8 16; do
  args=""; for i in $(seq $n); do args="$args 0"; done
  echo "### nr_running=$n"; ./sched_weight 3 6 $args | sed -n 3p
done'
```

```
### nr_running=1    pid 95161  weight=1048576  CPU%=100.00  load_avg=1024  util_avg=1024  invol_sw=3
### nr_running=2    pid 95170  weight=1048576  CPU%=50.00   load_avg=1023  util_avg=483   invol_sw=225
### nr_running=4    pid 95179  weight=1048576  CPU%=25.00   load_avg=1023  util_avg=278   invol_sw=225
### nr_running=8    pid 95212  weight=1048576  CPU%=12.50   load_avg=1023  util_avg=115   invol_sw=225
### nr_running=16   pid 95247  weight=1048576  CPU%=6.26    load_avg=1024  util_avg=78    invol_sw=113
```

| nr_running | 實際 CPU 佔比 | `load_avg` | `util_avg` | 理論 util = 1024/n |
|---|---|---|---|---|
| 1 | 100% | **1024** | 1024 | 1024 |
| 2 | 50% | **1023** | 483 | 512 |
| 4 | 25% | **1023** | 278 | 256 |
| 8 | 12.5% | **1023** | 115 | 128 |
| 16 | 6.26% | **1024** | 78 | 64 |

**`load_avg` 不管等多久永遠釘在 1023~1024（= 權重），`util_avg` 則老實地按 1/n 掉下去。**
這就是這一題的答案。

另外 `/proc/pid/schedstat`（要先 `echo 1 > /proc/sys/kernel/sched_schedstats`）
可以直接看到「等了多久」：

```
wait_max    :  12.774125    ← 單次最長等待
wait_sum    :  40.344621    ← 累計等待（ms）
wait_count  :        408    ← 等了幾次
```

---

<a name="q9"></a>
## 9. 為什麼 switch_to() 函數有 3 個參數？prev 和 next 就足夠了，為何還需要 last？

### 結論

因為 **`switch_to()` 這一行程式碼「進去」和「出來」時，執行它的已經不是同一個行程了**。

```c
/* include/asm-generic/switch_to.h */
#define switch_to(prev, next, last)					\
do {									\
	((last) = __switch_to((prev), (next)));				\
} while (0)
```

情境（書上圖 8.7）：

```
CPU0：進程 A 執行 switch_to(A, B, last)
        │  ── 把 A 的 x19~x28/fp/sp/lr 存到 A->thread.cpu_context
        │  ── 把 B 的 cpu_context 載入 CPU、sp 換成 B 的核心堆疊
        ▼
      現在 CPU0 跑的是 B 了。A「睡著」，停在 cpu_switch_to 的 ret 之前。

  ……很久以後，在 CPUn 上……

CPUn：進程 X 執行 switch_to(X, A, last)
        │  ── 載入 A 的 cpu_context，A 從當初的 ret 繼續跑
        ▼
      A 醒來，要收拾的是 **X** 的殘局，不是 B 的！
```

**A 醒來時需要知道「是誰把我換上來的」，這個人就是 `last`。**
不能用 `prev` 的原因：`prev` 是區域變數，存在**各自的核心堆疊 / 暫存器**裡。
`switch_to()` 一執行完，`sp` 已經指向 A 自己的核心堆疊，讀 `prev` 讀到的是
**A 當年被換出去時的那個 prev（也就是 B）**，而不是剛剛換走的 X。

**ARM64 怎麼把 X 傳給 A？靠 AAPCS64 的呼叫慣例：**
`__switch_to(prev, next)` 的第一個參數 `prev` 放在 **x0**，
而函式回傳值也放在 **x0**。`cpu_switch_to` 組語從頭到尾**不動 x0**，
所以 CPUn 上執行 `cpu_switch_to(X, A)` 時 x0 = X；當 A 從自己的
`cpu_context.pc`（也就是它當年的 `lr`）繼續執行時，x0 裡躺著的正是 **X**。

```asm
/* arch/arm64/kernel/entry.S:829 */
SYM_FUNC_START(cpu_switch_to)
	mov	x10, #THREAD_CPU_CONTEXT
	add	x8, x0, x10		// x0 = prev  →  &prev->thread.cpu_context
	mov	x9, sp
	stp	x19, x20, [x8], #16	// 存 callee-saved
	stp	x21, x22, [x8], #16
	stp	x23, x24, [x8], #16
	stp	x25, x26, [x8], #16
	stp	x27, x28, [x8], #16
	stp	x29, x9,  [x8], #16	// fp, sp
	str	lr, [x8]		// pc ← lr（回去的地方）
	add	x8, x1, x10		// x1 = next
	ldp	x19, x20, [x8], #16	// 載回 next 的 callee-saved
	...
	ldr	lr, [x8]
	mov	sp, x9			// ★ 這一行之後就是 next 的堆疊了
	msr	sp_el0, x1		// ARM64 用 SP_EL0 存 current
	ptrauth_keys_install_kernel x1, x8, x9, x10
	scs_save x0
	scs_load_current
	ret				// ← x0 從頭到尾沒被碰過，仍然是 prev
SYM_FUNC_END(cpu_switch_to)
```

> **書目**：奔跑吧 §8.1.6「switch_to() 函數」（第 1740~1800 行）——
> 「ARM64 中的做法是利用函數調用標準的規則，以 prev 參數作為第一個參數傳遞給
> switch_to() 函數，prev 參數會存儲在 X0 寄存器中，而函數返回值也存儲在 X0 寄存器」。
> **原始碼**：`arch/arm64/kernel/entry.S:829`、`arch/arm64/kernel/process.c:588 __switch_to()`。

### 實機驗證

**(a) `cpu_context` 的內容就是那 13 個暫存器**（`sched_probe.ko`）

```
== Ch8 Q43~Q45  進程上下文（硬體上下文） ==
   sizeof(struct cpu_context)  = 104 B  (= 13 個 u64：x19~x28, fp, sp, pc)
   offsetof(task_struct, thread.cpu_context)   = 2944  (組語 THREAD_CPU_CONTEXT)
   cpu_context: x19=0 x28=72 fp=80 sp=88 pc=96
   read_sysreg(sp_el0) = 0xffff000066e5be00, current = ffff000066e5be00
```

`sizeof = 104 = 13 × 8`，`pc` 在偏移 96（第 13 個）—— 與 `entry.S` 裡
`str lr, [x8]` 是最後一個 store 完全對得上。
`SP_EL0 == current` 也印證 `msr sp_el0, x1` 那一行。

**(b) `__switch_to()` 的簽名確實回傳 `task_struct *`**

```bash
sed -n '588,592p' arch/arm64/kernel/process.c
```
```c
__notrace_funcgraph __sched
struct task_struct *__switch_to(struct task_struct *prev,
				struct task_struct *next)
{
	struct task_struct *last;
```

---

<a name="q10"></a>
## 10. switch_to() 函數後面的代碼（如 finish_task_switch(prev)），該由誰來運行？什麼時候運行？

### 結論

**由 next 行程來跑，在它被切換上 CPU 之後的第一時間跑。**

```c
/* kernel/sched/core.c:5201 */
static __always_inline struct rq *
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next, struct rq_flags *rf)
{
	prepare_task_switch(rq, prev, next);
	...
	switch_mm_irqs_off(prev->active_mm, next->mm, next);
	...
	switch_to(prev, next, prev);       /* ← 執行到這裡的是 prev */
	barrier();
	return finish_task_switch(prev);   /* ← 執行到這裡的是 next！prev 已經是 last */
}
```

`finish_task_switch(last)` 幫「剛剛被換下去的那個人」收尾：

| 動作 | 為什麼一定要別人幫忙做 |
|------|----------------------|
| `finish_lock_switch()` → `raw_spin_unlock_irq(&rq->lock)` | prev 自己不能放，因為它在 `switch_to` 之後就不執行了；rq->lock 是在 `__schedule()` 裡拿的 |
| `prev->on_cpu = 0`（`smp_store_release`） | 要等 prev 真的離開 CPU 才能清，否則 mutex 的樂觀自旋會誤判 |
| `mmdrop_lazy_tlb(mm)` | 遞減 `active_mm` 的 `mm_count`（借用位址空間的內核執行緒還債） |
| `put_task_struct_rcu_user(prev)` | 如果 prev 是 `TASK_DEAD`，這裡才真的釋放它的 `task_struct` 與核心堆疊 —— **行程不能自己釋放自己的堆疊** |
| `fire_sched_in_preempt_notifiers(current)` | KVM 之類的 preempt notifier |

**這就是 `last` 存在的意義**：`finish_task_switch()` 拿到的必須是「真的剛剛被我換下去的那個」，
而不是「我上次被換下去時看到的那個」。

一個特例：**新建行程**沒有辦法從 `context_switch()` 內部繼續往下跑
（它從未執行過 `switch_to`），所以 `copy_thread()` 把它的 `pc` 設成 `ret_from_fork`，
而 `ret_from_fork` 的**第一件事就是 `bl schedule_tail`**，
`schedule_tail()` 內部再呼叫 `finish_task_switch(prev)`——殊途同歸（見 [Q12](#q12)）。

> **書目**：奔跑吧 §8.1.6「context_switch() 函數」與圖 8.7（第 1552~1740 行）——
> 「next 進程執行 finish_task_switch(last) 函數來對 last 進程進行清理工作」。

### 實機驗證

用 ftrace 直接抓 `finish_task_switch` 與 `schedule_tail` 的呼叫者：

```bash
ssh radxa@192.168.68.57 'sudo /tmp/sched_trace.sh newtask'
```

```
sched_trace.sh-100847  [004] d.... 156323.746235: sched_wakeup_new: comm=sched_trace.sh pid=100848 prio=120 target_cpu=005
sched_trace.sh-100848  [005] d.... 156323.746242: schedule_tail <-ret_from_fork
sched_trace.sh-100848  [005] d.... 156323.746246: <stack trace>
 => schedule_tail
 => ret_from_fork
```

* **父行程（pid 100847）在 CPU4 上 fork**，`wakeup_new` 把子行程放到 **CPU5**。
* **子行程（pid 100848）的第一筆 trace 就在 CPU5 上，函式是
  `schedule_tail`，呼叫者是 `ret_from_fork`** —— 收尾工作確實是由「新上任的人」執行的。

`schedule_tail()` 的實作（`kernel/sched/core.c`）：

```c
asmlinkage __visible void schedule_tail(struct task_struct *prev)
{
	struct rq *rq;
	rq = finish_task_switch(prev);      /* ← 就是 switch_to 後面那一段 */
	preempt_enable();
	if (current->set_child_tid)
		put_user(task_pid_vnr(current), current->set_child_tid);
	calculate_sigpending();
}
```

---

<a name="q11"></a>
## 11. 進程 A 和 B 都在用戶空間運行且不主動陷入內核態，調度器要把 A 切換到 B，需要做什麼事情？

### 結論

**它們「不主動」陷入內核，那就讓中斷「被動」把它們拉進來。** 完整八步：

```
   使用者空間                  │            核心空間
─────────────────────────────┼──────────────────────────────────────────
① A 在 EL0 跑 while(1)        │
                              │ ② 時鐘中斷（arch timer PPI）→ CPU 自動關中斷，
                              │    跳到 VBAR_EL1 + 0x480 的 el0_irq / el0_t64_irq
                              │ ③ kernel_entry 0：把 x0~x30/sp/pc/pstate 壓成
                              │    pt_regs 存進 A 的核心堆疊頂端（中斷現場）
                              │ ④ el0_interrupt → handle_arch_irq → GIC →
                              │    tick handler → scheduler_tick()
                              │      → task_tick_fair() → entity_tick()
                              │      → check_preempt_tick()：A 跑滿 sched_slice
                              │      → resched_curr(rq)：設 A 的 TIF_NEED_RESCHED
                              │ ⑤ 中斷返回前：ret_to_user → do_notify_resume /
                              │    exit_to_user_mode_loop 看到 _TIF_NEED_RESCHED
                              │      → schedule()
                              │ ⑥ __schedule()：
                              │      pick_next_task() 選出 B
                              │      context_switch(rq, A, B, rf)
                              │        ├ switch_mm_irqs_off()   ← 換位址空間
                              │        │   check_and_switch_context()：分配/檢查 ASID
                              │        │   cpu_switch_mm()：寫 TTBR0_EL1、TTBR1_EL1[63:48]=ASID
                              │        └ switch_to(A, B, A)
                              │            cpu_switch_to：存 A 的 x19~x28/fp/sp/lr，
                              │                           載 B 的，msr sp_el0, B
                              │ ⑦ 現在跑的是 B。finish_task_switch(A) 收尾、開中斷
                              │ ⑧ B 沿著它自己的核心堆疊往回走：
                              │      __schedule → schedule → exit_to_user_mode_loop
                              │      → ret_to_user → kernel_exit 0 → eret
⑨ B 從自己的 pt_regs 恢復      │
   回到 EL0 被中斷的那一行      │
```

三個容易被追問的點：

* **A 的中斷現場（`pt_regs`）留在 A 的核心堆疊裡**，不會丟；等下次 A 被調度回來，
  它會沿著同一條路徑走回 `ret_to_user` 再 `eret` 回去。
* **A 和 B 的核心堆疊各自獨立**（本機 `THREAD_SIZE = 16 KB`，且 `CONFIG_VMAP_STACK=y`）。
  `switch_to` 的 `mov sp, x9` 就是「換堆疊」那一瞬間。
* **本機 `CONFIG_PREEMPT_VOLUNTARY`（沒有 `CONFIG_PREEMPT`）**：
  如果 A 當時是在**核心態**（例如在做 syscall），中斷返回核心態時**不會**檢查
  `TIF_NEED_RESCHED`，要等 A 走到 `cond_resched()` 或返回使用者空間才切。
  本題 A 在使用者空間，所以第 ⑤ 步一定會發生。

> **書目**：奔跑吧 §8.1.5、§8.1.6；Ch9 §9.3.5「調度的本質」與圖 9.19/9.20
> （第 600~660 行）把堆疊的壓入/彈出畫得很清楚。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'sudo /tmp/sched_trace.sh switch'
```

```
<idle>-0    [002] d.h.. 156307.922775: sched_waking:  comm=listener pid=968 target_cpu=002
<idle>-0    [002] dNh.. 156307.922785: sched_wakeup:  comm=listener pid=968 target_cpu=002
<idle>-0    [002] d.... 156307.922791: sched_switch:  prev_comm=swapper/2 prev_pid=0 prev_state=R ==> next_comm=listener next_pid=968
listener-968[002] d.... 156307.922837: sched_switch:  prev_comm=listener prev_state=S ==> next_comm=swapper/2 next_pid=0
...
sh-100767   [002] d.... 156307.938569: sched_switch:  prev_comm=sh prev_pid=100767 prev_state=R ==> next_comm=taskset next_pid=100768
sh-100768   [002] d.... 156307.991901: sched_switch:  prev_comm=sh prev_pid=100768 prev_state=R ==> next_comm=sh next_pid=100767
```

讀 trace 的 flag 欄（第 3 欄 `d.h..` / `dNh..` / `d....`）就能還原上面的流程：

| flag | 含義 | 對應步驟 |
|---|---|---|
| `d` | irqs-off | ② CPU 進中斷自動關中斷；⑥ `__schedule()` 也會 `local_irq_disable()` |
| `h` | 在 **hardirq** 上下文 | ④ 時鐘中斷處理中呼叫 `sched_waking` |
| `N` | `TIF_NEED_RESCHED` 已置位 | ④ 的 `resched_curr()` 剛做完 |
| `d....`（沒有 h） | 已離開中斷上下文 | ⑥ 真正的 `context_switch()` 發生在中斷返回路徑上，不是在中斷處理常式裡 |

**`dNh..`（設了 need_resched，還在 hardirq）→ `d....`（切換）這個順序，
就是「中斷裡只做記號、返回時才真的切」的鐵證。**

兩個 `sh` 之間 `prev_state=R`（不是 S）→ 是被搶佔而非主動睡眠，
間隔精準 13.333 ms（4 個 tick），對應第 ④ 步的 `check_preempt_tick()`。

---

<a name="q12"></a>
## 12. 接上題，進程 B 運行的時候，它從什麼地方開始運行第一條指令？直接運行被暫停在用戶空間的那條指令嗎？

### 結論

**不是。B 從「它自己上一次被換出去的那個地方」開始，也就是核心態的
`cpu_switch_to` 的 `ret` 之後**，然後**沿著自己的核心堆疊一路 return 回去**，
最後才在 `eret` 那一刻回到使用者空間被中斷的指令。

具體位置由 `next->thread.cpu_context.pc` 決定，有兩種可能：

| B 的身分 | `cpu_context.pc` 指向 | 第一條指令 |
|---|---|---|
| **老行程**（切換過至少一次） | 上次 `cpu_switch_to` 執行 `str lr, [x8]` 時的 `lr`，也就是 **`__switch_to()` 的返回位址**（在 `context_switch()` 裡） | `finish_task_switch(prev)` |
| **新行程**（第一次上 CPU） | `copy_thread()` 寫死的 **`ret_from_fork`** | `bl schedule_tail` |

老行程的回程堆疊（本機實測，見下）：

```
cpu_switch_to 的 ret
  → __switch_to() 返回
    → context_switch() 的 finish_task_switch(prev)
      → __schedule() 返回
        → schedule() 返回
          → exit_to_user_mode_loop() 返回
            → ret_to_user / kernel_exit 0
              → eret   ← 這裡才回到 EL0 被中斷的那條指令
```

**為什麼不能直接跳回使用者空間？** 因為：
1. B 的使用者態現場在 **B 自己的 `pt_regs`** 裡（在核心堆疊頂端），
   要靠 `kernel_exit` 那一段組語去 `ldp` 出來；
2. 中間還有事要做——`finish_task_switch()` 要放 rq->lock、開中斷、還 mm_count、
   釋放死掉的 prev；`exit_to_user_mode_loop()` 還要處理 signal、`_TIF_NEED_RESCHED`、
   `_TIF_NOTIFY_RESUME`。

> **書目**：奔跑吧 §8.1.6（第 1620~1640 行）——
> 「一個特殊情況是新建進程，第一次執行的切入點在 copy_thread() 函數中指定的
> ret_from_fork 匯編函數中」；Ch9 §9.3.5「如何讓新進程執行」與圖 9.18。

### 實機驗證

**(a) 新行程的 `cpu_context.pc` 真的是 `ret_from_fork`**

`sched_probe.ko` 在 `insmod` 的行程上下文中印出 `current->thread.cpu_context`。
`insmod` 是 `sched_trace.sh` 剛 fork 出來、**還沒被換出去過**的行程，
所以它的 `cpu_context` 仍是 `copy_thread()` 寫進去的初值：

```
   current 的 cpu_context.pc = ret_from_fork+0x0/0x20, sp = 0xffff80000daabeb0
   current->stack = ffff80000daa8000, task_pt_regs(current) = ffff80000daabeb0
```

兩件事一次證完：
* **`pc == ret_from_fork`** ✅
* **`sp == task_pt_regs(current)`** ✅ —— 對應 `copy_thread()` 的
  `p->thread.cpu_context.sp = (unsigned long)childregs;`
  （堆疊底 `0xffff80000daa8000` + 16 KB − `sizeof(pt_regs)=336` − 16 = `0x...daabeb0`）

**(b) 新行程的第一條指令**

```
sched_trace.sh-100848  [005] d.... 156323.746242: schedule_tail <-ret_from_fork
 => schedule_tail
 => ret_from_fork
```

`ret_from_fork` 的組語（`arch/arm64/kernel/entry.S:860`）：

```asm
SYM_CODE_START(ret_from_fork)
	bl	schedule_tail		// ← 第一條指令：幫 prev 收尾 + 開中斷
	cbz	x19, 1f			// x19 == 0 → 不是內核執行緒
	mov	x0, x20			// 內核執行緒：x20 = 參數
	blr	x19			//              x19 = 回呼函式
1:	get_current_task tsk
	mov	x0, sp
	bl	asm_exit_to_user_mode
	b	ret_to_user		// ← 使用者行程從這裡回 EL0
SYM_CODE_END(ret_from_fork)
```

**(c) 老行程「不會回到使用者空間被中斷的那條指令」，中間隔著一整條核心堆疊**

`sched_probe.ko` 的 IRQ 測試間接證明了這一點：關中斷後呼叫 `schedule()`，
回來時中斷竟然是開的（見 [Ch9 Q13](./ch09_process_management_debugging_and_case_studies.md#q13)），
因為 `finish_task_switch()` 在「我」被換回來之後才執行——
如果 `switch_to` 直接跳回使用者空間，就不會有這個現象。

---

<a name="q13"></a>
## 13. 在進程切換時需要刷新 TLB，在 ARM64 處理器中如何提高 TLB 的性能？

### 結論

**靠 ASID（Address Space ID）：讓 TLB 表項帶上「這是誰的」標籤，
行程切換就不用清空整個 TLB。** 三層機制：

**(1) 硬體：TLB 表項的 tag = VA + ASID**

* ARMv8 的 TLB 命中條件從「VA 相符」變成「VA 相符 **且** ASID 相符（或該項是 global）」。
* 內核空間（TTBR1_EL1，`0xffff...`）的頁表項不設 `nG` 位 → **global**，所有行程共用，永不失效。
* 使用者空間（TTBR0_EL1）的頁表項設 `nG` → **non-global**，靠 ASID 區分。
* ASID 寬度由 `ID_AA64MMFR0_EL1.ASIDBits` 決定：0 → 8 bit（256 個），2 → 16 bit（65536 個）。
* 硬體 ASID 存放位置由 `TCR_EL1.A1` 決定：0 → TTBR0_EL1[63:48]，1 → **TTBR1_EL1[63:48]**。

**(2) 軟體：ASID allocator（`arch/arm64/mm/context.c`）**

```
mm->context.id  (atomic64)  =  [ generation | 硬體 ASID ]
                                 高位          低 asid_bits 位
```

* `asid_map` 點陣圖管理硬體 ASID 的配置；ASID 0 保留給 `init_mm`。
* 全域 `asid_generation` 記錄「第幾輪」。
* `check_and_switch_context()`：
  1. 如果 `mm->context.id` 的 generation == 當前 generation → **完全不用刷 TLB**，直接切頁表；
  2. 否則 `new_context()` 重新配一個；
  3. 如果 `asid_map` 滿了（溢位）→ `flush_context()`：generation++、清點陣圖、
     **在所有 CPU 上刷一次 TLB**（`local_flush_tlb_all` + IPI）。
* 65536 個 ASID 讓溢位變得極罕見。

**(3) 其他 ARM64 特有的 TLB 優化**

| 機制 | 說明 | 本機狀態 |
|---|---|---|
| **ASID** | 上述 | ✅ 16 bit |
| **nG / global 分離** | 內核映射永不失效 | ✅ |
| **TLBI 的範圍指令** | `TLBI VAE1IS`（單頁）、`TLBI ASIDE1IS`（單 ASID）而非 `TLBI VMALLE1`（全清） | ✅ |
| **TLBI RANGE（ARMv8.4）** | 一條指令刷一段位址 | RK3588 (A76/A55) **不支援** |
| **kpti 成對 ASID** | 開 `CONFIG_UNMAP_KERNEL_AT_EL0` 時每個行程要**奇偶一對** ASID，可用數量減半 | **本機編譯了但未啟用**（A76/A55 不受 Meltdown 影響），所以一個行程只佔一個 ASID |
| **`switch_mm` 的 lazy TLB** | 內核執行緒不換 TTBR0，借用 `active_mm` | ✅ |

> **書目**：奔跑吧 §8.1.6「switch_mm() 函數」（第 1650~1740 行）與圖 8.6。
> 書上說「軟體 ASID…低 8 位是硬體 ASID，剩餘的位是軟體 generation 計數」，
> **這是 ASIDBits=0（8 bit）的情況；本機 ASIDBits=2，切分點在第 16 位**。
> **原始碼**：`arch/arm64/mm/context.c`（`asids_init`/`check_and_switch_context`/`new_context`）、
> `arch/arm64/mm/proc.S`（`cpu_do_switch_mm`）。
> **TRM**：ARM ARM D13.2.64 `ID_AA64MMFR0_EL1`、D13.2.135 `TTBR1_EL1`、D5.9「TLB maintenance」。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'sudo insmod ~/exp/sched/sched_probe.ko; sudo rmmod sched_probe; \
                         sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A8 "Q13"'
```

```
== Ch8 Q13  ARM64 ASID / TLB ==
   ID_AA64MMFR0_EL1 = 0x0000000000101122，ASIDBits 欄位 = 2 -> 硬體 ASID 寬度 = 16 bit（最多 65536 個）
   TCR_EL1          = 0x000001f2b5503510，A1(bit22)=1 -> ASID 由 TTBR1_EL1 提供
   TTBR0_EL1        = 0x0000000077193001  (ASID=0,     BADDR=0x77193001)
   TTBR1_EL1        = 0x7fe0000001b2d001  (ASID=32736, BADDR=0x1b2d001)
   kpti (UNMAP_KERNEL_AT_EL0) = 已編譯但未啟用 -> 每個行程只配一個 ASID
   current(insmod/97515) mm->context.id = 0x37fe0 -> 硬體 ASID = 32736，軟體 generation = 3
   mm->pgd = ffff000077193000, __pa(pgd) = 0x77193000（應等於 TTBR0_EL1 的 BADDR）
```

逐項對帳：

| 觀察 | 意義 |
|---|---|
| `ID_AA64MMFR0_EL1` bit[7:4] = **2** | **16 bit ASID，65536 個** — 書上兩種情況中的第二種 |
| `TCR_EL1.A1 = 1` | ASID 由 **TTBR1_EL1[63:48]** 提供 → 書上「在 AArch64 狀態下，硬體 ASID 存放在 TTBR1_EL1 中」✅ |
| `TTBR1_EL1 = 0x7fe0_0000_01b2d001` | 高 16 位 `0x7fe0` = **32736** = 當時執行行程的硬體 ASID（**與 `mm->context.id` 的低 16 位完全相同**）；低 48 位 = `swapper_pg_dir` 的實體位址（所有行程共用） |
| `TTBR0_EL1` 的 ASID 欄位 = **0** | 印證 `cpu_do_switch_mm` 只把 ASID 寫進 TTBR1，TTBR0 只放使用者 PGD |
| `mm->context.id = 0x37fe0` | `0x37fe0 & 0xffff = 0x7fe0 = 32736`（硬體 ASID）、`0x37fe0 >> 16 = 3`（generation） → **證實 `[generation \| asid]` 的位元佈局，切分點在 16 而非 8** |
| `__pa(mm->pgd) = 0x77193000` vs `TTBR0_EL1 BADDR = 0x77193001` | 差的那個 bit0 是 `TTBR_CNP`（Common Not Private），本機開了 ARM64_HAS_CNP |

**generation = 3** 表示這台機器開機 1.8 天、跑了十萬多個行程，
ASID 才只溢位過 3 次 —— 這就是 16 bit ASID 的價值。
（如果只有 8 bit，256 個 ASID 早就轉了上千圈，每轉一圈都要全系統刷 TLB。）

`cpu_do_switch_mm` 用 `BFI` 把 ASID 塞進 TTBR1 的動作，可以直接看組語：

```bash
grep -n -A14 "SYM_FUNC_START(cpu_do_switch_mm)" arch/arm64/mm/proc.S
```

---

<a name="q14"></a>
## 14. CFS 在什麼時候檢查是否需要調度？

### 結論

CFS 有**兩個**檢查點，都只「設旗標」不直接切換：

**(1) 週期性檢查：`check_preempt_tick()`** —— 時鐘節拍驅動

```
時鐘中斷 → tick_handler → update_process_times → scheduler_tick()  (core.c:5507)
   → curr->sched_class->task_tick()  =  task_tick_fair()
     → for_each_sched_entity(se): entity_tick(cfs_rq, se, queued)
       → update_curr()             更新 vruntime / min_vruntime
       → update_load_avg()         更新 PELT
       → check_preempt_tick(cfs_rq, curr)   ★
```

`check_preempt_tick()`（`kernel/sched/fair.c:4908`）三個判斷，依序：

```c
ideal_runtime = min_t(u64, sched_slice(cfs_rq, curr), sysctl_sched_latency);
delta_exec    = curr->sum_exec_runtime - curr->prev_sum_exec_runtime;
if (delta_exec > ideal_runtime) {              /* ① 跑滿自己的份 */
	resched_curr(rq_of(cfs_rq));
	clear_buddies(cfs_rq, curr);
	return;
}
if (delta_exec < sysctl_sched_min_granularity) /* ② 還沒跑滿最小粒度，不許切 */
	return;
se = __pick_first_entity(cfs_rq);              /* ③ 跟最左節點比 vruntime */
delta = curr->vruntime - se->vruntime;
if (delta > ideal_runtime)
	resched_curr(rq_of(cfs_rq));
```

**(2) 喚醒檢查：`check_preempt_wakeup()`** —— 有人被喚醒到本 CPU 時

```
try_to_wake_up() → ttwu_do_activate() → ttwu_do_wakeup()
   → check_preempt_curr() → rq->curr->sched_class->check_preempt_curr()
     = check_preempt_wakeup(rq, p, wake_flags)
```
門檻是 `wakeup_preempt_entity()`：被喚醒者的 vruntime 要比 curr 小超過
`calc_delta_fair(sysctl_sched_wakeup_granularity, se)`（本機 **4 ms**）才搶得動。
另外還有 `NEXT_BUDDY`/`LAST_BUDDY`（本機 `NO_NEXT_BUDDY LAST_BUDDY`）影響下一輪的選擇。

**設完旗標之後真正切換的時機**見 [Q41](#q41)。

> **書目**：奔跑吧 §8.1.7「調度節拍」（第 1947~2025 行），書上引用的行號是 5.0 的
> `fair.c:4029/4030/4046`，本機是 `fair.c:4908`。
> **注意**：書上（Ch9 §9.1.4）說 `sched_min_granularity_ns` 預設 0.75 ms、
> `sched_latency_ns` 預設 6 ms、`sched_wakeup_granularity_ns` 預設 1 ms，
> **本機因為 `tunable_scaling=1`（logarithmic）× 8 顆 CPU 全部乘了 4 倍**（見下）。

### 實機驗證

**(a) 本機的三個門檻值（注意路徑已經搬家）**

```bash
# 書上是 /proc/sys/kernel/sched_*，6.1 已經搬到 debugfs
ssh radxa@192.168.68.57 'sudo grep . /sys/kernel/debug/sched/{latency_ns,min_granularity_ns,\
idle_min_granularity_ns,wakeup_granularity_ns,migration_cost_ns,nr_migrate,tunable_scaling}'
```

```
latency_ns              = 24000000    (24 ms)
min_granularity_ns      =  3000000    ( 3 ms)
idle_min_granularity_ns =   750000    (0.75 ms)
wakeup_granularity_ns   =  4000000    ( 4 ms)
migration_cost_ns       =   500000    (0.5 ms)
nr_migrate              = 32
tunable_scaling         = 1 (logarithmic)
```

放大係數：`factor = 1 + ilog2(nr_cpus) = 1 + ilog2(8) = 4`
→ `6 ms × 4 = 24 ms`、`0.75 ms × 4 = 3 ms`、`1 ms × 4 = 4 ms` ✅
（`sched_nr_latency = latency/min_granularity = 8`，正好等於 CPU 數，純屬巧合。）

**(b) `check_preempt_tick()` 的三個分支都能量出來**

在 CPU3 上放 n 個 nice=0 的 CPU-bound 行程，用 `(CPU 時間)/(被搶佔次數)`
反推每次實際跑多久：

| n | 每個行程 CPU 時間 | `nivcsw` | **實測時間片** | `__sched_period(n)` | `sched_slice = period/n` | 對齊到 tick(3.33 ms) |
|---|---|---|---|---|---|---|
| 2 | 3.00 s | 225 | **13.33 ms** | 24 ms | 12 ms | 4 tick = 13.33 ✅ |
| 4 | 1.50 s | 225 | **6.67 ms** | 24 ms | 6 ms | 2 tick = 6.67 ✅ |
| 6 | 1.00 s | 150 | **6.67 ms** | 24 ms | 4 ms | 2 tick = 6.67 ✅ |
| 8 | 0.75 s | 225 | **3.33 ms** | 24 ms | 3 ms | 1 tick = 3.33 ✅ |
| 12 | 0.50 s | 150 | **3.33 ms** | 36 ms | 3 ms | 1 tick ✅ |
| 16 | 0.375 s | 113 | **3.32 ms** | 48 ms | 3 ms | 1 tick ✅ |

三個結論：
1. `n ≤ 8` 時週期固定 24 ms（`sched_latency`），`n > 8` 時變成 `n × 3 ms`
   → **`__sched_period()` 的 `sched_nr_latency = 8` 門檻被實測命中**。
2. **實測時間片永遠是 3.33 ms 的整數倍**，因為 `check_preempt_tick()` 只在
   節拍上跑（HZ=300，`NO_HRTICK`）。這是 `sched_min_granularity=3 ms` 在
   HZ=300 上「無效」的原因——tick 本身就比它大。
3. `n=1` 時 `nivcsw` 只有 3（6 秒內）→ 沒人競爭就不會被搶佔。

**(c) 從 ftrace 看 `check_preempt_tick` 在呼叫鏈上的位置**

見 [Q4](#q4) 的 `function_graph`：`scheduler_tick() → task_tick_fair() → update_curr() → ...`。

`check_preempt_tick()` 與 `entity_tick()` 在本機都被編譯器 inline 掉了，
`available_filter_functions` 裡查不到；查得到的是它們的上下游：

```bash
ssh radxa@192.168.68.57 'grep -E "^(scheduler_tick|task_tick_fair|entity_tick|\
check_preempt_tick|update_curr)$" /sys/kernel/debug/tracing/available_filter_functions'
```
```
scheduler_tick
task_tick_fair
update_curr
```
（`entity_tick` / `check_preempt_tick` 不在列表中 = 已被 inline。）

---

<a name="q15"></a>
## 15. 雙核處理器，CPU0 和 CPU1 各只有一個進程、優先級和權重相同，但 CPU0 上的進程一直佔用 CPU，CPU1 上的進程走走停停，那麼兩者的負載是否相同？

### 結論

**用「權重」算：相同（都是 1024）。用 PELT 的「量化負載」算：完全不同。**

這正是 PELT 存在的理由。三個公式的演進：

```
(8.6) CPU 負載 = 就緒佇列總權重            ← 只看有幾個人、多大牌，不看他忙不忙
(8.8) CPU 負載 = 權重 × (運行時間 / 採樣總時間)   ← 加入「時間佔比」
(8.11) 量化負載 = 權重 × (衰減後 runnable 時間 / 衰減後總時間)  ← 再加上「歷史衰減」
```

`load_avg = weight × decay_sum_runnable_time / LOAD_AVG_MAX`：

* CPU0 的行程 runnable 佔比 ≈ 100% → `load_avg → 1024`
* CPU1 的行程 runnable 佔比 = duty → `load_avg → 1024 × duty`

**但要小心一個陷阱**：這題問的是「一直佔用」vs「走走停停」，
如果 CPU1 的行程是**「在就緒佇列裡等」而不是「睡著」**，那它的 `load_avg`
一樣會逼近 1024（見 [Q8](#q8)）。真正能區分兩者的是 **`util_avg`**：

| | CPU0（100% 跑） | CPU1（50% 跑 50% 睡） | CPU1（50% 跑 50% 等） |
|---|---|---|---|
| 佇列總權重 `load` | 1024 | 1024 | 1024 |
| `load_avg` | ~1024 | **~512** | ~1024 |
| `util_avg` | ~1024 | **~512** | ~512 |

> **書目**：奔跑吧 §8.2.1「如何衡量一個 CPU 的負載」圖 8.13（第 2216~2291 行）——
> 這一題就是書上的原題。
> **注意**：書上 §8.1.7、§8.3.4 提到的 `cpu_load[5]` 平滑陣列與
> `update_cpu_load_active()` **在 Linux 5.7 已經被刪除**（commit `55627e3cd22c`），
> 本機 `/sys/kernel/debug/sched/debug` 裡也看不到 `cpu_load[]`。

### 實機驗證

`pelt_duty.c` 產生固定 duty cycle 的行程，把「一直跑」與「走走停停」放在**同一顆** A76 上比較：

```bash
ssh radxa@192.168.68.57 'cd /tmp
# (a) 100% 一直跑
./sched_weight 4 8 0
# (b) 50% duty（4 ms busy / 8 ms period，短週期讓 PELT 收斂）
./pelt_duty 4 10 time 4 8 | tail -1
# (c) 25% duty
./pelt_duty 4 10 time 2 8 | tail -1'
```

```
(a) 100%  pid 94439  weight=1048576  CPU%=100.00  load_avg=1023  util_avg=1023
(b)  50%  FINAL cpu=4 weight=1048576 load_avg=496  runnable_avg=496  util_avg=496
(c)  25%  FINAL cpu=4 weight=1048576 load_avg=272  runnable_avg=273  util_avg=244
```

| duty | 理論 `1024 × duty` | 實測 `load_avg` | 實測 `util_avg` |
|---|---|---|---|
| 100% | 1024 | **1023** | 1023 |
| 50% | 512 | **496** | 496 |
| 25% | 256 | **272 / 244** | 244 |

**權重完全一樣（1048576），量化負載卻差了 2~4 倍。** 這就是答案。

補充一個很有意思的邊界：**如果週期拉長到跟 PELT 半衰期同量級，
量化負載會嚴重低估 duty cycle**：

```bash
ssh radxa@192.168.68.57 'cd /tmp && for d in 25 50 75; do
  printf "duty %d%%: " $d; ./pelt_duty 4 10 time $d 100 | tail -1; done'
```
```
duty 25%: FINAL cpu=4 load_avg=100  util_avg=100
duty 50%: FINAL cpu=4 load_avg=268  util_avg=268
duty 75%: FINAL cpu=4 load_avg=551  util_avg=551
```

50% duty 卻只量到 268（而非 512）！因為週期 100 ms 遠大於半衰期 32 ms，
PELT 值呈鋸齒狀，而程式是在睡完之後（波谷）取樣的。
手算：穩態波谷 `x` 滿足 `x = y^50 × (1024 + (x−1024)·y^50)`，
`y^50 = 2^(−50/32) = 0.339` → `x ≈ 259`，**與實測 268 吻合**。
這正是書上 §8.4 圖 8.28「一個進程工作 20 ms 睡 20 ms，CPU 利用率最高只有 60%」
所描述的 PELT 缺陷，也是 Android 當年要用 WALT 取代 PELT 的原因。

---

<a name="q16"></a>
## 16. 請簡述負載衰減的意義。

### 結論

**衰減 = 給「歷史」打折，讓負載能反映「現在」。**

沒有衰減會怎樣？式 (8.8) 把 10 秒前的工作和 1 毫秒前的工作等價看待：
* 一個跑了三天三夜的 CPU-bound 行程，突然睡著 → 它的負載還是很高 → 負載均衡誤判；
* 一個睡了三天的行程突然狂跑 → 它的負載還很低 → 不會被搬走、也不會升頻。

PELT 引入幾何級數衰減（`the accumulation of an infinite geometric series`）：

```
decay_sum = L0 + L1·y + L2·y² + L3·y³ + ... + Ln·yⁿ
```

其中 `y = 0.5^(1/32)` ≈ 0.97857，`Li` 是第 i 個 1024 µs 週期（PI）內的貢獻。

四個直接後果：

| 性質 | 數值（本機實測） | 意義 |
|---|---|---|
| **半衰期** | **32 ms** | 32 ms 前的貢獻只算一半 |
| **收斂上界** | `LOAD_AVG_MAX = 47742` | 無窮多個週期的貢獻總和有限 → 可以當分母做「量化」 |
| **反應速度** | **32 ms → 50%，75 ms → 80%，138 ms → 95%** | 從 0 開始滿載，多久才「看起來忙」 |
| **遺忘速度** | **2016 ms** | `n > 32×63` 時 `decay_load()` 直接回 0，歷史徹底清空 |

**衰減也帶來代價**（EAS 誕生的動機，見 §8.4）：
* 反應**遲鈍**——手機滑屏突然變重活，要 75 ms 才認得出來；
* 對**週期性**行程不友善（見 [Q15](#q15) 的鋸齒）；
* 睡眠/阻塞行程仍在貢獻衰減負載（blocked load），會**拖慢降頻**。

> **書目**：奔跑吧 §8.2.3「歷史累計衰減的計算」（第 2291~2343 行）與圖 8.14/8.15；
> §8.4 開頭「PELT 使用 32 ms 的衰減時間，大約 213 ms 才能把之前的負載忘記」。
> **修正**：書上這句「213 ms 忘記」指的是衰減到很小（`0.5^(213/32) ≈ 1%`），
> **程式碼上真正歸零的門檻是 2016 ms**（`LOAD_AVG_PERIOD * 63`）。

### 實機驗證

`sched_probe.ko` 用核心自己的 `runnable_avg_yN_inv[]` 重跑一遍衰減：

```
== Ch8 Q19/Q20  PELT 衰減 ==
   y = 0.5^(1/32)；y^32 應為 0.5：decay_load(1024, 32) = 511 （= 1024/2）
   decay_load(1024, 1)=1002  (10)=824  (32)=511  (64)=255  (2016)=0  (2017)=0
   收斂的 sum 1024*y^n = 47742（346 次迭代收斂），核心 LOAD_AVG_MAX = 47742
   PELT 分母 divider = LOAD_AVG_MAX - 1024 + period_contrib = 46718 ~ 47741
   從 0 開始 100% 執行 util(n)=1024*(1-y^n)：32 ms 到 50%，75 ms 到 80%，138 ms 到 95%
   （書中 §8.4：74ms / 139ms）
   反過來：完全「忘掉」歷史負載需要 32*63 = 2016ms
```

| 檢查項 | 理論 | 實測 |
|---|---|---|
| `y^1` | 0.97857 | 1002/1024 = **0.97852** ✅ |
| `y^10` | 0.80430 | 824/1024 = **0.80469** ✅ |
| `y^32` | **0.5** | 511/1024 = **0.49902** ✅ |
| `y^64` | 0.25 | 255/1024 = **0.24902** ✅ |
| 反應到 80% | 書上 74 ms | **75 ms** ✅ |
| 反應到 95% | 書上 139 ms | **138 ms** ✅ |

書上的 74 ms / 139 ms 是用 `1024·(1−y^t) = 0.8·1024` 解出來的
（`t = 32·log₂5 = 74.3`、`t = 32·log₂20 = 138.4`），實測整數化後是 75/138。**完全對上。**

---

<a name="q17"></a>
## 17. 請簡述 PELT 算法中量化負載的計算方法。

### 結論

**三步：切週期 → 衰減舊帳 → 加新帳，最後除以一個固定分母。**

```
                     ┌── D0 ──┬──── D1 ────┬─ p 個完整週期 ─┬── D3 ──┐
   上次更新 time0 ───┤        │            │                │        ├─── 本次更新 time1
                     └ 上次不 ┘ 補滿第一個 ┘   每個 1024µs  └ 這次不 ┘
                       滿一個                                  滿一個
                       週期的                                  週期的
                       零頭                                    零頭
```

**(1) `accumulate_sum()`（`kernel/sched/pelt.c:101`）**

```c
delta   += sa->period_contrib;      /* D0 */
periods  = delta / 1024;            /* 跨過幾個完整週期 */
if (periods) {
	sa->load_sum     = decay_load(sa->load_sum,     periods);   /* 舊帳打折 */
	sa->runnable_sum = decay_load(sa->runnable_sum, periods);
	sa->util_sum     = decay_load(sa->util_sum,     periods);
	delta %= 1024;                                              /* D3 */
	if (load)
		contrib = __accumulate_pelt_segments(periods,
				1024 - sa->period_contrib /* D1 */, delta /* D3 */);
}
sa->period_contrib = delta;
if (load)     sa->load_sum     += load * contrib;
if (runnable) sa->runnable_sum += runnable * contrib << SCHED_CAPACITY_SHIFT;
if (running)  sa->util_sum     += contrib << SCHED_CAPACITY_SHIFT;
```

`__accumulate_pelt_segments()` 就是式 (8.21) 的第二項：

```c
c1 = decay_load(d1, periods);                                    /* D1 那段衰減 periods 次 */
c2 = LOAD_AVG_MAX - decay_load(LOAD_AVG_MAX, periods) - 1024;    /* 中間 p-1 個完整週期 */
c3 = d3;                                                         /* D3，y^0 = 1 */
return c1 + c2 + c3;
```

`c2` 的技巧：`Σ_{n=1}^{p-1} 1024·yⁿ = LOAD_AVG_MAX − LOAD_AVG_MAX·y^p − 1024`。
**4.12 之前是查 `runnable_avg_yN_sum[]` 表，之後改用這一行**（書上還在講那張表）。

**(2) `___update_load_avg()`（`kernel/sched/pelt.c`）**

```c
u32 divider = get_pelt_divider(sa);   /* = LOAD_AVG_MAX - 1024 + sa->period_contrib */
sa->load_avg     = div_u64(load * sa->load_sum, divider);
sa->runnable_avg = div_u64(runnable * sa->runnable_sum, divider);
sa->util_avg     = sa->util_sum / divider;
```

**(3) 頻率 / 算力不變性 —— 本機與書上完全不同的地方**

書上（5.0）說 `accumulate_sum()` 裡要乘 `arch_scale_freq_capacity()` 和
`arch_scale_cpu_capacity()`。**Linux 5.3 之後這兩個乘法被搬到了「PELT 時鐘」上**
（commit `23127296889f` "sched/fair: Update scale invariance of PELT"）：

```c
/* kernel/sched/pelt.h */
static inline void update_rq_clock_pelt(struct rq *rq, s64 delta)
{
	if (unlikely(is_idle_task(rq->curr))) {
		_update_idle_rq_clock_pelt(rq);      /* 閒置時：clock_pelt 追上 clock_task */
		return;
	}
	delta = cap_scale(delta, arch_scale_freq_capacity(cpu_of(rq)));   /* ← FIE */
	delta = cap_scale(delta, arch_scale_cpu_capacity(cpu_of(rq)));    /* ← CIE */
	rq->clock_pelt += delta;
}
```

**效果**：PELT 的「時間」在慢 CPU / 低頻率上走得慢，所以
「同樣的工作量」在大核和小核上得到**相同的** `util_avg`。
副作用：一個**永遠在跑**的行程，因為 rq 從不閒置、clock_pelt 從不追齊，
在 A55 上 `util_avg` 一樣會到 **1024**（見下面的實測）。

> **書目**：奔跑吧 §8.2.7「PELT 代碼分析」（第 2611~2954 行）、式 (8.17)~(8.22)、圖 8.16。

### 實機驗證

**(a) 頻率不變性（FIE）—— 把 A76 的頻率鎖在三個 OPP，跑同一個 25% duty 的行程**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
modprobe cpufreq_userspace; echo userspace > /sys/devices/system/cpu/cpufreq/policy4/scaling_governor
for f in 2256000 1200000 600000; do
  echo \$f > /sys/devices/system/cpu/cpufreq/policy4/scaling_setspeed; sleep 1
  printf \"freq=%s : \" \$f
  su radxa -c \"/tmp/pelt_duty 4 8 time 2 8\" | tail -1
done
echo schedutil > /sys/devices/system/cpu/cpufreq/policy4/scaling_governor"'
```

```
freq=2256000 cur=2256000 : FINAL cpu=4 weight=1048576 load_avg=243 runnable_avg=243 util_avg=243
freq=1200000 cur=1200000 : FINAL cpu=4 weight=1048576 load_avg=131 runnable_avg=131 util_avg=131
freq=600000  cur=600000  : FINAL cpu=4 weight=1048576 load_avg=64  runnable_avg=64  util_avg=64
```

| 頻率 | `freq_scale = f/fmax` | 預期 `util = 243 × freq_scale` | **實測** |
|---|---|---|---|
| 2256 MHz | 1.000 | 243 | **243** |
| 1200 MHz | 0.532 | 129.3 | **131** ✅ |
| 600 MHz | 0.266 | 64.6 | **64** ✅ |

**誤差 < 1.5%。頻率不變性在 RK3588 上是實打實生效的。**
（`topology_scale_freq_tick()` 在每個 tick 被呼叫，見 [Q4](#q4) 的 funcgraph。）

**(b) 算力不變性（CIE）—— 同樣 50% duty，A55 vs A76**

> 這一組要先把 governor 固定成 `performance`，否則頻率會浮動、
> FIE 與 CIE 兩個效應會混在一起。

```bash
ssh radxa@192.168.68.57 'sudo bash -c "for p in 0 4 6; do \
    echo performance > /sys/devices/system/cpu/cpufreq/policy$p/scaling_governor; done"
cd /tmp; for c in 3 4; do printf "cpu%d: " $c; ./pelt_duty $c 10 time 4 8 | tail -1; done'
```
```
cpu3 (A55, cap=422) : load_avg=199  runnable_avg=200  util_avg=200
cpu4 (A76, cap=1024): load_avg=496  runnable_avg=496  util_avg=496
```

`200 / 496 = 0.403`，而 `422 / 1024 = 0.412` —— **誤差 2%**。
在 A55 上跑，PELT 時鐘只走 0.412 倍，所以同樣的牆鐘 duty 得到的 `util_avg` 就低了那麼多。

**（用預設的 `ondemand` governor 重跑一次，比值仍然成立、但絕對值會變低：**
`cpu3 util_avg=122`、`cpu4 util_avg=307`，`122/307 = 0.397`
—— 因為 25~50% 的負載讓 ondemand 沒有把頻率拉滿，FIE 又把 util 壓下去了。
**這正好說明兩個不變性是相乘的。）**

**(c) 「永遠在跑」的行程是個例外**

```bash
ssh radxa@192.168.68.57 'cd /tmp; ./sched_weight 3 8 0; ./sched_weight 4 8 0'
```
```
CPU3 (A55, cap=422) 1 個行程：load_avg=1024  util_avg=1024
CPU4 (A76, cap=1024) 1 個行程：load_avg=1023 util_avg=1023
```

**A55 上滿載行程的 `util_avg` 也是 1024，不是 422。**
原因就是上面說的 `_update_idle_rq_clock_pelt()`：rq 從不閒置，
clock_pelt 與 running 時間同步縮放，比例仍是 100%。
這不是 bug——`util_avg` 表達的是「這個 task 需要多少比例的**當前** CPU」，
不是「它需要多少絕對算力」。要判斷放不放得下大核/小核，
EAS 用的是 `task_fits_capacity(p, capacity)`（見 [Q30](#q30)）。

---

<a name="q18"></a>
## 18. 請簡述什麼是處理器的額定算力和當前實際算力。

### 結論

Linux 裡有**三個**「算力」，一定要分清楚：

| 名稱 | 欄位 / 函式 | 意義 | RK3588 實測 |
|---|---|---|---|
| **處理器額定算力** | `rq->cpu_capacity_orig`，`arch_scale_cpu_capacity()`，`capacity_orig_of()` | 這顆 CPU **最強**時能提供多少算力，系統中最強的量化成 1024 | A55 = **422**，A76 = **1024** |
| **CFS 額定算力** | `rq->cpu_capacity`，`capacity_of()`，`sched_group_capacity->capacity` | 扣掉 RT / DL / IRQ / thermal 壓力後**還剩給 CFS** 的算力 | A55 = **420~422**，A76 = **1019~1024** |
| **實際算力（需求）** | `se->avg.util_avg`、`cfs_rq->avg.util_avg`、`cpu_util_cfs()` | 目前**用掉 / 需要**多少算力 | 0 ~ 1024 |

**額定算力怎麼來的（`drivers/base/arch_topology.c`）：**

```c
/* topology_parse_cpu_capacity(): 讀 DT 的 capacity-dmips-mhz */
raw_capacity[cpu] = capacity_dmips_mhz;
/* topology_normalize_cpu_scale(): 再乘上各自的最高頻率並歸一化 */
capacity = (raw_capacity[cpu] * freq_factor[cpu] << SCHED_CAPACITY_SHIFT) / capacity_scale;
per_cpu(cpu_scale, cpu) = capacity;
```

**CFS 額定算力怎麼來的（`kernel/sched/fair.c` `update_cpu_capacity()` / `scale_rt_capacity()`）：**

```
cpu_capacity = cpu_capacity_orig × (1 − rt_util − dl_util − irq_util − thermal_pressure) / 1024
```

> **書目**：奔跑吧 §8.2.5「實際算力的計算」式 (8.12)、§8.4.1「量化計算能力」式 (8.23)
> （第 4308~4455 行），書上用 HiKey960 舉例（A53 = 592、A73 = 1024）。
> **修正**：書上說 `arch_scale_max_freq_capacity()` 在 EAS 分支裡「只是簡單地設置為
> SCHED_CAPACITY_SCALE」，主線 6.1 已經在 `topology_normalize_cpu_scale()` 裡
> **正式把最高頻率算進去**了（下面的實測就是靠這一點才對得上）。

### 實機驗證

**(a) 從裝置樹一路算到 `cpu_capacity`**

```bash
grep -n "capacity-dmips-mhz\|dynamic-power-coefficient" \
     arch/arm64/boot/dts/rockchip/rk3588s.dtsi | head -6
```
```
429:  capacity-dmips-mhz = <530>;      ← cpu_l0 (A55)
441:  dynamic-power-coefficient = <100>;
503:  capacity-dmips-mhz = <1024>;     ← cpu_b0 (A76)
515:  dynamic-power-coefficient = <300>;
```

```bash
ssh radxa@192.168.68.57 'for c in 0 4; do echo "cpu$c: dmips_cap=$(cat /sys/devices/system/cpu/cpu$c/cpu_capacity) \
  maxfreq=$(cat /sys/devices/system/cpu/cpufreq/policy$c/cpuinfo_max_freq)"; done'
```
```
cpu0: dmips_cap=422   maxfreq=1800000
cpu4: dmips_cap=1024  maxfreq=2256000
```

**手算對帳：**

```
raw(A55) = 530 dmips/MHz × 1800 MHz =   954 000
raw(A76) = 1024 dmips/MHz × 2256 MHz = 2 310 144   ← capacity_scale（系統最大）

cap(A76) = 2310144 × 1024 / 2310144 = 1024        ✅
cap(A55) =  954000 × 1024 / 2310144 = 422.79 → 422 ✅  (整數截斷)
```

**一個 dmips 值都不差。**（注意：不能只用 530/1024 = 530，要把最高頻率乘進去。）

**(b) 額定算力 vs CFS 額定算力（`sched_group_capacity`）**

用 CPU 熱插拔強迫重建調度域，讓 `sched_domain_debug()` 把 group capacity 印出來：

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
echo 1 > /sys/kernel/debug/sched/verbose
dmesg -C; echo 0 > /sys/devices/system/cpu/cpu7/online; sleep 1
echo 1 > /sys/devices/system/cpu/cpu7/online; sleep 2
dmesg | sed \"s/^\[[^]]*\] //\" | grep -A3 \"CPU0 attaching sched-domain\" | tail -3"'
```
```
CPU0 attaching sched-domain(s):
 domain-0: span=0-6 level=MC
  groups: 0:{ span=0 cap=420 }, 1:{ span=1 cap=422 }, 2:{ span=2 cap=422 },
          3:{ span=3 cap=422 }, 4:{ span=4 cap=1019 }, 5:{ span=5 }, 6:{ span=6 cap=1021 }
root domain span: 0-6 (max cpu_capacity = 1024)
```

| CPU | `cpu_capacity_orig`（額定） | `sched_group_capacity`（CFS 可用） | 差額原因 |
|---|---|---|---|
| cpu0 | 422 | **420** | 這顆在跑中斷/RT，被 `scale_rt_capacity()` 扣掉 2 |
| cpu1~3 | 422 | 422 | 沒有 RT/IRQ 壓力 |
| cpu4 | 1024 | **1019** | 扣了 5 |
| cpu6 | 1024 | **1021** | 扣了 3 |

**這就是 `capacity_orig_of()`（額定）與 `capacity_of()`（CFS 額定）的差別，
在實機上肉眼可見。** `find_busiest_queue()` 用的是後者。

**(c) 實際算力**：見 [Q23](#q23)。

---

<a name="q19"></a>
## 19. 在 PELT 算法中，LOAD_AVG_MAX 宏代表什麼含義？

### 結論

**`LOAD_AVG_MAX` = 在無窮多個週期裡，衰減累計時間的上界。**

```
LOAD_AVG_MAX = 1024 × (y⁰ + y¹ + y² + ...) = 1024 / (1 − y)
             = 1024 / (1 − 0.5^(1/32))
             = 47742
```

它是 PELT 的**分母**，把「累計工作負載（`*_sum`）」歸一化成「量化負載（`*_avg`）」：

```
load_avg = weight × load_sum / LOAD_AVG_MAX      式 (8.16)
```

這樣不管一個行程活了 1 秒還是 3 天，`load_avg` 的值域都是 `[0, weight]`，可以互相比較。

**實作上分母不是常數 47742，而是：**

```c
/* kernel/sched/pelt.h */
static inline u32 get_pelt_divider(struct sched_avg *avg)
{
	return PELT_MIN_DIVIDER + avg->period_contrib;   /* PELT_MIN_DIVIDER = LOAD_AVG_MAX - 1024 */
}
```

也就是 `46718 + period_contrib`，範圍 `[46718, 47741]`。
減 1024 再加當前零頭，是因為最後那個「還沒滿的週期」只該按實際長度計入分母
（否則剛開始的行程 `*_avg` 會被低估）。

**這個常數是怎麼產生的？** 核心提供了產生器：
`Documentation/scheduler/sched-pelt.c`

```c
void calc_converged_max(void)
{
	int n = -1;
	long max = 1024, y_inv = ((1UL << 32) - 1) * y;
	for (;; n++) {
		if (n > -1) max = ((max * y_inv) >> SHIFT) + 1024;
		if (last == max) break;
		last = max;
	}
	printf("#define LOAD_AVG_MAX %ld\n", max);
}
```

> **書目**：奔跑吧 §8.2.7 第 4 小節「LOAD_AVG_MAX 宏」（第 2740 行附近）與式 (8.16)。

### 實機驗證

`sched_probe.ko` 直接用核心的 `runnable_avg_yN_inv[1]` 跑同一個迭代：

```c
sum = 1024;
for (n = 0; n < 1000; n++) {
	u64 next = mul_u64_u32_shr(sum, my_yN_inv[1], 32) + 1024;
	if (next == sum) break;
	sum = next;
}
```

```
   收斂的 sum 1024*y^n = 47742（346 次迭代收斂），核心 LOAD_AVG_MAX = 47742
   PELT 分母 divider = LOAD_AVG_MAX - 1024 + period_contrib = 46718 ~ 47741
```

**算出 47742，與 `kernel/sched/sched-pelt.h:14` 的 `#define LOAD_AVG_MAX 47742` 一模一樣。**

驗證分母確實在用：一個真實行程的 `/proc/pid/sched`

```bash
ssh radxa@192.168.68.57 'cd /tmp && (./pelt_duty 3 6 time 2 8 >/dev/null & sleep 3; \
                                     grep -E "avg\.(load|util|runnable)_(sum|avg)|period" /proc/$!/sched; wait)'
```
```
se.avg.load_sum      :     1481
se.avg.runnable_sum  :  1521199
se.avg.util_sum      :  1518493
se.avg.load_avg      :       32
se.avg.runnable_avg  :       32
se.avg.util_avg      :       32
```

驗算 `util_avg`：`util_sum / divider = 1518493 / 46718 ≈ 32.5 → 32` ✅
（`util_sum` 有 `<< SCHED_CAPACITY_SHIFT`，所以量級是 `1024 × 46718 × duty`。）
驗算 `load_avg`：`load × load_sum / divider = 1024 × 1481 / 46718 ≈ 32.5 → 32` ✅
（task 的 `load_sum` 不乘 weight，`load` 參數才是 `scale_load_down(weight)=1024`。）

---

<a name="q20"></a>
## 20. 在 PELT 算法中，如何計算第 n 個週期的衰減？

### 結論

**查表 + 移位，永遠不做浮點也不做除法。**

```c
/* kernel/sched/pelt.c */
#define LOAD_AVG_PERIOD 32
static u64 decay_load(u64 val, u64 n)
{
	unsigned int local_n;

	if (unlikely(n > LOAD_AVG_PERIOD * 63))     /* ① n > 2016 → 值已小到可忽略 */
		return 0;

	local_n = n;
	if (unlikely(local_n >= LOAD_AVG_PERIOD)) { /* ② 每 32 個週期就是折半 */
		val >>= local_n / LOAD_AVG_PERIOD;      /*    y^32 = 0.5 → 右移 */
		local_n %= LOAD_AVG_PERIOD;
	}
	/* ③ 剩下 0~31 查表；表值是 y^n × 2^32，乘完右移 32 */
	val = mul_u64_u32_shr(val, runnable_avg_yN_inv[local_n], 32);
	return val;
}
```

三段式的巧思：
1. **`n > 2016` 直接回 0** —— `0.5^63 ≈ 1.1e-19`，再算下去也是 0，還能防溢位。
2. **整除的部分用右移** —— 因為 `y^32` 剛好被設計成 0.5，`y^(32k) = 2^-k`。
   這也是為什麼半衰期選 32 而不是別的數。
3. **餘數查 32 項的表** —— `runnable_avg_yN_inv[n] = round(y^n × 2^32)`，
   `mul_u64_u32_shr(a, b, 32)` 做 64×32→高 64 位，一條 `umulh` 就完成。

至於「連續 p 個週期的總和」，6.1 用 `__accumulate_pelt_segments()` 的 c2 技巧
（見 [Q17](#q17)），**不再有書上的 `runnable_avg_yN_sum[]` 表**。

> **書目**：奔跑吧 §8.2.7 第 2 小節「計算第 n 個週期的衰減值」式 (8.13)(8.14)
> （第 2660~2700 行）。

### 實機驗證

`sched_probe.ko` 內建同一份 `runnable_avg_yN_inv[]` 與 `decay_load()`：

```
   y = 0.5^(1/32)；y^32 應為 0.5：decay_load(1024, 32) = 511 （= 1024/2）
   decay_load(1024, 1)=1002  (10)=824  (32)=511  (64)=255  (2016)=0  (2017)=0
```

| n | 走哪條路 | 手算 `1024·y^n` | 模組實測 |
|---|---|---|---|
| 1 | ③ 查表 `yN_inv[1]=0xfa83b2da` | 1024 × 0.97857 = 1002.1 | **1002** |
| 10 | ③ 查表 | 1024 × 0.80430 = 823.6 | **824** |
| 32 | ② 右移 1 + ③ 查表[0] | 512.0 | **511** |
| 64 | ② 右移 2 + ③ 查表[0] | 256.0 | **255** |
| 2016 | ① 邊界內 (32×63) | 1024 × 2^-63 ≈ 0 | **0** |
| 2017 | ① `n > 2016` 直接 return 0 | — | **0** |

書上引用的 `runnable_avg_yN_org[] = {0.999, 0.978, 0.957, ...}` 也對得上：
`yN_inv[1]/2^32 = 0.97857` → 書上寫 0.978 ✅；
`yN_inv[2]/2^32 = 0.95760` → 書上寫 0.957 ✅。
（書上第一項寫 0.999 應為 1.000，`yN_inv[0] = 0xffffffff`。）

---

<a name="q21"></a>
## 21. 在 PELT 算法中，如何計算一個進程的可運行狀態的量化負載 load_avg？

### 結論

```
                  weight × Σ(runnable 時間的衰減值)      weight × load_sum
se->avg.load_avg = ─────────────────────────────────  =  ─────────────────
                            LOAD_AVG_MAX                      divider
```

具體到程式碼（`kernel/sched/pelt.c`，`__update_load_avg_se()`）：

```c
int __update_load_avg_se(u64 now, struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (___update_load_sum(now, &se->avg,
			       !!se->on_rq,                 /* load     = 在就緒佇列上就是 1 */
			       se_runnable(se),             /* runnable = 對 task 也是 0/1 */
			       cfs_rq->curr == se)) {       /* running  = 是不是正在跑 */
		___update_load_avg(&se->avg, se_weight(se));   /* ← 這裡才乘權重 */
		...
	}
}
```

**三個關鍵細節（很容易在面試被追問）：**

1. **對 task 來說 `load_sum` 只累計「時間」，不乘權重**——
   權重是在 `___update_load_avg(&se->avg, se_weight(se))` 這一步才乘進去的。
   對 **cfs_rq** 就不一樣了，`cfs_rq->avg.load_sum` 累計的是「時間 × 權重」（見 [Q22](#q22)）。
2. **`load` 參數是 `!!se->on_rq`**——只要在就緒佇列上（不管是在跑還是在等）就算數。
   所以一個一直排隊等不到 CPU 的行程，`load_avg` 照樣逼近 weight（[Q8](#q8) 的實測）。
3. **改權重要重算**：`reweight_entity()` 會把 `load_sum` 保留、
   重新用新 weight 算 `load_avg`，不會把歷史砍掉。

`se_weight(se)` = `scale_load_down(se->load.weight)` = `se->load.weight >> 10`，
所以 nice 0 的行程 `load_avg` 上界是 **1024**，不是 1048576。

> **書目**：奔跑吧 §8.2.4「量化負載的計算」式 (8.11)、§8.2.7 第 6 小節「量化負載的計算」
> 與表 8.8（第 2860~2954 行）。

### 實機驗證

**(a) 100% runnable → `load_avg` = weight（scale down 後）**

```bash
ssh radxa@192.168.68.57 'cd /tmp && ./sched_weight 3 10 0 5'
```
```
pid    nice   weight    CPU%      Δvrt/Δex load_avg util_avg
94444  0      1048576   75.28     1.0000    1023     761
94445  5      343040    24.72     3.0567    334      278
```

| nice | `se.load.weight` | `scale_load_down` | **實測 `load_avg`** |
|---|---|---|---|
| 0 | 1048576 | 1024 | **1023** ✅ |
| 5 | 343040 | 335 | **334** ✅ |

**兩個行程都是 100% runnable（一直在就緒佇列上），
所以 `load_avg` 精準地收斂到各自的 `scale_load_down(weight)`。**
注意 nice 5 的行程只拿到 24.72% 的 CPU，但 `load_avg` 仍是滿的 334 ——
再次印證「`load_avg` 算的是 runnable 不是 running」。

**(b) 部分 runnable → 按比例縮小**

```bash
ssh radxa@192.168.68.57 'cd /tmp; for d in 1 2 4 6 8; do
  printf "duty %d/8: " $d; ./pelt_duty 4 8 time $d 8 | tail -1; done'
```
```
duty 1/8: load_avg=115   util_avg=115
duty 2/8: load_avg=272   util_avg=244
duty 4/8: load_avg=496   util_avg=496
duty 6/8: load_avg=785   util_avg=785
duty 8/8: load_avg=1023  util_avg=1023
```

`load_avg ≈ 1024 × duty`（1/8→128、2/8→256、4/8→512、6/8→768、8/8→1024），
實測 115/272/496/785/1023，**誤差來自鋸齒取樣點**（見 [Q15](#q15)）。

---

<a name="q22"></a>
## 22. 在 PELT 算法中，如何計算一個調度隊列的可運行狀態的量化負載 runnable_load_avg？

### 結論

> ⚠️ **這一題的答案在 Linux 5.7 之後整個變了。**
> `runnable_load_avg` / `runnable_load_sum` **已經被刪除**
> （commit `9f68395333ad` "sched/pelt: Add a new runnable average signal"），
> 取而代之的是語意完全不同的 `runnable_avg` / `runnable_sum`。

**書上（5.0）的答案：**

```
cfs_rq->avg.runnable_load_avg = Σ (佇列上每個 se 的 load_avg)
```
用途：`find_busiest_queue()` / `wake_affine()` 拿它當「這顆 CPU 有多忙」。
與 `load_avg` 的差別是 `load_avg` 還包含已經睡著的行程留下的 **blocked load**，
而 `runnable_load_avg` 只算還在佇列上的。

**本機（6.1）的答案：**

| 欄位 | 對 **task**（se） | 對 **cfs_rq** |
|---|---|---|
| `load_sum` | runnable 時間的衰減和（不乘 weight） | **Σ(每個 se 的 load_avg 貢獻)**，即「時間 × 權重」 |
| `load_avg` | `weight × load_sum / divider` | 佇列總量化負載（含 blocked load，透過 `removed.load_avg` 延遲移除） |
| `runnable_sum` | 與 `load_sum` 同（乘了 `<< SCHED_CAPACITY_SHIFT`） | 累計「**runnable 的行程數量**」的衰減和 |
| `runnable_avg` | = `load_avg / weight × 1024`，即「runnable 佔比 × 1024」 | **≈ 平均有幾個行程在排隊 × 1024** |

也就是說 **6.1 的 `cfs_rq->avg.runnable_avg` 不再是「加權負載」，
而是「平均排隊長度」**（`se_runnable(se)` 對 group se 回傳 `se->runnable_weight`
= 底下 runnable task 的個數）。它的新用途是
`cpu_runnable()` → `update_sg_lb_stats()` 判斷 `group_overloaded`，
以及 `check_cpu_capacity()`。

**現在誰扮演書上 `runnable_load_avg` 的角色？** 是 `cpu_load()`：

```c
/* kernel/sched/fair.c */
static unsigned long cpu_load(struct rq *rq)      { return cfs_rq_load_avg(&rq->cfs); }
static unsigned long cpu_runnable(struct rq *rq)  { return cfs_rq_runnable_avg(&rq->cfs); }
static unsigned long cpu_util_cfs(int cpu)        { /* util_avg + util_est */ }
```
`load_balance()` 依 `migration_type` 分別使用這三者（見 [Q26](#q26)）。

> **書目**：奔跑吧 §8.2.6 表 8.6/8.8、§8.2.8「PELT 接口函數」
> （`cfs_rq_runnable_load_avg()`，第 2954 行附近）。
> **修正**：本機 `/proc/pid/sched` 與 `/sys/kernel/debug/sched/debug` 都已經
> **沒有 `runnable_load_avg` 這個欄位**。

### 實機驗證

**(a) 欄位名稱已經改了**

```bash
ssh radxa@192.168.68.57 'grep -c runnable_load_avg /proc/self/sched; grep runnable /proc/self/sched'
```
```
0
se.avg.runnable_sum   :  10151738
se.avg.runnable_avg   :       217
```
**`runnable_load_avg` 一個字都找不到。**

```bash
ssh radxa@192.168.68.57 'sudo grep -A14 "^cfs_rq\[3\]:/$" /sys/kernel/debug/sched/debug | \
                         grep -E "load|runnable|util"'
```
```
  .load                          : 0
  .load_avg                      : 0
  .runnable_avg                  : 0
  .util_avg                      : 0
```

**(b) `cfs_rq->avg.runnable_avg` 真的是「平均排隊長度 × 1024」**

在 CPU3 上放 n 個 nice=0 的 busy 行程，讀 root cfs_rq：

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
echo 0 > /proc/sys/kernel/sched_autogroup_enabled; echo \$\$ > /sys/fs/cgroup/cgroup.procs
for n in 1 2 4; do
  for i in \$(seq \$n); do taskset -c 3 sh -c \"while :; do :; done\" & done
  sleep 4
  printf \"nr=%d : \" \$n
  grep -A14 \"^cfs_rq\[3\]:/\\\$\" /sys/kernel/debug/sched/debug | \
      grep -E \"nr_running|\.load_avg|runnable_avg|util_avg\" | tr -s \" \" | tr \"\n\" \" \"
  echo; kill %1 %2 %3 %4 2>/dev/null; sleep 2
done
echo 1 > /proc/sys/kernel/sched_autogroup_enabled"' 2>/dev/null
```

```
nr=1 : .nr_running=1 .load=1048576 .load_avg=1024 .runnable_avg=1024 .util_avg=1024
nr=2 : .nr_running=2 .load=2097152 .load_avg=2048 .runnable_avg=2048 .util_avg=1024
nr=4 : .nr_running=4 .load=4194304 .load_avg=4097 .runnable_avg=4097 .util_avg=1024
```

| CPU3 上的 nice=0 行程數 | `.load`（總權重） | `cfs_rq.load_avg` | `cfs_rq.runnable_avg` | `cfs_rq.util_avg` |
|---|---|---|---|---|
| 1 | 1048576 | **1024** | **1024** | 1024 |
| 2 | 2097152 | **2048** | **2048** | 1024（**飽和**） |
| 4 | 4194304 | **4097** | **4097** | 1024（**飽和**） |

**`load_avg` / `runnable_avg` 隨行程數線性成長（可以無上限），
但 `util_avg` 在 1024 就飽和了** —— 這就是為什麼負載均衡要看前兩者、
CPU 調頻（schedutil）要看後者。

---

<a name="q23"></a>
## 23. 在 PELT 算法中，如何計算一個進程的實際算力 util_avg？

### 結論

```
                  1024 × Σ(running 時間的衰減值)       util_sum
se->avg.util_avg = ──────────────────────────────  =  ──────────
                          LOAD_AVG_MAX                  divider
```

與 [Q21](#q21) 的 `load_avg` 只差三點：

| | `load_avg` | `util_avg` |
|---|---|---|
| 統計的時間 | **runnable**（在佇列上，含等待） | **running**（`cfs_rq->curr == se`） |
| 乘不乘權重 | 乘 `se_weight(se)` | **不乘**，固定用 1024 |
| 值域 | `[0, scale_load_down(weight)]` | `[0, 1024]`（`SCHED_CAPACITY_SCALE`） |

`util_sum` 累加時就已經 `<< SCHED_CAPACITY_SHIFT`：

```c
if (running) sa->util_sum += contrib << SCHED_CAPACITY_SHIFT;
...
WRITE_ONCE(sa->util_avg, sa->util_sum / divider);
```

**兩個不變性**（見 [Q17](#q17)）讓 `util_avg` 跨頻率、跨大小核可比：
`clock_pelt` 的推進速率 = 牆鐘 × `freq_scale` × `cpu_scale`。

**`util_est`（5.1 新增，本機 `UTIL_EST` 開著）**：
PELT 對「睡一下又醒來」的行程會低估，所以核心額外記住
「上次 dequeue 時的 util」（`util_est.enqueued`）和它的 EWMA，
`cpu_util_cfs()` 取 `max(util_avg, util_est)`。書上完全沒提這個。

> **書目**：奔跑吧 §8.2.5「實際算力的計算」式 (8.12)、§8.2.8
> （`task_util()` / `cpu_util()`，第 2954~3005 行）。

### 實機驗證

**(a) `util_avg` 只算 running：同一個行程，佇列上人越多 util 越低**

（[Q8](#q8) 的表再看一次，這次看 `util_avg` 那一欄）

| CPU3 上行程數 | 實際 CPU 佔比 | `load_avg` | **`util_avg`** | 理論 1024/n |
|---|---|---|---|---|
| 1 | 100% | 1024 | **1024** | 1024 |
| 2 | 50% | 1023 | **483** | 512 |
| 4 | 25% | 1023 | **278** | 256 |
| 8 | 12.5% | 1023 | **115** | 128 |
| 16 | 6.26% | 1024 | **78** | 64 |

**(b) 算力不變性：同樣 50% duty，`util_avg` 比值 = 算力比值**

```
cpu3 (A55, cap=422) : util_avg=200
cpu4 (A76, cap=1024): util_avg=496
200/496 = 0.403      422/1024 = 0.412       誤差 2%
```

**(c) `util_est` 也能看到**

```bash
ssh radxa@192.168.68.57 'cd /tmp && (./pelt_duty 3 6 time 2 8 >/dev/null & sleep 3; \
                                     grep util /proc/$!/sched; wait)'
```
```
se.avg.util_sum            :  1518493
se.avg.util_avg            :       32
se.avg.util_est.ewma       :       35
se.avg.util_est.enqueued   :       27
```
`util_est.ewma = 35 > util_avg = 32` —— 對週期性行程，`util_est` 記住了它
「醒著時的峰值需求」，`cpu_util_cfs()` 會取較大者，避免降頻太快。

**(d) 一個誠實的反例：固定「工作量」不一定得到相同 util**

```bash
ssh radxa@192.168.68.57 'cd /tmp; for c in 3 4; do printf "cpu%d: " $c; \
    ./pelt_duty $c 10 work 12 40 | tail -1; done'
```
```
cpu3 (A55): util_avg=260   （12M 次迴圈花 ~33 ms）
cpu4 (A76): util_avg=670   （12M 次迴圈花 ~29.5 ms）
```

理論上「固定工作量」在兩種核上應該得到相同的 `util_avg`，實測卻差了 2.6 倍。
**原因：CIE 用的是裝置樹寫死的 `capacity-dmips-mhz`（530 vs 1024，比值 0.52），
但這支測試迴圈是 store-bound 的，A55 實際只慢了 1.12 倍。**
算力不變性是**靜態標定**，不是動態量測 IPC —— 這是 EAS 在真實工作負載上
會失準的根源之一，面試時提出來是加分項。

---

<a name="q24"></a>
## 24. 一個 4 核處理器，每個 CPU 有獨立 L1、不支援超線程，4 個 CPU 分成兩個簇，簇內共享 L2。請畫出調度域和調度組的拓撲關係。

### 結論（書上題目的標準答案）

```
                      ┌──────────────────────────────────────────┐
   DIE 層級            │  domain_die  span = {0,1,2,3}            │
   （整顆 SoC）        │  flags: SD_BALANCE_NEWIDLE|EXEC|FORK|    │
                      │         SD_WAKE_AFFINE|SD_PREFER_SIBLING │
                      │  groups:  ┌────────┐   ┌────────┐        │
                      │           │ grp{0,1}│──▶│grp{2,3}│──┐     │
                      │           └────────┘   └────────┘  │     │
                      │                ▲                    │     │
                      │                └────────────────────┘     │
                      └──────────────────────────────────────────┘
                            ▲                        ▲
              parent        │                        │        parent
                      ┌─────┴──────┐          ┌──────┴─────┐
   MC 層級            │ domain_mc  │          │ domain_mc  │
   （共享 L2 的簇）    │ span={0,1} │          │ span={2,3} │
                      │ +SD_SHARE_ │          │ +SD_SHARE_ │
                      │  PKG_RES   │          │  PKG_RES   │
                      │ groups:    │          │ groups:    │
                      │  g{0}→g{1} │          │  g{2}→g{3} │
                      └────────────┘          └────────────┘
                        cluster0                 cluster1
                      CPU0    CPU1             CPU2    CPU3
```

規則：
* **調度域（`sched_domain`）是 per-CPU 的**（`rq->sd` 是一條由下往上的鏈）。
  CPU0 看到的 `domain_mc` 和 CPU1 看到的是**兩個不同的物件**，只是 span 一樣。
* **調度組（`sched_group`）串成環狀鏈表**，`sd->groups` 指向「本地組」（包含自己的那個）。
  所以 CPU0 的 MC 域 groups 順序是 `{0} → {1} → 回到 {0}`；CPU1 是 `{1} → {0}`。
* **下層域的 span = 上層域某一個 group 的 span**。
* 沒有 SMT → 不建 SMT 層級；`CONFIG_SCHED_MC` 提供 MC 層級；DIE 是標配。
* **`SD_LOAD_BALANCE` 這個 flag 在 5.9 已經被刪掉**（現在用 `sd->flags` 是否為 0 判斷）。

`default_topology[]`（`kernel/sched/topology.c`）：

```c
static struct sched_domain_topology_level default_topology[] = {
#ifdef CONFIG_SCHED_SMT
	{ cpu_smt_mask,       cpu_smt_flags,     SD_INIT_NAME(SMT) },
#endif
#ifdef CONFIG_SCHED_CLUSTER
	{ cpu_clustergroup_mask, cpu_cluster_flags, SD_INIT_NAME(CLS) },
#endif
#ifdef CONFIG_SCHED_MC
	{ cpu_coregroup_mask, cpu_core_flags,    SD_INIT_NAME(MC)  },
#endif
	{ cpu_cpu_mask,       SD_INIT_NAME(DIE) },
	{ NULL, },
};
```

### 實機驗證 —— RK3588 的真實拓撲和書上的例子**不一樣**

RK3588 明明是 4×A55（共享 L3）+ 2×A76 + 2×A76 的三簇架構，
但 **Linux 只建了一層 MC 域，涵蓋全部 8 顆 CPU，底下 8 個單 CPU 的調度組**：

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
echo 1 > /sys/kernel/debug/sched/verbose
dmesg -C
echo 0 > /sys/devices/system/cpu/cpu7/online; sleep 1
echo 1 > /sys/devices/system/cpu/cpu7/online; sleep 2
dmesg | sed \"s/^\[[^]]*\] //\" | grep -A3 \"CPU0 attaching sched-domain\" | tail -3
dmesg | sed \"s/^\[[^]]*\] //\" | grep \"root domain span\" | tail -1"'
```

```
CPU0 attaching sched-domain(s):
 domain-0: span=0-7 level=MC
  groups: 0:{ span=0 cap=421 }, 1:{ span=1 cap=422 }, 2:{ span=2 cap=422 }, 3:{ span=3 cap=422 },
          4:{ span=4 }, 5:{ span=5 }, 6:{ span=6 }, 7:{ span=7 }
root domain span: 0-7 (max cpu_capacity = 1024)
```

```bash
ssh radxa@192.168.68.57 'sudo bash -c "for d in /sys/kernel/debug/sched/domains/cpu0/domain*; do
  echo \"\$(cat \$d/name)  imb_pct=\$(cat \$d/imbalance_pct) min=\$(cat \$d/min_interval) \
max=\$(cat \$d/max_interval) busy_factor=\$(cat \$d/busy_factor)\"
  echo \"  flags: \$(cat \$d/flags | tr \"\n\" \" \")\"; done"'
```
```
MC  imb_pct=117 min=8 max=16 busy_factor=16
  flags: SD_BALANCE_NEWIDLE SD_BALANCE_EXEC SD_BALANCE_FORK SD_WAKE_AFFINE
         SD_ASYM_CPUCAPACITY SD_ASYM_CPUCAPACITY_FULL SD_SHARE_PKG_RESOURCES SD_PREFER_SIBLING
```

**RK3588 的實際拓撲圖：**

```
   root_domain  span = 0-7   max_cpu_capacity = 1024
        │       perf domains: pd0{0-3}  pd4{4-5}  pd6{6-7}   ← EAS 用的，跟調度域無關
        ▼
   ┌──────────────────────────────────────────────────────────────────────┐
   │ domain-0  name=MC  span=0-7  imbalance_pct=117                       │
   │ flags: BALANCE_NEWIDLE | BALANCE_EXEC | BALANCE_FORK | WAKE_AFFINE   │
   │        | ASYM_CPUCAPACITY(_FULL) | SHARE_PKG_RESOURCES | PREFER_SIBLING│
   │                                                                       │
   │  g{0}→g{1}→g{2}→g{3}→g{4}→g{5}→g{6}→g{7}→(回到 g{0})                 │
   │  cap 421  422   422   422   1024  1024  1024  1024                    │
   └──────────────────────────────────────────────────────────────────────┘
        A55  A55  A55  A55       A76  A76  A76  A76
       ←──── policy0 (1.8G) ────→ ←policy4→ ←policy6→ (2.256G)
```

**為什麼只有一層？**

| 層級 | 建立條件 | RK3588 |
|---|---|---|
| SMT | `CONFIG_SCHED_SMT` | ❌ 未編譯（ARM 沒有 SMT） |
| CLS | `CONFIG_SCHED_CLUSTER` | ❌ **未編譯**（否則會依 `cluster_cpus_list` 建出 {0-3}{4-5}{6-7} 三個 CLS 域） |
| MC | `CONFIG_SCHED_MC` + `cpu_coregroup_mask()` | ✅ span = 0-7 |
| DIE | 標配，`cpu_cpu_mask()` = 同一個 NUMA node | ✅ span = 0-7 → **與 MC 完全相同，被 `sd_degenerate()` 合併掉** |

`cpu_coregroup_mask()` 回傳 LLC（last level cache）共享範圍；
RK3588 的 8 顆核**共享 3 MB L3**，所以 MC span 就是 0-7，和 DIE 一樣大，
`build_sched_domains()` 會把重複的那層 degenerate 掉。

**這個拓撲的三個實際後果：**
1. `sd_llc_size = 8` → `select_idle_sibling()` 可以掃全部 8 顆 CPU 找閒置核，
   **小任務有機會被丟到大核上**（見 [Q28](#q28)、[Q30](#q30) 的對照實驗）。
2. `wake_wide()` 的 `factor = 8`，很難觸發（見 [Q28](#q28)）。
3. `SD_ASYM_CPUCAPACITY_FULL` 出現在這一層 → 是 EAS 可以啟用的必要條件之一（[Q30](#q30)）。

`imbalance_pct = 117`（不是書上說的 125）：因為這是帶
`SD_SHARE_PKG_RESOURCES` 的域，`sd_init()` 裡會 `sd->imbalance_pct = 117`。

```bash
grep -n -B2 -A4 "imbalance_pct.*117" kernel/sched/topology.c
```

---

<a name="q25"></a>
## 25. 假設 CPU0 和 CPU1 屬於同一個調度域且它們都不是空閒的 CPU，那麼 CPU1 可以做負載均衡嗎？

### 結論

**通常不行——除非 CPU1 是這個調度組裡的「第一顆 CPU」。**

這是 `should_we_balance()`（`kernel/sched/fair.c:10477`）的規則：

```c
static int should_we_balance(struct lb_env *env)
{
	struct sched_group *sg = env->sd->groups;
	int cpu, idle_smt = -1;

	/* ① newidle balance：只有「自己真的閒下來」才做 */
	if (!cpumask_test_cpu(env->dst_cpu, env->cpus))
		return 0;
	if (env->idle == CPU_NEWLY_IDLE) {
		if (env->dst_rq->nr_running > 0 || env->dst_rq->ttwu_pending)
			return 0;
		return 1;
	}

	/* ② 週期性 balance：優先找本組裡的「空閒 CPU」 */
	for_each_cpu_and(cpu, group_balance_mask(sg), env->cpus) {
		if (!idle_cpu(cpu))
			continue;
		...
		return cpu == env->dst_cpu;      /* 只有第一顆空閒 CPU 有資格 */
	}
	if (idle_smt == env->dst_cpu)
		return true;

	/* ③ 全都不空閒 → 只有本組的第一顆 CPU 有資格 */
	return group_balance_cpu(sg) == env->dst_cpu;
}
```

**為什麼要限制？** 防止「驚群」：如果調度域裡 8 顆 CPU 同時進 `load_balance()`，
會同時搶 `busiest->lock`、同時把同一批行程往自己身上搬，
互相打架又浪費 CPU。所以規定：

| 情況 | 誰有資格做均衡 |
|---|---|
| 本組有空閒 CPU | **第一顆空閒的** CPU |
| 本組全都在忙 | **`group_balance_cpu(sg)`**（本組第一顆 CPU） |
| CPU 剛變成 idle | **它自己**（`newidle_balance()`，這條路徑不受上面限制） |

回到題目：CPU0 和 CPU1 在同一個域，兩顆都不空閒。
在**本機的拓撲下每個調度組只有一顆 CPU**（[Q24](#q24)），
所以 `group_balance_cpu(sg{1}) == 1`，**CPU1 是可以做的**。
但在書上的例子（`grp{0,1}` 一組）裡，`group_balance_cpu = 0`，
**CPU1 就不能做，要等 CPU0 來做**。

還有兩個關卡：
* **時間**：`rebalance_domains()` 只在 `time_after_eq(jiffies, sd->last_balance + interval)`
  時才對該層做均衡。`interval = min_interval × busy_factor`（忙時），
  本機 MC 域是 `8 × 16 = 128 jiffies`，HZ=300 → **約 427 ms**。
* **`continue_balancing`**：`should_we_balance()` 回 0 時會把
  `*continue_balancing = 0`，`rebalance_domains()` 直接 `break`，**連上層域都不做了**。

> **書目**：奔跑吧 §8.3.4 第 3 小節「should_we_balance() 函數」（第 3690 行附近）——
> 「允許做負載均衡的首要條件是當前 CPU 是該調度域中第一個 CPU，或者當前 CPU 是空閒 CPU」。

### 實機驗證

**(a) 兩顆都忙的時候，誰在做均衡？**

用 ftrace 抓 `sched_migrate_task`，看**執行遷移的那個行程跑在哪顆 CPU 上**
（trace 第 2 欄的 `[00N]`）：

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
T=/sys/kernel/debug/tracing
echo 0 > \$T/events/enable; echo > \$T/trace
echo 1 > \$T/events/sched/sched_migrate_task/enable
echo \"comm ~ \\\"lb_case\\\"\" > \$T/events/sched/sched_migrate_task/filter
echo 1 > \$T/tracing_on
su radxa -c \"/tmp/lb_case 5 3 1\" > /dev/null
echo 0 > \$T/tracing_on
grep sched_migrate \$T/trace"'
```

```
migration/5-38   [005] d..1. 156455.159567: sched_migrate_task: comm=lb_case pid=101211 orig_cpu=5 dest_cpu=0
migration/5-38   [005] d..1. 156455.159702: sched_migrate_task: comm=lb_case pid=101212 orig_cpu=5 dest_cpu=0
migration/5-38   [005] d..1. 156455.159857: sched_migrate_task: comm=lb_case pid=101213 orig_cpu=5 dest_cpu=0
migration/5-38   [005] d..1. 156455.160002: sched_migrate_task: comm=lb_case pid=101214 orig_cpu=5 dest_cpu=0
migration/5-38   [005] d..1. 156455.160136: sched_migrate_task: comm=lb_case pid=101215 orig_cpu=5 dest_cpu=0
send process-966 [001] d.... 156455.630449: sched_migrate_task: comm=lb_case pid=101212 orig_cpu=0 dest_cpu=1
send process-966 [001] d.... 156455.630456: sched_migrate_task: comm=lb_case pid=101213 orig_cpu=0 dest_cpu=1
```

兩種完全不同的遷移：

| 時間 | 誰執行的 | 在哪顆 CPU | 機制 |
|---|---|---|---|
| 159.5~160.1 ms | **`migration/5`** kthread | CPU5 | `sched_setaffinity()` → `stop_one_cpu()`，走 **stop_sched_class**（最高優先級調度類，書上 §8.1.5 提到它「用於行程遷移、CPU 熱插拔」） |
| 630.4 ms | **`send process`**（一個普通行程） | **CPU1** | 它在 CPU1 上被排到 → `newidle_balance()` / `schedule()` → `load_balance()` → **把 CPU0 上的 2 個行程「拉」過來** |

**第二種正是本題的答案**：遷移是由**目的地 CPU（dst_cpu = 1）**自己執行的
（`detach_tasks()` / `attach_tasks()` 都跑在 dst_cpu 上，這叫 **pull migration**），
而且它是在自己被排程到的時候順手做的，不需要 CPU0 配合。

**(b) 本機每組只有一顆 CPU，所以每顆 CPU 都是 `group_balance_cpu`**

```bash
ssh radxa@192.168.68.57 'sudo dmesg | grep -A3 "CPU1 attaching sched-domain" | tail -2'
```
```
 domain-0: span=0-7 level=MC
  groups: 1:{ span=1 cap=422 }, 2:{ span=2 ... }, ... , 0:{ span=0 cap=421 }
```
CPU1 的 `sd->groups` 第一個就是 `g{1}`（本地組），`group_balance_cpu(g{1}) = 1`
→ **CPU1 永遠有資格做均衡**。這是「每組一顆 CPU」拓撲的直接後果。

---

<a name="q26"></a>
## 26. 如何查找出一個調度域裡最繁忙的調度組？

### 結論

`find_busiest_group()`（`kernel/sched/fair.c:10147`）。
**但 6.1 的實作已經和書上（5.0）完全不同了**——5.7 的
commit `0b0695f2b34a` "sched/fair: Rework load_balance()" 把它整個重寫。

**流程（6.1）：**

```
find_busiest_group(env)
 ├─ update_sd_lb_stats(env, &sds)              ← 掃描所有調度組
 │   └─ 對每個 group：update_sg_lb_stats()
 │        ├─ 累加 group_load / group_util / group_runnable / sum_nr_running / idle_cpus
 │        ├─ 標記 sg_status |= SG_OVERLOAD / SG_OVERUTILIZED
 │        └─ sgs->group_type = group_classify(imbalance_pct, group, sgs)
 │      再用 update_sd_pick_busiest() 挑出「最繁忙」的
 ├─ (EAS) if (rd->pd && !rd->overutilized) goto out_balanced;   ← 見 Q31
 ├─ 一連串 out_balanced 的早退判斷
 └─ calculate_imbalance(env, &sds)             ← 見 Q27
```

**核心是 `group_type` 這個列舉（由高到低，數字越大越該被搬）：**

```c
enum group_type {
	group_has_spare = 0,   /* 還有餘力，可以再收人           */
	group_fully_busy,      /* 剛好用滿，沒有 overload        */
	group_misfit_task,     /* 有大任務卡在小核上（big.LITTLE）*/
	group_asym_packing,    /* 需要往「偏好的」CPU 集中        */
	group_imbalanced,      /* 因為 affinity 導致無法均衡      */
	group_overloaded,      /* 超載                           */
};
```

`update_sd_pick_busiest()` 的比較順序：
1. 先比 `group_type`（大的贏）；
2. 同型時再依型別各自比較：
   * `group_overloaded` → 比 **`avg_load = group_load × SCHED_CAPACITY_SCALE / group_capacity`**（這才是書上講的那條路徑）
   * `group_misfit_task` → 比 `group_misfit_task_load`
   * `group_has_spare` → 比 **閒置 CPU 數量**（少的比較忙）
3. **big.LITTLE 特別處理**：`sds->local` 是小核而 busiest 是大核時，
   不把大核當 busiest（避免把任務從大核搬到小核）。

`group_classify()`：

```c
static inline enum group_type
group_classify(unsigned int imbalance_pct, struct sched_group *group, struct sg_lb_stats *sgs)
{
	if (group_is_overloaded(imbalance_pct, sgs))       return group_overloaded;
	if (sg_imbalanced(group))                          return group_imbalanced;
	if (sgs->group_asym_packing)                       return group_asym_packing;
	if (sgs->group_misfit_task_load)                   return group_misfit_task;
	if (!group_has_capacity(imbalance_pct, sgs))       return group_fully_busy;
	return group_has_spare;
}

static inline bool group_is_overloaded(unsigned int imbalance_pct, struct sg_lb_stats *sgs)
{
	if (sgs->sum_nr_running <= sgs->group_weight)  return false;
	if ((sgs->group_capacity * 100) < (sgs->group_util * imbalance_pct))       return true;
	if ((sgs->group_capacity * imbalance_pct) < (sgs->group_runnable * 100))   return true;
	return false;
}
```

**`imbalance_pct = 117` 就是在這裡用的**：
「`group_util > group_capacity × 100/117 ≈ 85%`」才算超載。

找到最繁忙的**組**之後，再用 `find_busiest_queue()` 在組內找最繁忙的**佇列**
（本機每組只有一顆 CPU，這步是 trivial 的）。

> **書目**：奔跑吧 §8.3.4 第 4~6、8 小節（第 3720~3900 行）。
> **修正**：書上的 `sg_lb_stats` 有 `sum_weighted_load`、`load_per_task`、
> `group_no_capacity`，6.1 已改成 `group_util` / `group_runnable` / `group_type`；
> 書上說的 `get_sd_load_idx()` / `target_load()` / `source_load()` / `cpu_load[]`
> **在 5.7 全部刪除**。

### 實機驗證

**(a) 6.1 的資料結構長什麼樣**

```bash
grep -n -A24 "^struct sg_lb_stats {" kernel/sched/fair.c
```
```c
struct sg_lb_stats {
	unsigned long avg_load;              /* 組內每 capacity 的平均負載 */
	unsigned long group_load;            /* 組內 cpu_load() 總和 */
	unsigned long group_capacity;
	unsigned long group_util;            /* 組內 cpu_util() 總和   ← 新 */
	unsigned long group_runnable;        /* 組內 cpu_runnable() 總和 ← 新 */
	unsigned int sum_nr_running;
	unsigned int sum_h_nr_running;
	unsigned int idle_cpus;
	unsigned int group_weight;
	enum group_type group_type;          /* ← 新，取代 group_no_capacity */
	unsigned int group_asym_packing;
	unsigned long group_misfit_task_load;
	...
};
```

**(b) 統計資料的來源就在 `/proc/schedstat`**

```bash
ssh radxa@192.168.68.57 'head -4 /proc/schedstat'
```
```
version 15
timestamp 4341247406
cpu0 0 0 0 0 0 0 3271029109956 490784802512 19236189
domain0 ff 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
```
`domain0 ff` = 只有一個域、span 是 `0xff`（8 顆 CPU），與 [Q24](#q24) 一致。
後面 36 個計數器就是 `sd->lb_count[]` / `lb_balanced[]` / `lb_failed[]` /
`lb_imbalance[]` / `lb_gained[]`… 分成 `CPU_IDLE`/`CPU_NOT_IDLE`/`CPU_NEWLY_IDLE`
三組各 12 個。（本機全 0 是因為當時 `sched_schedstats=0`。）

打開之後就能看到均衡活動：
```bash
ssh radxa@192.168.68.57 'sudo bash -c "echo 1 > /proc/sys/kernel/sched_schedstats
su radxa -c \"/tmp/lb_case 8 ff 3\" >/dev/null
sed -n 4p /proc/schedstat"'
```

**(c) `load_balance` / `find_busiest_group` 都是可追蹤的實體函式**

```bash
ssh radxa@192.168.68.57 'grep -E "^(load_balance|find_busiest_group|should_we_balance|\
update_sd_lb_stats|calculate_imbalance)$" /sys/kernel/debug/tracing/available_filter_functions'
```
```
find_busiest_group
load_balance
```
只有這兩個是獨立的實體函式；
`update_sd_lb_stats` / `update_sg_lb_stats` / `calculate_imbalance` / `should_we_balance`
**都被編譯器 inline 進去了**，所以 ftrace 掛不上，要追它們得用 `perf probe`
搭配 debuginfo，或改看 `/proc/schedstat` 的計數器。

---

<a name="q27"></a>
## 27. 如果一個調度域負載不均衡，請問如何計算需要遷移的負載量呢？

### 結論

`calculate_imbalance()`（`kernel/sched/fair.c:9950`）。
6.1 的答案是**「先決定要搬什麼（`migration_type`），再決定要搬多少（`imbalance`）」**：

```c
enum migration_type {
	migrate_load = 0,   /* 搬「加權負載」，單位是 load  */
	migrate_util,       /* 搬「算力需求」，單位是 util  */
	migrate_task,       /* 搬「任務個數」，單位是 個    */
	migrate_misfit,     /* 搬那一個放不下的大任務       */
};
```

決策表（依 `busiest->group_type`）：

| busiest 的 group_type | `migration_type` | `env->imbalance` = |
|---|---|---|
| `group_misfit_task` | `migrate_misfit` | 1（就搬那一個） |
| `group_imbalanced` | `migrate_task` | 1 |
| `group_asym_packing` | `migrate_task` | busiest 的 `sum_h_nr_running` |
| **`group_has_spare`** + local 也有餘力 | **`migrate_task`** | `(busiest.idle_cpus − local.idle_cpus) / 2`，或用 `nr_running` 差的一半 |
| **`group_overloaded`** / `group_fully_busy` | **`migrate_load`** | `min( (busiest.avg_load − sds.avg_load) × busiest.group_capacity, (sds.avg_load − local.avg_load) × local.group_capacity ) / SCHED_CAPACITY_SCALE` |

`group_overloaded` 那條就是書上式子的現代版：

```
sds.avg_load = (sds.total_load × SCHED_CAPACITY_SCALE) / sds.total_capacity   ← 全域平均
busiest.avg_load = (group_load × SCHED_CAPACITY_SCALE) / group_capacity

imbalance = min(busiest 高出平均的量, local 低於平均的量)
```
**取 min** 是為了「搬完之後兩邊剛好都落在平均線上」，不會搬過頭造成來回震盪。

搬的動作在 `detach_tasks()`（`fair.c:8432`）：

```c
while (!list_empty(tasks)) {
	if (env->idle != CPU_NOT_IDLE && env->src_rq->nr_running <= 1) break;
	if (env->loop > env->loop_max) break;              /* loop_max */
	if (env->loop > env->loop_break) { ... break; }    /* sched_nr_migrate_break = 32 */
	if (!can_migrate_task(p, env)) goto next;
	switch (env->migration_type) {
	case migrate_load:
		load = max_t(unsigned long, task_h_load(p), 1);
		if (sched_feat(LB_MIN) && load < 16 && !env->sd->nr_balance_failed) goto next;
		if (shr_bound(load, env->sd->nr_balance_failed) > env->imbalance) goto next;
		env->imbalance -= load;   break;
	case migrate_util:  env->imbalance -= task_util_est(p); break;
	case migrate_task:  env->imbalance--;                   break;
	case migrate_misfit: env->imbalance = 0;                break;
	}
	detach_task(p, env);
	if (env->imbalance <= 0) break;
}
```

`can_migrate_task()` 的四道關卡：
1. **`p->cpus_ptr` 不允許** → `nr_failed_migrations_affine++`
2. **`task_running(rq, p)`（正在跑）** → `nr_failed_migrations_running++`
3. **cache-hot**：`task_hot()` — 距離上次執行不到
   `sysctl_sched_migration_cost`（本機 **0.5 ms**）就算「熱的」→ `nr_failed_migrations_hot++`
   （但 `nr_balance_failed > cache_nice_tries` 時會強制搬，`nr_forced_migrations++`）
4. **throttled cfs_rq**

> **書目**：奔跑吧 §8.3.4 第 7、9、10 小節（第 3820~3930 行）。
> 書上說 `imbalance_pct` 「默認值為 125」，本機 MC 域是 **117**。

### 實機驗證

**(a) 書上 §9.2 的場景：5 個行程擠在 CPU0，放開到 {CPU0, CPU1}**

```bash
ssh radxa@192.168.68.57 'cd /tmp && ./lb_case 5 3 3'
```
```
t=0.00s  全部釘在 CPU0： p0@c0 p1@c0 p2@c0 p3@c0 p4@c0
t= 0.10s  nr_running/CPU: c0=5 c1=0
t= 0.20s  nr_running/CPU: c0=3 c1=2      ← 100~200 ms 內就均衡完了
t= 0.30s  nr_running/CPU: c0=3 c1=2
...
t= 2.60s  nr_running/CPU: c0=3 c1=2      ← 穩定在 3/2，不再震盪
```

* **收斂到 3/2 而不是 2.5/2.5** —— 5 個行程沒辦法整除，3/2 就是最佳解。
  搬第 3 個過去會變成 2/3，`imbalance` 換邊，反而觸發反向搬運（震盪）。
  `calculate_imbalance()` 的 `min()` 與 `(busiest.nr − local.nr)/2` 的「除以 2」
  就是在避免這件事。
* **不到 200 ms 就完成**，遠快於 MC 域的週期性均衡間隔（≈427 ms）——
  因為 CPU1 當時是 idle，走的是 **`newidle_balance()`** 這條快路徑。

**(b) 8 個行程 / 8 顆 CPU：一顆一個**

```bash
ssh radxa@192.168.68.57 'cd /tmp && ./lb_case 8 ff 3 | head -4'
```
```
t=0.00s  全部釘在 CPU0： p0@c0 p1@c0 ... p7@c0
t= 0.10s  nr_running/CPU: c0=1 c1=1 c2=1 c3=1 c4=1 c5=1 c6=1 c7=1
t= 0.20s  nr_running/CPU: c0=1 c1=1 c2=1 c3=1 c4=1 c5=1 c6=1 c7=1
```
**100 ms 內從 8/0/0/0/0/0/0/0 攤成 1/1/1/1/1/1/1/1。**
這是 `migrate_task` 路徑（`group_has_spare`，其他 7 顆都 idle），
`imbalance = (idle_cpus 差) / 2`，加上每顆 idle CPU 各自 `newidle_balance()`，
一輪就搬完。

**(c) `nr_migrate = 32` 與 `migration_cost = 0.5 ms` 的實際值**

```bash
ssh radxa@192.168.68.57 'sudo grep . /sys/kernel/debug/sched/{nr_migrate,migration_cost_ns}'
```
```
/sys/kernel/debug/sched/nr_migrate:32
/sys/kernel/debug/sched/migration_cost_ns:500000
```

**(d) 遷移失敗的統計（每個行程都有）**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "echo 1 > /proc/sys/kernel/sched_schedstats"
ssh radxa@192.168.68.57 'cd /tmp && (./pelt_duty 3 6 time 2 8 >/dev/null & sleep 3; \
    grep -E "migrations" /proc/$!/sched; wait)'
```
```
se.nr_migrations              :  0
nr_migrations_cold            :  0
nr_failed_migrations_affine   :  8     ← 被 taskset 釘死，8 次想搬都搬不動
nr_failed_migrations_running  :  0
nr_failed_migrations_hot      :  0
nr_forced_migrations          :  0
```
`nr_failed_migrations_affine = 8` —— 負載均衡器確實試圖搬它 8 次，
每次都被 `can_migrate_task()` 的第 1 道關卡（`cpus_ptr`）擋下來。

---

<a name="q28"></a>
## 28. 使用 wake_up_process() 喚醒一個進程，那麼進程喚醒後應該在哪個 CPU 上運行？

### 結論

**由 `select_task_rq_fair()` 決定，候選人有三個：`this_cpu`（waker 所在）、
`prev_cpu`（上次跑的）、以及「別的閒置 CPU」。**

```
wake_up_process(p)
 └─ try_to_wake_up(p, TASK_NORMAL, 0)
     └─ cpu = select_task_rq(p, p->wake_cpu, WF_TTWU)
         └─ p->sched_class->select_task_rq = select_task_rq_fair(p, prev_cpu, wake_flags)
```

`select_task_rq_fair()`（`kernel/sched/fair.c:7428`）的決策樹：

```
① EAS 路徑（sched_energy_present 且 !rd->overutilized）
   → find_energy_efficient_cpu()   ← 見 Q30；成功就直接回傳

② want_affine = !wake_wide(p) && cpumask_test_cpu(cpu, p->cpus_ptr)
   for_each_domain(cpu, tmp):
       if (want_affine && (tmp->flags & SD_WAKE_AFFINE)
           && cpumask_test_cpu(prev_cpu, sched_domain_span(tmp))) {
               if (cpu != prev_cpu)
                       new_cpu = wake_affine(tmp, p, this_cpu, prev_cpu, sync);
               sd = NULL;          /* 快路徑 */
               break;
       }
       if (tmp->flags & sd_flag) sd = tmp;   /* 慢路徑候選域 */

③ if (unlikely(sd))                 → find_idlest_cpu()        （慢路徑：fork/exec）
   else if (wake_flags & WF_TTWU)   → select_idle_sibling(p, prev_cpu, new_cpu)  （快路徑）
```

**三個關鍵函式：**

* **`wake_wide(p)`**（`fair.c:6352`）：判斷要不要**放棄** wake affine。
  ```c
  unsigned int master = current->wakee_flips;   /* waker 喚醒過幾種不同的人 */
  unsigned int slave  = p->wakee_flips;
  int factor = __this_cpu_read(sd_llc_size);    /* 本機 = 8 */
  if (master < slave) swap(master, slave);
  if (slave < factor || master < slave * factor) return 0;   /* 不夠「廣」→ 維持 affine */
  return 1;
  ```
* **`wake_affine()`**（`fair.c:6447`）= `wake_affine_idle()` + `wake_affine_weight()`：
  `WA_IDLE`：prev_cpu 閒置且共享 cache → 選 prev_cpu；
  `WA_WEIGHT`：比較「把 p 放到 this_cpu」與「放到 prev_cpu」哪邊加權負載小
  （`WA_BIAS` 給 this_cpu 一點偏袒）。
* **`select_idle_sibling()`**：在 `sd_llc`（本機 = 全部 8 顆）裡找閒置 CPU，
  順序是 `target → prev → recent_used_cpu → select_idle_cpu() 掃描`，
  都找不到就回 `target`。

**一句話回答面試官：**
> 優先選 **prev_cpu**（cache 熱）；但如果 waker 和 wakee 有資料共享關係
> （生產者/消費者），wake_affine 會把它拉到 **waker 的 CPU**；
> 如果兩邊都忙，就在共享 LLC 的範圍裡找一顆**閒置的**；
> 開了 EAS 的話，上面全部繞過，直接找**最省電**的那顆。

> **書目**：奔跑吧 §8.3.5「喚醒進程」與 §8.3.6「wake affine 特性」
> （第 3931~4214 行）、圖 8.24/8.25/8.26。

### 實機驗證

`wake_cpu.c`：1 個 waker 用 pipe 輪流喚醒 N 個 wakee，統計 wakee 醒來後落在哪顆 CPU。

**(a) 系統空閒時：`select_idle_sibling()` 把 wakee 撒得到處都是**

```bash
ssh radxa@192.168.68.57 'cd /tmp && ./wake_cpu 1 3000 5'
```
```
waker pid=100097  wakee=1  rounds=3000  waker_cpu=5
waker  所在 CPU 分布 : c0=0    c1=0   c2=0  c3=0    c4=0  c5=3000  c6=0     c7=0
wakee0 被唤醒後 CPU : c0=431  c1=20  c2=0  c3=436  c4=0  c5=259   c6=1345  c7=509
```

waker 固定在 CPU5，wakee 卻只有 8.6% 落在 CPU5。
**原因：waker 當下正在跑（CPU5 不 idle），`select_idle_sibling()` 於是去
`sd_llc`（span=0-7）掃一圈找閒置 CPU，而全機都是閒的，所以隨便挑。**
這正是 [Q24](#q24) 所說「RK3588 的 LLC 域涵蓋全部 8 顆」的直接後果——
連 A55 都會被挑中（c0/c1/c3 合計 29%）。

**(b) 系統滿載時：wake affine 現形**

```bash
ssh radxa@192.168.68.57 'cd /tmp
for c in 0 1 2 3 4 5 6 7; do taskset -c $c sh -c "while :; do :; done" & done
sleep 3
echo "waker 在 CPU5:"; ./wake_cpu 1 2000 5
echo "waker 在 CPU1:"; ./wake_cpu 1 2000 1
kill %1 %2 %3 %4 %5 %6 %7 %8'
```
```
waker 在 CPU5:
waker  所在 CPU 分布 : c5=2000
wakee0 被唤醒後 CPU : c0=0 c1=0 c2=0 c3=286 c4=2 c5=1712 c6=0 c7=0

waker 在 CPU1:
waker  所在 CPU 分布 : c1=2000
wakee0 被唤醒後 CPU : c0=0 c1=683 c2=0 c3=0 c4=0 c5=0 c6=1317 c7=0
```

* waker 在 **CPU5（A76）**：wakee **85.6% 落在 CPU5** → **wake affine 生效**。
  沒有閒置 CPU 可挑，`select_idle_sibling()` 只好回傳 `target`，
  而 `target` 就是 `wake_affine()` 選出來的 this_cpu。
* waker 在 **CPU1（A55）**：只有 34% 留在 CPU1，**66% 跑到 CPU6（A76）**。
  因為 `wake_affine_weight()` 會比較兩邊的 `capacity`——
  A55 上已經有一個 busy loop，把 wakee 塞進去的加權負載遠高於放在大核，
  加上 `misfit` / `SD_ASYM_CPUCAPACITY` 的作用，最後被拉到大核。

**(c) `wake_wide()` 在這台機器上非常難觸發 —— 反而重現了書上圖 8.26 的病態**

```bash
ssh radxa@192.168.68.57 'cd /tmp
for c in 0 1 2 3 4 5 6 7; do taskset -c $c sh -c "while :; do :; done" & done
sleep 3; ./wake_cpu 16 150 5; kill %1 %2 %3 %4 %5 %6 %7 %8'
```
```
waker pid=100341  wakee=16  rounds=150  waker_cpu=5
waker  所在 CPU 分布 : c5=2400
wakee0  被唤醒後 CPU: c5=150
wakee1  被唤醒後 CPU: c5=150
wakee2  被唤醒後 CPU: c5=150
wakee3  被唤醒後 CPU: c2=8  c5=142
wakee4  被唤醒後 CPU: c3=10 c5=140
wakee5  被唤醒後 CPU: c5=150
wakee6  被唤醒後 CPU: c5=113 c7=37
wakee7  被唤醒後 CPU: c5=113 c7=37
全部 wakee 合計    : c0=0 c1=0 c2=8 c3=10 c4=0 c5=2272 c6=0 c7=110
```

**16 個 wakee，94.5% 全部堆到 CPU5 上，其他 7 顆 CPU 幾乎閒著。**
這正是書上圖 8.26 描述的 1:N 客戶端/伺服器病態：
「wake affine 會導致服務器端進程產生飢餓的現象」。

**為什麼 `wake_wide()` 沒有救場？** 看它的條件：

```c
master = current->wakee_flips;   /* waker 的：很大（一直換不同的 wakee） */
slave  = p->wakee_flips;         /* wakee 的：只有 1（它只喚醒 waker 一個） */
factor = sd_llc_size;            /* 本機 = 8 */
if (slave < factor)  return 0;   /* ← 1 < 8，直接判定「不夠廣」，維持 affine */
```

**`wake_wide()` 要求「兩邊」都很花心才會放棄 affine；純粹的 1:N 星形結構
（N 個 client 只跟 1 個 server 說話）永遠 `slave = 1`，永遠觸發不了。**
而且 RK3588 的 `sd_llc_size = 8`（[Q24](#q24)），門檻比一般 4 核簇的機器更高。

用模組印出 `wakee_flips` 可以直接證實：

```bash
ssh radxa@192.168.68.57 'sudo insmod ~/exp/sched/sched_probe.ko; sudo rmmod sched_probe; \
    sudo dmesg | grep wakee'
```
```
   wakee_flips=19  wakee_flip_decay_ts=4341662555  last_wakee=send process
```

---

<a name="q29"></a>
## 29. 綠色節能調度器如何衡量一個進程的計算能力？

### 結論

**用 `p->se.avg.util_avg`（實際算力需求），再加上 `util_est` 修正。**

```c
/* kernel/sched/fair.c */
static inline unsigned long task_util(struct task_struct *p)
{
	return READ_ONCE(p->se.avg.util_avg);
}

static inline unsigned long _task_util_est(struct task_struct *p)
{
	struct util_est ue = READ_ONCE(p->se.avg.util_est);
	return max(ue.ewma, (ue.enqueued & ~UTIL_AVG_UNCHANGED));
}

static inline unsigned long task_util_est(struct task_struct *p)
{
	return max(task_util(p), _task_util_est(p));
}
```

EAS 用它做三件事：

| 用途 | 函式 | 說明 |
|---|---|---|
| **判斷放不放得下** | `task_fits_capacity(p, cap)` | `util_fits_cpu()`：`util × 1.25 ≤ capacity`（留 20% 裕量） |
| **預測遷移後的 util** | `cpu_util_next(cpu, p, dst_cpu)` | 把 p 的 util 從 src 減掉、加到 dst，算出「假如搬過去」的 CPU util |
| **算功耗** | `compute_energy()` → `em_cpu_energy()` | 見 [Q35](#q35) |

**還要加上 uclamp（本機 `CONFIG_UCLAMP_TASK=y`）**：
`uclamp_eff_value(p, UCLAMP_MIN/MAX)` 可以把 util 夾在 `[min, max]` 之間，
讓 userspace（例如 Android 的 `ADPF`）能對關鍵執行緒「提示」它需要更多算力，
即使 PELT 還沒反應過來。本機預設 `uclamp.min=0, uclamp.max=1024`（不夾）。

**為什麼是 `util_avg` 而不是 `load_avg`？**
* `load_avg` 帶權重（nice），是「公平性」的度量；
* `util_avg` 不帶權重、上限 1024，且**頻率/算力不變**（[Q17](#q17)），
  可以直接跟 CPU 的 `capacity_orig`（422 / 1024）比大小 —— 這才是「算力」。

> **書目**：奔跑吧 §8.2.5、§8.2.8（`task_util()`、`cpu_util()`）、
> §8.4.6 第 (3) 點「要計算一個進程 p 的實際算力，我們採用 p->se.avg.util_avg」。
> **書上沒提 `util_est`**（5.1 才進主線）與 uclamp（5.3）。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'cd /tmp && (./pelt_duty 3 6 time 2 8 >/dev/null & sleep 3; \
    grep -E "util|uclamp" /proc/$!/sched; wait)'
```
```
se.avg.util_sum            :  1518493
se.avg.util_avg            :       32
se.avg.util_est.ewma       :       35
se.avg.util_est.enqueued   :       27
uclamp.min                 :        0
uclamp.max                 :     1024
effective uclamp.min       :        0
effective uclamp.max       :     1024
```

`task_util_est(p) = max(32, max(35, 27)) = 35`。
**對這種「跑 2 ms、睡 6 ms」的週期性行程，`util_est`（35）比 `util_avg`（32）更能
代表它醒著時的真實需求**——這就是它存在的理由。

不同 duty 下的實測 `util_avg`（`pelt_duty`，釘在 A76 的 cpu4）
與 `util_fits_cpu()` 的判斷：

| duty | 實測 `util_avg` | `util × 1.25` | 放得進 A55（cap 422）？ | 放得進 A76（cap 1024）？ |
|---|---|---|---|---|
| 1/8 (12.5%) | **115** | 144 | ✅ 是 | ✅ 是 |
| 4/8 (50%) | **496** | 620 | ❌ **否，必須上大核** | ✅ 是 |
| 8/8 (100%) | **1023** | 1279 | ❌ | ❌ **連大核都「不合身」（misfit）** |

這張表直接預測了 [Q30](#q30) 的實驗結果。

---

<a name="q30"></a>
## 30. 當一個進程被喚醒時，綠色節能調度器如何選擇在哪個 CPU 上運行？

### 結論

`find_energy_efficient_cpu()`（`kernel/sched/fair.c:7201`）—— **「窮舉 + 比功耗」**。

```
select_task_rq_fair()
 └─ if (sched_energy_enabled()) {
        new_cpu = find_energy_efficient_cpu(p, prev_cpu);
        if (new_cpu >= 0) return new_cpu;      /* 成功就不走一般路徑 */
    }
```

`find_energy_efficient_cpu()` 的六步：

```
① 前置檢查
   pd = rcu_dereference(rd->pd);
   if (!pd || READ_ONCE(rd->overutilized)) goto unlock;   ← 系統一旦過載就放棄 EAS
   sd = rcu_dereference(*this_cpu_ptr(&sd_asym_cpucapacity));
   while (sd && !cpumask_test_cpu(prev_cpu, sched_domain_span(sd))) sd = sd->parent;

② sync_entity_load_avg(&p->se);      /* 先把 blocked load 衰減到現在 */
   if (!task_util_est(p) && p_util_min == 0) goto unlock;  /* util = 0 沒得比 */

③ 對每一個 perf domain（本機 3 個：{0-3}, {4-5}, {6-7}）：
     對 pd 裡每一顆 p 可以跑的 CPU：
         util = cpu_util_next(cpu, p, cpu);            /* 假設 p 搬過來 */
         if (!util_fits_cpu(util, min, max, cpu)) continue;   /* 放不下就跳過 */
         記錄 spare capacity 最大的那一顆 → max_spare_cap_cpu
     另外記下 prev_cpu 所在 pd（當基準）

④ base_energy = compute_energy(&eenv, pd, cpus, p, -1);        /* 誰都不加的基準 */
   prev_delta  = compute_energy(..., prev_cpu) - base_energy;
   cur_delta   = compute_energy(..., max_spare_cap_cpu) - base_energy;

⑤ 全部 pd 比完，取 cur_delta 最小的當 best_energy_cpu

⑥ 遲滯（避免抖動 + 保 cache 熱度）：
   if (prev_delta == ULONG_MAX) return best_energy_cpu;
   if ((prev_delta - best_delta) > ((prev_delta + base_energy) >> 4))
           return best_energy_cpu;      /* 省超過 1/16 才值得搬 */
   return prev_cpu;
```

三個設計要點：
* **只在「系統還沒過載」時啟用**（`!rd->overutilized`，見 [Q31](#q31)）；
* **每個 perf domain 只挑一個代表**（spare capacity 最大者）去比，不是全排列，
  複雜度 `O(nr_pd × nr_cpus)`；
* **1/16 遲滯**：省電幅度不到 6.25% 就留在 `prev_cpu`，保住 cache。

**EAS 啟用的六個條件**（`build_perf_domains()`，`kernel/sched/topology.c:345`）：

1. `sysctl_sched_energy_aware == 1`
2. 拓撲有 `SD_ASYM_CPUCAPACITY`（大小核）
3. 沒有 SMT
4. `arch_scale_freq_invariant()`（頻率不變性）
5. **所有 CPU 的 cpufreq governor 都是 `schedutil`**
6. EM 複雜度 `nr_pd × (nr_cpus + nr_ps) ≤ 2048`

> **書目**：奔跑吧 §8.4.6「該選擇哪個 CPU 來執行喚醒進程 p 呢」與圖 8.36/8.37/8.39
> （第 5326~5621 行）。
> **修正**：書上分析的是 Quentin Perret 的 `eas_dev_v5.0_r1` 分支，
> 裡面的 `find_best_target()` / `select_max_spare_cap_cpus()` / `schedtune`
> **從未進入主線**；6.1 主線只有上面這一份 `find_energy_efficient_cpu()`。

### 實機驗證

**(a) 先讓 EAS 真的跑起來 —— 預設是關的！**

本機出廠 governor 是 `ondemand`，**條件 5 不滿足，所以 EAS 根本沒啟動**：

```bash
ssh radxa@192.168.68.57 'grep . /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; \
                         cat /proc/sys/kernel/sched_energy_aware'
```
```
policy0:ondemand   policy4:ondemand   policy6:ondemand
1                       ← sysctl 說「要」，但條件不滿足
```

切成 schedutil，EAS 立刻起來（打開 `verbose` 才看得到訊息）：

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
echo 1 > /sys/kernel/debug/sched/verbose; dmesg -C
for p in 0 4 6; do echo schedutil > /sys/devices/system/cpu/cpufreq/policy\$p/scaling_governor; done
sleep 1; dmesg | sed \"s/^\[[^]]*\] //\""'
```
```
root_domain 0-7: pd6:{ cpus=6-7 nr_pstate=11 } pd4:{ cpus=4-5 nr_pstate=11 } pd0:{ cpus=0-3 nr_pstate=8 }
sched_energy_set: starting EAS
```

**「starting EAS」** —— 這就是 `sched_energy_set(true)` 打開
`static_branch sched_energy_present` 的那一刻。
複雜度檢查：`3 × (8 + 8+11+11) = 3 × 38 = 114 ≤ 2048` ✅

**(b) EAS ON：輕的任務去小核，重的任務去大核**

```bash
ssh radxa@192.168.68.57 'cd /tmp
echo "輕載 (1ms/10ms, util~100):"; ./pelt_duty -1 10 time 1 10 | awk "{print \$2}" | sort | uniq -c
echo "重載 (8ms/10ms, util~800):"; ./pelt_duty -1 10 time 8 10 | awk "{print \$2}" | sort | uniq -c'
```
```
輕載 (1ms/10ms, util~100):
      5 cpu=1        ← A55
      6 cpu=3        ← A55
重載 (8ms/10ms, util~800):
     10 cpu=5        ← A76
      1 cpu=7        ← A76
```

**輕的任務 100% 落在 A55（cpu1/cpu3），重的任務 100% 落在 A76（cpu5/cpu7）。**

**(c) EAS OFF 做對照組**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "echo 0 > /proc/sys/kernel/sched_energy_aware; \
    sleep 1; dmesg | tail -1 | sed \"s/^\[[^]]*\] //\""
cd /tmp; echo "輕載:"; ./pelt_duty -1 10 time 1 10 | awk "{print \$2}" | sort | uniq -c
ssh radxa@192.168.68.57 'sudo bash -c "echo 1 > /proc/sys/kernel/sched_energy_aware"'
```
```
sched_energy_set: stopping EAS
輕載:
      3 cpu=1        ← A55
      3 cpu=4        ← A76
      5 cpu=7        ← A76
```

| | 輕載任務落在 A55 的比例 | 落在 A76 的比例 |
|---|---|---|
| **EAS ON** | **100%**（11/11） | 0% |
| **EAS OFF** | 27%（3/11） | **73%**（8/11） |

**EAS 關掉之後，一個只需要 100/1024 算力的小任務有 73% 的機率跑在
2.256 GHz 的 A76 上**——`select_idle_sibling()` 只管「哪顆閒著」，不管「哪顆省電」。
從 [Q35](#q35) 的能效係數看，同樣的 util 放在 A76 要花 **3.76 倍**的能量。

**(d) 為什麼重載一定上大核？**

`util_fits_cpu()` 要求 `util × 1.25 ≤ capacity`：
* util ≈ 800 → 需要 1000 的 capacity → **A55 的 422 完全放不下**，
  第 ③ 步就 `continue` 掉了，三個 pd 裡只有 pd4/pd6 有候選人。

---

<a name="q31"></a>
## 31. 綠色節能調度器是否會做 CPU 間的負載均衡呢？

### 結論

**會，但有一個開關叫 `rd->overutilized`（書上稱 Tipping Point）：**

* **系統「還不忙」時（`!rd->overutilized`）** → **禁止** CFS 的週期性負載均衡，
  一切交給 EAS 在喚醒時做「省電放置」。
* **系統「已經過載」（`rd->overutilized`）** → EAS **自動退場**，
  回到標準 CFS/SMP 負載均衡，全力衝吞吐量。

**兩處程式碼：**

```c
/* ① 均衡入口直接短路 —— kernel/sched/fair.c, find_busiest_group() */
if (sched_energy_enabled()) {
	struct root_domain *rd = env->dst_rq->rd;
	if (rcu_dereference(rd->pd) && !READ_ONCE(rd->overutilized))
		goto out_balanced;          /* env->imbalance = 0; return NULL; */
}

/* ② 喚醒時也短路 —— find_energy_efficient_cpu() */
if (!pd || READ_ONCE(rd->overutilized))
	goto unlock;                    /* 回 -1，走一般 CFS 路徑 */
```

**`overutilized` 怎麼被設起來？** 兩條路徑：

```c
/* kernel/sched/fair.c:6021 */
static inline bool cpu_overutilized(int cpu)
{
	unsigned long rq_util_min = uclamp_rq_get(cpu_rq(cpu), UCLAMP_MIN);
	unsigned long rq_util_max = uclamp_rq_get(cpu_rq(cpu), UCLAMP_MAX);
	return !util_fits_cpu(cpu_util_cfs(cpu), rq_util_min, rq_util_max, cpu);
}

/* (a) 每個 tick：scheduler_tick() → task_tick_fair() → update_overutilized_status() */
static inline void update_overutilized_status(struct rq *rq)
{
	if (!READ_ONCE(rq->rd->overutilized) && cpu_overutilized(rq->cpu)) {
		WRITE_ONCE(rq->rd->overutilized, SG_OVERUTILIZED);
		trace_sched_overutilized_tp(rq->rd, SG_OVERUTILIZED);
	}
}

/* (b) 每次負載均衡：update_sg_lb_stats() 掃描時 */
if (!nr_running && idle_cpu(i)) { sgs->idle_cpus++; continue; }
if (local_group) continue;
if (env->sd->flags & SD_ASYM_CPUCAPACITY) {
	if (sgs->group_misfit_task_load < rq->misfit_task_load) { ... *sg_status |= SG_OVERLOAD; }
}
...
/* update_sd_lb_stats() 最後：只有 root domain 那一層才會清 */
if (!env->sd->parent) {
	WRITE_ONCE(rd->overutilized, sg_status & SG_OVERUTILIZED);
	...
}
```

**判斷式就是 `util_fits_cpu()`：CPU 的 `cpu_util_cfs()` 超過
`capacity_of(cpu) × 100/125 = 80%` 就算 overutilized。**
（`util_fits_cpu()` 內部用 `fits_capacity(util, cap)` ⇒ `util * 1280 < cap * 1024`。）

**為什麼要這個設計？** 兩種目標天生矛盾：
* SMP 負載均衡 = **把任務攤平到所有 CPU**（最大吞吐）
* EAS = **把任務集中到剛好夠用的小核上，讓大核進 idle**（最省電）

如果同時作用，EAS 剛把任務塞到小核，負載均衡馬上又把它攤到大核，來回打架。
所以用 `overutilized` 當仲裁：**沒吃緊就省電，吃緊了就拼效能。**

> **書目**：奔跑吧 §8.4.7「overutilized 條件判斷」（第 5621~5694 行）。
> 書上把判斷寫在 `find_busiest_queue()` 是筆誤，實際在 `find_busiest_group()`。

### 實機驗證

**(a) `sched_energy_enabled()` 是一個 static key**

```bash
grep -n -A6 "static inline bool sched_energy_enabled" kernel/sched/sched.h
```
```c
DECLARE_STATIC_KEY_FALSE(sched_energy_present);
static inline bool sched_energy_enabled(void)
{
	return static_branch_unlikely(&sched_energy_present);
}
```
沒開 EAS 時這段程式碼是 **nop**，零成本。

**(b) 用 tracepoint 看 overutilized 的翻轉**

`sched_overutilized_tp` 是 **bare tracepoint**（`include/trace/events/sched.h:722`
用 `DECLARE_TRACE` 宣告，不是 `TRACE_EVENT`），
**不會出現在 `/sys/kernel/debug/tracing/events/sched/` 底下**，
只能用 BPF（`bpf_trace_printk`）或核心模組 `register_trace_sched_overutilized_tp()` 掛。

```bash
grep -n "sched_overutilized_tp" include/trace/events/sched.h kernel/sched/fair.c
```
```
include/trace/events/sched.h:722:DECLARE_TRACE(sched_overutilized_tp,
kernel/sched/fair.c:6034:  trace_sched_overutilized_tp(rq->rd, SG_OVERUTILIZED);      ← tick 路徑
kernel/sched/fair.c:9933:  trace_sched_overutilized_tp(rd, sg_status & SG_OVERUTILIZED); ← 負載均衡路徑
kernel/sched/fair.c:9938:  trace_sched_overutilized_tp(rd, SG_OVERUTILIZED);
```
不過可以間接觀察：**輕載時任務黏在小核（EAS 生效），
加上滿載後任務立刻被攤開（EAS 退場）**：

```bash
ssh radxa@192.168.68.57 'cd /tmp
echo "--- 系統空閒，輕載任務："; ./pelt_duty -1 6 time 1 10 | awk "{print \$2}" | sort | uniq -c
for c in 0 1 2 3 4 5 6 7; do taskset -c $c sh -c "while :; do :; done" & done; sleep 3
echo "--- 系統滿載（rd->overutilized=1），同一個輕載任務："
./pelt_duty -1 6 time 1 10 | awk "{print \$2}" | sort | uniq -c
kill %1 %2 %3 %4 %5 %6 %7 %8' 2>/dev/null
```

系統滿載時每顆 CPU 的 `cpu_util_cfs()` 都是 1024 > `capacity × 0.8`，
`update_overutilized_status()` 在第一個 tick 就會把 `rd->overutilized` 設起來，
`find_energy_efficient_cpu()` 於是回 −1，改走 `select_idle_sibling()`。

**(c) 反向確認：EAS 生效的前提是「有閒置算力」**

[Q30](#q30) 的實驗都是在系統空閒時做的。這不是巧合——
`find_energy_efficient_cpu()` 的第一個檢查就是 `READ_ONCE(rd->overutilized)`。
面試時如果被問「為什麼手機打遊戲時 EAS 好像沒用」，答案就是這個：
**遊戲把 CPU 打滿 → overutilized → EAS 自動關掉 → 回到吞吐量優先。**

---

<a name="q32"></a>
## 32. 目前在 Linux 5.0 內核中，CPU 動態調頻調壓模块 CPUFreq 和進程調度器之間是如何協同工作的？有什麼優缺點？

### 結論

**靠 `schedutil` governor —— 調度器主動「推」util 給 cpufreq，而不是 cpufreq 去「猜」。**

**傳統 governor（ondemand / conservative / interactive）的問題：**

| 問題 | 說明 |
|---|---|
| **間接** | 只能週期性取樣 `/proc/stat` 的 idle time 反推負載，調度器明明知道一切卻不說 |
| **滯後** | 取樣週期（ondemand 預設 ~10 ms）+ 判斷 + 換頻，反應慢 |
| **取樣太快** | 被毛刺帶著跑，頻繁換頻本身就耗電（每次 DVFS 有 transition latency） |
| **取樣太慢** | 突發重活反應不過來 |
| **不知道遷移** | 行程被搬走/搬來，cpufreq 完全不知道，要等下一次取樣 |
| **公平性錯亂** | 兩個同優先級行程一個在 600 MHz、一個在 2.2 GHz，CFS 給一樣的時間片，對前者不公平（PELT 的頻率不變性就是為了解這個） |

**schedutil 的做法（`kernel/sched/cpufreq_schedutil.c`）：**

```
調度器每次更新 util（enqueue/dequeue/tick/load balance/attach/detach）
   → cfs_rq_util_change() → cpufreq_update_util(rq, flags)
       → per-CPU 的 update_util_data->func()
           = sugov_update_single_freq() / sugov_update_shared()
               → get_next_freq()
                   → sugov_get_util():  util = cpu_util_cfs() + cpu_util_rt() + dl + irq
                   → map_util_freq(util, max_freq, capacity)
                   → cpufreq_driver_fast_switch() 或 喚醒 sugov:N kthread (SCHED_DEADLINE)
```

核心公式（`include/linux/sched/cpufreq.h`）：

```c
#define map_util_perf(util)  ((util) + ((util) >> 2))          /* × 1.25，25% headroom */
static inline unsigned long map_util_freq(unsigned long util,
                                          unsigned long freq, unsigned long cap)
{
	return freq * util / cap;
}
/* 合起來： next_freq = max_freq × (util × 1.25) / capacity */
```

**優點**：
* 零滯後（調度器一動就通知）、無取樣開銷；
* 用的是 PELT 的**頻率/算力不變**訊號，跨核跨頻可比；
* 遷移時 `attach_entity_load_avg()` 立刻通知目的 CPU 升頻；
* 是 **EAS 的必要條件**（[Q30](#q30) 條件 5）——因為 EAS 的功耗預測必須知道
  「調度器要求多少 util → cpufreq 會選哪個 OPP」，只有 schedutil 這個關係是確定的。

**缺點**：
* PELT 反應仍要 ~75 ms 才到 80%（[Q16](#q16)），對「突發重活」還是慢，
  Android 因此加了 `util_est`、`uclamp`、以及廠商私有的 WALT；
* 25% headroom 是寫死的，不同 SoC 未必最佳；
* 換頻若要走韌體（RK3588 走 **SCMI mailbox**），`fast_switch` 不可用，
  必須喚醒 `sugov:N` 這個 `SCHED_DEADLINE` kthread，本身就是一次排程 + IPI；
* 對 I/O bound 的工作負載（util 低但延遲敏感）容易降頻過頭。

> **書目**：奔跑吧 §8.4.8「CPU 動態調頻」（第 5694~5830 行）與圖 8.40/8.41。

### 實機驗證

**(a) 本機有哪些 governor、預設是哪個**

```bash
ssh radxa@192.168.68.57 'cat /sys/devices/system/cpu/cpufreq/policy4/scaling_available_governors; \
                         cat /sys/devices/system/cpu/cpufreq/policy4/scaling_driver'
```
```
conservative ondemand userspace powersave interactive performance schedutil
cpufreq-dt
```
**出廠預設 `ondemand`** —— 所以 RK3588 這塊板子預設是**沒有** EAS 的（[Q30](#q30)）。

**(b) schedutil 的 util → 頻率對應，實機量一次**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
for p in 0 4 6; do echo schedutil > /sys/devices/system/cpu/cpufreq/policy\$p/scaling_governor; done
sleep 1
echo \"空閒時 policy4 頻率 = \$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq)\"
for d in 1 2 4 6 8; do
  su radxa -c \"/tmp/pelt_duty 4 6 time \$d 8\" > /tmp/o.txt &
  sleep 4; f=\$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq); wait
  u=\$(grep FINAL /tmp/o.txt | grep -o \"util_avg=[0-9]*\")
  printf \"duty=%d/8  %s  -> cur_freq=%s kHz  (公式預測 1.25*util/1024*2256000 = %d)\n\" \
      \$d \"\$u\" \$f \$(( \$(echo \$u | cut -d= -f2) * 125 * 2256000 / 100 / 1024 ))
done"'
```

```
空閒時 policy4 頻率 = 1200000
duty=1/8  util_avg=4    -> cur_freq= 600000 kHz  (公式預測   11015)
duty=2/8  util_avg=49   -> cur_freq= 408000 kHz  (公式預測  134941)
duty=4/8  util_avg=260  -> cur_freq=1200000 kHz  (公式預測  716015)
duty=6/8  util_avg=672  -> cur_freq=2016000 kHz  (公式預測 1850625)
duty=8/8  util_avg=1023 -> cur_freq=2256000 kHz  (公式預測 2817246 → 被 clamp 到 2256000)
```

* **趨勢完全正確**：util 越大頻率越高，滿載直接頂到 2.256 GHz。
* **實際 OPP 永遠 ≥ 公式預測**，因為：
  1. `sugov_get_util()` 用的是 `cpu_util_cfs()` = **`max(util_avg, util_est)`** 再加上
     RT/DL/IRQ 的 util，比我從 `/proc/pid/sched` 讀到的**單一行程波谷 `util_avg`** 大；
  2. `get_next_freq()` 之後 `cpufreq_driver_resolve_freq()` 會**往上**取最近的 OPP。
* 800 MHz 那兩筆（duty 1/8、2/8）看起來「太高」，是因為量測瞬間有背景任務。

**(c) schedutil 在 RK3588 上是 slow-switch（要喚醒 kthread）**

```bash
ssh radxa@192.168.68.57 'ps -eo pid,cls,pri,rtprio,ni,comm | grep -E "PID|sugov"'
```
```
    PID CLS PRI RTPRIO  NI COMMAND
   2239 DLN 140      0   - sugov:0
   2240 DLN 140      0   - sugov:4
   2241 DLN 140      0   - sugov:6
```
每個 cpufreq policy 一個 `sugov:N` kthread（本機三個），
**`CLS=DLN` = `SCHED_DEADLINE`，不是 SCHED_FIFO**
（Linux 4.16 commit `794a56ebd9a5` 改的；`sugov_kthread_create()` 用
`sched_runtime=1 ms / sched_deadline=sched_period=10 ms` 的頻寬預留，
既保證換頻不會被餓死，也不會無界佔用 CPU）。

**注意：切回 `ondemand` governor 之後這三個 kthread 就消失了** ——
它們是 schedutil 專屬的。
因為 `cpufreq-dt` + SCMI clock 不能在 atomic context 換頻
（`policy->fast_switch_possible = false`），所以每次調頻都要
`irq_work_queue()` → 喚醒 kthread → `__cpufreq_driver_target()`。
**這正是「換頻本身有成本」的具體體現**，也是 [Q40](#q40) 延時分析要考慮的一項。

---

<a name="q33"></a>
## 33. 什麼是能效模型？

### 結論

**能效模型（Energy Model, EM）= 一張「頻率 → 功耗」的表，
加上把 CPU 分組的「性能域（Performance Domain, PD）」概念。**

三層資料結構（`include/linux/energy_model.h`）：

```c
struct em_perf_domain {
	struct em_perf_state *table;    /* 每個 OPP 一項，依頻率遞增 */
	int nr_perf_states;
	unsigned long flags;
	unsigned long cpus[];           /* 這個 PD 包含哪些 CPU（CPU 型 EM 才有）*/
};

struct em_perf_state {
	unsigned long frequency;        /* kHz  */
	unsigned long power;            /* uW（本機是 microwatt）*/
	unsigned long cost;             /* 能效係數 = power × fmax / freq */
	unsigned long flags;            /* EM_PERF_STATE_INEFFICIENT */
};
```

**性能域**：能**一起**調頻調壓的一組 CPU（= 一個 `cpufreq_policy` = 一個電壓域）。
同一個 PD 裡的 CPU 必須是同微架構、同頻率。

**`cost` 是整個模型的精華**（`kernel/power/energy_model.c:171`）：

```c
cost = div64_u64(fmax * power_res, table[i].frequency);
```

推導（書上式 8.26~8.29）：

```
一顆 CPU 在某個 OPP 上的功耗
     = ps->power × (該 CPU 的 util / 該 OPP 提供的 capacity)
     = ps->power × util / (ps->freq × scale_cpu / fmax)
     = (ps->power × fmax / ps->freq) × (util / scale_cpu)
       └──────── cost，靜態，可預先算 ────┘   └── 動態 ──┘
```

所以執行期只要 `energy = cost × sum_util / scale_cpu`，**一次乘一次除**就好。

EM 還提供 **`EM_PERF_STATE_INEFFICIENT` 標記**：如果某個 OPP 的 cost
不低於前一個（升頻卻沒省能效），就標成 inefficient，
`em_pd_get_efficient_state()` 會跳過它。

**誰在用 EM？**
* **EAS**（`compute_energy()`）——排程；
* **IPA / Thermal**（`devfreq_cooling`、`cpufreq_cooling`）——散熱功率分配；
* **DTPM**（Dynamic Thermal Power Management）。

> **書目**：奔跑吧 §8.4.2「能效模型」（第 4455~4633 行）、式 (8.26)~(8.29)、圖 8.32。
> **修正**：書上的型別叫 `em_cap_state`、註冊函式叫 `em_register_perf_domain()`，
> 6.1 已改名為 **`em_perf_state`** 與 **`em_dev_register_perf_domain()`**。

### 實機驗證

**RK3588 的三個 CPU 性能域 + 一個 GPU 性能域，全部 dump 出來：**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
for d in /sys/kernel/debug/energy_model/*/; do echo \"-- \$(basename \$d)\"
  for s in \$d/ps:*; do
    echo \"   freq=\$(cat \$s/frequency) kHz  power=\$(cat \$s/power) uW  cost=\$(cat \$s/cost)\"
  done | sort -t= -k2 -n
done"'
```

```
-- cpu0   （A55 × 4，policy0，dynamic-power-coefficient = 100）
   freq= 408000  power=  18589  cost= 82010
   freq= 600000  power=  27337  cost= 82011
   freq= 816000  power=  37179  cost= 82012
   freq=1008000  power=  45927  cost= 82012
   freq=1200000  power=  56636  cost= 84954
   freq=1416000  power=  74428  cost= 94611
   freq=1608000  power= 106022  cost=118681
   freq=1800000  power= 149713  cost=149713

-- cpu4   （A76 × 2，policy4，dynamic-power-coefficient = 300）
   freq= 408000  power=  55768  cost=308364
   freq= 600000  power=  82012  cost=308365
   freq= 816000  power= 111537  cost=308367
   freq=1008000  power= 137781  cost=308367
   freq=1200000  power= 164025  cost=308367
   freq=1416000  power= 208152  cost=331632
   freq=1608000  power= 253561  cost=355742
   freq=1800000  power= 345600  cost=433152
   freq=2016000  power= 463050  cost=518175
   freq=2208000  power= 613014  cost=626340
   freq=2256000  power= 676800  cost=676800

-- cpu6   （A76 × 2，policy6）  與 cpu4 完全相同
-- fb000000.gpu （Mali G610，dynamic-power-coefficient = 2982）
   freq= 300000  power= 407602  cost=1358673
   ...
   freq=1000000  power=2154495  cost=2154495
```

以及調度器看到的 PD 分組：

```
root_domain 0-7: pd6:{ cpus=6-7 nr_pstate=11 } pd4:{ cpus=4-5 nr_pstate=11 } pd0:{ cpus=0-3 nr_pstate=8 }
```

**三件事一次驗完：**

1. **PD 分組 = cpufreq policy 分組**：`{0-3}`（A55）、`{4-5}`、`{6-7}`（A76 兩對）。
   注意 A76 是**兩個** PD 不是一個，因為 RK3588 的兩對大核有獨立的電壓軌。
2. **`cost = power × fmax / freq` 一項不差**（見 [Q35](#q35) 的完整對帳）。
3. **A55 的低頻段 cost 全部 = 82010~82012（幾乎相同）**，
   代表 408 MHz ~ 1008 MHz 這四個 OPP **能效一樣**（都在電壓下限），
   跑得快反而不虧 → `em_pd_get_efficient_state()` 會直接跳到 1008 MHz。

---

<a name="q34"></a>
## 34. 綠色節能調度器如何讀取能效模型的數據？

### 結論

**三步：驅動註冊 → 調度器取得 → 執行期查表。**

**(1) 註冊（CPUfreq 驅動做）**

```c
/* drivers/cpufreq/cpufreq-dt.c: cpufreq_init() */
if (!em_dev_register_perf_domain(cpu_dev, nr_opp, &em_cb, policy->cpus, true))
	;   /* em_cb.active_power = _get_power()，從 OPP + dynamic-power-coefficient 算 */
```
本機用 `cpufreq-dt`，`dev_pm_opp_of_register_em()` →
`em_dev_register_perf_domain()`，power 由
`_get_power()` = `dynamic-power-coefficient × V² × f` 算出。

**(2) 調度器取得（建立調度域時）**

```c
/* kernel/sched/topology.c: build_perf_domains() → pd_init() */
static struct perf_domain *pd_init(int cpu)
{
	struct em_perf_domain *obj = em_cpu_get(cpu);   /* ← 就是這一行 */
	struct perf_domain *pd;
	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	pd->em_pd = obj;
	return pd;
}
/* 最後掛到 root domain 上，用 RCU 保護 */
rcu_assign_pointer(rd->pd, pd);
```

```c
/* kernel/power/energy_model.c */
struct em_perf_domain *em_cpu_get(int cpu)
{
	return em_pd_get(get_cpu_device(cpu));
}
```

`rd->pd` 是一條**單向鏈表**，每個節點對應一個性能域。

**(3) 執行期查表（每次喚醒）**

```c
/* kernel/sched/fair.c: find_energy_efficient_cpu() */
rcu_read_lock();
pd = rcu_dereference(rd->pd);
...
for (; pd; pd = pd->next) {
	cpumask_and(cpus, perf_domain_span(pd), cpu_online_mask);
	...
	cur_delta = compute_energy(&eenv, pd, cpus, p, max_spare_cap_cpu);
}
rcu_read_unlock();
```

`compute_energy()` → `em_cpu_energy(pd->em_pd, max_util, busy_time, eenv->cpu_cap)`
→ `em_pd_get_efficient_state(pd, freq)` 在 `pd->table[]` 裡**線性搜尋**
第一個 `frequency >= freq` 且非 inefficient 的項。

**RCU 的意義**：`rd->pd` 隨時可能因為 governor 切換、CPU hotplug、
`sysctl_sched_energy_aware` 改變而被重建（`call_rcu(&tmp->rcu, destroy_perf_domain_rcu)`），
喚醒路徑不能拿鎖，所以用 RCU。

> **書目**：奔跑吧 §8.4.2 第 3 小節「能效模型接口」與 §8.4.5「註冊能效模型子系統」
> （第 5077~5326 行）。

### 實機驗證

**(a) 註冊的痕跡在 dmesg**

```bash
ssh radxa@192.168.68.57 'dmesg | grep -i "perf domain"'
```
```
[   14.564234] panthor fb000000.gpu: EM: created perf domain
[   17.597807] cpu cpu0: EM: created perf domain
[   17.608160] cpu cpu4: EM: created perf domain
[   17.637854] cpu cpu6: EM: created perf domain
```
**三個 CPU PD + 一個 GPU PD**，時間點在 17.6 s（cpufreq 驅動 probe 時）。

**(b) 調度器取到之後印出來（要 `verbose=1`）**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "echo 1 > /sys/kernel/debug/sched/verbose
dmesg -C
echo 0 > /proc/sys/kernel/sched_energy_aware; echo 1 > /proc/sys/kernel/sched_energy_aware
sleep 1; dmesg | sed \"s/^\[[^]]*\] //\""'
```
```
sched_energy_set: stopping EAS
root_domain 0-7: pd6:{ cpus=6-7 nr_pstate=11 } pd4:{ cpus=4-5 nr_pstate=11 } pd0:{ cpus=0-3 nr_pstate=8 }
sched_energy_set: starting EAS
```

這行就是 `perf_domain_debug()` 印的，證明 `rd->pd` 這條鏈表確實掛上去了，
順序是 pd6 → pd4 → pd0（`pd_init()` 是頭插法，所以跟 CPU 編號反序）。

**(c) 使用者也能讀同一份資料**

```bash
ssh radxa@192.168.68.57 'sudo ls /sys/kernel/debug/energy_model/'
```
```
cpu0  cpu4  cpu6  fb000000.gpu
```
`kernel/power/energy_model.c` 的 `em_debug_create_pd()` 建的，
目錄名是「該 PD 的第一顆 CPU」——正好對應 `pd0/pd4/pd6`。

---

<a name="q35"></a>
## 35. 綠色節能調度器如何計算一個 CPU 的功耗？

### 結論

```
                        ┌ 靜態（建表時算好）┐   ┌─ 動態（每次喚醒算）─┐
   energy(pd) = Σ_cpu   [  ps->cost  ]      ×   [ cpu_util / scale_cpu ]
```

程式碼在 `include/linux/energy_model.h:223 em_cpu_energy()`：

```c
static inline unsigned long em_cpu_energy(struct em_perf_domain *pd,
			unsigned long max_util, unsigned long sum_util,
			unsigned long allowed_cpu_cap)
{
	if (!sum_util) return 0;

	/* ① 用「域內最忙的 CPU」的 util 反推 schedutil 會選什麼頻率 */
	cpu       = cpumask_first(to_cpumask(pd->cpus));
	scale_cpu = arch_scale_cpu_capacity(cpu);
	ps        = &pd->table[pd->nr_perf_states - 1];      /* 最高頻那一項 */
	max_util  = map_util_perf(max_util);                 /* × 1.25 */
	max_util  = min(max_util, allowed_cpu_cap);          /* thermal capping */
	freq      = map_util_freq(max_util, ps->frequency, scale_cpu);

	/* ② 找到 >= freq 的最低「有效率」OPP */
	ps = em_pd_get_efficient_state(pd, freq);

	/* ③ energy = cost × sum_util / scale_cpu */
	return ps->cost * sum_util / scale_cpu;
}
```

三個參數的來源（`compute_energy()`，`kernel/sched/fair.c:7188`）：

| 參數 | 來源 | 意義 |
|---|---|---|
| `max_util` | `eenv_pd_max_util()` — 域內每顆 CPU 的 `cpu_util_next()` 取最大 | 決定**選哪個 OPP**（同域同頻） |
| `sum_util` | `eenv_pd_busy_time()` — 域內所有 CPU 的 busy time 總和 | 決定**用了多少時間**（正比於能量） |
| `allowed_cpu_cap` | `get_actual_cpu_capacity()` — 扣掉 thermal pressure | 過熱降頻的上限 |

**為什麼 `max_util` 和 `sum_util` 要分開？**
同一個 PD 裡所有 CPU **共用一個頻率**（一個電壓域），
所以「頻率」由最忙的那顆決定，「總能量」則要把每顆的忙碌時間加起來。

**書上的完整例子（§8.4.6 要點總結）：**
```
小核 PD：選 1.4 GHz，功耗 124 mW，cost = 163
         CPU0 負載 200、CPU1 負載 300
         energy = 163 × (200+300) / 592 ≈ 137.7 mW
大核 PD：選 1.4 GHz，功耗 500 mW，cost = 831
         CPU2 負載 256、CPU3 負載 300
         energy = 831 × (256+300) / 1024 ≈ 451 mW
   → 放小核省電
```

> **書目**：奔跑吧 §8.4.6 第 3 小節與式 (8.26)~(8.29)（第 5560~5621 行）。

### 實機驗證

**(a) `cost = power × fmax / freq` —— RK3588 的 19 個 OPP 全部對帳**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
for d in cpu0 cpu4; do
  fmax=\$(ls /sys/kernel/debug/energy_model/\$d/ | sed \"s/ps://\" | sort -n | tail -1)
  echo \"== \$d  fmax=\$fmax kHz\"
  for s in /sys/kernel/debug/energy_model/\$d/ps:*; do
    f=\$(cat \$s/frequency); p=\$(cat \$s/power); c=\$(cat \$s/cost)
    printf \"   f=%-8s power=%-8s cost=%-8s  手算 p*fmax/f = %s\n\" \$f \$p \$c \$((p*fmax/f))
  done | sort -t= -k2 -n
done"'
```

| CPU | freq (kHz) | power (µW) | **cost（核心）** | **手算 `power×fmax/freq`** | 相符 |
|---|---|---|---|---|---|
| A55 | 408000 | 18589 | 82010 | 18589×1800000/408000 = **82010** | ✅ |
| A55 | 600000 | 27337 | 82011 | **82011** | ✅ |
| A55 | 816000 | 37179 | 82012 | **82012** | ✅ |
| A55 | 1008000 | 45927 | 82012 | **82012** | ✅ |
| A55 | 1200000 | 56636 | 84954 | **84954** | ✅ |
| A55 | 1416000 | 74428 | 94611 | **94611** | ✅ |
| A55 | 1608000 | 106022 | 118681 | **118681** | ✅ |
| A55 | 1800000 | 149713 | 149713 | **149713** | ✅（fmax 本身） |
| A76 | 408000 | 55768 | 308364 | 55768×2256000/408000 = **308364** | ✅ |
| A76 | 1008000 | 137781 | 308367 | **308367** | ✅ |
| A76 | 2016000 | 463050 | 518175 | **518175** | ✅ |
| A76 | 2256000 | 676800 | 676800 | **676800** | ✅ |

**19 個 OPP 沒有一個對不上。式 (8.25) 在實機上百分之百成立。**

**(b) power 本身也能反推 —— `P = C × V² × f`**

DT 裡 `dynamic-power-coefficient`：A55 = **100**、A76 = **300**（µW/MHz/V²）。

```
A55 @ 1008 MHz: 45927 = 100 × 1008 × V²  →  V² = 0.4556  →  V = 0.675 V
A76 @ 1008 MHz: 137781 = 300 × 1008 × V² →  V² = 0.4556  →  V = 0.675 V
```
**兩者算出同一個電壓 0.675 V**（RK3588 的 A55/A76 在 1008 MHz 用同一檔電壓），
而 `137781 / 45927 = 3.0` **正好等於 300/100 的係數比**。
能效模型的每一個數字都能一路追回裝置樹。

**(c) 用實機數據重做書上的例子**

假設一個 `util = 200` 的行程要被喚醒，比較放 A55(pd0) 還是 A76(pd4)：

```
【放 A55 (cpu0，pd0 = {0,1,2,3})】
  max_util = 200 × 1.25 = 250
  freq     = 1800000 × 250 / 422 = 1066 MHz   → 取 OPP 1200 MHz
  cost     = 84954
  假設 pd 內其他 CPU 都閒，sum_util = 200
  energy   = 84954 × 200 / 422 = 40263

【放 A76 (cpu4，pd4 = {4,5})】
  max_util = 250
  freq     = 2256000 × 250 / 1024 = 550 MHz   → 取 OPP 600 MHz
  cost     = 308365
  energy   = 308365 × 200 / 1024 = 60227

  40263 < 60227  →  EAS 選 A55                                 ✅ 與 Q30 實測一致
```

再看一個「重活」：`util = 800`
```
【A55】util_fits_cpu(800, 422)? 800×1.25 = 1000 > 422 → 放不下，pd0 沒有候選人
【A76】max_util = 1000 → freq = 2256000×1000/1024 = 2203 MHz → OPP 2208 MHz
       cost = 626340，energy = 626340 × 800 / 1024 = 489328
  → 只能選 A76                                                  ✅ 與 Q30 實測一致
```

**(d) 能效比一目了然**

同樣 `sum_util = 200`：

| 放哪裡 | 選中的 OPP | cost | energy |
|---|---|---|---|
| A55 (pd0) | 1200 MHz | 84954 | **40263** |
| A76 (pd4) | 600 MHz | 308365 | **60227**（1.50 倍） |

如果不看 OPP、直接比同頻率下的 cost：
`308367 / 82012 = 3.76` —— **A76 在同樣頻率下的能效係數是 A55 的 3.76 倍**。
這就是 [Q30](#q30) 「EAS OFF 時輕載跑上大核，多花 3.76 倍能量」的出處。

---

<a name="q36"></a>
## 36. 什麼是硬實時和軟實時？

### 結論

| | **硬實時（hard real-time）** | **軟實時（soft real-time）** |
|---|---|---|
| 定義 | 任務**必須**在確定的截止期限（deadline）前完成，**任何情況下都要保證** | 大多數情況下能及時完成即可，偶爾超時可以接受 |
| 超時後果 | **災難性**——結果不只是遲到，而是**錯誤** | 品質下降（掉幀、卡頓） |
| 衡量指標 | **最壞情況（WCET / worst-case latency）** | 平均值、p99 |
| 例子 | 飛控、ABS 煞車、氣囊、工業馬達伺服 | 影片播放、音訊、遊戲、UI |
| 作業系統 | RTOS（FreeRTOS/VxWorks/Zephyr）、Linux + **PREEMPT_RT** | 一般 Linux |

**關鍵不在「快」，在「可預測」。** 一個平均 1 µs、最壞 10 ms 的系統，
不如一個平均 100 µs、最壞 150 µs 的系統來得「實時」。

**標準 Linux 為什麼不是硬實時？** 有太多**無界**的延遲來源：
* 關中斷 / 關搶佔的臨界區長度沒有上界；
* 自旋鎖持有時間沒有上界；
* softirq / tasklet 沒有優先級也不能被搶佔（本機沒開 `threadirqs`）；
* 記憶體回收、`stop_machine()`、cpufreq 換頻、TLB shootdown IPI…

**PREEMPT_RT 做了什麼？** 把 spinlock 變成可睡眠的 rtmutex、
中斷處理常式執行緒化、實作優先級繼承、把 softirq 也執行緒化。

**本機的搶佔模型**（這一題答完一定要提）：

```bash
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep -E "^CONFIG_PREEMPT"'
```
```
CONFIG_PREEMPT_VOLUNTARY_BUILD=y
CONFIG_PREEMPT_VOLUNTARY=y
CONFIG_PREEMPT_NOTIFIERS=y
```
**沒有 `CONFIG_PREEMPT`、沒有 `CONFIG_PREEMPT_RT`、
甚至沒有 `CONFIG_PREEMPT_COUNT`。**

四種模型的比較：

| 模型 | 核心態能否被搶佔 | `preempt_disable()` | 本機 |
|---|---|---|---|
| `PREEMPT_NONE` | 否 | nop | |
| **`PREEMPT_VOLUNTARY`** | **否**（只在 `might_sleep()`/`cond_resched()` 讓出） | **nop（無 PREEMPT_COUNT）** | **✅** |
| `PREEMPT` | 是（`preempt_count == 0` 時） | 遞增計數 | |
| `PREEMPT_RT` | 是（連 spinlock 內都可以） | 遞增計數 | |

> **書目**：奔跑吧 §8.5「實時調度」開頭與 §8.5.2「Linux 內核實時性改進」表 8.15
> （第 5853 行起）。
> **注意**：書上 §8.5.1 明確假設「我們假設 Linux 操作系統已經打開了內核搶佔功能，
> 即配置了 CONFIG_PREEMPT 宏」，**本機不是**，所以延時會比書上的模型更差。

### 實機驗證

**(a) 沒有 `CONFIG_PREEMPT_COUNT` 的直接後果：`preempt_disable()` 是空的**

`sched_probe.ko` 在行程上下文裡 `preempt_disable()` 之後印 `preempt_count()`：

```bash
ssh radxa@192.168.68.57 'sudo insmod ~/exp/sched/sched_probe.ko test_atomic=1; \
    sudo rmmod sched_probe; sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A8 "Ch9 Q12"'
```
```
== Ch9 Q12  在原子上下文呼叫 schedule() ==
   (a) 先看軟中斷（timer）上下文的 preempt_count：
[timer softirq] in_interrupt()=256 preempt_count=0x100
[timer softirq] in_atomic()=1 -> 若此時呼叫 schedule()，schedule_debug() 會噴 "BUG: scheduling while atomic"
   (b) 行程上下文 + preempt_disable() + schedule() -> 應該噴 BUG splat：
       呼叫前 preempt_count = 0x0, in_atomic() = 0     ← ★ preempt_disable() 完全沒作用
       呼叫後 preempt_count = 0x0
```

* **軟中斷上下文** `preempt_count = 0x100` = `SOFTIRQ_OFFSET`，`in_atomic() = 1` ✅
  （hardirq/softirq/NMI 這幾個欄位**永遠**維護）
* **行程上下文 `preempt_disable()` 之後 `preempt_count` 還是 0** ——
  因為 `CONFIG_PREEMPT_COUNT=n` 時 `preempt_disable()` 只剩 `barrier()`。

**這代表本機的 `BUG: scheduling while atomic` 檢查只抓得到「中斷上下文」，
抓不到「spinlock 臨界區裡睡眠」這種 bug。**

**(b) 「軟實時」的具體數字**

見 [Q37](#q37)：本機 SCHED_FIFO 在滿載下的最壞喚醒延時是 **9.6 ms**——
比 hard real-time 系統可接受的範圍（通常要求 < 100 µs 且有界）差了兩個數量級。
**ROCK 5B + Debian 桌面是徹頭徹尾的軟實時系統。**

---

<a name="q37"></a>
## 37. 如何計算實時系統的延時？

### 結論

實時延時 = 從**外設中斷發生**到**目標行程真的執行第一條指令**，
書上圖 8.43 把它拆成四段：

```
 T0 ────────── T1 ────────────── T2 ─────────── T3 ────────── T4
 中斷發生      CPU 響應中斷      中斷處理完成    調度器選中 A   A 開始跑
     │             │                  │              │
     └ 中斷延時 ───┘                  │              │
       (interrupt latency)            │              │
                   └─ 中斷處理延時 ───┘              │
                      (interrupt handling latency)   │
                                      └─ 調度延時 ───┘
                                         (scheduling latency)
                                                     └ 上下文切換延時 ┘

實時延時 = 中斷延時 + 中斷處理延時 + 調度延時 + 上下文切換延時
```

**怎麼量？**

| 方法 | 工具 | 量到什麼 |
|---|---|---|
| **端到端**（最實用） | `cyclictest` / 本文的 `rt_latency.c` | timer 到期時刻 → 行程醒來時刻 |
| 分段 | `preemptirqsoff` / `irqsoff` / `preemptoff` tracer | 最長關中斷/關搶佔區段 |
| 分段 | `wakeup` / `wakeup_rt` tracer | 最長喚醒延時 |
| 事件 | `ftrace` 的 `irq_handler_entry/exit`、`sched_waking`、`sched_switch` | 每一段的實際時間 |
| 中斷延時 | GPIO 迴路 + 示波器 | 硬體層級最準 |

**本機只編了三個 tracer**：

```bash
ssh radxa@192.168.68.57 'cat /sys/kernel/debug/tracing/available_tracers'
```
```
blk function_graph function nop
```
**沒有 `irqsoff` / `preemptirqsoff` / `wakeup_rt`**（`CONFIG_IRQSOFF_TRACER` 等沒開），
所以只能用端到端測量 + `sched_switch` 事件分析。

**報告延時一定要報「最壞值」，不是平均值。**

> **書目**：奔跑吧 §8.5.1「實時延時分析」與圖 8.43~8.46（第 5865~5914 行）。

### 實機驗證

`rt_latency.c` 是迷你版 cyclictest：`clock_nanosleep(TIMER_ABSTIME)` 每 1 ms 醒一次，
量「應該醒的絕對時刻」到「真的跑起來」的差。

**一個必踩的坑：預設 timer slack = 50 µs。**
`prctl(PR_SET_TIMERSLACK, 1)` 之前量到的 p50 是 70 µs，清掉之後只剩 15.5 µs——
50 µs 全是 `current->timer_slack_ns` 造成的。（實時行程核心會自動忽略 slack。）

**另一個坑：`CONFIG_RT_GROUP_SCHED=y` + cgroup v2 → 只有 root cgroup 的行程能設 SCHED_FIFO。**
```bash
ssh radxa@192.168.68.57 'sudo chrt -f 80 /bin/true'
```
```
chrt: failed to set pid 0's policy: Operation not permitted
```
要先 `echo $$ > /sys/fs/cgroup/cgroup.procs`。

**(a) 系統空閒**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "cd /tmp; echo \$\$ > /sys/fs/cgroup/cgroup.procs
echo \"A) SCHED_OTHER:\"; ./rt_latency 1000 20000 0 4
echo \"B) SCHED_FIFO 80:\"; ./rt_latency 1000 20000 80 4"'
```
```
A) SCHED_OTHER, 空閒, cpu4
   min=2.8us  avg=18.8us  p50=17.3us  p99=35.9us  p99.9=56.3us  max=82.9us
   直方圖(us): [2-4)=102 [4-8)=246 [8-16)=611 [16-32)=17605 [32-64)=1429 [64-128)=7

B) SCHED_FIFO 80, 空閒, cpu4
   min=2.2us  avg=15.1us  p50=15.5us  p99=60.3us  p99.9=64.4us  max=71.2us
   直方圖(us): [2-4)=4505 [4-8)=181 [8-16)=8391 [16-32)=5932 [32-64)=966 [64-128)=25
```

**(b) 8 顆 CPU 全部跑 busy loop**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "cd /tmp; echo \$\$ > /sys/fs/cgroup/cgroup.procs
for c in 0 1 2 3 4 5 6 7; do taskset -c \$c sh -c \"while :; do :; done\" & done; sleep 2
echo \"C1) SCHED_OTHER:\"; ./rt_latency 1000 20000 0 4
echo \"C2) SCHED_FIFO 80:\"; ./rt_latency 1000 20000 80 4
kill %1 %2 %3 %4 %5 %6 %7 %8"'
```
```
C1) SCHED_OTHER, 滿載
   min=2.6us  avg=110.2us  p50=3.6us  p99=4826us  p99.9=11385us  max=21769us
C2) SCHED_FIFO 80, 滿載
   min=2.8us  avg=59.6us   p50=3.7us  p99=1804us  p99.9=8798us   max=9574us
```

**整理成一張表：**

| 場景 | min | p50 | p99 | p99.9 | **max** |
|---|---|---|---|---|---|
| OTHER / 空閒 | 2.8 µs | 17.3 µs | 35.9 µs | 56.3 µs | **82.9 µs** |
| FIFO / 空閒 | 2.2 µs | **15.5 µs** | 60.3 µs | 64.4 µs | **71.2 µs** |
| OTHER / 滿載 | 2.6 µs | 3.6 µs | 4826 µs | 11385 µs | **21769 µs** |
| **FIFO / 滿載** | 2.8 µs | **3.7 µs** | **1804 µs** | 8798 µs | **9574 µs** |

**三個結論：**
1. **SCHED_FIFO 有用**：滿載下 p99 從 4.8 ms 降到 1.8 ms，max 從 21.8 ms 降到 9.6 ms
   （改善 2.3 倍），但**沒有把最壞值壓進微秒級**。
2. **空閒時反而 p50 較高（15~17 µs）**：因為 CPU 進了 idle state，
   要付退出延時（見 [Q38](#q38)）；滿載時 CPU 一直醒著，p50 只有 3.6 µs。
3. **最壞值 9.6 ms 就是「軟實時」的鐵證**——一個硬實時系統不可能容忍
   p99.9 = 8.8 ms 的抖動。原因見 [Q40](#q40)。

---

<a name="q38"></a>
## 38. 請列舉產生中斷延時的場景。

### 結論

「中斷延時」= T0（外設拉中斷線）→ T1（CPU 進入 IRQ handler 第一條指令）。

| # | 場景 | 本機的具體數字 / 證據 |
|---|---|---|
| 1 | **CPU 正在關中斷的臨界區裡**（`spin_lock_irqsave()`、`local_irq_disable()`、`raw_spin_lock_irq()`） | 沒有上界；`__schedule()` 本身就關中斷 |
| 2 | **CPU 在更高優先級的中斷處理常式裡**（GICv3 支援中斷優先級與搶佔，但 Linux 預設把所有 IRQ 設成同一優先級，所以**一個 handler 執行期間其他 IRQ 全部等**） | 見下 (b) |
| 3 | **CPU 處於 idle 狀態，要先喚醒** | **`cpu-sleep` 的 exit latency = 220 µs** |
| 4 | **CPU 在低頻率**（schedutil 剛降到 408 MHz），進中斷的指令跑得慢 | 408 MHz vs 2256 MHz = 5.5 倍 |
| 5 | **中斷控制器的排隊/仲裁**（GIC distributor → redistributor → CPU interface） | GICv3，通常 < 1 µs |
| 6 | **TLB/cache miss**：中斷向量表、handler 程式碼不在 cache 裡 | ARM64 的 `VBAR_EL1` 向量表 |
| 7 | **`stop_machine()`**（CPU hotplug、module load、`text_poke`）把所有 CPU 停下來 | `insmod` 時可見 |
| 8 | **虛擬化 / TrustZone**：SMC 呼叫進 EL3（PSCI、SCMI）期間 | RK3588 換頻走 SCMI mailbox |
| 9 | **NMI / SError / FIQ**（ARM64 的 pseudo-NMI 未開） | — |

### 實機驗證

**(a) cpuidle 的退出延時是最大的一項**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "for s in /sys/devices/system/cpu/cpu4/cpuidle/state*; do
  echo \"\$(cat \$s/name)  exit_latency=\$(cat \$s/latency)us  target_residency=\$(cat \$s/residency)us\"
done"'
```
```
WFI        exit_latency=1us    target_residency=1us
cpu-sleep  exit_latency=220us  target_residency=1000us
```

**RK3588 的 `cpu-sleep`（power-gating）退出要 220 µs！**
這解釋了為什麼「空閒時的延時反而比滿載時高」：

```
空閒：p50 = 15.5 µs   ← 大多落在 WFI（1 µs），偶爾落到 cpu-sleep
滿載：p50 =  3.6 µs   ← CPU 從不 idle，沒有 exit latency
```

驗證：把 `cpu-sleep` 停掉再量一次

```bash
ssh radxa@192.168.68.57 'sudo bash -c "cd /tmp; echo \$\$ > /sys/fs/cgroup/cgroup.procs
for c in 0 1 2 3 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu\$c/cpuidle/state1/disable; done
./rt_latency 1000 20000 80 4
for c in 0 1 2 3 4 5 6 7; do echo 0 > /sys/devices/system/cpu/cpu\$c/cpuidle/state1/disable; done"'
```
```
min=27.3us  avg=73.8us  p50=70.0us  p99=73.8us  p99.9=107.6us  max=10231.9us
```
（這一輪是在還沒清 timer slack 前跑的，所以 p50 = 70 µs；重點是
**停掉深層 idle 之後 p99.9 從 100 µs 降不下來**，因為瓶頸換成了別的。）

**(b) 中斷處理期間其他中斷被擋住**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
T=/sys/kernel/debug/tracing; echo 0 > \$T/events/enable; echo > \$T/trace
echo 1 > \$T/events/irq/irq_handler_entry/enable
echo 1 > \$T/events/irq/irq_handler_exit/enable
echo 1 > \$T/tracing_on; sleep 0.5; echo 0 > \$T/tracing_on
grep irq_handler \$T/trace | head -8; echo 0 > \$T/events/enable"'
```

trace 的 flag 欄位 `d.h..` 中的 `h` 就代表 hardirq context、`d` 代表關中斷。
handler 執行期間 CPU 對同一優先級的其他 IRQ 完全不回應。

**(c) 關中斷臨界區——本機沒有 `irqsoff` tracer，只能靠源碼審查**

```bash
ssh radxa@192.168.68.57 'cat /sys/kernel/debug/tracing/available_tracers'
```
```
blk function_graph function nop
```
若要精確量測，需要重編核心開啟
`CONFIG_IRQSOFF_TRACER` / `CONFIG_PREEMPT_TRACER` / `CONFIG_HWLAT_TRACER`。

---

<a name="q39"></a>
## 39. 請列舉產生中斷處理延時的場景。

### 結論

「中斷處理延時」= T1（進入 handler）→ T2（該中斷完整處理完、喚醒目標行程）。

| # | 場景 | 說明 / 本機情況 |
|---|---|---|
| 1 | **上半部（hardirq）本身很長** | 有些驅動在 top half 做太多事；ARM64 上 hardirq 全程關中斷 |
| 2 | **下半部（softirq）被其他中斷打斷** | softirq 在開中斷下執行，隨時可被新的 IRQ 插隊（書上圖 8.45） |
| 3 | **softirq 積壓 → 丟給 `ksoftirqd`** | `__do_softirq()` 跑滿 `MAX_SOFTIRQ_TIME`(2 ms) 或 10 輪就喚醒 `ksoftirqd/N`，變成**普通 CFS 行程**，要排隊！ |
| 4 | **NET_RX / TIMER / RCU softirq 風暴** | 網路大流量時 `NET_RX` 可以吃掉整顆 CPU |
| 5 | **threaded IRQ 的喚醒延時** | `request_threaded_irq()` 的 `irq/N-xxx` kthread 預設 `SCHED_FIFO 50`，要先被排程 |
| 6 | **共享中斷線**：一條 IRQ 上掛多個 handler，要逐一呼叫 | `/proc/interrupts` 看得出來 |
| 7 | **中斷處理裡拿自旋鎖，鎖被別的 CPU 持有** | 自旋等待 |
| 8 | **timer / hrtimer 回呼串太長**：`__hrtimer_run_queues()` 一次跑完所有到期的 timer | 見下 |
| 9 | **RCU callback**（`RCU_SOFTIRQ`）一次處理太多 | `rcu_cpu_kthread` |

### 實機驗證

**(a) 本機的 softirq 分佈**

```bash
ssh radxa@192.168.68.57 'cat /proc/softirqs | awk "{print \$1, \$2+\$3+\$4+\$5+\$6+\$7+\$8+\$9}"'
```

**(b) `ksoftirqd` 確實存在而且是普通 CFS 行程**

```bash
ssh radxa@192.168.68.57 'ps -eo pid,cls,rtprio,ni,comm | grep -E "ksoftirqd|^ *[0-9]+ FF|irq/|sugov|migration"'
```

本機的三類「代跑」kthread 與它們的調度屬性：

| kthread | 調度類 | 意義 |
|---|---|---|
| `ksoftirqd/N` | **`TS`（SCHED_OTHER，nice 0）** | softirq 積壓時的接手者 —— **要跟一般行程搶 CPU** |
| `sugov:N` | **`DLN`（SCHED_DEADLINE）** | schedutil 換頻（只在 schedutil governor 下存在） |
| `migration/N` | stop 類（`ps` 顯示 `FF 99`） | 行程遷移 / CPU hotplug |
| `irq/N-xxx` | `FF` 50 | threaded IRQ（本機只有少數驅動用） |

**`ksoftirqd/N` 是 `SCHED_OTHER`** —— 一旦 softirq 積壓被丟給它，
延時就完全取決於 CFS 的排隊情況，這是很大的不確定性來源。
（下面 (c) 的 trace 就抓到 `ksoftirqd/2` 這個 pid 24 的普通行程插隊 40 µs。）

**(c) 在 [Q37](#q37) 的 trace 裡直接抓到 softirq 造成的延時**

```
sh-100768 [002] dNs.. 156307.951941: sched_waking: comm=ksoftirqd/2 pid=24 target_cpu=002
sh-100768 [002] dNs.. 156307.951950: sched_wakeup: comm=ksoftirqd/2 pid=24 target_cpu=002
sh-100768 [002] d.... 156307.951954: sched_switch: prev_comm=sh ==> next_comm=ksoftirqd/2
ksoftirqd/2-24 [002] d.... 156307.951994: sched_switch: prev_comm=ksoftirqd/2 prev_state=S ==> next_comm=sh
```
flag `dNs..` 的 **`s` 表示 softirq context**。
可以看到 softirq 積壓 → 喚醒 `ksoftirqd/2` → 它插隊跑了 **40 µs** → 才還給 `sh`。
如果此時有一個實時行程在等，這 40 µs 就是它的延時。

**(d) `threadirqs` 沒有開**

```bash
ssh radxa@192.168.68.57 'cat /proc/cmdline; ls /sys/kernel/debug/tracing/events/irq/'
```
沒有 `threadirqs` 參數 → 大部分 IRQ 走傳統 hardirq + softirq 模型。
（PREEMPT_RT 會強制所有 IRQ 執行緒化，讓它們可以被排程與設優先級。）

---

<a name="q40"></a>
## 40. 請列舉產生調度延時的場景。

### 結論

「調度延時」= T2（目標行程被 `wake_up()` 設成 RUNNABLE）→ T3/T4（它真的上 CPU）。

| # | 場景 | 本機情況 |
|---|---|---|
| 1 | **核心不可搶佔**：喚醒發生在核心態，當前行程要跑到 `cond_resched()` 或返回使用者空間才會讓出 | **`PREEMPT_VOLUNTARY`，這是本機最大的來源** |
| 2 | **當前行程在自旋鎖臨界區** | 沒有 `PREEMPT_COUNT`，連 `preempt_enable()` 的搶佔點都沒有 |
| 3 | **就緒佇列裡有更高優先級的行程** | RT 行程排隊 |
| 4 | **CFS 的公平性**：被喚醒者 vruntime 不夠小，`wakeup_granularity(4 ms)` 擋著不讓搶佔 | `check_preempt_wakeup()` |
| 5 | **被喚醒到「錯的 CPU」**：`select_task_rq_fair()` 把它放到一顆很忙的 CPU 上 | 見 [Q28](#q28) |
| 6 | **RT throttling**：RT 行程用超過 `sched_rt_runtime_us/period` 會被強制下車 | 本機 950 ms / 1000 ms |
| 7 | **cgroup CFS bandwidth throttling** | `CONFIG_CFS_BANDWIDTH=y` |
| 8 | **`stop_machine()` / CPU hotplug / `text_poke`** | 全機停擺 |
| 9 | **長時間執行的 kworker / kthread** | **本機實測的主因，見下** |
| 10 | **上下文切換本身**（TLB/cache 冷、`switch_mm` 的 ASID 檢查） | 見 [Q45](#q45) |

### 實機驗證 —— 直接抓出本機那 10 ms 是誰造成的

在跑 `rt_latency`（SCHED_FIFO 80，釘在 CPU4）的同時，
用 ftrace 記錄 CPU4 上的 `sched_switch`，找出「rt_latency 兩次被排程之間的最大間隔」：

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
T=/sys/kernel/debug/tracing; cd /tmp; echo \$\$ > /sys/fs/cgroup/cgroup.procs
echo 0 > \$T/events/enable; echo > \$T/trace
echo 16 > \$T/tracing_cpumask                 # 只追 CPU4
echo 1 > \$T/events/sched/sched_switch/enable
for c in 0 1 2 3 4 5 6 7; do taskset -c \$c sh -c \"while :; do :; done\" & done
sleep 1; echo 1 > \$T/tracing_on
./rt_latency 1000 8000 80 4
echo 0 > \$T/tracing_on; kill %1 %2 %3 %4 %5 %6 %7 %8
awk \"/rt_latency/ {split(\\\$4,a,\\\":\\\"); t=a[1]+0;
     if(p>0 && t-p>0.002) printf \\\"gap %.3f ms  上一個跑的是: %s\n\\\", (t-p)*1000, \\\$0; p=t}\" \$T/trace | head -8
echo \"--- CPU4 上出現過的行程：\"
grep -o \"next_comm=[^ ]*\" \$T/trace | sort | uniq -c | sort -rn | head -10
echo ff > \$T/tracing_cpumask; echo 0 > \$T/events/enable"'
```

```
policy=SCHED_FIFO prio=80 cpu=4 period=1000us loops=8000
  min=3.8us  avg=72.5us  p50=5.6us  p99=2871.6us  p99.9=8818.8us  max=9200.3us

gap  9.957 ms  上一個跑的是: kworker/u16:6-99001 [004] d.... 157274.442608: sched_switch:
      prev_comm=kworker/u16:6 prev_pid=99001 prev_state=R+ ==> next_comm=rt_latency next_prio=19
gap  9.769 ms  ... prev_comm=kworker/u16:6 prev_state=R+ ==> next_comm=rt_latency
gap  9.803 ms  ... prev_comm=kworker/u16:6 prev_state=R+ ==> next_comm=rt_latency
gap 10.057 ms  ... prev_comm=kworker/u16:6 prev_state=R+ ==> next_comm=rt_latency
gap  9.800 ms  ... prev_comm=kworker/u16:2 prev_state=R+ ==> next_comm=rt_latency
gap  9.810 ms  ... prev_comm=kworker/u16:2 prev_state=R+ ==> next_comm=rt_latency
gap  9.810 ms  ... prev_comm=kworker/u16:2 prev_state=R+ ==> next_comm=rt_latency
gap  9.961 ms  ... prev_comm=kworker/u16:2 prev_state=R+ ==> next_comm=rt_latency

--- CPU4 上出現過的行程：
   6317 next_comm=sh                ← 我們自己放的 busy loop
   6101 next_comm=rt_latency
    115 next_comm=kscreenlocker_g
    102 next_comm=napi/phy0-9       ← 網路 NAPI kthread
    102 next_comm=kworker/u16:6     ← ★ 元兇
     99 next_comm=QSGRenderThread   ← Qt 場景圖（VNC 桌面）
     65 next_comm=napi/phy0-11
     58 next_comm=kworker/u16:2
     42 next_comm=napi/phy0-10
     38 next_comm=Xtigervnc         ← VNC 伺服器
     36 next_comm=x11vnc
```

**分析：**

1. **所有 ~10 ms 的 gap，前一個執行的都是 `kworker/u16:N`（unbound workqueue worker）**，
   而且 `prev_state=R+` 表示它是**被搶佔**下去的（還在就緒佇列上）。
2. `next_prio=19` 就是我們的 FIFO-80 行程（`prio = 99 - 80 = 19`）。
3. **一個 SCHED_FIFO 80 的行程竟然要等一個普通 kworker 10 ms** ——
   唯一的可能是：**它在那 10 ms 裡根本不是 RUNNABLE 狀態**，
   也就是**喚醒它的 hrtimer 本身晚了 10 ms 才到期**。
   結合 `CONFIG_PREEMPT_VOLUNTARY`（核心態不可搶佔），
   最可能的解釋是 `kworker/u16` 在核心態執行了一段很長、
   沒有任何 `cond_resched()` 的工作，把 CPU4 的 timer 中斷處理與後續的
   `hrtimer_run_queues()` 一起延後了。
4. 這台機器跑著 **VNC 桌面**（`Xtigervnc`、`x11vnc`、`QSGRenderThread`、
   `kscreenlocker`）與**網路 NAPI kthread**，
   這些正是產生 unbound work 的來源。

**結論：本機調度延時的最壞值來自「不可搶佔的核心態 kworker」，
和書上圖 8.46「自旋鎖臨界區」屬於同一類問題。**
要改善的話：
* 換 `CONFIG_PREEMPT`（甚至 `PREEMPT_RT`）；
* 用 `isolcpus=` / `cpuset` 把實時任務的 CPU 隔離；
* `nohz_full=` 減少 tick；
* 把 unbound workqueue 綁到非實時 CPU（`/sys/devices/virtual/workqueue/cpumask`）。

驗證第 3 點的補充實驗（換 `performance` governor 排除換頻因素）：

```
performance governor + 滿載, SCHED_FIFO 80: p99=1972us  max=9611us
performance governor + 滿載, SCHED_OTHER  : p99=5074us  max=22657us
```
**換 governor 沒有改善最壞值** → 排除「cpufreq 換頻（SCMI mailbox）」這個嫌疑犯。

---

<a name="q41"></a>
## 41. 調度的時機是什麼？操作系統在什麼時候會發生調度？

### 結論

分成**「設 `TIF_NEED_RESCHED` 旗標」**與**「真的呼叫 `__schedule()`」**兩件事。

**A. 誰會設 `TIF_NEED_RESCHED`（`resched_curr()`）：**

| 來源 | 路徑 |
|---|---|
| 時鐘節拍 | `scheduler_tick() → task_tick_fair() → check_preempt_tick()` |
| 喚醒行程 | `try_to_wake_up() → check_preempt_curr() → check_preempt_wakeup()` |
| 新建行程 | `wake_up_new_task() → check_preempt_curr()` |
| 改優先級 | `set_user_nice()` / `sched_setscheduler()` / rt-mutex PI |
| 負載均衡 | `attach_task()`、`active_load_balance_cpu_stop()` |
| cgroup 頻寬 | `throttle_cfs_rq()` / `unthrottle_cfs_rq()` |
| CPU 下線 | `sched_cpu_deactivate()` |
| 使用者主動 | `sched_yield()` |

**B. 什麼時候真的切換：**

| # | 時機 | 條件 | 本機 |
|---|---|---|---|
| 1 | **主動阻塞**（voluntary） | `mutex_lock()`、`wait_event()`、`msleep()`、`read()` 阻塞…最終呼叫 `schedule()` | ✅ 一定會 |
| 2 | **返回使用者空間前** | `ret_to_user → exit_to_user_mode_loop()` 檢查 `_TIF_NEED_RESCHED` | ✅ 一定會 |
| 3 | **中斷返回核心態前** | `el1_interrupt → arm64_preempt_schedule_irq()`，需要 `CONFIG_PREEMPTION` | **❌ 本機沒有** |
| 4 | **`preempt_enable()`** | `preempt_count` 歸零時檢查，需要 `CONFIG_PREEMPTION` | **❌ 本機沒有** |
| 5 | **`cond_resched()` / `might_sleep()`** | `PREEMPT_VOLUNTARY` 的顯式讓出點 | ✅ **本機核心態唯一的搶佔點** |

`schedule()` 的三個變體：

```c
schedule()                 /* 通用 */
preempt_schedule()         /* preempt_enable() 觸發，PREEMPTION only */
preempt_schedule_irq()     /* 中斷返回核心態觸發，PREEMPTION only */
schedule_timeout(t)        /* 睡到逾時 */
```

> **書目**：奔跑吧 §8.1.5 開頭列的三種時機（第 1394~1420 行）與
> Ch9 §9.3.5 圖 9.16/9.17（可搶佔 vs 不可搶佔核心的差別）。

### 實機驗證

**(a) 本機沒有核心搶佔**

```bash
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep -E "^CONFIG_PREEMPT(ION)?=|^CONFIG_PREEMPT_VOLUNTARY="'
```
```
CONFIG_PREEMPT_VOLUNTARY=y
```
沒有 `CONFIG_PREEMPTION=y` → `arm64_preempt_schedule_irq()` 整個被編譯掉：

```bash
grep -n -B3 -A12 "static void __sched arm64_preempt_schedule_irq" arch/arm64/kernel/entry-common.c
```
```c
#ifdef CONFIG_PREEMPTION
static void __sched arm64_preempt_schedule_irq(void)
{
	if (!need_irq_preemption())  return;
	...
	preempt_schedule_irq();
}
#else
static inline void arm64_preempt_schedule_irq(void) { }
#endif
```

**(b) 三種切換的實際樣貌（同一份 trace）**

```bash
ssh radxa@192.168.68.57 'sudo /tmp/sched_trace.sh switch'
```

| trace 行 | `prev_state` | 對應時機 |
|---|---|---|
| `prev_comm=listener prev_state=S ==> next_comm=swapper/2` | **S**（睡眠） | **① 主動阻塞** — `listener` 呼叫 `read()`/`epoll_wait()` 睡了 |
| `prev_comm=sh prev_state=R ==> next_comm=sh` | **R**（仍可執行） | **② 返回使用者空間前被搶佔** — tick 設了旗標 |
| `prev_comm=kworker/2:2 prev_state=I ==> next_comm=sh` | **I**（idle kthread） | ① 的變體 — worker 做完事回去睡 |
| `<idle>-0 prev_state=R ==> next_comm=listener` | R | idle → 有人被喚醒 |

**(c) `dNh..` → `d....` 證明「中斷裡只設旗標，返回時才切」**

```
<idle>-0    [002] dNh.. 156307.922785: sched_wakeup:  comm=listener pid=968 target_cpu=002
<idle>-0    [002] d.... 156307.922791: sched_switch:  prev_comm=swapper/2 ==> next_comm=listener
```
`sched_wakeup` 帶 `h`（hardirq）與 `N`（need_resched 已置位），
6 µs 後的 `sched_switch` 已經沒有 `h` —— **切換發生在中斷返回路徑上，
不在中斷處理常式內部**。

**(d) `cond_resched()` 是本機核心態唯一的讓出點**

```bash
ssh radxa@192.168.68.57 'sudo grep -c "^cond_resched$\|^__cond_resched$" \
    /sys/kernel/debug/tracing/available_filter_functions'
```
在 `PREEMPT_VOLUNTARY` 下，`might_sleep()` 內含 `_cond_resched()`，
所以每一個可能睡眠的核心 API 都是一個潛在的搶佔點。
反過來說，**一段沒有任何 `might_sleep()` 的長核心迴圈就會造成 [Q40](#q40) 那 10 ms 的延時**。

---

<a name="q42"></a>
## 42. 如何合理選擇下一個進程？

### 結論

`pick_next_task()`（`kernel/sched/core.c`）—— **按調度類優先級由高到低問一輪，誰先給就是誰。**

```c
static inline struct task_struct *
__pick_next_task(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	const struct sched_class *class;
	struct task_struct *p;

	/* 快路徑：如果 rq 上全都是 CFS 行程，直接問 fair */
	if (likely(!sched_class_above(prev->sched_class, &fair_sched_class) &&
		   rq->nr_running == rq->cfs.h_nr_running)) {
		p = pick_next_task_fair(rq, prev, rf);
		if (unlikely(p == RETRY_TASK)) goto restart;
		if (!p) {                              /* 沒有 CFS 行程 → idle */
			put_prev_task(rq, prev);
			p = pick_next_task_idle(rq);
		}
		return p;
	}
restart:
	put_prev_task_balance(rq, prev, rf);
	for_each_class(class) {                    /* 慢路徑：逐類詢問 */
		p = class->pick_next_task(rq);
		if (p) return p;
	}
	BUG();                                     /* idle_sched_class 永遠有貨 */
}
```

**五個調度類，優先級由高到低**（`kernel/sched/sched.h` 的 linker section 排序）：

| # | 調度類 | policy | 選誰 | 用途 |
|---|---|---|---|---|
| 1 | `stop_sched_class` | — | 唯一的 `migration/N` kthread | CPU hotplug、行程遷移、`stop_machine()` |
| 2 | `dl_sched_class` | `SCHED_DEADLINE` | **absolute deadline 最早**的（EDF + CBS），紅黑樹 | 硬實時 |
| 3 | `rt_sched_class` | `SCHED_FIFO`/`SCHED_RR` | **prio 最小**的；同 prio FIFO 先到先跑、RR 輪轉（本機 `sched_rr_timeslice_ms`） | 軟實時 |
| 4 | `fair_sched_class` | `SCHED_NORMAL`/`BATCH`/`IDLE` | **vruntime 最小**（紅黑樹最左），O(1) | 一般 |
| 5 | `idle_sched_class` | — | `swapper/N` | 沒事做 |

**CFS 內部的 `pick_next_entity()`：**

```c
static struct sched_entity *pick_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
	struct sched_entity *left = __pick_first_entity(cfs_rq);   /* 紅黑樹最左 */
	struct sched_entity *se;

	if (!left || (curr && entity_before(curr, left)))
		left = curr;
	se = left;

	/* buddy 機制：儘量選 cache 熱的 */
	if (cfs_rq->skip && cfs_rq->skip == se) { ... }
	if (cfs_rq->last && wakeup_preempt_entity(cfs_rq->last, left) < 1)
		se = cfs_rq->last;      /* LAST_BUDDY：剛剛把 CPU 讓出去的那個 */
	if (cfs_rq->next && wakeup_preempt_entity(cfs_rq->next, left) < 1)
		se = cfs_rq->next;      /* NEXT_BUDDY：剛被喚醒的那個 */
	return se;
}
```
本機 features：**`NO_NEXT_BUDDY LAST_BUDDY`**（NEXT_BUDDY 關、LAST_BUDDY 開）。

**組調度時要逐層往下鑽：**

```c
do {
	se = pick_next_entity(cfs_rq, curr);
	cfs_rq = group_cfs_rq(se);
} while (cfs_rq);
p = task_of(se);
```

> **書目**：奔跑吧 §8.1.5 後半「pick_next_task() 函數」（第 1500~1552 行）。

### 實機驗證

**(a) 五個調度類在本機都活著**

```bash
ssh radxa@192.168.68.57 'ps -eo cls --no-headers | sort | uniq -c'
ssh radxa@192.168.68.57 'ps -eo pid,cls,rtprio,ni,comm --sort=cls | awk "NR==1 || \$2!=p {print; p=\$2}"'
```
本機只會看到兩種 `CLS`：
* **`TS`**（`SCHED_OTHER`）—— 絕大多數行程，走 `fair_sched_class`；
* **`FF`**（`SCHED_FIFO`）—— `migration/N`（其實是 stop 類，`ps` 一律顯示成 `FF 99`）、
  少數 threaded IRQ（`irq/N-xxx`，RTPRIO 50）。
  用 schedutil governor 時還會多出 **`DLN`（`SCHED_DEADLINE`）** 的 `sugov:N`。

`idle_sched_class` 的 `swapper/N`（pid 0）**不會出現在 `ps` 裡**，
要從 `/sys/kernel/debug/sched/debug` 的 `.curr->pid : 0` 才看得到。

**(b) RT 一定壓過 CFS**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "cd /tmp; echo \$\$ > /sys/fs/cgroup/cgroup.procs
taskset -c 4 sh -c \"while :; do :; done\" & sleep 1
./rt_latency 1000 5000 80 4 | head -2
kill %1"'
```
```
policy=SCHED_FIFO prio=80 cpu=4 period=1000us loops=5000
  min=2.8us  p50=3.7us ...
```
p50 = 3.7 µs —— CFS 的 busy loop 完全擋不住 FIFO 行程。

**(c) buddy 設定**

```bash
ssh radxa@192.168.68.57 'sudo cat /sys/kernel/debug/sched/features | tr " " "\n" | grep -i buddy'
```
```
NO_NEXT_BUDDY
LAST_BUDDY
CACHE_HOT_BUDDY
```

**(d) `pick_next_task_fair` 是可追蹤的**

```bash
ssh radxa@192.168.68.57 'grep -x pick_next_task_fair /sys/kernel/debug/tracing/available_filter_functions'
```
```
pick_next_task_fair
```

---

<a name="q43"></a>
## 43. 什麼是進程上下文？進程上下文包含哪些內容？

### 結論

**行程上下文 = 為了讓一個行程「原封不動地繼續跑」，必須保存/恢復的全部狀態。**
在 Linux/ARM64 上分成三層：

| 層 | 內容 | 存在哪裡 | 大小（本機實測） |
|---|---|---|---|
| **① 硬體上下文（核心態切換用）** | `x19~x28`（callee-saved）、`fp(x29)`、`sp`、`pc(lr)` | `task_struct.thread.cpu_context` | **104 B**（13 × u64） |
| **② 使用者態現場（異常/中斷用）** | `x0~x30`、`sp_el0`、`pc(elr_el1)`、`pstate(spsr_el1)`、`orig_x0`、`syscallno`、`sdei_ttbr1`、`pmr_save`、`stackframe[2]`、`lockdep_hardirqs`、`exit_rcu` | **核心堆疊頂端的 `pt_regs`** | **336 B** |
| **③ 其他架構狀態** | FPSIMD/SVE 暫存器、TLS(`tpidr_el0`/`tpidrro_el0`)、硬體斷點、PAC keys、MTE、SSBS、`fault_address`/`fault_code` | `task_struct.thread`（`thread_struct`） | **1024 B** |
| **④ 軟體上下文** | `mm`（位址空間 + ASID）、`files`、`fs`、`signal`、`cred`、`nsproxy`、排程狀態（`se`/`rt`/`dl`/`prio`）… | `task_struct` 本體 | **3968 B** |

**為什麼 `cpu_context` 只有 x19~x28？**
根據 **AAPCS64**（ARM 64-bit 呼叫慣例）：
* `x0~x7` 傳參數/回傳值 —— **caller-saved**，呼叫者自己會存
* `x8` 間接結果、`x9~x15` 臨時 —— caller-saved
* `x16/x17` IP0/IP1（linker veneer）、`x18` 平台暫存器
* **`x19~x28` callee-saved** —— 被呼叫者必須保護，所以切換時要存
* `x29` FP、`x30` LR、`SP`

`cpu_switch_to()` 是一個**普通 C 函式**，編譯器已經幫呼叫者把 caller-saved
存在堆疊上了，所以只需要保 callee-saved + fp/sp/lr。

**「行程上下文」的另一個意思**（面試常混淆）：
`in_task()` / `in_interrupt()` 說的是**執行上下文**——
「現在執行的程式碼是代表某個行程在跑（可以睡眠、可以存取 `current`）」，
還是「在中斷上下文」（不能睡眠）。這一題問的是前一個意思，但值得順帶提。

> **書目**：奔跑吧 §8.1.6「switch_to() 函數」對 `thread_struct` / `cpu_context` 的說明
> （第 1800~1900 行）。

### 實機驗證

`sched_probe.ko`：

```
== Ch8 Q43~Q45  進程上下文（硬體上下文） ==
   sizeof(struct task_struct)   = 3968 B
   sizeof(struct thread_struct) = 1024 B
   sizeof(struct cpu_context)   = 104 B  (= 13 個 u64：x19~x28, fp, sp, pc)
   sizeof(struct pt_regs)       = 336 B  (中斷/異常現場，壓在核心堆疊頂端)
   THREAD_SIZE                  = 16384 B  (核心堆疊)
   offsetof(task_struct, thread)             = 2944
   offsetof(task_struct, thread.cpu_context) = 2944  (組語 THREAD_CPU_CONTEXT)
   cpu_context: x19=0 x28=72 fp=80 sp=88 pc=96
   current 的 cpu_context.pc = ret_from_fork+0x0/0x20, sp = 0xffff80000daabeb0
   current->stack = ffff80000daa8000, task_pt_regs(current) = ffff80000daabeb0
   read_sysreg(sp_el0) = 0xffff000066e5be00, current = ffff000066e5be00
```

逐項驗證：

| 項目 | 驗算 |
|---|---|
| `cpu_context` 104 B | 13 個成員 × 8 B ✅ |
| 成員偏移 x19=0 … pc=96 | 96/8 = 12 → pc 是第 13 個 ✅ 與 `entry.S` 的 store 順序一致 |
| `pt_regs` 336 B | 34 個 u64（x0~x30, sp, pc, pstate）= 272，再加 orig_x0/syscallno/pmr/stackframe/… = 336 ✅ |
| 核心堆疊 16 KB | `MIN_THREAD_SHIFT = 14` → `1<<14` ✅ |
| `pt_regs` 位置 | `stack(0x...daa8000) + 16384 − 336 − 16 = 0x...daabeb0` = `task_pt_regs()` ✅ |
| `SP_EL0 == current` | ARM64 用 SP_EL0 存 `task_struct` 指標（`CONFIG_THREAD_INFO_IN_TASK=y`）✅ |

`thread_struct` 佔 1024 B 是因為裡面內嵌了 `user_fpsimd_state`（512 B 的 v0~v31）
與 `debug_info`。

---

<a name="q44"></a>
## 44. 進程上下文保存到哪裡？

### 結論

**分兩個地方，取決於「為什麼切」：**

```
┌─ task_struct（在 slab 裡，3968 B）────────────────────────────────┐
│  ...                                                              │
│  struct thread_struct thread;    ← offsetof = 2944                │
│      struct cpu_context cpu_context;   ← x19~x28, fp, sp, pc      │  ← ① 主動切換
│      uw.tp_value / uw.fpsimd_state / sve_state / debug / ...      │  ← ③ 延遲保存
│  ...                                                              │
│  void *stack;  ──────────────┐                                    │
└──────────────────────────────┼────────────────────────────────────┘
                               ▼
        ┌─ 核心堆疊 16 KB（vmalloc，CONFIG_VMAP_STACK=y）──────────┐
        │ 低位址                                                   │
        │   ...                                                    │
        │   context_switch() 的 stack frame                        │
        │   __schedule()    的 stack frame                         │
        │   ret_to_user     的 stack frame                         │
        │   el0_irq         的 stack frame                         │
        │ ┌──────────────────────────────────────────┐             │
        │ │ struct pt_regs (336 B)                   │ ← ② 中斷現場│
        │ │  x0~x30, sp, pc(elr), pstate(spsr) ...   │             │
        │ └──────────────────────────────────────────┘             │
        │   [16 B 保留]                                            │
        │ 高位址 = stack + THREAD_SIZE                             │
        └──────────────────────────────────────────────────────────┘
```

| 什麼狀態 | 存在哪 | 誰寫的 |
|---|---|---|
| **① 核心態的 callee-saved 暫存器** | `task_struct.thread.cpu_context` | `cpu_switch_to()`（組語） |
| **② 使用者態全部暫存器（中斷/例外現場）** | **核心堆疊頂端的 `pt_regs`** | `kernel_entry` 巨集（`entry.S`） |
| **③ FPSIMD/SVE** | `task_struct.thread.uw.fpsimd_state` | `fpsimd_thread_switch()`，**lazy**：只有真的要換人用 FP 才存 |
| **④ TLS** | `thread.uw.tp_value` ↔ `tpidr_el0` | `tls_thread_switch()` |
| **⑤ 位址空間** | `mm->pgd`（→ TTBR0_EL1）、`mm->context.id`（→ TTBR1_EL1 的 ASID） | `switch_mm()` |
| **⑥ 硬體斷點 / PAC / MTE / SSBS** | `thread.debug` / `thread.keys_kernel` 等 | `__switch_to()` 裡各自的 helper |

**關鍵區別**：
* `cpu_context` 只在**行程切換**時用（13 個暫存器，很省）；
* `pt_regs` 在**每次進出核心**時用（34+ 個暫存器，很貴，但只在 EL0↔EL1 邊界）；
* **兩者是巢狀的**：一個行程可以有「使用者態現場（pt_regs）」躺在堆疊上，
  同時「核心態現場（cpu_context）」記著它在核心裡走到哪。

**`CONFIG_VMAP_STACK=y` 的意義**：核心堆疊用 `vmalloc()` 配置，兩端有 guard page，
溢位直接觸發缺頁而不是默默踩壞鄰居的 `task_struct`。

> **書目**：奔跑吧 §8.1.6、Ch9 §9.3.5 圖 9.20「進程調度的棧幀變化情況」。

### 實機驗證

**(a) 兩個位置的實際位址**

```
current->stack             = ffff80000daa8000    ← 核心堆疊底（vmalloc 區）
task_pt_regs(current)      = ffff80000daabeb0    ← pt_regs 在堆疊頂
current 的 cpu_context.sp  = 0xffff80000daabeb0  ← 新行程：sp 指向 pt_regs
current (task_struct)      = ffff000066e5be00    ← 在 linear map（slab）
```

* 核心堆疊在 `0xffff8000_xxxx`（**vmalloc 區**）→ `CONFIG_VMAP_STACK=y` ✅
* `task_struct` 在 `0xffff0000_xxxx`（**linear map**）→ 從 slab 配置 ✅
* **兩者位址空間分離**，這也是 `CONFIG_THREAD_INFO_IN_TASK=y` 的安全好處。

**(b) `THREAD_CPU_CONTEXT` 這個組語常數就是 offsetof**

```bash
grep -n "THREAD_CPU_CONTEXT" arch/arm64/kernel/asm-offsets.c
```
```c
DEFINE(THREAD_CPU_CONTEXT, offsetof(struct task_struct, thread.cpu_context));
```
模組印出 **2944**，`entry.S:830` 的 `mov x10, #THREAD_CPU_CONTEXT` 用的就是它。

**(c) 核心堆疊總量**

```bash
ssh radxa@192.168.68.57 'grep KernelStack /proc/meminfo; ps -eL --no-headers | wc -l'
```
```
KernelStack:   11088 kB
690
11088 / 690 = 16.07 kB/task    ✅ 印證 THREAD_SIZE = 16384
```

---

<a name="q45"></a>
## 45. 進程切換時需要切換哪些東西？

### 結論

**兩大步 + 若干雜項**（`context_switch()`，`kernel/sched/core.c:5201`）：

```c
static __always_inline struct rq *
context_switch(struct rq *rq, struct task_struct *prev,
	       struct task_struct *next, struct rq_flags *rf)
{
	prepare_task_switch(rq, prev, next);        /* next->on_cpu = 1，preempt notifier */

	arch_start_context_switch(prev);

	/* ── 第一步：切位址空間 ────────────────────────────── */
	if (!next->mm) {                            /* to kernel thread：借用 */
		enter_lazy_tlb(prev->active_mm, next);
		next->active_mm = prev->active_mm;
		if (prev->mm) mmgrab_lazy_tlb(prev->active_mm);
	} else {                                    /* to user task：真的換 */
		membarrier_switch_mm(rq, prev->active_mm, next->mm);
		switch_mm_irqs_off(prev->active_mm, next->mm, next);
		lru_gen_use_mm(next->mm);
		if (!prev->mm) {                        /* from kernel thread：還債 */
			rq->prev_mm = prev->active_mm;
			prev->active_mm = NULL;
		}
	}
	rq->clock_update_flags &= ~(RQCF_ACT_SKIP|RQCF_REQ_SKIP);
	prepare_lock_switch(rq, next, rf);

	/* ── 第二步：切核心堆疊 + 硬體上下文 ────────────────── */
	switch_to(prev, next, prev);
	barrier();

	return finish_task_switch(prev);            /* 由 next 執行，見 Q10 */
}
```

**完整清單：**

| # | 切什麼 | 在哪切 | ARM64 具體動作 | 本機證據 |
|---|---|---|---|---|
| 1 | **位址空間（頁表）** | `switch_mm_irqs_off()` → `check_and_switch_context()` → `cpu_do_switch_mm()` | 寫 **TTBR0_EL1**（使用者 PGD）；**TTBR1_EL1[63:48]** 寫 ASID；`isb` | [Q13](#q13)：`mm->context.id = 0x37fe0` |
| 2 | **ASID / TLB** | 同上 | generation 相同 → **不刷 TLB**；溢位才 `flush_context()` + IPI | generation = 3（開機 1.8 天才 3 次） |
| 3 | **核心堆疊** | `cpu_switch_to()` | `mov sp, x9` | `cpu_context.sp` |
| 4 | **callee-saved 暫存器 + fp/lr** | `cpu_switch_to()` | `stp x19..x28, x29, sp, lr` → `cpu_context` | 104 B |
| 5 | **`current` 指標** | `cpu_switch_to()` | `msr sp_el0, x1` | `SP_EL0 == current` ✅ |
| 6 | **FPSIMD / SVE** | `__switch_to() → fpsimd_thread_switch()` | **lazy**：設 `TIF_FOREIGN_FPSTATE`，真的用到才存/載 | `thread_struct` 1024 B |
| 7 | **TLS** | `tls_thread_switch()` | `tpidr_el0` / `tpidrro_el0` | |
| 8 | **硬體斷點/watchpoint** | `hw_breakpoint_thread_switch()` | `DBGBVR/DBGBCR` | |
| 9 | **CONTEXTIDR**（追蹤用） | `contextidr_thread_switch()` | 寫 pid 進 `CONTEXTIDR_EL1`（給 ETM/trace） | 需 `CONFIG_PID_IN_CONTEXTIDR` |
| 10 | **SSBS**（Spectre-v4） | `ssbs_thread_switch()` | `PSTATE.SSBS` | |
| 11 | **PAC keys** | `ptrauth_thread_switch_user/kernel()` | `APIAKey` 等 | `entry.S:850` |
| 12 | **MTE** | `mte_thread_switch()` | `GCR_EL1`、`TFSR_EL1` | |
| 13 | **記憶體屏障** | `__switch_to()` | **`dsb(ish)`** — 完成待處理的 TLB/cache 維護；也是 `membarrier` 需要的 | `process.c:609` |
| 14 | **統計/會計** | `prepare_task_switch()` / `finish_task_switch()` | `nvcsw`/`nivcsw`、`sched_info`、perf events、`rq->nr_switches` | `/proc/pid/sched` |

**不需要切的**（常見陷阱題）：
* `x0~x18`（caller-saved，編譯器已處理）
* 使用者態的 `pt_regs`（**留在各自的核心堆疊上**，不搬動）
* 檔案描述符表、signal handler 表（透過 `current` 指標間接切換，不用複製）
* 核心空間的頁表映射（TTBR1_EL1 的 BADDR 全系統共用 `swapper_pg_dir`）

> **書目**：奔跑吧 §8.1.6 全節（第 1552~1947 行），特別是最後
> 「進程切換可以總結為如下兩步：(1) 切換進程地址空間…(2) 切換到 next 進程的內核態棧和硬體上下文」。

### 實機驗證

**(a) `__switch_to()` 做了哪 8 件事（原始碼）**

```bash
sed -n '588,620p' arch/arm64/kernel/process.c
```
```c
struct task_struct *__switch_to(struct task_struct *prev, struct task_struct *next)
{
	struct task_struct *last;

	fpsimd_thread_switch(next);              /* ⑥ */
	tls_thread_switch(next);                 /* ⑦ */
	hw_breakpoint_thread_switch(next);       /* ⑧ */
	contextidr_thread_switch(next);          /* ⑨ */
	entry_task_switch(next);                 /* SP_EL0 相關 */
	ssbs_thread_switch(next);                /* ⑩ */
	erratum_1418040_thread_switch(next);
	ptrauth_thread_switch_user(next);        /* ⑪ */

	dsb(ish);                                /* ⑬ 完成 TLB/cache 維護 + membarrier */
	mte_thread_switch(next);                 /* ⑫ */
	...
	last = cpu_switch_to(prev, next);        /* ③④⑤ */
	return last;
}
```

**(b) TTBR1_EL1 確實同時帶著 ASID 與共用的核心頁表**

```
TTBR1_EL1 = 0x7fe0000001b2d001
             └┬─┘└──────┬─────┘
           ASID=32736   swapper_pg_dir 的 PA（所有行程一樣）
```
每次切換只有高 16 位變，低 48 位永遠不變 —— 印證「核心位址空間全系統共用，
使用者位址空間靠 ASID 區分」。

**(c) 一次切換要多少時間？**

從 [Q2](#q2) 的 trace，兩個 `sh` 互切的間隔是 13.333 ms，
`sched_switch` 事件本身的處理只有幾微秒。用 `perf` 量：

```bash
ssh radxa@192.168.68.57 'sudo perf stat -e context-switches,cpu-migrations -a sleep 3 2>&1 | tail -6'
```
```
             8,207      context-switches
                20      cpu-migrations

       3.008137851 seconds time elapsed
```
閒置的桌面系統每秒 **2700 次**行程切換、**7 次**跨 CPU 遷移。

**(d) 切換次數的累計統計**

```bash
ssh radxa@192.168.68.57 'sudo grep -A3 "^cpu#4" /sys/kernel/debug/sched/debug | head -3; \
                         grep ctxt /proc/stat'
```
```
cpu#4
  .nr_running                    : 0
  .nr_switches                   : 57896627      ← 光 CPU4 就 5789 萬次
ctxt 397976536                                    ← 全系統 3.98 億次
```
以 [Q43](#q43) 的 104 B `cpu_context` 計，光是存/載 callee-saved 暫存器
就搬了 `3.98e8 × 104 × 2 ≈ 83 GB` 的資料。
這就是為什麼 ARM64 只保 13 個暫存器而不是 34 個。

---

## 附錄：實驗環境的復原

本章實驗改動過機台的下列設定，跑完請記得還原：

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
# cpufreq governor（出廠是 ondemand；schedutil 才有 EAS）
for p in 0 4 6; do echo ondemand > /sys/devices/system/cpu/cpufreq/policy\$p/scaling_governor; done
echo 1 > /proc/sys/kernel/sched_energy_aware
echo 1 > /proc/sys/kernel/sched_autogroup_enabled
echo 0 > /proc/sys/kernel/sched_schedstats
echo 0 > /sys/kernel/debug/sched/verbose
for c in 0 1 2 3 4 5 6 7; do
    echo 0 > /sys/devices/system/cpu/cpu\$c/cpuidle/state1/disable 2>/dev/null
    echo 1 > /sys/devices/system/cpu/cpu\$c/online 2>/dev/null
done
T=/sys/kernel/debug/tracing
echo 0 > \$T/tracing_on; echo nop > \$T/current_tracer
echo 0 > \$T/events/enable; echo ff > \$T/tracing_cpumask; echo > \$T/trace
echo > \$T/set_ftrace_filter; echo > \$T/set_graph_function
rmmod sched_probe 2>/dev/null; true"'
```
