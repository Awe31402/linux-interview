# 第 9 章 進程管理之調試與案例分析 — 高頻面試題解答

> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588，4×A55 + 4×A76），`192.168.68.57`（帳密皆為 `radxa`）
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`
> **關鍵組態**：`CONFIG_SCHED_DEBUG=y`、`CONFIG_SCHEDSTATS=y`、`CONFIG_HZ=300`、
> **`CONFIG_PREEMPT_VOLUNTARY=y`（沒有 `CONFIG_PREEMPT_COUNT`）**、`CONFIG_FAIR_GROUP_SCHED=y`
>
> **書目對照**
> - 《奔跑吧 Linux 內核》（第二版）卷 1 第 9 章 —
>   `books/running-linux-kernel/running-kernel-1-txt/17_第9章_进程管理之调试与案例分析.txt`
>   （下稱「奔跑吧 §9.x」）
> - 第 8 章的細節請見 📝 [ch08_process_management_scheduling_and_load_balancing.md](./ch08_process_management_scheduling_and_load_balancing.md)
> - 核心程式碼路徑相對於本專案樹
>   `/home/awe/disk/yocto-rockchip-sdk/build/tmp/work-shared/rockchip-rk3588-rock-5b/kernel-source`
>
> ## ⚠️ 本章最重要的一件事：**調試節點在 5.13 / 5.16 全部搬家了**
>
> | 書上（Linux 5.0） | 本機（Linux 6.1） | 搬家的 commit |
> |---|---|---|
> | `/proc/sched_debug` | **`/sys/kernel/debug/sched/debug`** | `8a99b6833c88`（5.13） |
> | `/proc/sys/kernel/sched_latency_ns` | **`/sys/kernel/debug/sched/latency_ns`** | `8a99b6833c88` |
> | `/proc/sys/kernel/sched_min_granularity_ns` | **`/sys/kernel/debug/sched/min_granularity_ns`** | 同上 |
> | `/proc/sys/kernel/sched_wakeup_granularity_ns` | **`/sys/kernel/debug/sched/wakeup_granularity_ns`** | 同上 |
> | `/proc/sys/kernel/sched_migration_cost_ns` | **`/sys/kernel/debug/sched/migration_cost_ns`** | 同上 |
> | `/proc/sys/kernel/sched_nr_migrate` | **`/sys/kernel/debug/sched/nr_migrate`** | 同上 |
> | `/proc/sys/kernel/sched_domain/...` | **`/sys/kernel/debug/sched/domains/cpuN/domainN/`** | `3b87f136f8fc`（5.13） |
> | `/sys/kernel/debug/sched_features` | **`/sys/kernel/debug/sched/features`** | 同上 |
> | `/sys/kernel/debug/sched_debug`（開關） | **`/sys/kernel/debug/sched/verbose`** | 同上 |
>
> 只有 `sched_schedstats`、`sched_child_runs_first`、`sched_rt_*`、
> `sched_energy_aware`、`sched_autogroup_enabled`、`sched_cfs_bandwidth_slice_us`
> **還留在 `/proc/sys/kernel/`**。

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | 如何查看行程的調度資訊 | `/proc/PID/sched` 完整 40 個欄位 |
| [2](#q2) | 如何查看 CFS 的調度資訊 | **`/proc/sched_debug` 已不存在，改 debugfs** |
| [3](#q3) | 如何查看調度域的拓撲關係 | **RK3588 只有 1 層 MC 域、8 個單 CPU 組** |
| [4](#q4) | `sched_latency_ns` vs `sched_min_granularity_ns` | **24 ms / 3 ms，門檻 8 個行程，實測時間片全部命中** |
| [5](#q5) | 雙核 5 個行程，test 程式的核心流程 | **實測 100 ms 內從 5/0 收斂成 3/2** |
| [6](#q6) | 行程的本質是什麼 | — |
| [7](#q7) | 與行程優先級相關的概念 | 模組 dump 出 8 個欄位 |
| [8](#q8) | CPU 會不會區分 prev 和 next | — |
| [9](#q9) | next 行程的下一條語句是什麼 | **`cpu_context.pc = ret_from_fork`** |
| [10](#q10) | 時鐘中斷的中斷現場何時恢復 | ftrace + 堆疊佈局 |
| [11](#q11) | 新建行程從哪裡開始執行？會不會關中斷跑死 | **`schedule_tail <-ret_from_fork` 實機抓到** |
| [12](#q12) | 中斷處理函式裡能不能呼叫 `schedule()` | **實測軟中斷 `preempt_count=0x100`，`in_atomic()=1`** |
| [13](#q13) | 關中斷後直接 `schedule()` 會不會癱瘓 | **實測：回來時 `irqs_disabled()` 從 128 變 0** |

---

<a name="q1"></a>
## 1. 如何查看進程的調度信息？

### 結論

**四個地方，由淺入深：**

| 來源 | 需要什麼 | 看得到什麼 |
|---|---|---|
| `ps -eo pid,cls,rtprio,ni,psr,pcpu,comm` | — | policy、優先級、在哪顆 CPU |
| **`/proc/<pid>/sched`** | `CONFIG_SCHED_DEBUG=y` | vruntime、PELT、切換次數、uclamp… |
| **`/proc/<pid>/schedstat`** | `CONFIG_SCHEDSTATS=y` + `sched_schedstats=1` | `sum_exec_runtime / wait_sum / nr_switches` 三個數 |
| `/proc/<pid>/sched`（開了 schedstats 之後） | 兩者都要 | **多出 20 幾個統計欄位** |
| `/proc/<pid>/stat`、`/proc/<pid>/status` | — | utime/stime、`voluntary_ctxt_switches` |
| `perf sched` / ftrace | — | 逐次切換的時間軸 |

輸出函式：`proc_sched_show_task()`（`kernel/sched/debug.c`）。

> **書目**：奔跑吧 §9.1.1「查看與進程相關的調度信息」與圖 9.1~9.3（第 39~86 行）。

### 實機驗證

**(a) 先打開統計（預設是關的，因為有效能成本）**

```bash
ssh radxa@192.168.68.57 'cat /proc/sys/kernel/sched_schedstats'   # 預設 0
ssh radxa@192.168.68.57 'sudo bash -c "echo 1 > /proc/sys/kernel/sched_schedstats"'
```

**(b) 完整的 `/proc/PID/sched`（一個 25% duty 的週期性行程）**

```bash
ssh radxa@192.168.68.57 'cd /tmp && (./pelt_duty 3 6 time 2 8 >/dev/null & sleep 3; \
                                     cat /proc/$!/sched; wait)'
