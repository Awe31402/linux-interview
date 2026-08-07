# 《奔跑吧 Linux內核》（第二版）卷1：第 1~9 章 高頻面試題筆記

本目錄收錄《奔跑吧 Linux內核》（第二版）卷1 基礎架構 各章節開篇之「本章的高頻面試題」，共計 245 道題目。

## 📚 卷1 章節目錄索引 (基礎架構 245 題)

1. 📖 [第 1 章 處理器架構 (34 題)](file:///home/awe/disk/linux-rockchip/notes/ch01_processor_architecture.md)
2. 📖 [第 2 章 ARM64 在 Linux 內核中的實現 (23 題)](file:///home/awe/disk/linux-rockchip/notes/ch02_arm64_in_linux_kernel.md)
3. 📖 [第 3 章 內存管理之預備知識 (7 題)](file:///home/awe/disk/linux-rockchip/notes/ch03_memory_management_prerequisites.md)
4. 📖 [第 4 章 物理內存與虛擬內存 (46 題)](file:///home/awe/disk/linux-rockchip/notes/ch04_physical_and_virtual_memory.md)
5. 📖 [第 5 章 內存管理之高級主題 (48 題)](file:///home/awe/disk/linux-rockchip/notes/ch05_memory_management_advanced_topics.md)
6. 📖 [第 6 章 內存管理之實戰案例分析 (14 題)](file:///home/awe/disk/linux-rockchip/notes/ch06_memory_management_case_studies.md)
7. 📖 [第 7 章 進程管理之基本概念 (15 題)](file:///home/awe/disk/linux-rockchip/notes/ch07_process_management_basic_concepts.md)
8. 📖 [第 8 章 進程管理之調度與負載均衡 (45 題)](file:///home/awe/disk/linux-rockchip/notes/ch08_process_management_scheduling_and_load_balancing.md)
9. 📖 [第 9 章 進程管理之調試與案例分析 (13 題)](file:///home/awe/disk/linux-rockchip/notes/ch09_process_management_debugging_and_case_studies.md)

---
* 📄 [卷 1 彙整單檔 (全部 245 題)](file:///home/awe/disk/linux-rockchip/notes/running_linux_kernel_v1_interview_questions.md)

---

## 📚 卷2 章節目錄索引 (調試與案例分析 95 題)

1. 📖 [第 1 章 (全書第10章) 併發與同步 (37 題)](file:///home/awe/disk/linux-rockchip/notes/ch10_concurrency_and_synchronization.md)
2. 📖 [第 2 章 (全書第11章) 中斷管理 (15 題)](file:///home/awe/disk/linux-rockchip/notes/ch11_interrupt_management.md)
3. 📖 [第 3 章 (全書第12章) 內核調試與性能優化 (14 題)](file:///home/awe/disk/linux-rockchip/notes/ch12_kernel_debugging_and_performance_optimization.md)
4. 📖 [第 4 章 (全書第13章) 基於 x86_64 解決宕機難題 (13 題)](file:///home/awe/disk/linux-rockchip/notes/ch13_x86_64_crash_debugging.md)
5. 📖 [第 5 章 (全書第14章) 基於 ARM64 解決宕機難題 (7 題)](file:///home/awe/disk/linux-rockchip/notes/ch14_arm64_crash_debugging.md)
6. 📖 [第 6 章 (全書第15章) 安全漏洞分析 (9 題)](file:///home/awe/disk/linux-rockchip/notes/ch15_security_vulnerabilities.md)

---
* 📄 [卷 2 彙整單檔 (全部 95 題)](file:///home/awe/disk/linux-rockchip/notes/running_linux_kernel_v2_interview_questions.md)

---


## 🛠 工具指南

* 📘 [crash 工具使用指南](./crash_tool_guide.md) — 從「三個前提（vmcore／vmlinux／版本相符）」、如何設定 Kdump、
  50 個子命令的分類速查表，到八個實戰流程（oops 定位、D 狀態死鎖、從堆疊推導區域變數與參數、找鎖的持有者與等待者、
  算行程被阻塞多久、記憶體耗盡、批次化腳本）、**ARM64 專屬注意事項**與**錯誤訊息排錯表**。
  含本專案在 ROCK 5B 上「把 crash 搬上板子」的完整實測紀錄（重建相符的 vmlinux、`/dev/crash` driver）與替代方案。

---

## ✅ 解答（含 RK3588 實機實驗）

以下解答皆在 **Radxa ROCK 5B（RK3588, Linux 6.1.115+ aarch64, 8GB）** 上實測，
每一題都標註了**書目出處**與**下給機台的指令 + 實際輸出**。

> ⚠ 機台走 DHCP，IP 會變（本文成稿期間出現過 `192.168.68.57` 與 `192.168.68.58`）。
> 連不上時先做一次區網掃描確認：
> `for i in $(seq 1 254); do (ping -c1 -W1 192.168.68.$i >/dev/null 2>&1 && echo 192.168.68.$i) & done`

| 章節 | 解答檔 | 題數 | 亮點 |
|------|--------|------|------|
| 第 2 章 | 📝 [ch02_arm64_in_linux_kernel.md](./ch02_arm64_in_linux_kernel.md) | 23 | 自寫核心模組**直接讀 EL1 系統暫存器**（TCR/TTBR/MAIR/CLIDR…）＋軟體巡覽 4 級頁表；**TTBR0 == `__pa(mm->pgd)` 一位不差**；抓到活的 **2MB BLOCK 描述符**；SB litmus **31% 亂序 → `dmb ish` 後 0**。**修正三項常見誤解**（`PAGE_OFFSET` 值、佈局方向、實體位址寬度） |
| 第 3 章 | 📝 [ch03_memory_management_prerequisites.md](./ch03_memory_management_prerequisites.md) | 7 | 核心模組把 **Q3 的九種轉換全部跑出實際數值**；cache 延遲階梯的**轉折點與 TRM Table 3-1 的 cache 大小完全吻合**（A76 L1 = 4.0 cycles）；DTB→memblock→MemTotal 一路對帳。**修正 ZONE_DMA32 的錯誤說法** |
| 第 4 章 | 📝 [ch04_physical_and_virtual_memory.md](./ch04_physical_and_virtual_memory.md) | 46 | ESR_EL1 逐位解碼（`WnR` 只差 bit 6）；**fault-around 讓 4096 頁只缺頁 256 次**；COW **reuse vs copy** 用「缺頁 4096 次但 RSS 不變」證明；vmalloc **15/16 頁實體不連續**；**8 CPU 併發缺頁 86% 是白工**。修正 SLAB→SLUB、`kmalloc-128` 最小 |
| 第 5 章 | 📝 [ch05_memory_management_advanced_topics.md](./ch05_memory_management_advanced_topics.md) | 48 | **KSM 64 頁合併成 1 頁**（PFN `0x731ab`）寫入後 COW 分家；**規整實際搬動 284 頁**（PFN 低→高）資料 100% 完整；換出換入 **`pgmajfault`=`pswpin`=65529 一頁不差**；`boost=6463` 打通 fallback→boost→kcompactd 完整因果鏈 |
| 第 6 章 | 📝 [ch06_memory_management_case_studies.md](./ch06_memory_management_case_studies.md) | 14 | MemTotal 差值 **259092 kB 逐項對帳成功**；LRU 恆等式**完全吻合**；`MADV_PAGEOUT` 直接證明 shmem 不計入 `VmSwap`；抓到 **watermark boost 正在生效且已飽和（6463 頁）** |
| 第 7 章 | 📝 [ch07_process_management_basic_concepts.md](./ch07_process_management_basic_concepts.md) | 15 | strace 抓出 fork/vfork/pthread 的 clone flags；**16384 次 COW 缺頁精準命中**；VmPTE 逐級變化證明頁表按需配置；ftrace 抓到 **`schedule_tail <-ret_from_fork`**；fork 輸出 **6 vs 8 兩種答案都重現** |
| 第 8 章 | 📝 [ch08_process_management_scheduling_and_load_balancing.md](./ch08_process_management_scheduling_and_load_balancing.md) | 45 | **Δvruntime/Δexec = 3.0567 = 1024/335 一位不差**；時間片實測全部命中 `__sched_period()`（n≤8→24 ms、n>8→n×3 ms）；模組算出 **`LOAD_AVG_MAX = 47742`** 與核心相同；**頻率不變性 2256/1200/600 MHz → util 243/131/64**；**算力不變性 A55/A76 = 0.403 vs 422/1024**；`cost = power×fmax/f` **19 個 OPP 全部對帳**；**切成 schedutil 後 dmesg 噴出「starting EAS」**，輕載 100% 落 A55、關掉 EAS 後 73% 跑上 A76；**ftrace 抓出 `kworker/u16` 讓 SCHED_FIFO 行程等了 10 ms**。**修正 12 處 5.0→6.1 的差異** |
| 卷2 第 1 章 | 📝 [ch10_concurrency_and_synchronization.md](./ch10_concurrency_and_synchronization.md) | 37 | **從記憶體讀出被 alternatives patch 過的指令**，8 種原子操作全部對應到 LSE（`stadd`/`ldaddal`/`casal`/`casa`/`casl`/`cas`/`swpal`）；**LL/SC vs LSE 大小核實測推翻「LSE 一定比較快」**（A55 上 LSE 慢 1.8 倍、A76 激烈爭用時慢 3.5 倍）；qspinlock 三元組狀態機完整重現 `{0,0,1}→{0,1,1}→{CPU2,..}→{CPU3,..}` 且**證明嚴格 FIFO**（三個競爭者相隔 200 µs 依序接棒）；**樂觀自旋 34444 次拿鎖只睡 2 次 vs 645 次拿鎖睡 1156 次**；`synchronize_rcu()` **43.7 ms vs expedited 54 µs（810 倍）**；ftrace 抓到 GP 狀態機與 `qsmask` 位圖 `8>f7→2>f5→1>f4→f4>0`；**修正書上表 1.4 的 pending 位寬**。附「我把機器鎖死」的死鎖活教材 |
| 第 9 章 | 📝 [ch09_process_management_debugging_and_case_studies.md](./ch09_process_management_debugging_and_case_studies.md) | 13 | **`/proc/sched_debug` 與 `sched_latency_ns` 全部搬到 debugfs**（書上路徑已失效）；`latency_ns=24 ms / min_granularity_ns=3 ms`，**臨界點 nr_running=8 實測命中**；**RK3588 只有 1 層 MC 域、8 個單 CPU 調度組**（與書上兩層拓撲不同）；書上 §9.2 場景重現：**5 個行程 200 ms 內收斂成 3/2**；**關中斷後 `schedule()` 回來 `irqs_disabled()` 從 128 變 0** |
| 卷2 第 2 章 | 📝 [ch11_interrupt_management.md](./ch11_interrupt_management.md) | 15 | 用 kprobe + `get_irq_regs()` **抓下真實中斷的 `pt_regs`**：使用者態被打斷時 `pc=0xaaaae6faad0c`、`sp` 是 user stack、`stackframe={0,0}`，`&pt_regs` 距核心棧頂**剛好 336 B = `sizeof(pt_regs)`**；**VBAR_EL1 不是 `vectors` 而是 `__bp_harden_el1_vectors`**（Spectre-BHB 副本）；**PSTATE.M=EL2h → 這台機器的核心跑在 EL2（VHE）**；**TRM #237 `irq_emmc` → DTB `<0 205 4>` → hwirq 237 → virq 160** 四層對帳（重開機後 **virq 變成 171 而 hwirq 不變**，證明 virq 是每次開機重配的）；**8 顆 CPU 的中斷棧位址實測**（4526 次中斷、每 CPU 一段、間隔 0x8000）；tasklet 忙等 30 ms **收到 10 次時鐘中斷**＋ftrace `d.H..` 旗標；**同類軟中斷 8 CPU 並行 vs 同一 tasklet 恆為 1**；**行程被軟中斷卡住 40009218 ns**；`local_bh_enable` 的 **`preempt_count=0x101`「留 1」看得見**；CMWQ **6 個睡 300 ms 的 work 只花 316 ms、kworker 6→10** vs 燒 CPU 的 **1 個 worker 360 ms**。**修正 14 處 5.0/GIC-V2/QEMU → 6.1/GIC-600/實機的差異**。**全部實驗重開機後完整重跑驗證過**（25 項指標 24 項完全重現，唯一差異揭露「virq 每次開機重配、hwirq 不變」） |
| 卷2 第 3 章 | 📝 [ch12_kernel_debugging_and_performance_optimization.md](./ch12_kernel_debugging_and_performance_optimization.md) | 14 | 同一函式 **-O0 是 37 條指令／6 個變數全在堆疊，-O2 是 18 條／0 個**，-O2 行號表**同一位址 0x8 掛了 9 個行號**（游標亂跳的真身），且 **-O0 在本機真的編不過**（`asm goto` 約束失敗）；**U-Boot `kernel_addr_r=0x00400000` → `/proc/iomem` → `_stext=0xffff800008010000`** 三個位址一路對上，DTB/initrd 位址也對上；把機器碼搬家後 **`adr`/`bl` 跟著走、`ldr x0,=sym` 文風不動**；重定位三連拍：使用者態 PIE 的 `R_AARCH64_RELATIVE addend=e18`、vmlinux **262510 筆**、模組把 `bl 0 <_printk>` **就地改寫成 `95fbdf99`**；樹外 `TRACE_EVENT()` 不重編核心就長出 `events/tp_lab/`，並拍到 **static key 把 `d503201f`(NOP) 改成 `14000002`(B)**；不改 cmdline 用私有 kmem_cache 重現 slub_debug **五種錯誤全部**；**沒有 lockdep 的機器上量死鎖**：AA 自旋鎖 500 ms 內 trylock 失敗 **92,897,122 次**、AA mutex 睡死 4.17 秒且**被 SIGKILL 叫醒後竟「假裝」拿到了鎖**（附 `mutex.c:689` 原始碼解釋）、書上 `cancel_delayed_work_sync` 死鎖**完整重現**（`dl_book` 進 D 狀態，堆疊正是 `__flush_work → __cancel_work_timer`）；真 oops 的 **ESR `0x96000044`(寫) vs `0x96000004`(讀)**、`Code:` 行 → `decodecode` → `faddr2line` 直指 **oops_lab.c:46** |
| 卷2 第 4 章 | 📝 [ch13_x86_64_crash_debugging.md](./ch13_x86_64_crash_debugging.md) | 13 | **這章沒有 Kdump 可用**（板子 `CONFIG_KEXEC` 沒開、主機 `kexec_crash_size=0`），於是把 crash 的 `ps`/`bt`/`bt -f`/`rd`/`struct rw_semaphore`/`list`/`task -R`/`runq -t` **全部用核心模組自己實作一遍**；三個偵測器（softlockup/hardlockup/hung_task）本機也全沒編進去，照著 `kernel/watchdog.c` 與 `kernel/hung_task.c` 各做一份迷你版，抓到 **`BUG: soft lockup - CPU#3 stuck for 12s!`**（心跳照跳、`touch_ts` 落後 21 秒）與 **關中斷 14 秒被鄰居 CPU2 抓到（心跳凍在 17）**；書上 §4.10 的自鎖案例在 ARM64 上完整重演——`insmod` 卡死、**`pgrep`/`ps` 跟著一起排進 `wait_list`**、從堆疊推出 **`priv = x29-0x60 = 0xffff80001024ba40`** 讀到 `benshushu`、阻塞時間 **29.157 秒 vs 實際 29.16 秒**。**修正 13 處 3.10→6.1 的差異**（`watchdog/N` 執行緒已刪、`mmap_sem`→`mmap_lock`、偏移 0x78→0x88、`rwsem.owner` 變成帶旗標的 `atomic_long_t`、卡住的 `ps` 現在是 TASK_KILLABLE…）|
| 卷2 第 5 章 | 📝 [ch14_arm64_crash_debugging.md](./ch14_arm64_crash_debugging.md) | 7 | **這章板子本身就是主場**：三層核心呼叫鏈的框架指標 `bd90/bdd0/be50` 用框架鏈爬出來，**和模組自己印的 ground truth 一位不差**；用四種序幕推翻書上「FP 一定等於 SP」——**真葉子函式連框架記錄都沒有**（實測 oops 的 calltrace 因此少了一層 `oops_lab_init`，名字只出現在 `lr :`），核心 `-Os` 是 `add x29,sp,#0x10`，VLA 讓 sp 再掉 64 B；書上式(5.2) 的「LR−4」在 6.1 是 **`%pSb` → `sprint_backtrace()` 減 1**（`kallsyms.c:605-621`）；**參數不在堆疊上**——照書上 §5.5.2 從 `rwsem_down_read_slowpath` 的 `stp x19,x20,[sp,#96]` 回推，**4 個參數 4/4 完全命中**，局部變數 `priv = x29-0x60` 讀到 `benshushu`；阻塞時間 **3276.599 秒 vs 實際 3276.9 秒**。**還原地重建了一個和跑著的核心逐位元組相符、帶 DWARF 的 vmlinux**（版本橫幅一字不差、符號位址全對），裝了 crash 8.0.2 + `/dev/crash` driver，最後卡在 crash 猜錯 VA_BITS（47 vs 48）——完整分析與「要怎樣才有真 Kdump」寫在附錄 A |
| 卷2 第 6 章 | 📝 [ch15_security_vulnerabilities.md](./ch15_security_vulnerabilities.md) | 9 | **一台「熔斷免疫、幽靈仍中」的對照組**：自寫 `perf_user_access=1 + config1=0x2` 拿到週期級 `PMCCNTR_EL0`，Flush+Reload 直方圖**命中 52 vs 未命中 399 週期兩峰完全分離**、隱蔽通道把 `"benshushu"` **9/9** 傳出來；**熔斷 PoC 打不穿**——核心植入已知祕密 `0x5a`，signal 法探測 2000 次**一個位元組都偷不到**（A76 `CSV3=1`、A55 白名單）；**但幽靈變體1 PoC 在 A76 上越過 `if(x<size)` 洩漏 40/40 位元組** "The Magic Words are Squeamish Ossifrage."；**KPTI 編了卻沒生效**（核心頁 `nG=0`、`/proc/kallsyms` 無 `tramp_vectors`、VBAR≠跳板）；**v2/BHB 硬化向量表只掛 A76**（`__bp_harden_el1_vectors`）、in-order 的 **A55 用原始 `vectors`**；分支誤判懲罰 **A76 2.29× vs A55 1.74×**；在**正在跑的核心** `invoke_syscall` 反組譯出 `cmp/sbc/csdb`（`csdb=0xd503229f`）證明 `array_index_nospec`。**踩坑記**：4096 步長會把探針全撞進同一個 L1 cache set（4-way）而失真，改 4160 步長散到不同 set 才穩定 |

### 實驗程式

`experiments/` 底下是解答中用到的所有程式碼。

**核心模組**（在機台上編譯；`/lib/modules/$(uname -r)/build` 已指向完整核心原始碼樹）：

| 檔案 | 用途 | 題目 |
|------|------|------|
| `armv8_dump.c` | 讀 EL1 系統暫存器（TCR/TTBR/MAIR/ID_AA64MMFR0/SCTLR/CTR/CLIDR）、印核心 VA 佈局、軟體巡覽 4 級頁表 | 2-1~6, 2-9~16, 2-20, 2-22, 2-23, 3-4~6 |
| `mm_convert.c` | 把 mm/VMA/page/PFN/paddr/PTE/zone/pgdat 的九種轉換全部跑一遍 | **3-3** |
| `mm_probe.c` | 伙伴系統 free_area（66 條鏈）、gfp_zone 表、zonelist、水位、SLUB、`page->flags` 佈局、外碎片指標 | **4-1~4-7, 4-18, 5-4, 5-38, 5-42~5-47** |
| `sched_probe.c` | 權重/wmult 表對帳、`__calc_delta()`、PELT 衰減表與 `LOAD_AVG_MAX`、**ARM64 ASID**、`cpu_context` 佈局、行程優先級欄位、**關中斷後 `schedule()`**、原子上下文檢查 | **8-1, 8-3, 8-7, 8-13, 8-19, 8-20, 8-28, 8-43~8-45, 9-7, 9-9, 9-11~9-13** |
| `sync_probe.c` | **把 alternatives patch 後的指令從記憶體讀出來**（LSE/屏障/`smp_cond_load`）、qspinlock 欄位、MCS/OSQ、mutex/rwsem/semaphore 結構、`ULONG_CMP`、`PG_locked` | **卷2 1-1~1-9, 1-11~1-15, 1-17~1-19, 1-22, 1-23, 1-25, 1-26, 1-31, 1-35** |
| `lock_bench.c` | qspinlock 三元組狀態機、六種同步機制吞吐量對比、樂觀自旋 vs 睡眠等待 | **卷2 1-10, 1-12, 1-16, 1-20, 1-21, 1-25, 1-27, 1-33** |
| `rcu_demo.c` | 書上 §1.10.1 讀者/寫者範例、GP 延遲量測、`SLAB_TYPESAFE_BY_RCU`、RCU 停滯偵測 | **卷2 1-27~1-30, 1-32, 1-36** |
| `Makefile.mod` | 兩個模組的 Kbuild Makefile（上傳時改名為 `Makefile`） | — |

**使用者態程式**：

| 檔案 | 用途 | 題目 |
|------|------|------|
| `pagemap_walk.c` | 使用者態 VA→PFN→PA（`/proc/self/pagemap`），與核心模組交叉驗證 | 2-2, 2-4, 3-3 |
| `barrier_sb.c` | Store-Buffer litmus test：證明 ARM64 弱序 + 屏障有效 | **2-17, 2-18** |
| `barrier_mp.c` | Message-Passing litmus test（含「為何測不到」的誠實說明） | 2-18 |
| `cache_ladder.c` | Cache 延遲階梯（pointer chase），量出 L1/L2/L3/DRAM | **3-2** |
| `hold_page.c` | 配一頁匿名記憶體 + fork 共享，供 `mm_convert.ko` 查詢 | 3-3 |
| `fault_types.c` | 五種缺頁的精準計數（fault-around、ZERO_PAGE、COW reuse vs copy） | 4-24, 4-25, 4-32, 4-38~4-42 |
| `esr_far.c` | 從訊號處理常式讀 `ESR_EL1`/`FAR_EL1` 並解碼 | 4-34~4-36, 4-41 |
| `mmap_vma.c` | MAP_FIXED / MAP_FIXED_NOREPLACE / VMA 合併 / brk | 4-21, 4-31~4-33 |
| `concurrent_fault.c` | 8 CPU 同時對同一頁缺頁 | 4-46 |
| `slub_info.sh` | 從 sysfs 讀 SLUB 每個 cache 的 order/objs_per_slab | 4-13, 4-17 |
| `ksm_test.c` | KSM 合併 + COW 分家（自動還原 sysfs） | 5-31~5-34, 5-37 |
| `migrate_compact.c` | 頁面遷移 + 記憶體規整，PFN 前後對照 | 5-23~5-26, 5-28~5-30, 5-48 |
| `reclaim_test.c` | LRU / swappiness / workingset / kswapd 統計 | 5-10~5-22 |
| `lru_shmem.c` | shmem/anon 對 LRU、AnonPages、Cached、Mapped 的影響 | 6-5, 6-6, 6-7 |
| `swap_shmem.c` | `MADV_PAGEOUT` 比對 `VmSwap` vs `SwapFree` | 6-9 |
| `life.c` | 僵屍態、`wait()` 回收、孤兒託孤 | 7-3, 7-5 |
| `tid.c` | `getpid()` vs `gettid()` | 7-4 |
| `prims.c` / `vfork_order.c` | fork/vfork/clone 的差異 | 7-8 |
| `cow.c` | COW 缺頁計數與 smaps Shared/Private_Dirty | 7-9 |
| `q11.c` / `q11_pid.c` | fork 迴圈輸出幾個 `_` | 7-11 |
| `pgtbl.c` / `pgfork.c` | 頁表按需配置與 fork 複製 | 7-12 |
| `sched_weight.c` | nice→weight→CPU 佔比、Δvruntime/Δexec、時間片 vs nr_running | **8-1, 8-3, 8-8, 8-14, 8-21, 9-4** |
| `pelt_duty.c` | 週期性負載產生器（固定 duty / 固定工作量），驗證 PELT 的頻率與算力不變性 | **8-15~8-23, 8-29~8-32** |
| `wake_cpu.c` | 1 waker + N wakee 的 pipe ping-pong，觀察 wake_affine / wake_wide | **8-28** |
| `lb_case.c` | 書上 §9.2 的負載均衡場景重現 | **8-25~8-27, 9-5** |
| `vruntime_place.c` | `place_entity()` 的 START_DEBIT 與 GENTLE_FAIR_SLEEPERS | **8-5, 8-6** |
| `rt_latency.c` | 迷你 cyclictest：SCHED_OTHER vs SCHED_FIFO、空閒 vs 滿載的喚醒延時 | **8-36~8-40** |
| `sched_trace.sh` | ftrace 腳本（switch / newtask / tick / wakeup / balance） | **8-4, 8-9~8-12, 8-41, 9-5, 9-8~9-11** |
| `lse_bench.c` | LL/SC vs LSE 原子指令吞吐量（可指定大核/小核） | **卷2 1-1** |

**中斷管理（卷2 第 2 章）用的模組與腳本**：

| 檔案 | 用途 | 題目 |
|------|------|------|
| `irq_probe.c` | 從 VBAR_EL1 dump 異常向量表、`pt_regs` 欄位偏移、`preempt_count` 佈局、中斷棧大小、掃描全部 `irq_desc`（virq↔hwirq↔chip↔domain↔action） | **卷2 2-1~2-4, 2-7, 2-14, 2-15** |
| `irq_live.c` | kprobe + `get_irq_regs()` 抓**真實中斷現場**；`stackmap=1` 統計每 CPU 中斷棧位址 | **卷2 2-1~2-4, 2-14, 2-15** |
| `ctx_probe.c` | 八種執行環境的上下文指紋表、tasklet 忙等期間數硬體中斷、軟中斷 vs 行程優先級、work 回呼裡 `msleep()` | **卷2 2-4, 2-5, 2-7, 2-8, 2-10** |
| `softirq_par.c` | 同類軟中斷多 CPU 並行 vs 同一 tasklet 串行化，重現書上 CPU0/CPU1 時序 | **卷2 2-6, 2-9** |
| `wq_probe.c` | CMWQ worker pool 動態伸縮、`alloc_ordered_workqueue`、`max_active` | **卷2 2-10~2-13** |
| `ksym.h` | 用 kprobe 取回 `kallsyms_lookup_name()`，解析未 EXPORT 的 `irq_to_desc()` / `irq_work_queue_on()` | 共用 |
| `irq_trace.sh` | ftrace 腳本（中斷呼叫鏈 / softirq / workqueue / `/proc/interrupts`↔DTB 對帳） | **卷2 2-3, 2-5, 2-7, 2-11** |
| `irq_rerun_all.sh` | 把上述 21 個實驗步驟串成一支（含重新編譯與自動還原），用來做重開機重跑驗證 | 卷2 第 2 章全部 |


**內核調試與性能優化（卷2 第 3 章）用的模組與程式**：

| 檔案 | 用途 | 題目 |
|------|------|------|
| `opt_lab.c` + `opt_build.sh` | 同一份程式碼用 -O0/-O1/-O2/-Os 各編一次，比指令數/堆疊框/內聯/DWARF 變數位置/行號表；`-DO0_BREAK` 重現「-O0 編不動內核」 | **卷2 3-1** |
| `pic_asm.S` + `pic_demo.c` | 把一段機器碼 memcpy 到別的位址再執行，量 PIC（`adr`/`bl`/`adrp`）與非 PIC（`ldr x0,=sym`/`blr`）的差異；PIE 的 `.rela.dyn` | **卷2 3-2~3-5** |
| `addr_probe.c` | 印出核心的鏈接/加載/運行地址、`KIMAGE_VADDR`、`kimage_voffset`、`kaslr_offset()`、各段實體位址、模組與 `_stext` 的距離 | **卷2 3-2, 3-5, 3-7** |
| `reloc_mod.c` | 模組重定位「前 vs 後」：`objdump -dr` 的留白 + relocation entry ↔ 記憶體裡填好的 `adrp`/`bl` | **卷2 3-4** |
| `tp_lab.c` + `tp_lab_trace.h` | 樹外 `TRACE_EVENT()`／`TRACE_EVENT_CONDITION()`、`register_trace_*()` probe、dump 出 static key 的 NOP↔B 改寫 | **卷2 3-8** |
| `slub_lab.c` | 私有 kmem_cache 帶 `SLAB_RED_ZONE/POISON/STORE_USER/CONSISTENCY_CHECKS`（= `slub_debug=FPUZ`），重現越界/UAF/double free/freepointer/洩漏 | **卷2 3-9** |
| `dl_lab.c` | 五種死鎖情境（AA 自旋鎖、AA mutex、AB-BA 阻塞版/trylock 版、mutex vs `cancel_delayed_work_sync`），全部可自我解除 | **卷2 3-10, 3-11** |
| `printk_lab.c` | 8 個輸出等級、`print_hex_dump()`、`dump_stack()`、`%p`/`%px`/`%pS` | **卷2 3-12** |
| `dyndbg_lab.c` | 5 條 `pr_debug()` 的動態開關（module/func/file+line 選擇器、`+pflmt` 旗標、`insmod dyndbg=`） | **卷2 3-13** |
| `oops_lab.c` | 七種掛掉方式（寫/讀空指標、野指標、執行空指標、`BUG_ON`、`WARN_ON`、kthread 內 oops） | **卷2 3-14** |
| `ch12_run_all.sh` | 卷2 第 3 章全部實驗的一鍵佈署與重現 | 卷2 第 3 章全部 |

> ⚠ `oops_lab` 在 `module_init` 裡 oops 之後，模組會永遠卡在 `MODULE_STATE_COMING`
> （`rmmod` 移不掉、同名模組無法再載入），所以腳本會複製成 `oops_m1..oops_m7`
> 七份不同名字的模組；要清乾淨只能重開機。


**基於 x86_64 解決宕機難題（卷2 第 4 章）用的模組與程式**：

| 檔案 | 跑在哪 | 用途 | 題目 |
|------|--------|------|------|
| `x86_abi.c` | 主機 x86_64 | System V AMD64 ABI 五種參數規則（暫存器/堆疊/XMM/結構回傳/AL） | **卷2 4-2** |
| `x86_frame.c` | 主機 **與** 板子 | 三層函式框架鏈 + 逐格 dump 堆疊 + 局部變數位址推導自我驗證（一份原始碼兩種架構） | **卷2 4-3, 4-10** |
| `x86_addr.c` | 主機 x86_64 | MOV vs LEA、直接/間接/基址/變址/RIP 相對定址 | **卷2 4-4~4-6** |
| `x86_oops_case.c` | 主機 x86_64 | 書上 §4.10 的模組原始碼用今天的 x86_64 編一次，驗證書上那段反組譯 | **卷2 4-10** |
| `lockup_lab.c` | 板子（模組） | 自製 softlockup / hardlockup / hung_task 三個偵測器 + 三種 lockup 製造機 | **卷2 4-7~4-9** |
| `rwsem_lab.c` | 板子（模組） | 讀寫信號量死鎖現場（mode=1 可回收、mode=2 書上原版會卡死機器） | **卷2 4-10~4-13** |
| `crash_probe.c` | 板子（模組） | crash 工具替身：`ps`/`bt`/`btf`/`rd`/`rwsem`/`mm`/`mmowner`/`sched`/`runq`/`sym` | **卷2 4-10~4-13** |
| `ch13_run_all.sh` | 主機 | 一鍵重現（`BOOK_MODE=1` 連書上原版的自鎖一起跑，跑完要重開機） | — |

> ⚠ `rwsem_lab.ko mode=2` 會讓 `insmod` 永遠停在 D 狀態，之後**任何掃 `/proc` 的指令
> （`ps`/`top`/`pgrep`）都會跟著卡死**，只有 `dmesg` 和 `crash_probe` 還能用；分析完必須重開機。

**基於 ARM64 解決宕機難題（卷2 第 5 章）用的模組與程式**：

| 檔案 | 用途 | 題目 |
|------|------|------|
| `arm64_frame.c` | 使用者態：四種序幕（葉子／小框架／大框架／VLA）+ 三層 FP 鏈 + 逐格 dump 堆疊 | **卷2 5-1, 5-2** |
| `arm64_lab.c` | 核心模組：三層呼叫最後卡死在 rwsem，**每層的 x29、局部變數位址、參數值都先印出來當標準答案** | **卷2 5-1, 5-3~5-7** |
| `build_vmlinux.sh` | 在板子上重建「和跑著的核心位址完全相符、帶 DWARF」的 vmlinux（約 25 分鐘） | **卷2 5-3, 5-4** |
| `ch14_run_all.sh` | 一鍵重現（有 vmlinux 時會改用真 crash 工具） | — |

> 卷2 第 5 章還沿用了第 3、4 章的 `oops_lab.c`（產生真 oops）與 `crash_probe.c`（crash 子命令替身）。

**安全漏洞分析（卷2 第 6 章）用的模組與程式**：

| 檔案 | 跑在哪 | 用途 | 題目 |
|------|--------|------|------|
| `pmu_user.c` | 模組 | `on_each_cpu` 打開 `PMUSERENR_EL0`，讓 EL0 直讀週期計數器（週期級時鐘的備援路徑） | Q1/Q3/Q7 |
| `sidechannel.h` | 共用 | Flush+Reload 原語（`dc civac`/`ldrb`/`dsb`）+ **perf 自我監控週期計數器**（`perf_user_access=1`+`config1=0x2`，附 SIGILL 保護的 fallback） | Q1/Q3/Q7 |
| `sec_probe.c` | 模組 | 每 CPU 讀 `ID_AA64PFR0.CSV2/CSV3`、`TCR.A1`/TTBR ASID、軟體巡覽核心頁 `nG` 位、每 CPU `VBAR_EL1`、反組譯 `invoke_syscall` 抓 `csdb`、**kmalloc 植入已知核心祕密供攻擊** | **Q3/Q4/Q5/Q7/Q8** |
| `flush_reload.c` | 使用者態 | cache 命中/未命中延遲直方圖 + 隱蔽通道傳字串 | **Q1** |
| `meltdown_test.c` | 使用者態 | 熔斷 PoC（讀核心位址）+ 兩種例外抑制（`SIGSEGV`+`siglongjmp` / `fork`） | **Q2/Q3** |
| `branch_pred.c` | 使用者態 | 分支誤判懲罰：已排序 vs 未排序同一迴圈（週期級量測） | **Q6** |
| `spectre_v1.c` | 使用者態 | 幽靈變體1 PoC：訓練分支預測器 → 越過 `if(x<size)` → Flush+Reload 洩漏 | **Q7** |
| `nospec_mask.c` | 使用者態 | `array_index_mask_nospec` 純 C 版 vs `cmp/sbc/csdb` 組語版 + `objdump` 看 `csdb` 機器碼 | **Q8/Q9** |
| `Makefile.ch15` | — | `pmu_user` + `sec_probe` 兩個模組的 Kbuild（上傳時改名 `Makefile`） | — |
| `ch15_run_all.sh` | 主機 | 一鍵佈署與重現（跑完自動卸載模組、`perf_user_access=0` 收回權限） | 卷2 第 6 章全部 |

> ⚠ 這章會改動機台設定（`kernel.perf_user_access`、載入 `pmu_user`/`sec_probe` 模組）。
> `ch15_run_all.sh` 結尾會自動還原；手動跑完請 `sudo rmmod sec_probe pmu_user; sudo sysctl kernel.perf_user_access=0`。

一鍵在機台上建置：

```bash
# 使用者態程式
scp notes/experiments/*.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp
  for s in life tid prims vfork_order cow q11 q11_pid pgtbl pgfork \
           lru_shmem swap_shmem pagemap_walk barrier_sb barrier_mp \
           cache_ladder hold_page fault_types esr_far mmap_vma \
           concurrent_fault ksm_test migrate_compact reclaim_test \
           sched_weight pelt_duty wake_cpu lb_case vruntime_place rt_latency; do
      gcc -O2 -w -o $s $s.c -lpthread 2>/dev/null; done'

# ftrace 腳本（第 8/9 章）
scp notes/experiments/sched_trace.sh radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'chmod +x /tmp/sched_trace.sh'

# 核心模組
ssh radxa@192.168.68.57 'mkdir -p ~/exp/armv8'
scp notes/experiments/armv8_dump.c notes/experiments/mm_convert.c \
    notes/experiments/mm_probe.c notes/experiments/sched_probe.c radxa@192.168.68.57:~/exp/armv8/
scp notes/experiments/Makefile.mod radxa@192.168.68.57:~/exp/armv8/Makefile
ssh radxa@192.168.68.57 'cd ~/exp/armv8 && make'
```

> **第 8/9 章的實驗會改動機台設定**（cpufreq governor、`sched_schedstats`、
> ftrace、debugfs 的 sched 參數）。兩份筆記的**附錄**都附了完整的還原指令，
> 跑完請記得執行。
