# 第 3 章 記憶體管理之預備知識 — 高頻面試題解答

> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588，4×Cortex-A76 @2.256GHz + 4×Cortex-A55 @1.8GHz），`192.168.68.57`
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`，8 GiB LPDDR
> **關鍵組態**：`# CONFIG_NUMA is not set`、`CONFIG_SPARSEMEM_VMEMMAP=y`、
> `CONFIG_ARM64_4K_PAGES=y`、`CONFIG_ARM64_VA_BITS=48`、`MAX_ORDER=11`
>
> **書目對照**
> - 《奔跑吧 Linux 內核》（第二版）卷 1 第 3 章 —
>   `books/running-linux-kernel/running-kernel-1-txt/11_第3章_内存管理之预备知识.txt`（下稱「奔跑吧 §3.x」）
> - **RK3588 TRM** — `books/rk3588_trm/part1/chapter_01.txt`（Address Mapping）、
>   `chapter_03.txt`（CPU / Cache，**Table 3-1 CPU Configuration**）
> - *Linux Memory Manager* — `books/memory-manager/chapters/02_physical_memory.txt`
> - 核心程式碼路徑相對於本專案樹
>   `/home/awe/disk/yocto-rockchip-sdk/build/tmp/work-shared/rockchip-rk3588-rock-5b/kernel-source`
>
> ## ⚠️ 本次實測修正了舊草稿的一項錯誤
>
> | 題 | 舊草稿說法 | **本機實測** |
> |---|-----------|-------------|
> | [Q3](#q3) | 「一般包含 **ZONE_DMA32**（涵蓋 0~4GB）與 ZONE_NORMAL」 | **ZONE_DMA 才是涵蓋 0~4GB 的那個（present 982528 頁）；`ZONE_DMA32` 是空的（present = 0）** |
> | [Q4](#q4) | 「`_stext` 與 `PAGE_OFFSET (0xffff800000000000)` 相差約 0x8010000」 | `PAGE_OFFSET` 其實是 **`0xffff000000000000`**；`_stext` 是相對 `KIMAGE_VADDR (0xffff800008000000)` 偏移 0x10000。詳見 [Ch2 Q5](./ch02_arm64_in_linux_kernel.md#q5) |
>
> **主要實驗工具**：`notes/experiments/mm_convert.c`（核心模組，把 Q3 的九種轉換全跑一遍）、
> `notes/experiments/cache_ladder.c`（Q2 的 cache 延遲階梯）

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | UMA 與 NUMA 的區別 | 本機是 UMA：只有 `Node 0`，無 `/sys/devices/system/node/` |
| [2](#q2) | CPU 存取各級儲存的速度是否一樣 | **量到 L1/L2/L3/DRAM 四段階梯，轉折點與 TRM 的 cache 大小完全吻合** |
| [3](#q3) | 記憶體管理資料結構關係圖 + 九種轉換 | **核心模組把九種轉換全部跑出實際數值** |
| [4](#q4) | 核心映像映射到核心空間何處 | `KIMAGE_VADDR = 0xffff800008000000` |
| [5](#q5) | 核心空間與使用者空間如何劃分 | `T0SZ = T1SZ = 16` |
| [6](#q6) | 開機時如何知道有多大實體記憶體 | **DTB `/memory/reg` 三段，總和 8,386,560 kB** |
| [7](#q7) | 實體頁面如何加入伙伴系統 | `__free_pages_memory()` + `/proc/buddyinfo` |

---

<a name="q1"></a>
## 1. 請簡述記憶體架構中 UMA 和 NUMA 的區別

### 結論

| | **UMA**（Uniform Memory Access） | **NUMA**（Non-Uniform Memory Access） |
|---|---|---|
| 記憶體視角 | 全系統**一整塊**記憶體 | 分成多個 **node**，各有本地 CPU + 本地記憶體 |
| 存取延遲 | 所有 CPU 存取任一位址**延遲一致** | **本地快、遠端慢**（差 1.5～3 倍很常見） |
| 核心資料結構 | **一個** `pg_data_t` | **每個 node 一個** `pg_data_t` |
| 需要的機制 | 無 | NUMA 感知的排程、記憶體策略（`mbind`/`set_mempolicy`）、`zone_reclaim_mode`、自動 NUMA balancing |
| 典型平台 | 手機 / 嵌入式 SoC、單插槽桌機 | 多插槽伺服器、Threadripper/EPYC 的 CCX |

**核心裡的實作**：`pg_data_t`（`include/linux/mmzone.h`）描述一個 node，
裡面有 `node_zones[]`（該 node 的所有 zone）、`node_start_pfn`、`kswapd` 等。
UMA 系統只有 `NODE_DATA(0)` 一個。

> **書目**：奔跑吧 §3.3.1「內存架構之 UMA 和 NUMA」。
> 書中並註明「由於本書的實驗對象 ARM64 虛擬機不支持 NUMA 架構，因此我們在行文中
> 忽略對 NUMA 相關代碼的討論」——本機情況相同。

### 實機驗證：RK3588 是標準的 UMA

```bash
ssh radxa@192.168.68.57 '
  numactl --hardware 2>&1 | head -3
  ls /sys/devices/system/node/ 2>&1
  zcat /proc/config.gz | grep -E "CONFIG_NUMA"
  grep "^Node" /proc/zoneinfo | sort -u'
```

```
No NUMA available on this system                                    ← numactl
ls: cannot access '/sys/devices/system/node/': No such file or directory
# CONFIG_NUMA is not set                                            ← 核心根本沒編進去
Node 0, zone      DMA
Node 0, zone    DMA32
Node 0, zone  Movable
Node 0, zone   Normal                                               ← 全部都是 Node 0
```

**四項獨立證據一致：本機是 UMA。**

**核心模組直接印出 `pg_data_t`**（`mm_convert.ko`，見 [Q3](#q3)）：

```
[9] pg_data <- zone: zone->zone_pgdat = 0xffff80000a15b740  node_id=0
    pgdat->node_start_pfn=0x200  node_spanned_pages=3145216  nr_zones=3
```

**全系統只有一個 `pg_data_t`，`node_id = 0`** ✅

### 為什麼 RK3588 必然是 UMA？

> **TRM**（`books/rk3588_trm/part1/chapter_03.txt` §3.1）：
> 「The RK3588 has **a cluster** with quad-core Cortex-A55 and quad-core Cortex-A76」
> §3.2：「the cores connect to system bus through **DSU-L3**」

8 個核在**同一個 DSU（DynamIQ Shared Unit）**裡，共用同一個 L3、同一組 DDR 控制器，
到記憶體的路徑完全對稱 → 沒有「本地/遠端」之分。

**⚠️ 面試常見的混淆**：RK3588 是 **big.LITTLE 非對稱架構**（A76 vs A55），
但那是**運算能力**不對稱，不是**記憶體存取**不對稱。前者由排程器的 EAS /
`cpu_capacity` 處理（本機 A55=422、A76=1024），後者才是 NUMA。**兩者不要混為一談。**

```bash
ssh radxa@192.168.68.57 'for c in 0 4; do printf "cpu%s capacity=%s\n" $c "$(cat /sys/devices/system/cpu/cpu$c/cpu_capacity)"; done'
# cpu0 capacity=422    ← A55
# cpu4 capacity=1024   ← A76      運算不對稱，但記憶體延遲對稱（見 Q2 的 DRAM 段）
```

**這個推論也被 [Q2](#q2) 的實測數據支持**：A76 與 A55 在工作集 ≥ 8 MB（全部落到 DRAM）時，
延遲分別是 193～242 ns 和 200～245 ns，**幾乎相同** —— 兩種核到 DDR 的距離一樣，
正是 UMA 的定義。

---

<a name="q2"></a>
## 2. CPU 存取各級儲存結構的速度是否一樣？

### 結論

**完全不一樣，而且是數量級的差距。** 儲存階層由快到慢、由小到大：

```
暫存器 → L1 Cache → L2 Cache → L3 Cache → 主記憶體(DRAM) → 輔助儲存(NVMe/eMMC)
 <1 cyc    ~4 cyc     ~12 cyc     ~60 cyc     ~500 cyc        ~10^5-10^7 cyc
```

每往下一級，**容量大 1～2 個數量級，延遲也大 1～2 個數量級**。
整個 cache 階層的存在意義就是：用小而快的儲存「快取」熱點資料，
把大而慢的儲存的延遲藏起來。

> **書目**：奔跑吧 §3.1「從硬件角度看內存管理」。

### RK3588 的 cache 組態（三方交叉驗證）

**(a) TRM 的權威數據**（`books/rk3588_trm/part1/chapter_03.txt` **Table 3-1 CPU Configuration**）：

```
Number of little cores(A55)     4
Number of big cores(A76)        4
L1 I cache size of A55         32K
L1 D cache size of A55         32K
L2 cache size of A55          128K
L1 I cache size of A76         64K
L1 D cache size of A76         64K
L2 cache size of A76          512K
L3 cache size                3072K
L3 data RAM output latency   3 cycles
L3 data RAM input latency    2p cycles
```

**(b) 核心從硬體 ID 暫存器讀出來的**：

```bash
ssh radxa@192.168.68.57 'for c in 0 4; do echo "-- cpu$c --"
  for d in /sys/devices/system/cpu/cpu$c/cache/index*; do
    printf "  L%s %-12s %-6s shared=%s\n" "$(cat $d/level)" "$(cat $d/type)" \
           "$(cat $d/size)" "$(cat $d/shared_cpu_list)"; done; done'
```

```
-- cpu0 (Cortex-A55) --          -- cpu4 (Cortex-A76) --
  L1 Data          32K   shared=0    L1 Data          64K   shared=4
  L1 Instruction   32K   shared=0    L1 Instruction   64K   shared=4
  L2 Unified      128K   shared=0    L2 Unified      512K   shared=4
  L3 Unified     3072K   shared=0-7  L3 Unified     3072K   shared=0-7
```

**與 TRM 完全一致** ✅。注意 **L3 的 `shared_cpu_list = 0-7`**——8 個核共用，
這正是 [Ch2 Q13](./ch02_arm64_in_linux_kernel.md#q13) 說的 DSU-L3，
也是 `CLIDR_EL1.LoC = 3`（PoC 在 L3 之後）的原因。

**(c) cache line 大小**（`armv8_dump.ko` 讀 `CTR_EL0`）：

```
CTR_EL0 = 0x000000009444c004
   IminLine=4 -> I-cache line = 64 bytes
   DminLine=4 -> D-cache line = 64 bytes
   L1Ip=3 (PIPT)
```

### 實機驗證：直接量出延遲階梯

程式 `notes/experiments/cache_ladder.c` 用**隨機打亂的環狀指標鏈**走訪
（每次 load 都相依於前一次的結果 → 硬體預取器完全失效），
工作集由 4 KB 掃到 128 MB：

```bash
ssh radxa@192.168.68.57 'cd ~/exp && gcc -O2 -o cache_ladder cache_ladder.c
  taskset -c 4 ./cache_ladder      # A76 @ 2.256 GHz
  taskset -c 0 ./cache_ladder'     # A55 @ 1.8 GHz
```

**Cortex-A76（cpu4，2.256 GHz）**：

| 工作集 | ns/access | **換算 cycles** | 命中層級 |
|--------|-----------|----------------|---------|
| 4 KB – **64 KB** | **1.78** | **4.0** | **L1d（64 KB）** ← 轉折正好在 64 KB |
| 96 KB – 256 KB | 5.12 – 5.44 | 11.6 – 12.3 | **L2（512 KB）** |
| 384 KB – 1 MB | 7.20 – 16.88 | 16 – 38 | L2 → L3 過渡 |
| 2 MB – 3 MB | 27.67 – 50.72 | 62 – 114 | **L3（3 MB）** |
| 4 MB – 6 MB | 105 – 169 | 238 – 381 | L3 溢位 → DRAM |
| **8 MB – 128 MB** | **193 – 242** | **436 – 545** | **DRAM** |

```
4 KB               1.79   ###
64 KB              1.79   ###          ← L1d 邊界 (TRM: 64K) ✅
96 KB              5.12   ##########   ← 掉進 L2
512 KB            10.33   ####################   ← L2 邊界 (TRM: 512K) ✅
3 MB              50.72   ############################################################  ← L3 邊界 (TRM: 3072K) ✅
128 MB           241.65   ############################################################  ← DRAM
```

**Cortex-A55（cpu0，1.8 GHz）**：

| 工作集 | ns/access | **換算 cycles** | 命中層級 |
|--------|-----------|----------------|---------|
| 4 KB – **32 KB** | **1.11 – 1.16** | **2.0 – 2.1** | **L1d（32 KB）** ← 轉折正好在 32 KB |
| 48 KB – **128 KB** | 3.07 – 6.72 | 5.5 – 12.1 | **L2（128 KB）** |
| 192 KB – 1 MB | 10.81 – 16.10 | 19 – 29 | **L3（3 MB）** |
| 2 MB – 4 MB | 32.33 – 123.85 | 58 – 223 | L3 溢位 |
| **8 MB – 128 MB** | **200 – 245** | **360 – 441** | **DRAM** |

### 三個值得在面試講出來的觀察

1. **量到的 L1 延遲精準命中 ARM 的官方數字**：
   - A76：`1.78 ns × 2.256 GHz = 4.01 cycles` —— Cortex-A76 的 L1 load-to-use 就是 **4 cycles**
   - A55：`1.11 ns × 1.8 GHz = 2.00 cycles` —— Cortex-A55 的 L1 load-to-use 是 **2 cycles**

2. **階梯的轉折點就是 cache 的實際大小**：
   A76 在 64 KB → 96 KB 之間跳一階（L1d = 64 K ✅）；
   A55 在 32 KB → 48 KB 之間跳一階（L1d = 32 K ✅）。
   **不用查規格，用測量就能反推出 cache 大小。**

3. **DRAM 段兩種核幾乎相同（193-242 vs 200-245 ns）**——
   這就是 [Q1](#q1) 說的 UMA：大小核到 DDR 的距離一樣。
   **「大核比小核快」只在 cache 命中時成立**；一旦掉到 DRAM，
   兩者都在等同一顆 LPDDR，差距從 1.6 倍縮到幾乎 0。
   *（這也解釋了為什麼 memory-bound 的工作負載搬到大核上往往沒什麼效果。）*

### 輔助儲存的量級

```bash
ssh radxa@192.168.68.57 'lsblk -d -o NAME,SIZE,ROTA,MODEL 2>/dev/null | head; cat /proc/iomem | grep -i nvme | head -3'
```

NVMe 的隨機讀延遲在 **50–100 µs** 量級 = DRAM 的 **200–500 倍**，
再往下的網路儲存則是毫秒級。這就是為什麼 major fault（`pgmajfault`）
比 minor fault 貴那麼多（見 [Ch7 Q9](./ch07_process_management_basic_concepts.md#q9)）。

---

<a name="q3"></a>
## 3. 記憶體管理資料結構的關係圖與九種轉換

### 關係圖

```
task_struct
   └─ mm_struct ──┬─→ pgd  ────────────────── 頁表（4 級）
                  │                              │ vaddr 逐級查表
                  │                              ▼
                  └─→ mm_mt (maple tree)        pte_t ──pte_page()──┐
                        │  find_vma(mm,vaddr)                       │
                        ▼                                            ▼
                     vm_area_struct ◄──────rmap──────────────► struct page
                     [vm_start, vm_end)                          │  ▲  │
                      vm_pgoff / vm_file                         │  │  │
                            ▲                                     │  │  │
                            │        page_to_pfn() ───────────────┘  │  │
              anon_vma ─────┤        pfn_to_page() ──────────────────┘  │
              (匿名頁)       │                                            │
              i_mmap ───────┘        PFN ◄──PFN_PHYS()/PHYS_PFN()──► paddr
              (檔案頁)                 │
                                       │  page_zone()
                                       ▼
                     pg_data_t ──node_zones[]──► zone ──zone_pgdat──► pg_data_t
                     (UMA 只有一個)                │
                                                   └─ [zone_start_pfn, +spanned_pages)
```

### 九種轉換的核心介面

| # | 轉換 | 核心 API | 原理 |
|---|------|---------|------|
| 1 | **mm + vaddr → VMA** | `find_vma(mm, vaddr)` | 在 `mm->mm_mt`（6.1 起是 **maple tree**，舊版是紅黑樹 `mm_rb`）中搜尋 |
| 2 | **VMA + vaddr → page** | `pgd_offset`→`p4d_offset`→`pud_offset`→`pmd_offset`→`pte_offset_map` → `pte_page()` | 走頁表；缺頁時由 `handle_mm_fault()` 建立 |
| 3 | **page + VMA → vaddr** | `vma_address()` / `page_address_in_vma()` | `vm_start + ((page->index − vma->vm_pgoff) << PAGE_SHIFT)` |
| 4 | **page → 所有映射它的 VMA** | `rmap_walk()`（`mm/rmap.c`） | 匿名頁走 `page->mapping` → `anon_vma` → `anon_vma_chain`；檔案頁走 `address_space->i_mmap` 區間樹 |
| 5 | **page ↔ PFN** | `page_to_pfn()` / `pfn_to_page()` | SPARSEMEM_VMEMMAP 下就是 `vmemmap + pfn` 的指標算術 |
| 6 | **PFN ↔ paddr** | `PFN_PHYS()` / `PHYS_PFN()` | `pfn << PAGE_SHIFT` / `paddr >> PAGE_SHIFT` |
| 7 | **page ↔ PTE** | `pte_page()` / `mk_pte(page, prot)` | PTE 的 bits[47:12] 就是 PFN |
| 8 | **zone ↔ page** | `page_zone()` / `zone->zone_start_pfn` | zone id 編碼在 `page->flags` 高位 |
| 9 | **zone ↔ pg_data** | `zone->zone_pgdat` / `pgdat->node_zones[]` | 直接指標 |

> **書目**：奔跑吧 §3.3.2「內存管理之數據結構」。

### 實機驗證（**核心模組把九種轉換全部跑一遍**）

程式：`notes/experiments/mm_convert.c`（核心模組）+ `notes/experiments/hold_page.c`（配角）。

```bash
# 1. 起一個使用者行程，配一頁匿名記憶體、fork 一個子行程共享它，然後停住
ssh radxa@192.168.68.57 'cd ~/exp && gcc -O2 -o hold_page hold_page.c && sudo ./hold_page &'
#   PID = 105166
#   VA  = 0xffffbd89c000
#   PFN = 0x74969   PA = 0x74969000        ← 使用者態透過 /proc/self/pagemap 取得

# 2. 把 PID 與 VA 餵給核心模組
ssh radxa@192.168.68.57 'cd ~/exp/armv8 && make
  sudo insmod mm_convert.ko target_pid=105166 target_va=0xffffbd89c000
  sudo dmesg | sed "s/^\[[^]]*\] //"
  sudo rmmod mm_convert'
```

#### 轉換 [1] mm + vaddr → VMA

```
  [1] mm + vaddr -> VMA : find_vma(mm, 0xffffbd89c000)
      vma = 0xffff00005be7b6e8  範圍 [0xffffbd89c000, 0xffffbd89f000)  大小 12 KB
      vm_flags=0x100073 (rw-p)  vm_pgoff=0xffffbd89c  vm_file=無（匿名映射）
      mm->pgd = 0xffff000055dcd000   __pa = 0x55dcd000  <- 會被寫進 TTBR0_EL1
```

#### 轉換 [2] VMA + vaddr → page（逐級走頁表）

```
      pgd_offset()     = 0xffff000055dcdff8  val=0x800000055dcc003
      pud_offset()     = 0xffff000055dccff0  val=0x80000005caa1003
      pmd_offset()     = 0xffff00005caa1f60  val=0x80000005d34d003
      pte_offset_map() = 0xffff00005d34d4e0  val=0xe0000074969fc3
```

#### 轉換 [7] PTE ↔ page

```
  [7] PTE -> page : pte_page() = 0xfffffc0001d25a40   pte_pfn()=0x74969
  [7] page -> PTE : mk_pte(page, prot) 會用 page_to_pfn(0x74969) 組回去
```

> **🎯 交叉驗證**：使用者態透過 `/proc/self/pagemap` 讀到 `PFN = 0x74969`，
> 核心態透過頁表巡覽讀到 `pte_pfn() = 0x74969` —— **兩條完全獨立的路徑得到同一個答案** ✅

#### 轉換 [3] page + VMA → vaddr

```
  [3] vm_start + ((page->index - vm_pgoff) << PAGE_SHIFT)
      = 0xffffbd89c000 + ((0xffffbd89c - 0xffffbd89c) << 12) = 0xffffbd89c000
      (原始 va = 0xffffbd89c000)  ✅ 完美還原
```

#### 轉換 [4] page → 所有映射它的 VMA（rmap）

```
  [4] page->mapping = 0xffff0001131bf481   PageAnon=1  PageKsm=0
      匿名頁：page->mapping 低位元帶 PAGE_MAPPING_ANON 標記，
              去掉標記後指向 anon_vma = 0xffff0001131bf480
      _mapcount = 1  =>  共有 2 個 PTE 映射到這個頁面
```

> **🎯 這裡藏了兩個重點**：
> 1. **`page->mapping` 的最低位元被拿來當標記**（`0x...481` 的 `1` 就是
>    `PAGE_MAPPING_ANON`）。去掉之後 `0x...480` 才是真正的 `anon_vma` 指標。
>    這是核心「指標低位元藏 flag」的經典手法（`struct anon_vma` 至少 4 byte 對齊）。
> 2. **`_mapcount = 1` 代表有 2 個 PTE 指向它**（`_mapcount` 是 **0-based**）——
>    正是 `hold_page` 裡 `fork()` 出來的父子行程 COW 共享同一頁的結果 ✅
>    這就是 rmap 存在的意義：**一個 page 可能被 N 個 VMA 映射**，
>    回收/遷移時必須把 N 條 PTE 全部斷開。

#### 轉換 [5] page ↔ PFN

```
  struct page *   = 0xfffffc0001d25a40
  page_to_pfn()   = 0x74969 (477545)
  pfn_to_page()   = 0xfffffc0001d25a40   ✔ 往返一致
  推導：vmemmap = 0xfffffc0000000000，sizeof(struct page) = 64
        vmemmap + pfn*64 = 0xfffffc0001d25a40  ✔ 與 page 相同
```

**手工驗算**：`0xfffffc0000000000 + 0x74969 × 64 = 0xfffffc0000000000 + 0x1d25a40`
= **`0xfffffc0001d25a40`** ✅
—— 這就是 `CONFIG_SPARSEMEM_VMEMMAP` 的價值：`struct page` 陣列在虛擬位址上**線性排布**，
`page_to_pfn()` 退化成一次減法 + 移位，不必查 section 表。
（VMEMMAP 區用 2MB block 映射，見 [Ch2 Q3](./ch02_arm64_in_linux_kernel.md#q3)。）

#### 轉換 [6] PFN ↔ paddr

```
  PFN_PHYS()      = 0x74969000    (pfn << PAGE_SHIFT，本機 PAGE_SHIFT=12)
  PHYS_PFN()      = 0x74969       ✔
  page_to_phys()  = 0x74969000
  page_to_virt()  = 0xffff000074969000   __pa() = 0x74969000
  virt_to_page(page_to_virt()) = 0xfffffc0001d25a40  ✔ 往返一致
```

注意 `page_to_virt()` 給的是**線性映射區**的位址（`0xffff0000...`，`PAGE_OFFSET` 之後），
和 `struct page` 自己所在的 **VMEMMAP 區**（`0xfffffc00...`）是兩回事。

#### 轉換 [8] zone ↔ page

```
  page_zone() = "DMA"   zone_start_pfn=0x200   spanned=1048064
  PFN 0x74969 落在 [0x200, 0x100000) 之內 ✔
  page->flags 中編碼的 zone id = 0
```

#### 轉換 [9] zone ↔ pg_data

```
  zone->zone_pgdat = 0xffff80000a15b740   node_id=0
  pgdat->node_start_pfn=0x200  node_spanned_pages=3145216  nr_zones=3
  pg_data -> zone[]：
     node_zones[0] "DMA    " present=982528    managed=959006    start_pfn=0x200
     node_zones[1] "DMA32  " present=0         managed=0         start_pfn=0x0
     node_zones[2] "Normal " present=1114112   managed=1073373   start_pfn=0x100000
     node_zones[3] "Movable" present=0         managed=0         start_pfn=0x0
```

### ⚠️ 修正：本機的 zone 劃分

> **舊草稿寫「一般包含 ZONE_DMA32（涵蓋 0~4GB）與 ZONE_NORMAL」——這是錯的。**

實測是：

| zone | `present` | 涵蓋的 PFN | 涵蓋的實體位址 |
|------|-----------|-----------|--------------|
| **ZONE_DMA** | **982,528** | `0x200` ~ `0x100000` | **2 MB ~ 4 GB** |
| ZONE_DMA32 | **0（空）** | — | — |
| **ZONE_NORMAL** | **1,114,112** | `0x100000` ~ `0x300000` | **4 GB ~ 12 GB** |
| ZONE_MOVABLE | 0（空） | — | — |

**為什麼 DMA32 是空的？** arm64 的 `zone_sizes_init()`（`arch/arm64/mm/init.c`）：

```c
max_zone_pfns[ZONE_DMA]   = PFN_DOWN(arm64_dma_phys_limit);   /* 由 DT 的 dma-ranges 決定 */
max_zone_pfns[ZONE_DMA32] = PFN_DOWN(max_zone_phys(32));      /* 固定 4 GB */
max_zone_pfns[ZONE_NORMAL] = max;
```

RK3588 上有 32-bit DMA 能力限制的週邊（GMAC、USB 等），使 `arm64_dma_phys_limit`
被算成 **4 GB**。於是 `ZONE_DMA` 的上界就已經是 4 GB，`ZONE_DMA32`（上界也是 4 GB）
自然一頁都分不到 → `present = 0`。

**這對回答 [Ch6 Q11](./ch06_memory_management_case_studies.md#q11)（lowmem_reserve_ratio）很關鍵**：
本機的「低端 zone」是 ZONE_DMA 且它占了一半以上的記憶體，
所以保護它不被高端請求吃光特別重要。

---

<a name="q4"></a>
## 4. ARM64 核心中，核心映像映射到核心空間的什麼地方？

### 結論

映射到 **`KIMAGE_VADDR`** 開始的一段專用區域，**不是**在 `PAGE_OFFSET` 的線性映射區裡。
兩者是兩個彼此獨立、位置解耦的映射。

```c
/* arch/arm64/include/asm/memory.h:46-49 */
#define KIMAGE_VADDR    (MODULES_END)
#define MODULES_END     (MODULES_VADDR + MODULES_VSIZE)   /* +128 MB */
#define MODULES_VADDR   (_PAGE_END(VA_BITS_MIN))
```

> **書目**：奔跑吧 §3.2.3「從內存分布的角度看內存管理」（指向 §2.1.5 的佈局圖）。
> 詳細討論見 [Ch2 Q6](./ch02_arm64_in_linux_kernel.md#q6) 與
> [Ch2 Q9](./ch02_arm64_in_linux_kernel.md#q9)。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'cd ~/exp/armv8
  STEXT=0x$(sudo grep -w _stext /proc/kallsyms | cut -d" " -f1)
  sudo insmod armv8_dump.ko stext=$STEXT
  sudo dmesg | grep -E "MODULES_|KIMAGE|PAGE_OFFSET|kimage_voffset"
  sudo rmmod armv8_dump'
```

```
  MODULES_VADDR: 0xffff800000000000
  MODULES_END  : 0xffff800008000000   (MODULES_VSIZE = 128 MB)
  KIMAGE_VADDR : 0xffff800008000000   <- == MODULES_END ✅
  PAGE_OFFSET  : 0xffff000000000000   <- 線性映射區在【更低】的位址
  kimage_voffset = 0xffff800007c00000
```

```bash
sudo grep -w _stext /proc/kallsyms
# ffff800008010000 T _stext
```

**`_stext = 0xffff800008010000` = `KIMAGE_VADDR + 0x10000`** ✅
（0x10000 = 64 KB 是 `Image` 的 header + 對齊區）

### ⚠️ 修正舊草稿

> 舊草稿：「`_stext = ffff800008010000`，與 `PAGE_OFFSET (0xffff800000000000)` 相差僅約 0x8010000」

`PAGE_OFFSET` 在 6.1 是 **`0xffff000000000000`** 而不是 `0xffff800000000000`
（Linux 5.4 把核心位址空間上下翻轉了，詳見
[Ch2 Q5](./ch02_arm64_in_linux_kernel.md#q5)）。
正確的說法是：**`_stext` 相對 `KIMAGE_VADDR` 偏移 0x10000**，
而 `_stext` 與 `PAGE_OFFSET` 之間隔了整個 128 TB 的線性映射區。

**為什麼要分開？** 因為核心映像的**虛擬**位址在連結時固定，
**實體**位址由 bootloader 決定（本機是 `0x400000`）。
分開之後兩者可以各自獨立，並各有各的換算錨點：

```
線性映射   __pa()        用 PAGE_OFFSET − PHYS_OFFSET = 0xffff000000000000
核心映像   __pa_symbol() 用 kimage_voffset            = 0xffff800007c00000
```

見 [Ch2 Q10](./ch02_arm64_in_linux_kernel.md#q10)。

---

<a name="q5"></a>
## 5. ARM64 核心中，核心空間和使用者空間如何劃分？

### 結論

用虛擬位址的**最高位元 `VA[63]`**（嚴格說是 `VA[63:48]` 必須全 0 或全 1）：

```
0x0000000000000000 ~ 0x0000ffffffffffff   256 TB   使用者空間  TTBR0  每行程 mm->pgd
0xffff000000000000 ~ 0xffffffffffffffff   256 TB   核心空間    TTBR1  全域 swapper_pg_dir
```

大小由 `TCR_EL1.T0SZ` / `T1SZ` 各自獨立設定（`VA bits = 64 − TxSZ`）。

> **書目**：奔跑吧 §3.3.6「空間劃分」——
> 「在 32 位 Linux 系統中，一共能使用的虛擬地址空間是 4GB，用戶空間和內核空間的劃分
> 通常按照 3∶1 來劃分…ARM64 架構處理器中虛擬地址空間的劃分方式見 2.1.5 節。」
> 完整討論見 [Ch2 Q4](./ch02_arm64_in_linux_kernel.md#q4)。

### 實機驗證

```bash
sudo dmesg | grep -E "T0SZ|T1SZ|user space"
```

```
   T0SZ = 16  -> TTBR0(user)   有效 VA = 48 bits = 256 TB
   T1SZ = 16  -> TTBR1(kernel) 有效 VA = 48 bits = 256 TB
  user space : 0x0000000000000000 - 0x0000ffffffffffff
```

**實際位址的分布**（`pagemap_walk` + `mm_convert` 的輸出）：

| 東西 | 位址 | 空間 |
|------|------|------|
| 使用者 `.text` | `0x0000aaaacae20800` | `0x0000...` → **使用者** |
| 使用者堆疊 | `0x0000ffffd4a4d5af` | `0x0000...` → **使用者** |
| 使用者 mmap | `0x0000ffffbd89c000` | `0x0000...` → **使用者** |
| `mm->pgd`（核心裡的物件） | `0xffff000055dcd000` | `0xffff...` → **核心**（線性映射區） |
| `struct page` | `0xfffffc0001d25a40` | `0xffff...` → **核心**（VMEMMAP 區） |
| `_stext` | `0xffff800008010000` | `0xffff...` → **核心**（映像區） |

**「32 位那套 3:1 / 2:2 的煩惱在 64 位完全消失」**——
48 位各給 256 TB，遠遠超過任何實際需求，所以
**64 位 Linux 沒有 highmem**（`ZONE_HIGHMEM` 在 arm64 根本不存在）。

---

<a name="q6"></a>
## 6. 系統啟動時，ARM64 Linux 核心如何知道系統有多大的實體記憶體？

### 結論

**從 bootloader 傳進來的 Device Tree（DTB）的 `/memory` 節點讀出來。**

流程：

```
U-Boot 初始化 DDR
  └─ 把實際容量填進 DTB 的 /memory 節點的 reg 屬性
       └─ 以 x1 暫存器把 DTB 實體位址傳給核心（ARM64 boot protocol）
            └─ start_kernel() → setup_arch() → setup_machine_fdt()
                 └─ early_init_dt_scan_memory()          drivers/of/fdt.c:1136
                      └─ early_init_dt_add_memory_arch()  drivers/of/fdt.c:1218
                           └─ memblock_add(base, size)     drivers/of/fdt.c:1257
                                └─ arm64_memblock_init()   arch/arm64/mm/init.c
                                     └─ 之後 zone 劃分、sparse_init、伙伴系統
                                        全部以 memblock 登記的區間為準
```

`reg` 屬性是一組或多組 `(base, size)`，每個欄位佔幾個 32-bit cell 由父節點的
`#address-cells` / `#size-cells` 決定。

**其他來源**：UEFI/ACPI 開機路徑改讀 **UEFI Memory Map**；
核心命令列的 `mem=` 參數可以覆蓋/裁剪。最終都彙整進 **memblock**。

> **書目**：奔跑吧 §3.3.3「內存大小」——
> 「在 ARM64 Linux 中，各種設備的相關屬性描述可以採用 DTS 方式或者 BIOS 方式來呈現。」

### 實機驗證（DTB → memblock → MemTotal 一路對帳）

**(a) 直接讀 DTB 的 `/memory/reg`**

```bash
ssh radxa@192.168.68.57 'sudo od -A d -t x1 /proc/device-tree/memory/reg | head -4'
```

```
0000000 00 00 00 00 00 20 00 00 | 00 00 00 00 ef e0 00 00
0000016 00 00 00 01 00 00 00 00 | 00 00 00 01 00 00 00 00
0000032 00 00 00 02 f0 00 00 00 | 00 00 00 00 10 00 00 00
```

`#address-cells = 2`、`#size-cells = 2` ⇒ 每組 base/size 各 8 bytes，解讀成三段：

| # | base | size | 大小 |
|---|------|------|------|
| 1 | `0x0000000000200000` | `0x00000000efe00000` | 3,930,112 kB（≈3.75 GB） |
| 2 | `0x0000000100000000` | `0x0000000100000000` | 4,194,304 kB（4 GB） |
| 3 | `0x00000002f0000000` | `0x0000000010000000` | 262,144 kB（256 MB） |
| | | **合計** | **8,386,560 kB** |

**(b) `/proc/iomem` 完全對應**

```bash
ssh radxa@192.168.68.57 'sudo cat /proc/iomem | grep "System RAM"'
```

```
00200000-efffffff : System RAM        ← 對應第 1 段
100000000-1ffffffff : System RAM      ← 對應第 2 段
2f0000000-2ffffffff : System RAM      ← 對應第 3 段
```

**(c) zoneinfo 的 `present` 也是這個數**

```
zone DMA    present   982,528 頁
zone Normal present 1,114,112 頁
                     ─────────
             合計   2,096,640 頁 × 4 kB = 8,386,560 kB    ✅ 完全一致
```

**(d) 開機日誌的分母也是這個數**

```
Memory: 7840408K/8386560K available (...)
                    ↑ 8,386,560 kB     ✅
```

### 為什麼 8 GiB 的板子只看到 8,386,560 kB（少了 2 MB）？

```
8 GiB = 8,388,608 kB
實際  = 8,386,560 kB
差    =     2,048 kB = 2 MB
```

**最低的 2 MB（`0x0` ~ `0x200000`）被 ATF/BL31（TrustZone 韌體）佔走，
根本沒寫進 DTB。** 所以核心從一開始就不知道有那 2 MB。
`zone DMA` 的 `start_pfn = 512`（= `0x200000 >> 12`）正是這件事的印證。

### 為什麼實體記憶體不是一整段連續的？

> **TRM**（`books/rk3588_trm/part1/chapter_01.txt` **Table 1-1 Address Mapping**）：
> `PCIe3_4L_S` 從 **`0xF0000000`** 開始，之後整片是 MMIO
> （`DSI HOST0 @ 0xFDE20000`、`SPDIF_RX2 @ 0xFDE18000`…）

**`0xF0000000` ~ `0xFFFFFFFF` 這 256 MB 被週邊暫存器佔走**，
所以第一段 DDR 只能到 `0xEFFFFFFF` 就必須斷開，剩下的容量被重新映射到
4 GB 以上（第 2、3 段）。這就是為什麼需要
**`SPARSEMEM`**（本機 `CONFIG_SPARSEMEM_VMEMMAP=y`）來管理有空洞的實體位址空間。

### 補充：`memstart_addr` 為什麼是 0 而不是 `0x200000`？

```
sudo dmesg | grep memstart
#   memstart_addr (PHYS_OFFSET) = 0x0
```

`arm64_memblock_init()` 會做：

```c
memstart_addr = round_down(memblock_start_of_DRAM(), ARM64_MEMSTART_ALIGN);
```

而 `ARM64_MEMSTART_ALIGN` 在 4 KB 頁時 = `1 << PUD_SHIFT` = **1 GB**
（`arch/arm64/include/asm/kernel-pgtable.h:131,147`），
所以 `round_down(0x200000, 1GB) = 0`。
把 `PHYS_OFFSET` 對齊到 1 GB，是為了讓線性映射能用 PUD 級的粗粒度映射、
也讓 VMEMMAP 的起點對齊。

---

<a name="q7"></a>
## 7. 實體頁面如何加入伙伴系統：一頁一頁，還是以 2ⁿ 加入？

### 結論

**以 2ⁿ（冪次對齊的連續塊）為單位批次加入**，不是逐頁。

實作在 `mm/memblock.c`：

```c
static void __init __free_pages_memory(unsigned long start, unsigned long end)
{
	int order;

	while (start < end) {
		order = min(MAX_ORDER - 1UL, __ffs(start));   /* ① 對齊允許的最大階 */

		while (start + (1UL << order) > end)          /* ② 不能超出剩餘區間 */
			order--;

		memblock_free_pages(pfn_to_page(start), start, order);   /* ③ 整塊丟進去 */

		start += (1UL << order);
	}
}
```

**演算法**：
- **① `__ffs(start)`** = `start` 二進位表示中末尾連續 0 的個數，
  也就是這個 PFN 最高能對齊到 2 的幾次方。取它與 `MAX_ORDER-1` 的較小值。
- **② ** 若這一塊會超出 `end`，就把 order 減小直到放得下。
- **③ ** 一次把 `1<<order` 個連續頁交給伙伴系統對應階的 free list。

呼叫鏈：`mem_init()` → `memblock_free_all()` → `free_low_memory_core_early()`
→ `__free_pages_memory()`。

**為什麼要這樣做？**

1. **伙伴系統本來就按 2ⁿ 階組織**。如果逐頁 `__free_page()`（order-0），
   之後得靠「伙伴合併」一步步把相鄰塊合回 order-1、order-2…
   200 萬個頁面要合併 200 萬次，開機時間直接爆掉。
2. **開機時我們「知道」這段記憶體本來就連續**，直接按最大對齊階餵進去，
   一步到位得到大階數的空閒塊。
3. **後續分配大塊連續記憶體（CMA、巨頁、DMA buffer）才有貨可拿。**

本機 `MAX_ORDER = 11`（`include/linux/mmzone.h:28`），所以最大一次可以丟
`2^10 = 1024` 頁 = **4 MB**。

> **書目**：奔跑吧 §3.3.7「物理內存初始化」——
> 「在內核啟動時，內核知道 DDR 物理內存的大小並且計算出…內核空間的內存布局後，
> 物理內存頁面就要添加到伙伴系統中。」

### 實機驗證

**(a) 用一個具體的例子手算演算法**

本機 `zone DMA` 的第一個 PFN 是 `0x200`（512）：

```
start = 0x200 = 0b1000000000
__ffs(0x200) = 9                      ← 末尾有 9 個 0
order = min(MAX_ORDER-1=10, 9) = 9    ← 一次丟 2^9 = 512 頁 = 2 MB
start += 512 → 0x400
__ffs(0x400) = 10 → order = min(10,10) = 10   ← 之後每次丟 2^10 = 1024 頁 = 4 MB
```

⇒ **從 PFN 0x200 開始，先丟一塊 order-9（2 MB），之後一路丟 order-10（4 MB）。**

**(b) `/proc/buddyinfo` 顯示各階的空閒塊數量**

```bash
ssh radxa@192.168.68.57 'cat /proc/buddyinfo'
```

```
                  order:   0    1    2    3    4    5   6   7   8   9  10
Node 0, zone  DMA          2    8  196  197  156   96  53  32  11   4 465
Node 0, zone  Normal       8    2   12   95   24    3   3  64   0   0   0
```

**`zone DMA` 在 order-10 有 465 塊**（465 × 4 MB = 1.8 GB）——
這種「高階塊特別多」的分布正是「開機時整段整段丟進去」留下的指紋。
若當初是逐頁 free，開機初期會是 order-0 堆積如山。

**（`zone Normal` 的 order-8/9/10 全是 0，是開機後跑了兩天 I/O 造成的碎片化，
與 [Ch6 Q13](./ch06_memory_management_case_studies.md#q13) 觀察到的
watermark boost 飽和是同一件事的兩面。）**

**(c) 用 order-10 的塊數反推**

```
zone DMA managed = 959,006 頁 ≈ 3.66 GB
order-10 空閒塊 465 × 1024 頁 = 476,160 頁 = 1.86 GB (占 49.6%)
```

開機兩天後仍有一半的 ZONE_DMA 保持在最大階完整可用——
這正是「批次以 2ⁿ 加入 + 反碎片化 migratetype 分組」的成果
（見 [Ch6 Q4](./ch06_memory_management_case_studies.md#q4)）。

**(d) 額外觀察：核心映像自己也被 `memblock_add`**

```c
/* arch/arm64/mm/init.c:328 */
memblock_add(__pa_symbol(_text), (u64)(_end - _text));
```

核心映像所在的實體區間也被登記進 memblock（然後立刻 `memblock_reserve`），
確保 `struct page` 陣列涵蓋它 —— 這樣 `free_initmem()` 才有辦法在開機末期
把 `__init_begin ~ __init_end` 那 7,296 kB **還給伙伴系統**
（見 [Ch2 Q8](./ch02_arm64_in_linux_kernel.md#q8) 與
[Ch6 Q3](./ch06_memory_management_case_studies.md#q3)）。

---

## 附錄：本章實驗程式

| 檔案 | 用途 | 對應題目 |
|------|------|---------|
| `experiments/mm_convert.c` + `Makefile.mod` | 核心模組：九種轉換全部跑一遍 | **Q3**，兼 Q1（pgdat）、Q6（zone） |
| `experiments/hold_page.c` | 配角：配一頁匿名記憶體 + fork 共享，供模組查詢 | Q3 |
| `experiments/cache_ladder.c` | Cache 延遲階梯（pointer chase） | **Q2** |
| `experiments/pagemap_walk.c` | 使用者態 VA→PFN→PA，與模組交叉驗證 | Q3 |
| `experiments/armv8_dump.c` | 系統暫存器 + 記憶體佈局 | Q4, Q5, Q6 |

一鍵重跑：

```bash
# 核心模組
ssh radxa@192.168.68.57 'mkdir -p ~/exp/armv8'
scp notes/experiments/{armv8_dump,mm_convert}.c radxa@192.168.68.57:~/exp/armv8/
scp notes/experiments/Makefile.mod radxa@192.168.68.57:~/exp/armv8/Makefile
scp notes/experiments/{cache_ladder,pagemap_walk,hold_page}.c radxa@192.168.68.57:~/exp/

ssh radxa@192.168.68.57 'cd ~/exp/armv8 && make'

# Q2：cache 階梯
ssh radxa@192.168.68.57 'cd ~/exp && gcc -O2 -o cache_ladder cache_ladder.c
  taskset -c 4 ./cache_ladder; taskset -c 0 ./cache_ladder'

# Q3：九種轉換
ssh radxa@192.168.68.57 'cd ~/exp && gcc -O2 -o hold_page hold_page.c && sudo ./hold_page &'
# 記下印出的 PID 與 VA，然後：
ssh radxa@192.168.68.57 'cd ~/exp/armv8
  sudo insmod mm_convert.ko target_pid=<PID> target_va=<VA>
  sudo dmesg | sed "s/^\[[^]]*\] //"
  sudo rmmod mm_convert'

# Q1/Q6/Q7：純指令
ssh radxa@192.168.68.57 '
  numactl --hardware; ls /sys/devices/system/node/
  sudo od -A d -t x1 /proc/device-tree/memory/reg | head -4
  sudo cat /proc/iomem | grep "System RAM"
  cat /proc/buddyinfo'
```

---

## 跨章節關聯

| 本章題目 | 關聯 |
|---------|------|
| [Q2](#q2) cache 階層 | [Ch2 Q13](./ch02_arm64_in_linux_kernel.md#q13) PoU/PoC（`CLIDR_EL1.LoC=3` ← L3 是 PoC）、[Ch2 Q16](./ch02_arm64_in_linux_kernel.md#q16) Inner Shareable 域 = DSU |
| [Q3](#q3) rmap | [Ch7 Q9](./ch07_process_management_basic_concepts.md#q9) COW（`_mapcount=1` 就是 fork 共享的結果） |
| [Q3](#q3) zone | [Ch6 Q11](./ch06_memory_management_case_studies.md#q11) lowmem_reserve、[Ch6 Q13](./ch06_memory_management_case_studies.md#q13) watermark boost |
| [Q4](#q4)/[Q5](#q5) | [Ch2 Q4-Q6](./ch02_arm64_in_linux_kernel.md#q4)、[Ch2 Q9-Q12](./ch02_arm64_in_linux_kernel.md#q9) |
| [Q6](#q6) memblock | [Ch6 Q3](./ch06_memory_management_case_studies.md#q3) MemTotal 對帳（8,386,560 → 8,129,516） |
| [Q7](#q7) 伙伴系統 | [Ch6 Q4](./ch06_memory_management_case_studies.md#q4) migratetype 反碎片化 |