```

```
pelt_duty (102900, #threads: 1)
-------------------------------------------------------------------
se.exec_start                  :  157517147.088600   ← 上次開始執行的 rq->clock_task
se.vruntime                    :        842.409472   ← 虛擬時間（相對於它所在的 cfs_rq）
se.sum_exec_runtime            :        802.904285   ← 累計真正跑了 802.9 ms
se.nr_migrations               :                 0   ← 被 taskset 釘死，一次都沒遷移
--------------------------------------------------- 以下要 sched_schedstats=1
sum_sleep_runtime              :       2165.520333   ← 累計睡了 2.17 s
sum_block_runtime              :          0.000000   ← D 狀態（不可中斷）時間
wait_start                     :          0.000000
sleep_start                    :  157517147.088600
block_start                    :          0.000000
sleep_max                      :          6.157666   ← 單次最久睡眠
block_max                      :          0.000000
exec_max                       :          3.322667   ← 單次最久執行（= 1 個 tick！）
slice_max                      :          0.000000
wait_max                       :         12.774125   ← ★ 單次最久「在就緒佇列裡等」
wait_sum                       :         40.344621   ← ★ 累計等待
wait_count                     :               408   ← ★ 等了 408 次
iowait_sum                     :          0.000000
iowait_count                   :                 0
nr_migrations_cold             :                 0
nr_failed_migrations_affine    :                 8   ← ★ 負載均衡想搬它 8 次，被 affinity 擋掉
nr_failed_migrations_running   :                 0
nr_failed_migrations_hot       :                 0
nr_forced_migrations           :                 0
nr_wakeups                     :               371
nr_wakeups_sync                :                 0
nr_wakeups_migrate             :                 0
nr_wakeups_local               :               371   ← 371 次都在本地 CPU 醒來
nr_wakeups_remote              :                 0
nr_wakeups_affine              :                 0
nr_wakeups_affine_attempts     :                 0
nr_wakeups_passive             :                 0
nr_wakeups_idle                :                 0
avg_atom                       :          1.972737   ← 平均每次上 CPU 跑 1.97 ms
avg_per_cpu                    :          0.000001
nr_switches                    :               407
nr_voluntary_switches          :               372   ← 主動睡（nanosleep）
nr_involuntary_switches        :                35   ← 被搶佔
--------------------------------------------------- 以下永遠都有
se.load.weight                 :           1048576   ← nice 0 → 1024 << 10
se.avg.load_sum                :              1481
se.avg.runnable_sum            :           1521199
se.avg.util_sum                :           1518493
se.avg.load_avg                :                32
se.avg.runnable_avg            :                32
se.avg.util_avg                :                32
se.avg.last_update_time        :   157414849561600
se.avg.util_est.ewma           :                35   ← 5.1 新增，書上沒有
se.avg.util_est.enqueued       :                27
uclamp.min                     :                 0   ← 5.3 新增，書上沒有
uclamp.max                     :              1024
effective uclamp.min           :                 0
effective uclamp.max           :              1024
policy                         :                 0   ← SCHED_NORMAL
prio                           :               120   ← nice 0
clock-delta                    :               292
```

**怎麼讀這份報告：**
* `nr_voluntary_switches(372) ≫ nr_involuntary_switches(35)`
  → 這是個**互動型 / I/O 型**行程（主動睡覺為主），不是 CPU-bound。
* `exec_max = 3.32 ms` 正好是**一個 tick**（HZ=300 → 3.333 ms）
  → 它從來沒有連續跑超過一個節拍。
* `wait_max = 12.77 ms` ≈ **4 個 tick** → 最慘的一次它在就緒佇列排了 12.8 ms。
* `nr_failed_migrations_affine = 8` → 負載均衡器確實想搬它，被 `taskset` 擋下 8 次。
* `avg_atom = 1.97 ms` → 平均每次上 CPU 只跑 2 ms（它 busy 2 ms 就去睡）。

**(c) 精簡版 `/proc/PID/schedstat`（三個數字）**

```bash
ssh radxa@192.168.68.57 'cat /proc/self/schedstat'
```
```
802904285 40344621 407
    ↑          ↑     ↑
sum_exec   wait_sum  nr_switches   （單位：ns）
```

**(d) `ps` 快速看**

```bash
ssh radxa@192.168.68.57 'ps -eo pid,cls,pri,rtprio,ni,psr,pcpu,comm --sort=-pcpu | head -8'
```
`CLS`：`TS`=SCHED_OTHER、`FF`=SCHED_FIFO、`RR`=SCHED_RR、`DLN`=SCHED_DEADLINE、
`B`=SCHED_BATCH、`IDL`=SCHED_IDLE。`PSR` 是目前所在的 CPU。

---

<a name="q2"></a>
## 2. 如何查看 CFS 的調度信息？

### 結論

> ⚠️ **書上說的 `/proc/sched_debug` 在 Linux 5.13 之後不存在了**，
> 改成 **`/sys/kernel/debug/sched/debug`**。

```bash
ssh radxa@192.168.68.57 'ls -la /proc/sched_debug 2>&1; sudo ls /sys/kernel/debug/sched/'
```
```
ls: cannot access '/proc/sched_debug': No such file or directory
debug                     latency_warn_ms      nr_migrate
domains                   latency_warn_once    tunable_scaling
features                  migration_cost_ns    verbose
idle_min_granularity_ns   min_granularity_ns   wakeup_granularity_ns
latency_ns
```

`/sys/kernel/debug/sched/debug` 分成四段（`kernel/sched/debug.c: sched_debug_show()`）：

1. **全域**：版本、`ktime`、`sched_clk`、`jiffies`、`sysctl_sched_*`
2. **每顆 CPU 的 rq**：`nr_running`、`nr_switches`、`next_balance`、`avg_idle`…
3. **每個 cfs_rq / rt_rq / dl_rq**（有 cgroup 時每個群組一份）
4. **所有 runnable 行程的列表**

> **書目**：奔跑吧 §9.1.2「查看 CFS 的信息」與圖 9.4~9.8（第 86~150 行）。
> **修正**：書上圖 9.5 列的 `nr_load_updates`、`cpu_load[]` 兩個欄位
> **在 5.7 已隨 `update_cpu_load_active()` 一起刪除**；
> 圖 9.6 的 `runnable_weight`、`runnable_load_avg` 在 5.7 改名為 `runnable_avg`。

### 實機驗證

**(a) 全域段**

```bash
ssh radxa@192.168.68.57 'sudo head -16 /sys/kernel/debug/sched/debug'
```
```
Sched Debug Version: v0.11, 6.1.115+ #1
ktime                                   : 154686054.464400
sched_clk                               : 154689454.870803
cpu_clk                                 : 154689454.870803
jiffies                                 : 4341283117

sysctl_sched
  .sysctl_sched_latency                    : 24.000000      ← 書上是 6.0
  .sysctl_sched_min_granularity            : 3.000000       ← 書上是 0.75
  .sysctl_sched_idle_min_granularity       : 0.750000       ← 5.13 新增
  .sysctl_sched_wakeup_granularity          : 4.000000      ← 書上是 1.0
  .sysctl_sched_child_runs_first           : 0
  .sysctl_sched_features                   : 58611259
  .sysctl_sched_tunable_scaling            : 1 (logarithmic)
```
數值比書上大 4 倍的原因見 [Q4](#q4)。

**(b) 每顆 CPU 的 rq**

```bash
ssh radxa@192.168.68.57 'sudo grep -A9 "^cpu#0" /sys/kernel/debug/sched/debug | head -10'
```
```
cpu#0
  .nr_running                    : 0
  .nr_switches                   : 37624448     ← 這顆 CPU 累計切換次數
  .nr_uninterruptible            : 4294966354   ← 有號數印成無號，實際是 -942
  .next_balance                  : 4341.283123  ← 下次做負載均衡的 jiffies
  .curr->pid                     : 0            ← 現在跑 swapper/0（idle）
  .clock                         : 154689455.021012
  .clock_task                    : 154689455.021012   ← 扣掉 irq/steal 時間
  .avg_idle                      : 959948       ← 平均閒置長度（ns），newidle_balance 用
  .max_idle_balance_cost         : 500000       ← = sysctl_sched_migration_cost
```
**`nr_load_updates` 和 `cpu_load[0..4]` 這兩項書上有、本機沒有。**

**(c) CFS 就緒佇列**

```bash
ssh radxa@192.168.68.57 'sudo grep -A22 "^cfs_rq\[3\]:/$" /sys/kernel/debug/sched/debug'
```
```
cfs_rq[3]:/
  .exec_clock                    : 0.000000
  .MIN_vruntime                  : 0.000001        ← 紅黑樹最左節點（空佇列時印 1ns）
  .min_vruntime                  : 3864604.880899  ← 佇列基準線（單調遞增）
  .max_vruntime                  : 0.000001        ← 紅黑樹最右節點
  .spread                        : 0.000000        ← max - MIN
  .spread0                       : -737063.074821  ← 本佇列 min_vruntime 與 cpu0 的差
  .nr_spread_over                : 0               ← vruntime 偏離超過 3×latency 的次數
  .nr_running                    : 0
  .h_nr_running                  : 0               ← 含子群組的總數（組調度）
  .idle_nr_running               : 0               ← SCHED_IDLE 的個數（5.13 新增）
  .idle_h_nr_running             : 0
  .load                          : 0               ← 總權重
  .load_avg                      : 0               ← PELT 量化負載
  .runnable_avg                  : 6               ← ★ 5.7 改名（原 runnable_load_avg）
  .util_avg                      : 6               ← PELT 實際算力
  .util_est_enqueued             : 0               ← 5.1 新增
  .removed.load_avg              : 0               ← 待移除的 blocked load
  .removed.util_avg              : 0
  .removed.runnable_avg          : 0
  .tg_load_avg_contrib           : 0               ← 對 task_group 的貢獻
  .tg_load_avg                   : 0
  .throttled                     : 0               ← CFS bandwidth
  .throttle_count                : 0
```

**組調度時每個 cgroup 各有一份**（本機開了 `CONFIG_FAIR_GROUP_SCHED` + autogroup）：

```bash
ssh radxa@192.168.68.57 'sudo grep "^cfs_rq\[3\]" /sys/kernel/debug/sched/debug'
```
```
cfs_rq[3]:/user.slice/user-107.slice/session-c2.scope
cfs_rq[3]:/user.slice/user-1000.slice/user@1000.service/session.slice
cfs_rq[3]:/user.slice/user-1000.slice/user@1000.service
cfs_rq[3]:/user.slice/user-1000.slice/session-23.scope
cfs_rq[3]:/user.slice
cfs_rq[3]:/system.slice
cfs_rq[3]:/
```
**單單 CPU3 就有 7 條 cfs_rq**（root + 6 層 cgroup 階層）。
**這就是為什麼直接比較 `/proc/pid/sched` 的 `se.vruntime` 和
`cfs_rq[N]:/` 的 `min_vruntime` 會對不上**——它們在不同的佇列上。
要對照請先關掉 autogroup 並把行程移到 root cgroup
（見 ch08 [Q5](./ch08_process_management_scheduling_and_load_balancing.md#q5)）。

**(d) 全部 runnable 行程的列表**

```bash
ssh radxa@192.168.68.57 'sudo awk "/^runnable tasks:/{f=1} f" /sys/kernel/debug/sched/debug | head -8'
```
```
runnable tasks:
 S            task   PID         tree-key  switches  prio     wait-time         sum-exec     sum-sleep
------------------------------------------------------------------------------------------------------
 S         systemd     1       650.968844     10265   120         0.000000   5889.413140      0.000000  /init.scope
 S        kthreadd     2   4600375.813508      5777   120         0.000000   1074.580777      0.000000  /
 I          rcu_gp     3        14.040436         2   100         0.000000      0.018375      0.000000  /
 I      rcu_par_gp     4        16.045668         2   100         0.000000      0.013417      0.000000  /
 I    slub_flushwq     5        18.051501         2   100         0.000000      0.013709      0.000000  /
 I           netns     6        20.057333         2   100         0.000000      0.014000      0.000000  /
 I    mm_percpu_wq   10        26.829501         2   100         0.000000      0.013707      0.000000  /
```
欄位：狀態、名稱、PID、**紅黑樹 key（= `vruntime − min_vruntime`）**、切換次數、
優先級、等待時間、累計執行、累計睡眠、**所屬 cgroup**。

三個可以直接讀出來的事實：
* `systemd` 的 tree-key 只有 650，`kthreadd` 卻有 4600375 —— 因為它們**在不同的 cfs_rq 上**
  （`/init.scope` vs root），tree-key 不可跨佇列比較。
* `rcu_gp` / `rcu_par_gp` 等 workqueue rescuer 的 **prio = 100**
  → 它們是 `SCHED_FIFO` 或 nice=−20？其實是 `MAX_RT_PRIO = 100`，即 nice −20 的 CFS 行程。
* 狀態 `I` = `TASK_IDLE`（不可中斷但不計入 loadavg），是 kthread 常見的閒置狀態。

**(e) 檔案有多大**

```bash
ssh radxa@192.168.68.57 'sudo cat /sys/kernel/debug/sched/debug | wc -l'
```
```
4665
```
**4665 行**（8 顆 CPU × 幾十個 cgroup），所以用 `awk`/`grep` 抓自己要的段落，
不要整份 `cat`——**讀這個檔本身會拿 `rq->lock`，在高負載時會影響量測**。

---

<a name="q3"></a>
## 3. 如何查看調度域的拓撲關係？

### 結論

**三個方法：**

| 方法 | 需要什麼 | 看到什麼 |
|---|---|---|
| **① `/sys/kernel/debug/sched/domains/cpuN/domainM/`** | `CONFIG_SCHED_DEBUG=y` | 每個域的 name / flags / 各種參數（**隨時可讀**） |
| **② dmesg**（`sched_domain_debug()`） | 開機帶 `sched_debug` 參數，或 `echo 1 > /sys/kernel/debug/sched/verbose` **並觸發重建** | **完整的 domain + group 拓撲** |
| ③ `/proc/schedstat` | `CONFIG_SCHEDSTATS=y` | 每個域的 span 遮罩 + 均衡統計 |
| ④ `/sys/devices/system/cpu/cpuN/topology/` | — | 硬體拓撲（不是調度域） |

> **書目**：奔跑吧 §9.1.3「查看調度域信息」與圖 9.9/9.10（第 150~179 行）。
> **修正**：書上說在 `/proc/sys/kernel/sched_domain/` 下，
> 6.1 已搬到 `/sys/kernel/debug/sched/domains/`；
> 而且**光 `echo 1 > verbose` 不會印任何東西**，必須讓調度域「真的被重建一次」。

### 實機驗證

**(a) 方法① —— debugfs（最方便）**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
for c in /sys/kernel/debug/sched/domains/*; do
  echo \"--- \$(basename \$c)\"
  for d in \$c/domain*; do [ -d \$d ] || continue
    echo \"  \$(basename \$d) name=\$(cat \$d/name) imb_pct=\$(cat \$d/imbalance_pct) \
min=\$(cat \$d/min_interval) max=\$(cat \$d/max_interval) bf=\$(cat \$d/busy_factor)\"
    echo \"     flags: \$(cat \$d/flags | tr \"\n\" \" \")\"
  done
done"'
```
```
--- cpu0
  domain0 name=MC imb_pct=117 min=8 max=16 bf=16
     flags: SD_BALANCE_NEWIDLE SD_BALANCE_EXEC SD_BALANCE_FORK SD_WAKE_AFFINE
            SD_ASYM_CPUCAPACITY SD_ASYM_CPUCAPACITY_FULL SD_SHARE_PKG_RESOURCES SD_PREFER_SIBLING
--- cpu1
  domain0 name=MC imb_pct=117 ...（八顆完全一樣）
```

**每顆 CPU 都只有 `domain0`，名字是 `MC`** —— RK3588 只有一層調度域。

**(b) 方法② —— dmesg（唯一能看到「調度組」的方法）**

`sched_domain_debug()` 只在 `cpu_attach_domain()` 時印，
所以要**真的觸發一次重建**（CPU 熱插拔最直接）：

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
echo 1 > /sys/kernel/debug/sched/verbose
dmesg -C
echo 0 > /sys/devices/system/cpu/cpu7/online; sleep 1
echo 1 > /sys/devices/system/cpu/cpu7/online; sleep 2
dmesg | sed \"s/^\[[^]]*\] //\" | grep -A3 \"CPU0 attaching sched-domain\" | tail -3"'
```
```
CPU0 attaching sched-domain(s):
 domain-0: span=0-7 level=MC
  groups: 0:{ span=0 cap=421 }, 1:{ span=1 cap=422 }, 2:{ span=2 cap=422 }, 3:{ span=3 cap=422 },
          4:{ span=4 }, 5:{ span=5 }, 6:{ span=6 }, 7:{ span=7 }
root domain span: 0-7 (max cpu_capacity = 1024)
root_domain 0-7: pd6:{ cpus=6-7 nr_pstate=11 } pd4:{ cpus=4-5 nr_pstate=11 } pd0:{ cpus=0-3 nr_pstate=8 }
```

**RK3588 的完整拓撲：**

```
   root_domain  span = 0-7     max_cpu_capacity = 1024
   perf domains（EAS 用，與調度域無關）：pd0{0-3}  pd4{4-5}  pd6{6-7}
        │
        ▼
   ┌───────────────────────────────────────────────────────────────┐
   │ domain-0   name = MC   span = 0-7   imbalance_pct = 117       │
   │ 8 個調度組，每組只有 1 顆 CPU：                                 │
   │   g{0}→g{1}→g{2}→g{3}→g{4}→g{5}→g{6}→g{7}→(環回 g{0})        │
   │   cap 421  422   422   422   1024  1024  1024  1024           │
   └───────────────────────────────────────────────────────────────┘
        A55   A55   A55   A55    A76   A76   A76   A76
        └──── policy0 1.8GHz ──┘ └policy4┘ └policy6┘  2.256GHz
```

**與書上圖 9.10（Intel i7-4770，SMT+MC 兩層）對照：**

| | 書上 i7-4770 | **本機 RK3588** |
|---|---|---|
| 層數 | 2（SMT + MC） | **1（MC）** |
| CPU0 的 domain-0 | span=0,4 level=SMT，2 組 | span=**0-7** level=MC，**8 組** |
| CPU0 的 domain-1 | span=0-7 level=MC，4 組 | **不存在** |

**為什麼？**
* `CONFIG_SCHED_SMT` 沒編（ARM 沒 SMT）；
* `CONFIG_SCHED_CLUSTER` **沒編**（否則會依 `cluster_cpus_list` 建出 {0-3}{4-5}{6-7} 三個 CLS 域）；
* MC 層的 `cpu_coregroup_mask()` 回傳 LLC 共享範圍，
  **RK3588 八顆核共享 3 MB L3 → span = 0-7**；
* DIE 層的 `cpu_cpu_mask()` 也是 0-7，與 MC 完全相同 → **被 `sd_degenerate()` 合併掉**。

**(c) 方法③ —— `/proc/schedstat`**

```bash
ssh radxa@192.168.68.57 'head -4 /proc/schedstat'
```
```
version 15
timestamp 4341247406
cpu0 0 0 0 0 0 0 3271029109956 490784802512 19236189
domain0 ff 0 0 0 0 0 ... （36 個計數器）
```
`domain0 ff` —— **只有一個域，span 遮罩是 `0xff`（8 顆 CPU）**，與 (a)(b) 一致。

**(d) 方法④ —— 硬體拓撲（對照組）**

```bash
ssh radxa@192.168.68.57 'for c in /sys/devices/system/cpu/cpu[0-9]*; do n=$(basename $c);
  echo "$n: pkg=$(cat $c/topology/physical_package_id) core=$(cat $c/topology/core_id) \
cluster=$(cat $c/topology/cluster_cpus_list) pkg_siblings=$(cat $c/topology/core_siblings_list) \
cap=$(cat $c/cpu_capacity)"; done'
```
```
cpu0: pkg=0 core=0 cluster=0-3 pkg_siblings=0-7 cap=422
cpu1: pkg=0 core=1 cluster=0-3 pkg_siblings=0-7 cap=422
cpu2: pkg=0 core=2 cluster=0-3 pkg_siblings=0-7 cap=422
cpu3: pkg=0 core=3 cluster=0-3 pkg_siblings=0-7 cap=422
cpu4: pkg=0 core=0 cluster=4-5 pkg_siblings=0-7 cap=1024
cpu5: pkg=0 core=1 cluster=4-5 pkg_siblings=0-7 cap=1024
cpu6: pkg=0 core=0 cluster=6-7 pkg_siblings=0-7 cap=1024
cpu7: pkg=0 core=1 cluster=6-7 pkg_siblings=0-7 cap=1024
```

**硬體上明明有三個簇（0-3 / 4-5 / 6-7），調度器卻只看到一個平坦的 MC 域** ——
因為 `CONFIG_SCHED_CLUSTER=n`。這個落差有實際後果：
`sd_llc_size = 8`，`select_idle_sibling()` 會在全部 8 顆核裡找閒置 CPU，
所以一個小任務有機會被丟到大核上
（見 ch08 [Q28](./ch08_process_management_scheduling_and_load_balancing.md#q28)）。

---

<a name="q4"></a>
## 4. 在 /proc/sys/kernel 目錄下面的 sched_latency_ns 和 sched_min_granularity_ns 這兩個節點有什麼區別？

### 結論

> ⚠️ **先修正題目本身：這兩個節點在 Linux 5.13 之後已經不在
> `/proc/sys/kernel/`，而是在 `/sys/kernel/debug/sched/`。**

| 節點 | 意義 | 書上預設 | **本機實測** |
|---|---|---|---|
| `latency_ns`（`sysctl_sched_latency`） | **一個「調度週期」**：就緒佇列上**每個**行程都至少被排到一次所需要的時間 | 6 ms | **24 ms** |
| `min_granularity_ns`（`sysctl_sched_min_granularity`） | **最小時間片**：一個行程一旦上 CPU，至少要跑這麼久才准被搶佔（防止過度切換） | 0.75 ms | **3 ms** |

兩者的關係全部濃縮在這個函式裡（`kernel/sched/fair.c:710`）：

```c
static unsigned int sched_nr_latency = 8;      /* = latency / min_granularity */

static u64 __sched_period(unsigned long nr_running)
{
	if (unlikely(nr_running > sched_nr_latency))
		return nr_running * sysctl_sched_min_granularity;   /* ② 人太多，週期拉長 */
	else
		return sysctl_sched_latency;                        /* ① 人不多，週期固定 */
}

static u64 sched_slice(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	unsigned int nr_running = cfs_rq->nr_running;
	u64 slice;

	if (sched_feat(ALT_PERIOD))
		nr_running = rq_of(cfs_rq)->cfs.h_nr_running;
	slice = __sched_period(nr_running + !se->on_rq);

	for_each_sched_entity(se) {                     /* 組調度：逐層按權重切 */
		...
		slice = __calc_delta(slice, se->load.weight, load);
	}
	if (sched_feat(BASE_SLICE))
		slice = max(slice, (u64)sysctl_sched_min_granularity);
	return slice;
}
```

**用一句話說明區別：**
> `sched_latency_ns` 是「**整個週期**多長」，
> `sched_min_granularity_ns` 是「**每個人至少**分到多長」；
> 當 `nr_running > latency/min_granularity` 時，**前者失效、後者接手**，
> 週期改成 `nr_running × min_granularity`，代價是延遲變長。

**臨界點就是 `sched_nr_latency = latency / min_granularity`（本機 = 8）。**

**為什麼本機是 24 ms / 3 ms 而不是 6 ms / 0.75 ms？**
因為 `sysctl_sched_tunable_scaling = 1`（logarithmic），
`sched_init_granularity()` / `update_sysctl()` 會依 CPU 數放大：

```c
static unsigned int get_update_sysctl_factor(void)
{
	unsigned int cpus = min_t(unsigned int, num_online_cpus(), 8);
	switch (sysctl_sched_tunable_scaling) {
	case SCHED_TUNABLESCALING_NONE:  factor = 1; break;
	case SCHED_TUNABLESCALING_LINEAR: factor = cpus; break;
	case SCHED_TUNABLESCALING_LOG:
	default:                          factor = 1 + ilog2(cpus); break;
	}
	return factor;
}
```
`factor = 1 + ilog2(8) = 4` → `6 × 4 = 24`、`0.75 × 4 = 3`、`1 × 4 = 4`。

> **書目**：奔跑吧 §9.1.4「與調度相關的調試節點」（第 179~224 行）——
> 「sched_latency_ns 表示一個運行隊列中所有進程運行一次的時間片…
> 如果進程數超過 sched_nr_latency（默認是 8），那麼調度週期就是
> sched_min_granularity_ns 乘以運行隊列裡的進程數量；否則，就是 sysctl_sched_latency」。

### 實機驗證

**(a) 節點位置與數值**

```bash
ssh radxa@192.168.68.57 'ls /proc/sys/kernel/ | grep sched'
```
```
sched_autogroup_enabled     sched_energy_aware        sched_rt_runtime_us
sched_cfs_bandwidth_slice_us sched_rr_timeslice_ms    sched_schedstats
sched_child_runs_first      sched_rt_period_us        sched_util_clamp_max
sched_deadline_period_max_us                          sched_util_clamp_min
sched_deadline_period_min_us                          sched_util_clamp_min_rt_default
```
**`sched_latency_ns` 和 `sched_min_granularity_ns` 都不在了。**

```bash
ssh radxa@192.168.68.57 'sudo grep . /sys/kernel/debug/sched/{latency_ns,min_granularity_ns,\
idle_min_granularity_ns,wakeup_granularity_ns,tunable_scaling}'
```
```
/sys/kernel/debug/sched/latency_ns:24000000
/sys/kernel/debug/sched/min_granularity_ns:3000000
/sys/kernel/debug/sched/idle_min_granularity_ns:750000
/sys/kernel/debug/sched/wakeup_granularity_ns:4000000
/sys/kernel/debug/sched/tunable_scaling:1
```

**(b) 臨界點 `nr_running = 8` 實測命中**

在 CPU3 上放 n 個 nice=0 的 CPU-bound 行程，用
「每個行程的 CPU 時間 ÷ 被搶佔次數」反推實際時間片：

```bash
ssh radxa@192.168.68.57 'cd /tmp && for n in 1 2 4 6 8 12 16; do
  args=""; for i in $(seq $n); do args="$args 0"; done
  echo "### nr_running=$n"; ./sched_weight 3 6 $args | sed -n 3p
done'
```

| n | CPU 時間/行程 | `nivcsw` | **實測時間片** | `__sched_period(n)` | `slice = period/n` | 對齊 tick (3.33 ms) |
|---|---|---|---|---|---|---|
| 1 | 6.00 s | 3 | — | 24 ms | 24 ms | 沒人競爭，不切 |
| 2 | 3.00 s | 225 | **13.33 ms** | **24 ms** | 12 ms | 4 tick ✅ |
| 4 | 1.50 s | 225 | **6.67 ms** | **24 ms** | 6 ms | 2 tick ✅ |
| 6 | 1.00 s | 150 | **6.67 ms** | **24 ms** | 4 ms | 2 tick ✅ |
| **8** | 0.75 s | 225 | **3.33 ms** | **24 ms**（臨界） | **3 ms** | 1 tick ✅ |
| **12** | 0.50 s | 150 | **3.33 ms** | **36 ms**（= 12×3） | **3 ms** | 1 tick ✅ |
| **16** | 0.375 s | 113 | **3.32 ms** | **48 ms**（= 16×3） | **3 ms** | 1 tick ✅ |

**三個結論：**
1. **`n ≤ 8` 時週期固定 24 ms**（時間片 = 24/n，越多人越短）；
   **`n > 8` 時時間片鎖死在 3 ms**（週期改成 n×3 ms，越多人**延遲越長**）。
   → 臨界點 `sched_nr_latency = 24/3 = 8` **實測命中**。
2. **實測時間片永遠是 3.33 ms 的整數倍**，因為 `check_preempt_tick()` 只在節拍上跑
   （`HZ=300`，`sched_features` 是 `NO_HRTICK`）。
   → **在 HZ=300 上，`min_granularity = 3 ms` 其實比一個 tick 還小，等於沒作用。**
3. 從 ftrace 直接看時間片（兩個行程輪流）：
   ```
   sh-100767 [002] 156307.978571: sched_switch: prev_state=R ==> next_comm=sh next_pid=100768
   sh-100768 [002] 156307.991901: sched_switch: prev_state=R ==> next_comm=sh next_pid=100767
   sh-100767 [002] 156308.005234: sched_switch: prev_state=R ==> next_comm=sh next_pid=100768
   ```
   間隔 **13.333 ms**，一絲不差。

**(c) 改一下試試看（可調參數的意義）**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
echo 6000000 > /sys/kernel/debug/sched/latency_ns          # 改回 6 ms
echo  750000 > /sys/kernel/debug/sched/min_granularity_ns  # 改回 0.75 ms
su radxa -c \"cd /tmp && ./sched_weight 3 6 0 0\" | sed -n 3p
# 還原
echo 24000000 > /sys/kernel/debug/sched/latency_ns
echo  3000000 > /sys/kernel/debug/sched/min_granularity_ns"'
```
把 `latency_ns` 調小 → 時間片變短 → **切換更頻繁、互動性更好、吞吐量下降**；
調大 → 相反。這就是「桌面 vs 伺服器」調優的旋鈕。

---

<a name="q5"></a>
## 5. 雙核系統，Shell 下運行 test 程序，CPU0 就緒隊列有 4 個進程，CPU1 有 1 個。請畫出 test 程序在內核空間的運行流程；若干時間之後兩個就緒隊列如何變化？

### 結論

### (1) test 程式在核心空間的運行流程

```
【使用者空間】 bash: ./test
      │
      ▼
┌─── ① fork() ────────────────────────────────────────────────────────┐
│  sys_clone → kernel_clone → copy_process()                          │
│    ├ dup_task_struct()      配 task_struct + 16 KB 核心堆疊         │
│    ├ sched_fork()           ── ★ 排程器相關 ──                       │
│    │    __sched_fork():  se.vruntime = 0, se.avg 清零               │
│    │    p->prio = current->normal_prio                              │
│    │    p->sched_class = &fair_sched_class                          │
│    │    p->on_cpu = 0；set_task_cpu(p, smp_processor_id())          │
│    ├ copy_mm()              COW 複製頁表                             │
│    ├ copy_files/fs/sighand/signal/namespaces                        │
│    └ copy_thread()          ── ★ 設定第一次上 CPU 的入口 ──          │
│         childregs = task_pt_regs(p);  childregs->regs[0] = 0;       │
│         p->thread.cpu_context.pc = (unsigned long)ret_from_fork;    │
│         p->thread.cpu_context.sp = (unsigned long)childregs;        │
└─────────────────────────────────────────────────────────────────────┘
      │
      ▼
┌─── ② wake_up_new_task(p) ───────────────────────────────────────────┐
│  ├ set_task_cpu(p, select_task_rq(p, task_cpu(p), WF_FORK))         │
│  │    → select_task_rq_fair(..., SD_BALANCE_FORK)                   │
│  │      走「慢路徑」find_idlest_cpu() → find_idlest_group()          │
│  │      → CPU1 只有 1 個行程，負載最小  ⇒ 選 CPU1                    │
│  ├ post_init_entity_util_avg()   新行程 util_avg 初始化              │
│  ├ activate_task(rq1, p, ENQUEUE_NOCLOCK)                           │
│  │    → enqueue_task_fair() → enqueue_entity()                      │
│  │      → place_entity(cfs_rq, se, initial=1)                       │
│  │          se->vruntime = cfs_rq->min_vruntime + sched_vslice()    │
│  │            ← ★ START_DEBIT：新行程先欠一個時間片                  │
│  │      → __enqueue_entity()  插入紅黑樹                             │
│  └ check_preempt_curr(rq1, p, WF_FORK)                              │
│       → check_preempt_wakeup()：p 的 vruntime 不夠小（剛被 debit）    │
│         ⇒ 通常「不會」立刻搶佔 CPU1 上正在跑的行程                    │
└─────────────────────────────────────────────────────────────────────┘
      │
      ▼
┌─── ③ CPU1 下一次時鐘節拍 ───────────────────────────────────────────┐
│  scheduler_tick() → task_tick_fair() → entity_tick()                │
│    → update_curr()：curr 的 vruntime 前進                            │
│    → check_preempt_tick()：curr 跑滿 sched_slice ⇒ resched_curr()   │
│      設 TIF_NEED_RESCHED                                            │
└─────────────────────────────────────────────────────────────────────┘
      │
      ▼
┌─── ④ 中斷返回使用者空間前 ──────────────────────────────────────────┐
│  el0_interrupt → exit_to_user_mode_loop() 看到 _TIF_NEED_RESCHED    │
│    → schedule() → __schedule()                                      │
│        pick_next_task_fair()：紅黑樹最左 = 新行程                    │
│        context_switch(rq1, prev, new_task, rf)                      │
│          ├ switch_mm_irqs_off()  換 TTBR0_EL1 + ASID                │
│          └ switch_to() → cpu_switch_to()                            │
│               載入 cpu_context：pc = ret_from_fork, sp = pt_regs     │
└─────────────────────────────────────────────────────────────────────┘
      │
      ▼
┌─── ⑤ 新行程開跑 ────────────────────────────────────────────────────┐
│  ret_from_fork:                                                     │
│      bl schedule_tail       ← 幫 prev 收尾、raw_spin_unlock_irq()    │
│                               （★ 中斷在這裡被打開）                 │
│      cbz x19, 1f            ← x19 == 0，不是內核執行緒               │
│  1:  b  ret_to_user  → kernel_exit 0 → eret                         │
│  回到使用者空間，fork() 回傳 0                                        │
│      → execve("./test")  →  bprm_execve → load_elf_binary()         │
│      → start_thread()：pt_regs->pc = ELF entry, sp = 新使用者堆疊    │
│      → test 的 while(1) i++ 開始跑                                   │
└─────────────────────────────────────────────────────────────────────┘
      │
      ▼
┌─── ⑥ SMP 負載均衡 ──────────────────────────────────────────────────┐
│  每個節拍：scheduler_tick() → trigger_load_balance()                │
│    if (time_after_eq(jiffies, rq->next_balance))                    │
│         raise_softirq(SCHED_SOFTIRQ)                                │
│  軟中斷：run_rebalance_domains() → rebalance_domains()              │
│    for_each_domain(cpu, sd):  load_balance(cpu, rq, sd, idle, &cb)  │
│      ├ should_we_balance()        我有資格做嗎？                     │
│      ├ find_busiest_group()       → CPU0 的組（4 個行程）            │
│      ├ find_busiest_queue()       → CPU0 的 rq                       │
│      ├ detach_tasks()             挑可以搬的（can_migrate_task）     │
│      └ attach_tasks()             掛到 CPU1 的 rq                    │
│  另外：CPU 變 idle 時 newidle_balance() 也會立刻拉人（更快）          │
└─────────────────────────────────────────────────────────────────────┘
```

### (2) 若干時間之後兩個就緒佇列如何變化？

**答：收斂到 3 / 3（總共 6 個行程平分），而且很快。**

* 一開始：CPU0 有 4 個，CPU1 有 1 個 + 新的 test = **4 / 2**
* 負載均衡把 CPU0 的 1 個搬到 CPU1 → **3 / 3**
* 之後就穩定了。再搬會變成 2/4，`imbalance` 換邊，`calculate_imbalance()`
  的 `min()` 與「除以 2」的設計就是在防止這種來回震盪。

> **書目**：奔跑吧 §9.2「綜合案例分析——系統調度」與圖 9.11~9.13（第 224~306 行）。

### 實機驗證

**(a) 完全復刻書上的場景**（`lb_case.c`：先把 5 個行程全塞 CPU0，
再把 affinity 放寬到 {CPU0, CPU1}，模擬雙核系統）

```bash
ssh radxa@192.168.68.57 'cd /tmp && ./lb_case 5 3 3'
```
```
t=0.00s  全部釘在 CPU0： p0@c0 p1@c0 p2@c0 p3@c0 p4@c0
t= 0.10s  nr_running/CPU: c0=5 c1=0
t= 0.20s  nr_running/CPU: c0=3 c1=2      ← 100~200 ms 內就均衡完了
t= 0.30s  nr_running/CPU: c0=3 c1=2
t= 0.40s  nr_running/CPU: c0=3 c1=2
   ...（保持不變）
t= 2.60s  nr_running/CPU: c0=3 c1=2
```

**5 個行程收斂到 3/2（不是 2.5/2.5，因為不能整除），在 200 ms 內完成，之後完全不震盪。**

**(b) 8 個行程 / 8 顆 CPU**

```bash
ssh radxa@192.168.68.57 'cd /tmp && ./lb_case 8 ff 3 | head -4'
```
```
t=0.00s  全部釘在 CPU0： p0@c0 ... p7@c0
t= 0.10s  nr_running/CPU: c0=1 c1=1 c2=1 c3=1 c4=1 c5=1 c6=1 c7=1
t= 0.20s  nr_running/CPU: c0=1 c1=1 c2=1 c3=1 c4=1 c5=1 c6=1 c7=1
```
**100 ms 內從 8/0/0/0/0/0/0/0 完美攤成每顆一個。**

**(c) 用 ftrace 抓「誰執行了遷移」—— 對應流程圖的第 ⑥ 步**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
T=/sys/kernel/debug/tracing; echo 0 > \$T/events/enable; echo > \$T/trace
echo 1 > \$T/events/sched/sched_migrate_task/enable
echo \"comm ~ \\\"lb_case\\\"\" > \$T/events/sched/sched_migrate_task/filter
echo 1 > \$T/tracing_on; su radxa -c \"/tmp/lb_case 5 3 1\" > /dev/null; echo 0 > \$T/tracing_on
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

**兩種完全不同的遷移，一次看清楚：**

| 時間 | 執行者 | 在哪顆 CPU | 機制 |
|---|---|---|---|
| 159.5~160.1 ms | **`migration/5`** kthread | CPU5 | `sched_setaffinity()` → `stop_one_cpu()` → **stop_sched_class**（最高優先級調度類） |
| 630.4 ms | **`send process`**（一個普通行程） | **CPU1** | 它在 CPU1 上被排到 → `load_balance()` → **從 CPU0「拉」了 2 個過來** |

**第二種正是本題的第 ⑥ 步**：遷移是由**目的地 CPU（dst_cpu）自己執行的**
（`detach_tasks()` / `attach_tasks()` 都跑在 dst_cpu 上，叫 **pull migration**），
不需要 CPU0 配合，而且是「順便」在某個行程被排程時做的。

**(d) 第 ② 步「fork 時選最閒的 CPU」也能直接抓到**

```bash
ssh radxa@192.168.68.57 'sudo /tmp/sched_trace.sh newtask'
```
```
sched_trace.sh-100847  [004] d.... 156323.746235: sched_wakeup_new: comm=sched_trace.sh pid=100848 prio=120 target_cpu=005
sched_trace.sh-100848  [005] d.... 156323.746242: schedule_tail <-ret_from_fork
```
* 父行程在 **CPU4** 上 fork；
* `sched_wakeup_new` 的 `target_cpu=005` → `find_idlest_cpu()` 選了 **CPU5**；
* 子行程的第一筆 trace 就在 CPU5，函式是 **`schedule_tail`，呼叫者是 `ret_from_fork`**
  —— 完全對應流程圖的第 ⑤ 步。

**(e) 第 ① 步 `copy_thread()` 的成果**

`sched_probe.ko` 印出一個「剛 fork 出來、還沒被換出去過」的行程：

```
current 的 cpu_context.pc = ret_from_fork+0x0/0x20, sp = 0xffff80000daabeb0
current->stack = ffff80000daa8000, task_pt_regs(current) = ffff80000daabeb0
```
`pc == ret_from_fork` 且 `sp == task_pt_regs()` —— 一字不差。

---

<a name="q6"></a>
## 6. 進程的本質是什麼？

### 結論

> **行程（執行緒）是分配 CPU 時間的基本單位。**

書上第 9.3.1 節給的就是這一句。展開來看有三層意思：

**(1) 從「抽象」的角度：行程 = 程式 + 執行**

* 程式（program）：磁碟上靜態的 ELF，沒有生命；
* 行程（process）：程式的一次**執行實例**，有 PC、有堆疊、有位址空間、有狀態。
* 行程的存在是為了**虛擬化 CPU**：讓每個行程都以為自己獨佔一顆 CPU。
  實現這個幻覺的兩大技術就是**上下文切換**與**調度**。

**(2) 從「資源」的角度：行程 = 資源容器，執行緒 = 執行流**

在 Linux 裡這兩者被拆開了：

| 傳統 OS 理論 | **Linux 的實作** |
|---|---|
| 行程 = 資源分配單位（PCB） | `mm_struct` / `files_struct` / `signal_struct`（可被共享） |
| 執行緒 = CPU 調度單位（TCB） | **`task_struct`（唯一的資料結構）** |
| 兩套資料結構 | **只有一套** |
| `fork()` vs `pthread_create()` | **都是 `clone()`，只差 flags** |

**Linux 沒有為執行緒設計任何特殊的資料結構或調度演算法。**
所謂「執行緒」不過是一群共享 `mm`/`files`/`signal` 的 `task_struct`。
所以嚴格講，**Linux 調度的單位是 `task_struct`（核心稱 task），不是「行程」。**

**(3) 從「調度」的角度：貫穿一切的是「優先級」**

書上 §9.3.2 的標題很傳神：「**逃離不掉的進程優先級**」。
從 O(n) → O(1) → CFS，演算法一直在變，但**優先級這個概念從沒被拋棄**：

```
nice → weight → { vruntime 走多快 、 分到多少時間片 、 量化負載多大 }
```

* **時間片**：`sched_slice = period × (weight / 佇列總 weight)`
* **虛擬時間**：`vruntime += delta_exec × 1024 / weight`（權重大 → 走得慢 → 跑得多）
* **負載均衡**：`load_avg = weight × runnable 佔比`
* **組調度**：cgroup 的 `cpu.weight` 就是群組層級的 weight

**(4) 書上 §9.3.4「用四維空間來理解負載」**

* 權重是**零維**的點（靜態，除非改 nice 就不變）；
* 量化負載是**四維**的物體（加上了時間軸、加上了歷史衰減）；
* PELT 的核心思想就是「負載是隨時間變化的量，要考慮歷史使用情況」；
* 缺點：「通過計算過去一段時間內的時間佔比來推測當前時間點的負載，
  這好比通過後視鏡來推測前面的車況」。

### 實機驗證

**(a) 「執行緒」在核心眼裡就是 task**

```bash
ssh radxa@192.168.68.57 'ps -eL --no-headers | wc -l; ls /proc | grep -c "^[0-9]"'
```
```
498      ← 系統中的 task（執行緒）總數
299      ← /proc 底下的 PID（thread group）數
```
**498 個 task 但只有 299 個 thread group** —— 差額 199 就是「執行緒」。
（第 7 章在同一台機器上量到 558 / 295。）

對調度器而言 task 與「行程」**完全平等**，每一個都有自己的 `se.vruntime`、
自己的 PELT 訊號、自己在紅黑樹上的節點 —— 這就是「行程（執行緒）是分配 CPU 時間的基本單位」。

**(b) 「分配 CPU 時間的基本單位」——同一個行程的每個執行緒各自被調度**

```bash
# 挑一個多執行緒行程，逐個 tid 看它自己的 vruntime
ssh radxa@192.168.68.57 'P=$(pgrep -f Xtigervnc | head -1); for t in /proc/$P/task/*; do
  printf "tid=%-8s %s\n" "$(basename $t)" \
    "$(grep -E "^se.vruntime|^se.sum_exec_runtime|^nr_switches" $t/sched | tr -s " " | tr "\n" " ")"
done'
```
每個 tid 在 `/proc/<pid>/task/<tid>/sched` 底下都有**獨立**的
`se.vruntime` / `se.sum_exec_runtime` / `nr_switches`
→ **調度單位確實是 task，不是 thread group。**

**(c) 優先級真的「逃離不掉」——一條線串起全部**

用 `sched_probe.ko` 對同一個行程 dump：

```
prio=120  static_prio=120  normal_prio=120  rt_priority=0  nice=0
policy=0  sched_class=0xffff800009666348
se.load.weight=1048576  se.load.inv_weight=4194304
se.vruntime=25177697  se.sum_exec_runtime=1022584
se.avg: load_avg=1024 runnable_avg=522 util_avg=522
```

```
nice 0
  → static_prio 120
     → sched_prio_to_weight[20] = 1024
        → se.load.weight = 1024 << 10 = 1048576         ← 時間片按這個比例分
        → se.load.inv_weight = 2^32/1024 = 4194304      ← vruntime 用它做乘法
           → vruntime += delta × 1024/1024 = delta      ← nice 0 走捷徑
        → load_avg 上限 = scale_load_down(weight) = 1024 ← 負載均衡看這個
```

實測（見 ch08 [Q1](./ch08_process_management_scheduling_and_load_balancing.md#q1)）：
nice 0 與 nice 5 的兩個行程搶同一顆 CPU，
CPU 時間比 = **75.28% : 24.72%**，理論 `1024:335` = **75.35% : 24.65%**。
**「優先級 → 權重 → CPU 時間」這條鏈在實機上誤差 0.07 個百分點。**

---

<a name="q7"></a>
## 7. 在 Linux 內核實現中有哪些概念和進程優先級相關？

### 結論

**九個，分成四類：**

**① `task_struct` 裡的四個優先級欄位**

| 欄位 | 範圍 | 意義 |
|---|---|---|
| `static_prio` | 100~139 | **靜態**優先級 = `nice + 120`，只有 `setpriority()`/`nice()` 能改 |
| `normal_prio` | 0~139 | 依 policy 算出的「應有」優先級：普通 = `static_prio`；RT = `99 - rt_priority`；DL = `MAX_DL_PRIO-1 = -1` |
| **`prio`** | 0~139 | **動態**優先級，**調度器真正比較的就是它**。平常 = `normal_prio`，但 rt-mutex 的**優先級繼承（PI）**會臨時把它拉高 |
| `rt_priority` | 0~99 | 使用者設定的 RT 優先級（**數字越大越優先**，和 prio 相反）。`SCHED_DEADLINE` 不用這欄，改用 `dl.dl_runtime/dl_deadline/dl_period` |

換算（`kernel/sched/core.c`）：

```c
static inline int __normal_prio(int policy, int rt_prio, int nice)
{
	int prio;
	if (dl_policy(policy))      prio = MAX_DL_PRIO - 1;      /* -1 */
	else if (rt_policy(policy)) prio = MAX_RT_PRIO - 1 - rt_prio;  /* 99 - rt_prio */
	else                        prio = NICE_TO_PRIO(nice);   /* nice + 120 */
	return prio;
}
```

**② 使用者可見的兩個**

| 概念 | 範圍 | 系統呼叫 |
|---|---|---|
| **nice** | −20~19 | `nice()` / `setpriority()` / `renice` |
| **policy** | `SCHED_NORMAL(0)` `FIFO(1)` `RR(2)` `BATCH(3)` `IDLE(5)` `DEADLINE(6)` | `sched_setscheduler()` / `chrt` |

**③ CFS 內部的三個**

| 概念 | 說明 |
|---|---|
| **weight**（`se.load.weight`） | `sched_prio_to_weight[static_prio-100] << 10`；**決定時間片與 vruntime 速率** |
| **inv_weight**（`se.load.inv_weight`） | `sched_prio_to_wmult[...]` = `2^32/weight`；把除法變乘法 |
| **`sched_class`** | 五個調度類的優先級鏈：stop > dl > rt > fair > idle |

**④ 群組層級 / 其他**

| 概念 | 說明 |
|---|---|
| **cgroup `cpu.weight` / `cpu.shares`** | 群組的權重，`sched_entity` 階層式分配 |
| **`uclamp.min/max`** | 5.3 新增，**不是優先級但會影響 CPU 選擇與調頻**（EAS/schedutil） |
| **`sched_prio_to_weight[]` / `sched_prio_to_wmult[]`** | 那兩張表 |
| **`MAX_RT_PRIO=100`、`MAX_PRIO=140`、`DEFAULT_PRIO=120`、`NICE_0_LOAD=1024<<10`** | 幾個關鍵常數 |

**完整的優先級軸：**

```
  prio:  -1      0 ────────── 99   100 ─────────── 139
        DL      RT (rt_priority 99→0)   CFS (nice -20→19)
        │        │                       │
     EDF 排序   prio 小者優先          vruntime 小者優先
                                       weight 88761 → 15
```

> **書目**：奔跑吧 §9.3.2「逃離不掉的進程優先級」與圖 9.14（第 328~358 行）。

### 實機驗證

**(a) 一個普通行程的全部優先級欄位**（`sched_probe.ko`）

```bash
ssh radxa@192.168.68.57 'sudo insmod ~/exp/sched/sched_probe.ko; sudo rmmod sched_probe; \
    sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A12 "調度欄位"'
```
```
== 行程 insmod/97515 的調度欄位（Ch9 Q7 / Ch8 Q28）==
   prio=120  static_prio=120  normal_prio=120  rt_priority=0  nice=0
   policy=0  sched_class=0xffff800009666348
   se.load.weight=1048576  se.load.inv_weight=4194304
   se.vruntime=25177697  se.sum_exec_runtime=1022584  se.exec_start=155954654097894
   se.avg: load_sum=47016 runnable_sum=24575492 util_sum=24575492 period_contrib=298
   se.avg: load_avg=1024 runnable_avg=522 util_avg=522 util_est=(0,0)
   on_cpu=1  on_rq=1  se.on_rq=1  cpu(task_cpu)=6  wake_cpu=6  recent_used_cpu=0
   wakee_flips=19  wakee_flip_decay_ts=4341662555  last_wakee=send process
   nr_cpus_allowed=8  cpus_mask=0-7  se.nr_migrations=0
   nvcsw=0(主動)  nivcsw=0(被搶佔)
```

`prio = static_prio = normal_prio = 120`，`rt_priority = 0`，`nice = 0` —— 全部一致。

**(b) 一個非 CFS 行程的欄位 —— 實測 `sugov:0`**

`sugov:N` 是 schedutil 的換頻 kthread。**只有把 governor 切成 schedutil 才會存在。**

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
for p in 0 4 6; do echo schedutil > /sys/devices/system/cpu/cpufreq/policy\$p/scaling_governor; done
sleep 1; ps -eo pid,cls,pri,rtprio,ni,comm | grep -E \"PID|sugov\"
P=\$(pgrep sugov | head -1); dmesg -C
insmod ~/exp/sched/sched_probe.ko target_pid=\$P; rmmod sched_probe
dmesg | sed \"s/^\[[^]]*\] //\" | grep -A4 \"行程 sugov\"
for p in 0 4 6; do echo ondemand > /sys/devices/system/cpu/cpufreq/policy\$p/scaling_governor; done"'
```

```
    PID CLS PRI RTPRIO  NI COMMAND
   2239 DLN 140      0   - sugov:0
   2240 DLN 140      0   - sugov:4
   2241 DLN 140      0   - sugov:6

sched_probe: == 行程 sugov:0/2239 的調度欄位（Ch9 Q7 / Ch8 Q28）==
sched_probe:    prio=-1  static_prio=120  normal_prio=-1  rt_priority=0  nice=0
sched_probe:    policy=6  sched_class=0xffff800009666198
sched_probe:    se.load.weight=1048576  se.load.inv_weight=4194304
sched_probe:    se.vruntime=0  se.sum_exec_runtime=2471583  se.exec_start=74240930091
```

> ⚠️ **一個容易答錯的點：`sugov:N` 不是 `SCHED_FIFO`，是 `SCHED_DEADLINE`。**
> Linux 4.16 的 commit `794a56ebd9a5`（"sched/cpufreq: Change the worker kthread to
> SCHED_DEADLINE"）把它改掉了，就是為了讓換頻工作不會被 RT 行程餓死、
> 又不會像 FIFO 那樣無界佔用 CPU。`ps` 顯示的 `DLN` 就是 `SCHED_DEADLINE`。
>
> ```c
> /* kernel/sched/cpufreq_schedutil.c:616 */
> struct sched_attr attr = {
>         .sched_policy   = SCHED_DEADLINE,
>         .sched_flags    = SCHED_FLAG_SUGOV,
>         .sched_runtime  =  1000000,     /* 1 ms   */
>         .sched_deadline = 10000000,     /* 10 ms  */
>         .sched_period   = 10000000,
> };
> ```

**逐欄對照 `__normal_prio()`（`kernel/sched/core.c:2108`）：**

```c
static inline int __normal_prio(int policy, int rt_prio, int nice)
{
	if (dl_policy(policy))       return MAX_DL_PRIO - 1;           /* = -1        */
	else if (rt_policy(policy))  return MAX_RT_PRIO - 1 - rt_prio; /* = 99-rt_prio */
	else                         return NICE_TO_PRIO(nice);        /* = nice+120   */
}
```

| 欄位 | 實測值 | 怎麼來的 |
|---|---|---|
| `policy` | **6** = `SCHED_DEADLINE` | `sched_setattr_nocheck()` 設的 |
| `normal_prio` | **−1** | `MAX_DL_PRIO(0) − 1` ✅ **公式命中** |
| `prio` | **−1** | = `normal_prio`（沒有 PI 提升） |
| `rt_priority` | **0** | DL 行程不用這個欄位 |
| `static_prio` | **120** | ⚠️ **從沒被 `nice()` 改過的殘留值，對 DL 行程完全無意義** |
| `se.load.weight` | **1048576** | ⚠️ 同理，CFS 的 `sched_entity` 對 DL 行程也是殘留值 |
| `se.vruntime` | **0** | ⚠️ **從來沒進過 CFS 紅黑樹，所以是 0** |

最後三列是這一題最好的教材：**`task_struct` 裡「四套調度器的欄位」是並存的
（`se` / `rt` / `dl`），只有 `p->sched_class` 指到的那一套才有意義。**
`sugov:0` 的 `se.sum_exec_runtime = 2.47 ms` 有值，是因為
`update_curr_dl()` 也會累加它（統計用），但 `vruntime` 永遠是 0。

**(c) 從命令列快速看（`ps` 的 `PRI` 是自己換算過的，不等於 `p->prio`）**

```bash
ssh radxa@192.168.68.57 'ps -eo pid,cls,pri,rtprio,ni,comm | grep -E "PID|migration/0|systemd$"'
```
```
    PID CLS PRI RTPRIO  NI COMMAND
      1  TS  19      -   0 systemd       ← SCHED_OTHER，nice 0
     15  FF 139     99   - migration/0   ← stop 類，ps 一律顯示成 FF 99
```
`CLS`：`TS`=SCHED_OTHER、`FF`=SCHED_FIFO、`RR`=SCHED_RR、
**`DLN`=SCHED_DEADLINE**、`B`=SCHED_BATCH、`IDL`=SCHED_IDLE。
`RTPRIO` 才是 `p->rt_priority`，`NI` 才是 nice；
`PRI` 是 `ps` 自己算的顯示值（`139 - prio`），**不是** `p->prio`。

**(d) 權重表對帳**（見 ch08 [Q1](./ch08_process_management_scheduling_and_load_balancing.md#q1)）

```
   nice  weight    wmult(表)    2^32/weight   1024/1.25^n
   -20   88761     48388        48388         88817
   0     1024      4194304      4194303       1024
   19    15        286331153    286331153     15
```

---

<a name="q8"></a>
## 8. 站在 CPU 的角度，進程切換時，CPU 會區分誰是 prev 進程，誰是 next 進程嗎？

### 結論

> **不會。CPU 完全不知道「行程」是什麼東西。**

CPU 只認得三樣硬體狀態：

| CPU 眼中 | 對應的行程概念 |
|---|---|
| **`SP` 指向哪塊記憶體** | 核心堆疊 → 哪個 task |
| **`TTBR0_EL1` + ASID** | 位址空間 → 哪個 mm |
| **`PC` 指向哪條指令** | 執行到哪 |

`cpu_switch_to(prev, next)` 對 CPU 而言就是一段**普通的指令序列**：

```asm
SYM_FUNC_START(cpu_switch_to)
	mov	x10, #THREAD_CPU_CONTEXT
	add	x8, x0, x10             /* x0 只是一個「位址」，CPU 不知道那是 prev */
	mov	x9, sp
	stp	x19, x20, [x8], #16     /* 對 CPU 而言：把暫存器存到某個記憶體 */
	...
	str	lr, [x8]
	add	x8, x1, x10             /* x1 也只是一個位址 */
	ldp	x19, x20, [x8], #16     /* 對 CPU 而言：從某個記憶體載入暫存器 */
	...
	mov	sp, x9                  /* ★ 這一條指令，SP 換了 */
	msr	sp_el0, x1              /* ★ 這一條指令，current 換了 */
	ret                             /* ★ 這一條指令，PC 換了（lr 已經是 next 的） */
SYM_FUNC_END(cpu_switch_to)
```

**「行程切換」是軟體概念，不是硬體事件。**
在 `mov sp, x9` 執行完的那一奈秒，CPU 只是把一個暫存器改了值；
是**軟體**（Linux）約定「SP 指向誰的堆疊，誰就是 current」。

**幾個推論（面試常追問）：**

1. **`prev` 和 `next` 只是 C 函式的兩個參數**，分別在 `x0`、`x1`。
   對 CPU 而言它們是對稱的，沒有任何特殊性。
2. **切換過程沒有原子性保證**。中間那幾條指令執行時，
   `SP` 已經是 next 的、但 `sp_el0` 還是 prev 的 —— 所以整段
   **必須在關中斷下執行**（`__schedule()` 一開始就 `local_irq_disable()`）。
3. **正因為 CPU 不區分，才需要 `last` 參數**：
   `switch_to()` 執行完之後，「誰是 prev」這個資訊只剩 `x0` 裡那一份
   （見 ch08 [Q9](./ch08_process_management_scheduling_and_load_balancing.md#q9)）。
4. **x86 有硬體 TSS + task gate**（曾經想讓硬體做行程切換），
   **但 Linux 從來不用**，因為軟體切換更快也更彈性。ARM64 連這個都沒有。

### 實機驗證

**(a) 「current」純粹是軟體約定**

```bash
ssh radxa@192.168.68.57 'sudo insmod ~/exp/sched/sched_probe.ko; sudo rmmod sched_probe; \
    sudo dmesg | grep sp_el0'
```
```
   read_sysreg(sp_el0) = 0xffff000066e5be00, current = ffff000066e5be00
```

`current` 巨集的定義（`arch/arm64/include/asm/current.h`）：

```c
static __always_inline struct task_struct *get_current(void)
{
	unsigned long sp_el0;
	asm ("mrs %0, sp_el0" : "=r" (sp_el0));
	return (struct task_struct *)sp_el0;
}
#define current get_current()
```

**`current` 就是「讀一個系統暫存器，然後把它當成指標」。**
CPU 對 `SP_EL0` 裡放什麼毫無概念——放 `task_struct` 是 Linux 的選擇
（x86 用 `gs:current_task`，ARM32 用堆疊對齊 mask 出 `thread_info`）。

**(b) 硬體上下文只有 13 個暫存器**

```
sizeof(struct cpu_context) = 104 B  (= 13 個 u64：x19~x28, fp, sp, pc)
cpu_context: x19=0 x28=72 fp=80 sp=88 pc=96
```
**CPU 有 31 個通用暫存器，切換卻只存 13 個** ——
因為其他的由 **AAPCS64 呼叫慣例**保證（caller-saved，編譯器已經處理）。
這再次說明：切換是**編譯器 + 軟體約定**的產物，不是硬體機制。

**(c) 從 trace 看，切換的「兩端」是兩個獨立的事件**

```
sh-100767 [002] d.... 156307.978571: sched_switch: prev_comm=sh prev_pid=100767 ==> next_comm=sh next_pid=100768
```
這一筆 trace 的「記錄者」欄位（最左邊）是 `sh-100767`（prev），
但它記錄的內容包含 `next_pid=100768`。
**tracepoint 是在 `context_switch()` 之前、還在 prev 的上下文裡打的**，
真正的硬體切換發生在之後幾十奈秒。

---

<a name="q9"></a>
## 9. 假設 next 進程和 prev 進程都是用戶進程，從 prev 切換到 next 後，next 進程執行的下一條語句是什麼？是 next 在用戶空間被中斷的那條指令嗎？

### 結論

> **不是。next 的下一條語句在「核心空間」，
> 是它自己上一次被換出去時停在的地方 —— `cpu_switch_to()` 的 `ret`。**

要回到使用者空間，還得沿著**它自己的核心堆疊**一路 return 回去：

```
      ┌── cpu_switch_to() 的 ret   ← ★ next 執行的第一條指令
      │
      ├── __switch_to() 返回（x0 = last = prev）
      │
      ├── context_switch() 的 finish_task_switch(prev)
      │     ← 這裡才 raw_spin_unlock_irq(&rq->lock)，把中斷打開
      │
      ├── __schedule() 返回
      │
      ├── schedule() 返回
      │
      ├── exit_to_user_mode_loop() 返回        （處理 signal、need_resched…）
      │
      ├── ret_to_user / kernel_exit 0
      │     ldp x0..x30, sp, elr_el1, spsr_el1  ← 從 pt_regs 恢復使用者現場
      │
      └── eret                                  ← ★ 這一刻才回到 EL0 被中斷的那條指令
```

**兩個「第一條指令」要分清楚：**

| 問法 | 答案 |
|---|---|
| next **在核心空間**的第一條指令？ | `cpu_switch_to` 的 `ret`（老行程）或 `ret_from_fork` 的 `bl schedule_tail`（新行程） |
| next **回到使用者空間**執行的第一條指令？ | **是**它上次被中斷的那條（由 `pt_regs->pc`（`elr_el1`）決定） |

**為什麼不能直接跳回使用者空間？** 三個理由：

1. **使用者現場在 `pt_regs` 裡**，必須執行 `kernel_exit` 那段組語才能 `ldp` 出來，
   而且最後要用 `eret` 才能同時切回 EL0 + 恢復 PSTATE。
2. **有事沒做完**：`finish_task_switch()` 要放 rq->lock、開中斷、
   還 `mm_count`、釋放死掉的 prev（見 ch08 [Q10](./ch08_process_management_scheduling_and_load_balancing.md#q10)）。
3. **`exit_to_user_mode_loop()` 還要檢查** `_TIF_SIGPENDING`、
   `_TIF_NEED_RESCHED`、`_TIF_NOTIFY_RESUME`、`_TIF_FOREIGN_FPSTATE`。

**一個特例**：如果 next 是**新建行程**，它的 `cpu_context.pc = ret_from_fork`，
第一條指令是 `bl schedule_tail`（見 [Q11](#q11)）。

> **書目**：奔跑吧 §9.3.5「調度的本質」與圖 9.19/9.20（第 600~660 行）——
> 「剛切換到 CPU 運行的進程（next 進程），它需要沿著上一次調度時保留在棧中的踪跡
> 一直返回，並且從棧中恢復上一次的中斷現場」。

### 實機驗證

**(a) `cpu_context.pc` 就是「下一條語句」的位址**

`sched_probe.ko` 對一個**還沒被換出去過**的行程（剛 fork 的 `insmod`）：

```
current 的 cpu_context.pc = ret_from_fork+0x0/0x20, sp = 0xffff80000daabeb0
current->stack = ffff80000daa8000, task_pt_regs(current) = ffff80000daabeb0
```

對一個**已經被換出去過**的行程，`cpu_context.pc` 會指向
`__switch_to` 的返回位址（在 `context_switch()` 內），可以用 `crash`/`gdb` 驗證。

**(b) 堆疊佈局的實測**

```
current->stack        = 0xffff80000daa8000     ← 核心堆疊底（vmalloc 區）
THREAD_SIZE           = 16384
task_pt_regs(current) = 0xffff80000daabeb0     ← = stack + 16384 - 336 - 16
sizeof(struct pt_regs)= 336
```

驗算：`0xffff80000daa8000 + 0x4000 = 0xffff80000daac000`，
`0xffff80000daac000 − 336 − 16 = 0xffff80000daabeb0` ✅

**`pt_regs` 就躺在核心堆疊的最頂端** —— 這就是「使用者態現場」的位置，
它從頭到尾**不會被搬動**，next 回去的時候原地取用。

**(c) 中斷確實是在 `finish_task_switch()` 裡才被打開的**

`sched_probe.ko` 的 IRQ 實驗（詳見 [Q13](#q13)）：

```
   schedule() 之前 irqs_disabled() = 128
   schedule() 之後 irqs_disabled() = 0  <-- 中斷已經被打開了！
```

**如果 `switch_to` 直接跳回使用者空間，就不會有這個現象。**
中斷之所以被打開，正是因為「我」被換回來之後，
**先跑了 `finish_task_switch() → finish_lock_switch() → raw_spin_unlock_irq()`**。

**(d) 兩個 sh 互切的 trace**

```
sh-100767 [002] d.... 156307.978571: sched_switch: prev_comm=sh prev_pid=100767 prev_state=R ==> next_comm=sh next_pid=100768
```
`prev_state=R`（不是 S）→ 是被時鐘中斷搶佔的。
`sh-100768` 這一輪的核心堆疊上，躺著它上次被中斷時壓的 `pt_regs`，
它會沿著 `el0_irq → ret_to_user → eret` 回到自己的 `while :; do :; done`。

---

<a name="q10"></a>
## 10. 假設 prev 進程正在執行時發生了時鐘中斷，然後發生了進程切換，並切換到 next 進程，那麼這個時鐘中斷的中斷現場會在什麼時候恢復？

### 結論

> **等到 prev 下一次被調度回 CPU 上、沿著自己的核心堆疊走回 `el0_irq`/`el1_irq`
> 的返回段時才恢復。**

完整時間軸：

```
T0  prev 在 EL0 跑
T1  時鐘中斷 → 進 el0_irq
       kernel_entry 0：把 x0~x30/sp/pc/pstate 壓成 pt_regs
       ★ 中斷現場「存」在 prev 的核心堆疊頂端
T2  scheduler_tick() → check_preempt_tick() → resched_curr()  設 TIF_NEED_RESCHED
T3  中斷處理結束，走到 ret_to_user → exit_to_user_mode_loop()
       看到 _TIF_NEED_RESCHED → schedule() → __schedule()
T4  context_switch(rq, prev, next) → switch_to()
       ★ prev 「睡著」，它的核心堆疊上還壓著：
            pt_regs (T1 存的中斷現場)
            el0_irq 的 frame
            ret_to_user 的 frame
            __schedule / context_switch / switch_to 的 frame   ← SP 停在這
T5  next 開始跑 …（可能過了幾毫秒、幾秒、甚至幾分鐘）
──────────────────────────────────────────────────────────────
Tn  某個 CPU 上某個行程執行 switch_to(X, prev, last)
Tn+1 prev 從 cpu_switch_to 的 ret 醒來
       → finish_task_switch(X)      ← 開中斷、幫 X 收尾
       → __schedule() 返回
       → schedule() 返回
       → exit_to_user_mode_loop() 返回
       → ret_to_user → kernel_exit 0
            ldp x0..x30, sp_el0, elr_el1, spsr_el1   ★ 中斷現場「恢復」
       → eret                                        ★ 回到 T0 那條指令
```

**核心觀念三句話：**

1. **中斷現場（`pt_regs`）存在「被中斷的那個行程」自己的核心堆疊上**，
   每個 task 一份，不會互相干擾，也不需要額外配置。
2. **「切換出去」不會丟掉中斷現場**——它就躺在堆疊上，
   `SP` 換走了，那塊記憶體仍然屬於 prev。
3. **恢復的時機不是「切換回來」那一瞬間**，而是
   「切換回來 → 沿堆疊 return 到 `kernel_exit` → `eret`」的時候。
   中間還隔著 `finish_task_switch()`、signal 處理等。

**如果中斷發生在核心態（`el1_irq`）？**
`pt_regs` 一樣壓在核心堆疊上（只是位置在堆疊中段而非頂端），
返回路徑是 `kernel_exit 1` + `eret` 回到核心態被打斷的地方。
本機 `PREEMPT_VOLUNTARY`，`el1_irq` 返回時**不會**檢查 `TIF_NEED_RESCHED`，
所以核心態被中斷不會導致切換。

> **書目**：奔跑吧 §9.3.5 圖 9.19/9.20（第 600~660 行）——
> 「中斷現場保存在中斷進程的棧裡，只有當調度器再一次調度該進程時，
> 它才會從棧中恢復中斷現場，然後繼續運行該進程」。

### 實機驗證

**(a) `pt_regs` 的位置與大小**

```
sizeof(struct pt_regs)      = 336 B
current->stack              = 0xffff80000daa8000
task_pt_regs(current)       = 0xffff80000daabeb0   = stack + 16384 - 336 - 16
THREAD_SIZE                 = 16384
```

**(b) `pt_regs` 的成員**

```bash
grep -n -A24 "^struct pt_regs {" arch/arm64/include/asm/ptrace.h
```
```c
struct pt_regs {
	union {
		struct user_pt_regs user_regs;
		struct { u64 regs[31]; u64 sp; u64 pc; u64 pstate; };  /* x0~x30 + sp + elr + spsr */
	};
	u64 orig_x0;
	s32 syscallno;
	u32 unused2;
	u64 sdei_ttbr1;
	u64 pmr_save;
	u64 stackframe[2];
	u64 lockdep_hardirqs;
	u64 exit_rcu;
};
```
`pc` 存的是 `ELR_EL1`（中斷返回位址），`pstate` 存的是 `SPSR_EL1`。

**(c) 中斷 → 設旗標 → 返回時才切，用 trace flag 一眼看出**

```bash
ssh radxa@192.168.68.57 'sudo /tmp/sched_trace.sh switch'
```
```
<idle>-0  [002] d.h.. 156307.922775: sched_waking: comm=listener pid=968 target_cpu=002
<idle>-0  [002] dNh.. 156307.922785: sched_wakeup: comm=listener pid=968 target_cpu=002
<idle>-0  [002] d.... 156307.922791: sched_switch: prev_comm=swapper/2 ==> next_comm=listener
```

| flag | 意義 | 對應時間軸 |
|---|---|---|
| `d.h..` | irqs-off + **hardirq 上下文** | T1~T2：還在中斷處理常式裡 |
| `dNh..` | 多了 **`N` = TIF_NEED_RESCHED 已置位** | T2：`resched_curr()` 剛做完 |
| `d....` | irqs-off，**已經離開 hardirq** | T4：`context_switch()` 發生在**中斷返回路徑**上 |

**6 µs 之後才發生 `sched_switch`，而且已經不在 hardirq 上下文** ——
證明切換發生在 T3/T4（中斷返回時），不是 T2（中斷處理常式內）。

**(d) 恢復的時機**

`sched_probe.ko` 的 IRQ 實驗（[Q13](#q13)）間接證明：
一個行程從 `schedule()` 回來時中斷已經被打開了，
表示它確實是「沿著堆疊往回走」而不是「直接跳回去」。

---

<a name="q11"></a>
## 11. 假設 prev 在時鐘中斷的驅動下發生了切換，選中的 next 是新創建的進程，那麼新進程從哪裡開始執行？由於時鐘中斷處理是在關中斷下進行的，若新進程一直在 loop 裡執行，是不是系統會因為沒辦法再響應時鐘中斷，一直運行這個新進程？

### 結論

**第一問：從 `ret_from_fork` 開始。**
**第二問：不會。因為 `ret_from_fork` 的第一件事就是 `bl schedule_tail`，
而 `schedule_tail() → finish_task_switch() → finish_lock_switch() → raw_spin_unlock_irq()`
會把中斷打開。**

**(1) 為什麼是 `ret_from_fork`？**

新行程從來沒執行過 `switch_to()`，所以它的 `cpu_context` 不是「存」出來的，
而是 `copy_thread()` **手工填**的：

```c
/* arch/arm64/kernel/process.c: copy_thread() */
int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	struct pt_regs *childregs = task_pt_regs(p);
	...
	if (likely(!args->fn)) {                 /* 使用者行程 */
		*childregs = *current_pt_regs();     /* 複製父行程的中斷現場 */
		childregs->regs[0] = 0;              /* ★ fork() 在子行程回傳 0 */
		...
		p->thread.cpu_context.x19 = 0;       /* x19 = 0 → 不是內核執行緒 */
	} else {                                 /* 內核執行緒 */
		memset(childregs, 0, sizeof(struct pt_regs));
		childregs->pstate = PSR_MODE_EL1h | PSR_IL_BIT;
		p->thread.cpu_context.x19 = (unsigned long)args->fn;      /* 回呼函式 */
		p->thread.cpu_context.x20 = (unsigned long)args->fn_arg;  /* 參數 */
	}
	p->thread.cpu_context.pc = (unsigned long)ret_from_fork;   /* ★ 入口 */
	p->thread.cpu_context.sp = (unsigned long)childregs;       /* ★ SP 指向 pt_regs */
	...
}
```

所以 `switch_to()` 的 `ldr lr, [x8]` 把 `lr` 載成 `ret_from_fork`，
`ret` 就跳過去了。

**(2) `ret_from_fork` 為什麼會開中斷？**

```asm
/* arch/arm64/kernel/entry.S:860 */
SYM_CODE_START(ret_from_fork)
	bl	schedule_tail       /* ★ 第一條指令 */
	cbz	x19, 1f             /* x19 == 0 → 不是內核執行緒 */
	mov	x0, x20
	blr	x19                 /* 內核執行緒：跳到回呼函式 */
1:	get_current_task tsk
	mov	x0, sp
	bl	asm_exit_to_user_mode
	b	ret_to_user         /* 使用者行程：回 EL0 */
SYM_CODE_END(ret_from_fork)
```

```c
asmlinkage __visible void schedule_tail(struct task_struct *prev)
{
	struct rq *rq;
	rq = finish_task_switch(prev);   /* ← 這裡面 */
	preempt_enable();
	if (current->set_child_tid) put_user(task_pid_vnr(current), current->set_child_tid);
	calculate_sigpending();
}

static struct rq *finish_task_switch(struct task_struct *prev)
{
	...
	finish_lock_switch(rq);          /* → raw_spin_unlock_irq(&rq->lock)  ★ 開中斷 */
	...
}
```

**(3) 更根本的理由：「關中斷」屬於 CPU，不屬於行程**

時鐘中斷處理是在關中斷下進行的沒錯，但：
* 那個「關中斷」狀態是**當時那顆 CPU** 的 `PSTATE.DAIF`；
* 行程切換之後，這顆 CPU 上跑的是 next，
  next 的第一件事就是 `raw_spin_unlock_irq()` **無條件開中斷**；
* **`DAIF` 不是被保存/恢復的行程上下文**（`cpu_context` 裡沒有它）。

所以「新行程繼承了關中斷狀態」這個前提本身就是錯的。

> **書目**：奔跑吧 §9.3.5「如何讓新進程執行」與圖 9.18（第 500~560 行）——
> 「當處理器切換到內核線程 1 時，它從 ret_from_fork 匯編函數開始執行，
> schedule_tail() 函數會打開中斷，因此，不用擔心內核線程 1 在關閉中斷的狀態下運行」。

### 實機驗證

**(a) 新行程的 `cpu_context.pc` 真的是 `ret_from_fork`**

```bash
ssh radxa@192.168.68.57 'sudo insmod ~/exp/sched/sched_probe.ko; sudo rmmod sched_probe; \
    sudo dmesg | sed "s/^\[[^]]*\] //" | grep -E "cpu_context.pc|task_pt_regs"'
```
```
   current 的 cpu_context.pc = ret_from_fork+0x0/0x20, sp = 0xffff80000daabeb0
   current->stack = ffff80000daa8000, task_pt_regs(current) = ffff80000daabeb0
```

`insmod` 是 `sched_trace.sh` 剛 fork 出來、**還沒被換出去過**的行程，
所以 `cpu_context` 還是 `copy_thread()` 的初值：
* **`pc == ret_from_fork`** ✅
* **`sp == task_pt_regs(current)`** ✅ 對應 `cpu_context.sp = (unsigned long)childregs`

**(b) 新行程的第一條指令 —— ftrace 直接抓到**

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

**子行程（pid 100848）的第一筆 trace：函式 `schedule_tail`，呼叫者 `ret_from_fork`，
堆疊回溯只有兩層** —— 因為它的核心堆疊是全新的，底下什麼都沒有。

**(c) 中斷確實會被打開 —— 用同一支模組的另一個實驗反證**

```bash
ssh radxa@192.168.68.57 'sudo insmod ~/exp/sched/sched_probe.ko test_irq=1; \
    sudo rmmod sched_probe; sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A5 "Ch9 Q13"'
```
```
== Ch9 Q13  raw_local_irq_disable() 之後直接 schedule() ==
   schedule() 之前 irqs_disabled() = 128
   schedule() 之後 irqs_disabled() = 0  <-- 中斷已經被打開了！
```

**這是同一個機制**：不管你進 `schedule()` 之前中斷是開是關，
`finish_task_switch()` 都會無條件 `raw_spin_unlock_irq()`。
新行程走的是 `schedule_tail()`，老行程走的是 `context_switch()` 裡那一行，
**兩條路徑最後都到 `finish_task_switch()`**。

**(d) 系統照樣正常 —— 直接跑一個死迴圈行程試試**

```bash
ssh radxa@192.168.68.57 'grep -i arch_timer /proc/interrupts; \
    taskset -c 3 sh -c "while :; do :; done" & sleep 3; \
    grep -i arch_timer /proc/interrupts; kill %1' 2>/dev/null
```
兩次讀數之間 CPU3 的 `arch_timer` 計數持續增加 → 死迴圈行程沒有讓時鐘中斷停止。

更直接的證據在 [Q4](#q4)：在 CPU3 上放 n 個死迴圈行程，
實測每個行程每 **3.33 ms（= 1 個 tick）** 就被搶佔一次
（`nivcsw` 與 CPU 時間相除得到），時鐘中斷顯然一直在進來。

---

<a name="q12"></a>
## 12. 在中斷處理函數中，能不能直接調用 schedule() 函數？為什麼？

### 結論

> **不能。會觸發 `BUG: scheduling while atomic`，而且極可能導致難以除錯的當機。**

**四個理由：**

**① 中斷上下文沒有「行程」可以被排程出去**

中斷處理常式**借用**被中斷那個行程的核心堆疊執行，它自己不是一個 task。
如果在裡面 `schedule()`，被換出去的會是**那個倒楣的、跟中斷毫無關係的行程**，
而中斷處理只做到一半，`pt_regs`、鎖、`in_interrupt()` 狀態全部錯亂。

**② 中斷處理完成前不會有 EOI，中斷控制器會卡住**

GIC 在 handler 返回並寫 `EOIR` 之前，該中斷處於 active 狀態，
同號中斷不會再送達。切換出去 = 這個中斷可能永遠不會結束。

**③ 核心有明確的檢查**

```c
/* kernel/sched/core.c: schedule_debug() */
static inline void schedule_debug(struct task_struct *prev, bool preempt)
{
	...
	if (unlikely(in_atomic_preempt_off())) {
		__schedule_bug(prev);
		preempt_count_set(PREEMPT_DISABLED);
	}
	...
}

/* kernel/sched/core.c:5776 */
static noinline void __schedule_bug(struct task_struct *prev)
{
	...
	printk(KERN_ERR "BUG: scheduling while atomic: %s/%d/0x%08x\n",
		prev->comm, prev->pid, preempt_count());
	debug_show_held_locks(prev);
	print_modules();
	if (irqs_disabled()) print_irqtrace_events(prev);
	check_panic_on_warn("scheduling while atomic");
	dump_stack();
}
```

`in_atomic()` 靠 `preempt_count` 的四個欄位判斷：

```
preempt_count (32 bit):
  bit  0- 7  PREEMPT      preempt_disable() 巢狀次數（需要 CONFIG_PREEMPT_COUNT）
  bit  8-15  SOFTIRQ      軟中斷（0x100 = SOFTIRQ_OFFSET）
  bit 16-19  HARDIRQ      硬中斷（0x10000 = HARDIRQ_OFFSET）
  bit 20     NMI
  bit 31     PREEMPT_NEED_RESCHED（反相）
```

**④ 所有可能睡眠的函式都不能在中斷上下文用**

`mutex_lock()`、`down()`、`kmalloc(GFP_KERNEL)`、`copy_from_user()`、
`msleep()`、`wait_event()` … 它們內部都有 `might_sleep()`。

**中斷裡要「觸發調度」的正確做法：**

| 需求 | 正確做法 |
|---|---|
| 讓當前行程稍後被搶佔 | `resched_curr()` / `set_tsk_need_resched()`（只設旗標） |
| 喚醒等待的行程 | `wake_up_interruptible()` / `complete()` |
| 做耗時的工作 | 丟給 workqueue（`schedule_work()`）或 threaded IRQ |
| 延遲執行 | tasklet / softirq |

> **書目**：奔跑吧 §9.3.5 最後的思考題；卷 2 §2.5.3 有完整分析。
> §8.1.5 也提到「若此時處於 atomic 上下文中，這是一個 bug，
> 那麼內核會發出警告並且輸出內核函數調用棧。發出的警告是『BUG: scheduling while atomic』」。

### 實機驗證

**(a) 軟中斷上下文的 `preempt_count`**

`sched_probe.ko` 用 timer callback（在 `TIMER_SOFTIRQ` 裡執行）：

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
       呼叫前 preempt_count = 0x0, in_atomic() = 0
       呼叫後 preempt_count = 0x0
```

**`preempt_count = 0x100 = SOFTIRQ_OFFSET`，`in_interrupt() = 256`，`in_atomic() = 1`** ✅
在這個上下文呼叫 `schedule()`，`in_atomic_preempt_off()` 會回 true，
`__schedule_bug()` 就會印出 splat。

（本實驗**刻意不真的呼叫** `schedule()`，因為在共用機台上讓軟中斷半途換走
有損壞系統的風險——書上也說「雖然內核能處理這種情況，但是在有些特殊場景下
會導致中斷棧被破壞，從而產生宕機問題」。）

**(b) 本機的一個重要限制：`CONFIG_PREEMPT_COUNT` 沒開**

```bash
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep -E "PREEMPT_COUNT|DEBUG_ATOMIC_SLEEP"'
```
```
# CONFIG_DEBUG_ATOMIC_SLEEP is not set
（沒有 CONFIG_PREEMPT_COUNT）
```

實測結果 (b)：**在行程上下文 `preempt_disable()` 之後，`preempt_count` 還是 0**。

**這代表本機的保護網有兩個破洞：**

| 檢查 | 需要什麼 | 本機 |
|---|---|---|
| `BUG: scheduling while atomic`（硬/軟中斷上下文） | `preempt_count` 的 HARDIRQ/SOFTIRQ 欄位（**永遠有**） | ✅ 抓得到 |
| `BUG: scheduling while atomic`（spinlock 臨界區） | `CONFIG_PREEMPT_COUNT` | ❌ **抓不到** |
| `BUG: sleeping function called from invalid context` | `CONFIG_DEBUG_ATOMIC_SLEEP` | ❌ **`might_sleep()` 是空操作** |

**所以在這台機器上，「在 spinlock 裡睡眠」這種 bug 完全不會被報出來，
只會表現成隨機當機。** 這是做核心開發時一定要在測試核心上打開
`CONFIG_DEBUG_ATOMIC_SLEEP` + `CONFIG_PREEMPT_COUNT` + `CONFIG_PROVE_LOCKING` 的理由。

**(c) 檢查程式碼本身確實編譯進去了**

```bash
grep -n -A4 "if (unlikely(in_atomic_preempt_off()))" kernel/sched/core.c
```
```c
	if (unlikely(in_atomic_preempt_off())) {
		__schedule_bug(prev);
		preempt_count_set(PREEMPT_DISABLED);
	}
```
`__schedule_bug()` 是**無條件編譯**的（不需要任何 DEBUG 選項），
所以在中斷上下文呼叫 `schedule()`，本機一定會噴 splat。

**(d) `in_interrupt()` 的定義**

```bash
grep -n "define in_interrupt\|define in_atomic\b" include/linux/preempt.h
```
```c
#define in_interrupt()  (irq_count())      /* = HARDIRQ + SOFTIRQ + NMI */
#define in_atomic()     (preempt_count() != 0)
```

---

<a name="q13"></a>
## 13. 假設在 raw_local_irq_disable() 函數後直接調用 schedule() 函數，若調度器選擇的 next 進程是一個 loop 執行的進程，那是不是系統就不能響應時鐘中斷，從而癱瘓？

### 結論

> **不會癱瘓。因為「關中斷」是 CPU 的狀態，不是行程的狀態；
> `schedule()` 內部的 `finish_task_switch()` 會無條件把中斷打開。**

追一遍程式碼：

```c
raw_local_irq_disable();          /* PSTATE.DAIF.I = 1，這顆 CPU 關中斷 */
schedule();
```

```c
asmlinkage __visible void __sched schedule(void)
{
	struct task_struct *tsk = current;
	sched_submit_work(tsk);
	do {
		preempt_disable();
		__schedule(SM_NONE);
		sched_preempt_enable_no_resched();
	} while (need_resched());
	sched_update_worker(tsk);
}

static void __sched notrace __schedule(unsigned int sched_mode)
{
	...
	local_irq_disable();          /* 本來就要關（已經關了，無所謂）*/
	rq_lock(rq, &rf);
	...
	next = pick_next_task(rq, prev, &rf);
	...
	if (likely(prev != next)) {
		rq = context_switch(rq, prev, next, &rf);   /* ★ 換人 */
	} else {
		rq_unlock_irq(rq, &rf);                     /* ★ 沒換人也開中斷 */
	}
	...
}

static __always_inline struct rq *
context_switch(struct rq *rq, struct task_struct *prev, struct task_struct *next, ...)
{
	...
	switch_to(prev, next, prev);
	barrier();
	return finish_task_switch(prev);
}

static struct rq *finish_task_switch(struct task_struct *prev)
{
	...
	finish_lock_switch(rq);
	...
}

static inline void finish_lock_switch(struct rq *rq)
{
	spin_acquire(&__rq_lockp(rq)->dep_map, 0, 0, _THIS_IP_);
	__balance_callbacks(rq);
	raw_spin_rq_unlock_irq(rq);      /* ★★ 無條件 local_irq_enable() ★★ */
}
```

**關鍵在最後那一行 `raw_spin_rq_unlock_irq()` —— 是 `_irq` 版本不是 `_irqrestore` 版本，
它不管你原本是開是關，一律把中斷打開。**

**所以會發生什麼事？**

1. 你關了中斷，呼叫 `schedule()`；
2. 調度器選了 next（那個 loop 行程），切過去；
3. **next 執行 `finish_task_switch()` → 中斷被打開**；
4. next 的 loop 正常跑，時鐘中斷正常進來，`scheduler_tick()` 正常運作；
5. 過一陣子 next 被搶佔，某個行程又切回「你」；
6. **「你」從 `schedule()` 返回時，中斷是「開著」的** —— 你關的那次已經失效了。

**所以真正該擔心的不是「系統癱瘓」，而是：**

> ⚠️ **`schedule()` 會偷偷改變你的中斷狀態。**
> 如果你的程式碼假設「`schedule()` 回來之後中斷還是關的」，那就是個 bug。
> 這也是為什麼核心裡幾乎所有 `schedule()` 的呼叫點都在**開中斷**的狀態下。

（順帶一提：本機 `CONFIG_PREEMPT_COUNT=n`，所以這段程式碼**不會**觸發
`BUG: scheduling while atomic`——`preempt_count` 仍是 0，`irqs_disabled()` 不在檢查範圍內。
在有 `CONFIG_DEBUG_ATOMIC_SLEEP` 的核心上，`might_sleep()` 會抓到 `irqs_disabled()`。）

> **書目**：奔跑吧 §9.3.5「調度的本質」（第 545~560 行）——
> 「顯然，上述分析是不正確的。因為進程 B 切換執行時會打開本地中斷，以防止系統癱瘓」。

### 實機驗證

**這一題可以「直接做」給面試官看。** `sched_probe.ko` 的 `test_irq=1`：

```c
static void test_irq_across_schedule(void)
{
	int before, inside_after;

	local_irq_disable();
	before = irqs_disabled();
	set_current_state(TASK_RUNNING);   /* 只是讓出 CPU，不睡眠 */
	schedule();
	inside_after = irqs_disabled();
	local_irq_enable();
	P("   schedule() 之前 irqs_disabled() = %d\n", before);
	P("   schedule() 之後 irqs_disabled() = %d\n", inside_after);
}
```

```bash
ssh radxa@192.168.68.57 'sudo insmod ~/exp/sched/sched_probe.ko test_irq=1; \
    sudo rmmod sched_probe; sudo dmesg | sed "s/^\[[^]]*\] //" | grep -A6 "Ch9 Q13"'
```

```
== Ch9 Q13  raw_local_irq_disable() 之後直接 schedule() ==
   schedule() 之前 irqs_disabled() = 128
   schedule() 之後 irqs_disabled() = 0  <-- 中斷已經被打開了！
   原因：__schedule() -> context_switch() -> finish_task_switch() ->
         finish_lock_switch() -> raw_spin_unlock_irq(&rq->lock) 會無條件開中斷。
         「關中斷」這個狀態屬於 CPU，不屬於行程；切換出去就不再屬於你。
```

**`irqs_disabled()` 從 128（非 0，表示關中斷）變成 0（中斷已開）。**

（`128` 是 ARM64 `irqs_disabled()` 的回傳值：它讀 `DAIF` 的 `I` 位，
`PSR_I_BIT = 1 << 7 = 128`。）

**而且系統毫髮無傷** —— `rmmod` 正常、後續指令正常，
證明「關中斷 + `schedule()`」不會讓系統癱瘓。

**補充：`128` 這個數字是什麼？**

ARM64 的 `irqs_disabled()` 讀 `DAIF` 的 `I` 位元：

```c
/* arch/arm64/include/asm/irqflags.h */
static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return (flags & (PSR_I_BIT | PSR_F_BIT)) == (PSR_I_BIT | PSR_F_BIT) ... ;
}
```
`PSR_I_BIT = 1 << 7 = 128`，所以「關中斷」回傳 128、「開中斷」回傳 0。

---

## 附錄 A：本章用到的全部指令速查

```bash
# ── 行程層級 ────────────────────────────────────────────────────
cat /proc/<pid>/sched                 # Q1  完整調度資訊（需 CONFIG_SCHED_DEBUG）
cat /proc/<pid>/schedstat             # Q1  sum_exec / wait_sum / nr_switches
echo 1 > /proc/sys/kernel/sched_schedstats   # 打開統計欄位
ps -eo pid,cls,pri,rtprio,ni,psr,pcpu,comm   # Q1/Q7

# ── CFS / 就緒佇列 ──────────────────────────────────────────────
cat /sys/kernel/debug/sched/debug      # Q2  取代 /proc/sched_debug
grep -A22 '^cfs_rq\[3\]:/$' /sys/kernel/debug/sched/debug
awk '/^runnable tasks:/{f=1} f' /sys/kernel/debug/sched/debug

# ── 可調參數 ────────────────────────────────────────────────────
grep . /sys/kernel/debug/sched/{latency_ns,min_granularity_ns,\
wakeup_granularity_ns,migration_cost_ns,nr_migrate,tunable_scaling}   # Q4
cat /sys/kernel/debug/sched/features                                   # 特性開關
ls /proc/sys/kernel/ | grep sched                                      # 還留在 sysctl 的

# ── 調度域 ──────────────────────────────────────────────────────
ls /sys/kernel/debug/sched/domains/cpu0/            # Q3
cat /sys/kernel/debug/sched/domains/cpu0/domain0/{name,flags,imbalance_pct}
echo 1 > /sys/kernel/debug/sched/verbose            # 開啟拓撲列印
echo 0 > /sys/devices/system/cpu/cpu7/online; echo 1 > /sys/devices/system/cpu/cpu7/online
dmesg | grep -A3 "attaching sched-domain"           # 才看得到 groups
head -4 /proc/schedstat

# ── 追蹤 ────────────────────────────────────────────────────────
/tmp/sched_trace.sh switch      # Q5/Q8/Q9/Q10  sched_switch / waking / wakeup
/tmp/sched_trace.sh newtask     # Q5/Q11        schedule_tail <- ret_from_fork
/tmp/sched_trace.sh tick        # Q4            scheduler_tick 呼叫圖
/tmp/sched_trace.sh balance     # Q5            sched_migrate_task

# ── 核心模組 ────────────────────────────────────────────────────
insmod sched_probe.ko                    # Q7/Q8/Q9/Q11  基本資訊
insmod sched_probe.ko target_pid=<pid>   # Q7            dump 指定行程
insmod sched_probe.ko test_irq=1         # Q13           關中斷 + schedule()
insmod sched_probe.ko test_atomic=1      # Q12           原子上下文
```

## 附錄 B：實驗環境的復原

```bash
ssh radxa@192.168.68.57 'sudo bash -c "
echo 0 > /proc/sys/kernel/sched_schedstats
echo 1 > /proc/sys/kernel/sched_autogroup_enabled
echo 0 > /sys/kernel/debug/sched/verbose
echo 24000000 > /sys/kernel/debug/sched/latency_ns
echo  3000000 > /sys/kernel/debug/sched/min_granularity_ns
for c in 0 1 2 3 4 5 6 7; do echo 1 > /sys/devices/system/cpu/cpu\$c/online 2>/dev/null; done
T=/sys/kernel/debug/tracing
echo 0 > \$T/tracing_on; echo nop > \$T/current_tracer; echo 0 > \$T/events/enable
echo ff > \$T/tracing_cpumask; echo > \$T/trace
echo > \$T/set_ftrace_filter; echo > \$T/set_graph_function
rmmod sched_probe 2>/dev/null; true"'
```
