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
| 第 6 章 | 📝 [ch06_memory_management_case_studies_ANSWERS.md](./ch06_memory_management_case_studies_ANSWERS.md) | 14 | MemTotal 差值 **259092 kB 逐項對帳成功**；LRU 恆等式**完全吻合**；`MADV_PAGEOUT` 直接證明 shmem 不計入 `VmSwap`；抓到 **watermark boost 正在生效且已飽和（6463 頁）** |
| 第 7 章 | 📝 [ch07_process_management_basic_concepts_ANSWERS.md](./ch07_process_management_basic_concepts_ANSWERS.md) | 15 | strace 抓出 fork/vfork/pthread 的 clone flags；**16384 次 COW 缺頁精準命中**；VmPTE 逐級變化證明頁表按需配置；ftrace 抓到 **`schedule_tail <-ret_from_fork`**；fork 輸出 **6 vs 8 兩種答案都重現** |

### 實驗程式

`experiments/` 底下是解答中用到的所有 C 程式：

| 檔案 | 用途 | 題目 |
|------|------|------|
| `lru_shmem.c` | shmem/anon 對 LRU、AnonPages、Cached、Mapped 的影響 | 6-5, 6-6, 6-7 |
| `swap_shmem.c` | `MADV_PAGEOUT` 比對 `VmSwap` vs `SwapFree` | 6-9 |
| `life.c` | 僵屍態、`wait()` 回收、孤兒託孤 | 7-3, 7-5 |
| `tid.c` | `getpid()` vs `gettid()` | 7-4 |
| `prims.c` / `vfork_order.c` | fork/vfork/clone 的差異 | 7-8 |
| `cow.c` | COW 缺頁計數與 smaps Shared/Private_Dirty | 7-9 |
| `q11.c` / `q11_pid.c` | fork 迴圈輸出幾個 `_` | 7-11 |
| `pgtbl.c` / `pgfork.c` | 頁表按需配置與 fork 複製 | 7-12 |

一鍵在機台上重跑：

```bash
scp notes/experiments/*.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp
  for s in life tid prims vfork_order cow q11 q11_pid pgtbl pgfork lru_shmem swap_shmem; do
      gcc -O0 -w -o $s $s.c -lpthread 2>/dev/null; done'
```
