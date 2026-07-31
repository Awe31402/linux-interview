# 《奔跑吧 Linux內核》（第二版）卷1：第 1~9 章 高頻面試題筆記

本目錄收錄《奔跑吧 Linux內核》（第二版）卷1 基礎架構 各章節開篇之「本章的高頻面試題」，共計 245 道題目。

## 📚 章節目錄索引

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
* 📄 [彙整單檔 (全部 245 題)](file:///home/awe/disk/linux-rockchip/notes/running_linux_kernel_v1_interview_questions.md)

---

## ✅ 解答（含 RK3588 實機實驗）

以下解答皆在 **Radxa ROCK 5B（RK3588, `192.168.68.57`, Linux 6.1.115+ aarch64, 8GB）**
上實測，每一題都標註了**書目出處**與**下給機台的指令 + 實際輸出**。

| 章節 | 解答檔 | 題數 | 亮點 |
|------|--------|------|------|
| 第 2 章 | 📝 [ch02_arm64_in_linux_kernel.md](./ch02_arm64_in_linux_kernel.md) | 23 | 自寫核心模組**直接讀 EL1 系統暫存器**（TCR/TTBR/MAIR/CLIDR…）＋軟體巡覽 4 級頁表；**TTBR0 == `__pa(mm->pgd)` 一位不差**；抓到活的 **2MB BLOCK 描述符**；SB litmus **31% 亂序 → `dmb ish` 後 0**。**修正三項常見誤解**（`PAGE_OFFSET` 值、佈局方向、實體位址寬度） |
| 第 3 章 | 📝 [ch03_memory_management_prerequisites.md](./ch03_memory_management_prerequisites.md) | 7 | 核心模組把 **Q3 的九種轉換全部跑出實際數值**；cache 延遲階梯的**轉折點與 TRM Table 3-1 的 cache 大小完全吻合**（A76 L1 = 4.0 cycles）；DTB→memblock→MemTotal 一路對帳。**修正 ZONE_DMA32 的錯誤說法** |
| 第 4 章 | 📝 [ch04_physical_and_virtual_memory.md](./ch04_physical_and_virtual_memory.md) | 46 | ESR_EL1 逐位解碼（`WnR` 只差 bit 6）；**fault-around 讓 4096 頁只缺頁 256 次**；COW **reuse vs copy** 用「缺頁 4096 次但 RSS 不變」證明；vmalloc **15/16 頁實體不連續**；**8 CPU 併發缺頁 86% 是白工**。修正 SLAB→SLUB、`kmalloc-128` 最小 |
| 第 5 章 | 📝 [ch05_memory_management_advanced_topics.md](./ch05_memory_management_advanced_topics.md) | 48 | **KSM 64 頁合併成 1 頁**（PFN `0x731ab`）寫入後 COW 分家；**規整實際搬動 284 頁**（PFN 低→高）資料 100% 完整；換出換入 **`pgmajfault`=`pswpin`=65529 一頁不差**；`boost=6463` 打通 fallback→boost→kcompactd 完整因果鏈 |
| 第 6 章 | 📝 [ch06_memory_management_case_studies.md](./ch06_memory_management_case_studies.md) | 14 | MemTotal 差值 **259092 kB 逐項對帳成功**；LRU 恆等式**完全吻合**；`MADV_PAGEOUT` 直接證明 shmem 不計入 `VmSwap`；抓到 **watermark boost 正在生效且已飽和（6463 頁）** |
| 第 7 章 | 📝 [ch07_process_management_basic_concepts.md](./ch07_process_management_basic_concepts.md) | 15 | strace 抓出 fork/vfork/pthread 的 clone flags；**16384 次 COW 缺頁精準命中**；VmPTE 逐級變化證明頁表按需配置；ftrace 抓到 **`schedule_tail <-ret_from_fork`**；fork 輸出 **6 vs 8 兩種答案都重現** |

### 實驗程式

`experiments/` 底下是解答中用到的所有程式碼。

**核心模組**（在機台上編譯；`/lib/modules/$(uname -r)/build` 已指向完整核心原始碼樹）：

| 檔案 | 用途 | 題目 |
|------|------|------|
| `armv8_dump.c` | 讀 EL1 系統暫存器（TCR/TTBR/MAIR/ID_AA64MMFR0/SCTLR/CTR/CLIDR）、印核心 VA 佈局、軟體巡覽 4 級頁表 | 2-1~6, 2-9~16, 2-20, 2-22, 2-23, 3-4~6 |
| `mm_convert.c` | 把 mm/VMA/page/PFN/paddr/PTE/zone/pgdat 的九種轉換全部跑一遍 | **3-3** |
| `mm_probe.c` | 伙伴系統 free_area（66 條鏈）、gfp_zone 表、zonelist、水位、SLUB、`page->flags` 佈局、外碎片指標 | **4-1~4-7, 4-18, 5-4, 5-38, 5-42~5-47** |
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

一鍵在機台上建置：

```bash
# 使用者態程式
scp notes/experiments/*.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp
  for s in life tid prims vfork_order cow q11 q11_pid pgtbl pgfork \
           lru_shmem swap_shmem pagemap_walk barrier_sb barrier_mp \
           cache_ladder hold_page fault_types esr_far mmap_vma \
           concurrent_fault ksm_test migrate_compact reclaim_test; do
      gcc -O2 -w -o $s $s.c -lpthread 2>/dev/null; done'

# 核心模組
ssh radxa@192.168.68.57 'mkdir -p ~/exp/armv8'
scp notes/experiments/armv8_dump.c notes/experiments/mm_convert.c notes/experiments/mm_probe.c radxa@192.168.68.57:~/exp/armv8/
scp notes/experiments/Makefile.mod radxa@192.168.68.57:~/exp/armv8/Makefile
ssh radxa@192.168.68.57 'cd ~/exp/armv8 && make'
```
