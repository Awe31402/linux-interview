# 第 6 章 內存管理之實戰案例分析 — 高頻面試題解答

> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588，8×Cortex-A76/A55），`192.168.68.57`
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`
> **關鍵組態**（`zcat /proc/config.gz`）：
> `CONFIG_ARM64_PAGE_SHIFT=12`（4KB page）、`CONFIG_ARM64_VA_BITS=48`、`CONFIG_PGTABLE_LEVELS=4`、
> `# CONFIG_NUMA is not set`、`# CONFIG_TRANSPARENT_HUGEPAGE is not set`、`CONFIG_ZRAM=y`
> **記憶體**：8 GiB LPDDR，swap = zram0（zstd）3.9 GiB
>
> **書目對照**
> - 《奔跑吧 Linux 內核》（第二版）卷 1 第 6 章 —
>   `books/running-linux-kernel/running-kernel-1-txt/14_第6章_内存管理之实战案例分析.txt`
>   （下稱「奔跑吧 §6.x」）
> - *Linux Memory Manager* —
>   `books/memory-manager/chapters/{02_physical_memory,09_the_page_cache,11_reclaim_and_memory_pressure,12_swap_memory}.txt`
> - 核心程式碼路徑皆相對於本專案樹
>   `/home/awe/disk/yocto-rockchip-sdk/build/tmp/work-shared/rockchip-rk3588-rock-5b/kernel-source`
>
> **登入方式**：`ssh radxa@192.168.68.57`（帳密皆為 `radxa`）；需要 root 的指令以 `sudo` 執行。

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | 內存管理模組統計了哪些頁面 | `/proc/vmstat`、`/proc/zoneinfo` |
| [2](#q2) | `/proc/meminfo` 每一項的含義 | 本機 meminfo 全欄位對照表 |
| [3](#q3) | 為什麼 MemTotal ≠ 實體記憶體大小 | 差值 259092 kB **逐項對帳成功** |
| [4](#q4) | slab 為何要分 SReclaimable / SUnreclaim | `/sys/kernel/slab/*/reclaim_account` |
| [5](#q5) | Active(anon)+Inactive(anon) ≠ AnonPages | 恆等式 **完全吻合** |
| [6](#q6) | Active(file)+Inactive(file) ≠ Mapped | 512MB shmem 實驗 |
| [7](#q7) | Active(file)+Inactive(file) ≠ Cached | 恆等式 **完全吻合** |
| [8](#q8) | `/proc/PID/status` 記憶體欄位 | PID 1 systemd 實測 |
| [9](#q9) | S_swap ≠ P_swap | `MADV_PAGEOUT` 實驗 **直接證明** |
| [10](#q10) | `min_free_kbytes` | 水位分攤公式 **逐 zone 驗算** |
| [11](#q11) | `lowmem_reserve_ratio` | `protection[]` = 4192 **驗算吻合** |
| [12](#q12) | `zone_reclaim_mode` | 本機**根本沒有這個節點**（無 NUMA） |
| [13](#q13) | `watermark_boost_factor` | 本機 boost=6463 **正在生效且已飽和** |
| [14](#q14) | 影響髒頁回寫的參數 | `nr_dirty_threshold` 驗算 |

---

<a name="q1"></a>
## 1. Linux 內核的內存管理模組都對哪些頁面進行了統計？

### 結論

核心用**三層**計數器來統計頁面，全部定義在 `include/linux/mmzone.h`，並透過 per-CPU 的
差分陣列（`vm_stat_diff[]`）緩衝後再回寫全域值，以避免快取行競爭：

| 層級 | 全域陣列 | 列舉型別 | 對外節點 |
|------|----------|----------|----------|
| **per-zone**（內存管理區） | `vm_zone_stat[]` | `enum zone_stat_item` | `/proc/zoneinfo` 的 `nr_zone_*` |
| **per-node**（內存節點） | `vm_node_stat[]` | `enum node_stat_item` | `/proc/vmstat`、`/proc/meminfo` |
| **事件計數**（累計次數） | `vm_event_states` | `enum vm_event_item` | `/proc/vmstat` 的 `pgfault`/`pgsteal_*`… |

**per-zone（`enum zone_stat_item`，`include/linux/mmzone.h`）**
`NR_FREE_PAGES`、`NR_ZONE_INACTIVE_ANON`、`NR_ZONE_ACTIVE_ANON`、`NR_ZONE_INACTIVE_FILE`、
`NR_ZONE_ACTIVE_FILE`、`NR_ZONE_UNEVICTABLE`、`NR_ZONE_WRITE_PENDING`、`NR_MLOCK`、
`NR_BOUNCE`、`NR_ZSPAGES`、`NR_FREE_CMA_PAGES`。

**per-node（`enum node_stat_item`）** 分成幾大類：

1. **LRU 五條鏈**：`NR_INACTIVE_ANON`／`NR_ACTIVE_ANON`／`NR_INACTIVE_FILE`／`NR_ACTIVE_FILE`／`NR_UNEVICTABLE`
2. **slab**：`NR_SLAB_RECLAIMABLE_B`、`NR_SLAB_UNRECLAIMABLE_B`
3. **反向映射**：`NR_ANON_MAPPED`（→ meminfo 的 `AnonPages`）、`NR_FILE_MAPPED`（→ `Mapped`）
4. **page cache**：`NR_FILE_PAGES`、`NR_SHMEM`、`NR_FILE_DIRTY`、`NR_WRITEBACK`、`NR_WRITEBACK_TEMP`
5. **核心自用**：`NR_KERNEL_STACK_KB`、`NR_PAGETABLE`、`NR_SECONDARY_PAGETABLE`、`NR_KERNEL_MISC_RECLAIMABLE`
6. **回收 / workingset**：`WORKINGSET_REFAULT_*`、`WORKINGSET_ACTIVATE_*`、`WORKINGSET_RESTORE_*`、`NR_VMSCAN_WRITE`
7. **巨頁**：`NR_ANON_THPS`、`NR_SHMEM_THPS`、`NR_FILE_THPS`、`NR_*_PMDMAPPED`
8. **隔離 / pin**：`NR_ISOLATED_ANON`、`NR_ISOLATED_FILE`、`NR_FOLL_PIN_ACQUIRED/RELEASED`
9. **swap**：`NR_SWAPCACHE`

> **書目**：奔跑吧 §6.1.1「vm_stat 計數值」介紹了
> `global_node_page_state()` / `inc_zone_page_state()` / `dec_zone_page_state()` 這組介面。
> 另見 *Linux Memory Manager* `02_physical_memory.txt`（zone 與 node 的統計模型）。

### 實機驗證

```bash
ssh radxa@192.168.68.57
# per-node 計數（vm_node_stat[] 的文字化）
cat /proc/vmstat
# per-zone 計數（vm_zone_stat[] 的文字化）
cat /proc/zoneinfo
```

本機 `/proc/zoneinfo` ZONE_DMA 節錄（`per-node stats` 區塊是 node 級，後面 `nr_zone_*` 才是 zone 級）：

```
Node 0, zone      DMA
  per-node stats
      nr_inactive_anon 358746      nr_active_anon 2350
      nr_inactive_file 179231      nr_active_file 681820
      nr_slab_reclaimable 107694   nr_slab_unreclaimable 32821
      nr_anon_pages 343227         nr_mapped    123859
      nr_file_pages 885925         nr_shmem     24869
      nr_kernel_stack 9116         nr_page_table_pages 5061
  pages free     604713
      nr_zone_inactive_anon 48664  nr_zone_active_anon 3
      nr_zone_inactive_file 47035  nr_zone_active_file 172950
      nr_free_cma  62670
```

注意 `nr_inactive_anon`（node 級 = 358746）與 `nr_zone_inactive_anon`（zone 級 = 48664）
是**兩個不同的計數器**：Linux 4.8 之後 LRU 鏈從 zone 搬到 node，但 zone 級仍保留一份
給 compaction / reclaim retry 用。

事件型計數（本機）：

```
pgfault 52275647     pgmajfault 11642
pgscan_kswapd 1039060   pgsteal_kswapd 335187
pgscan_direct 69880     pgsteal_direct 23254
allocstall_normal 208   compact_daemon_wake 100
```

---

<a name="q2"></a>
## 2. 請解釋 `/proc/meminfo` 節點中每一項的含義

### 結論

實作在 `fs/proc/meminfo.c` 的 `meminfo_proc_show()`。下表是**本機實測值**（`cat /proc/meminfo`）
＋ 對應的核心計數來源。

> **書目**：奔跑吧 §6.1.2「meminfo 分析」表 6.3 有完整對照表；本表以本機 6.1 核心的
> 實際欄位（新增 `SecPageTables`、`KReclaimable`、`Percpu` 等）補齊。

```bash
ssh radxa@192.168.68.57 'cat /proc/meminfo'
```

| 欄位 | 本機值 (kB) | 含義 / 計數來源（`fs/proc/meminfo.c`） |
|------|------------|--------------------------------------|
| `MemTotal` | 8129516 | 伙伴系統管理的總頁數 `_totalram_pages`（= 所有 zone 的 `managed`），**不含開機保留** → 見 [Q3](#q3) |
| `MemFree` | 2457588 | `NR_FREE_PAGES` |
| `MemAvailable` | 6182444 | `si_mem_available()` 估算「不觸發 swap/OOM 前提下可用的量」，見下方公式 |
| `Buffers` | 626392 | 區塊裝置 inode 的 page cache，`nr_blockdev_pages()` |
| `Cached` | 2917288 | `NR_FILE_PAGES − swapcache − Buffers`（`meminfo.c:46`） |
| `SwapCached` | 20 | `total_swapcache_pages()`：曾被換出、又被換回但 swap slot 還沒釋放的匿名頁 |
| `Active` / `Inactive` | 2736680 / 2151680 | ACTIVE(anon)+ACTIVE(file) / INACTIVE(anon)+INACTIVE(file) |
| `Active(anon)` | 9400 | `NR_ACTIVE_ANON`（LRU 鏈）|
| `Inactive(anon)` | 1434756 | `NR_INACTIVE_ANON` |
| `Active(file)` | 2727280 | `NR_ACTIVE_FILE` |
| `Inactive(file)` | 716924 | `NR_INACTIVE_FILE` |
| `Unevictable` | 28004 | `NR_UNEVICTABLE`：不可回收（mlock、ramfs、SHM_LOCK…） |
| `Mlocked` | 0 | `NR_MLOCK`：被 `mlock()` 鎖住 |
| `SwapTotal` / `SwapFree` | 4064756 / 4061172 | swap 分割區總量 / 剩餘（本機是 zram0） |
| `Dirty` | 552 | `NR_FILE_DIRTY` |
| `Writeback` | 0 | `NR_WRITEBACK`：正在回寫中 |
| `AnonPages` | 1372908 | `NR_ANON_MAPPED`：**有 anon RMAP 且映射到使用者空間**的匿名頁（不含 shmem）→ [Q5](#q5) |
| `Mapped` | 495436 | `NR_FILE_MAPPED`：被映射進頁表的 page cache 頁 → [Q6](#q6) |
| `Shmem` | 99476 | `NR_SHMEM`：tmpfs / devtmpfs / SysV shm / GEM 的頁 |
| `KReclaimable` | 430776 | `NR_SLAB_RECLAIMABLE_B + NR_KERNEL_MISC_RECLAIMABLE` |
| `Slab` | 562060 | SReclaimable + SUnreclaim |
| `SReclaimable` | 430776 | `NR_SLAB_RECLAIMABLE_B` → [Q4](#q4) |
| `SUnreclaim` | 131284 | `NR_SLAB_UNRECLAIMABLE_B` |
| `KernelStack` | 9116 | `NR_KERNEL_STACK_KB`：所有行程核心堆疊總和（ARM64 每個 16KB） |
| `PageTables` | 20244 | `NR_PAGETABLE`：使用者頁表佔用的頁 → 見 Ch7 Q12 |
| `SecPageTables` | 0 | `NR_SECONDARY_PAGETABLE`：KVM stage-2 頁表（本機沒跑 VM） |
| `Bounce` | 0 | `NR_BOUNCE`：bounce buffer |
| `WritebackTmp` | 0 | `NR_WRITEBACK_TEMP`：FUSE 回寫暫存 |
| `CommitLimit` | 8129512 | `overcommit_ratio` 下允許 commit 的上限 |
| `Committed_AS` | 3965248 | 目前所有行程「承諾」要用的總量 |
| `VmallocTotal` | 133143592960 | vmalloc 區大小（**≈127 TiB，來自 48-bit VA**） |
| `VmallocUsed` | 59680 | 已用 vmalloc |
| `Percpu` | 64032 | per-CPU 配置器佔用，`pcpu_nr_pages()` |
| `CmaTotal` | 262144 | CMA 池總量（**256 MiB**，見開機 log） |
| `CmaFree` | 250680 | CMA 空閒 |
| `HugePages_*` / `Hugepagesize` | 0 / 2048 | HugeTLB（本機未預留；RK3588 支援 64K/2M/32M/1G 四種） |

**`MemAvailable` 的公式**（`mm/page_alloc.c` `si_mem_available()`）：

```c
available  = NR_FREE_PAGES - totalreserve_pages;
pagecache  = ACTIVE_FILE + INACTIVE_FILE;
available += pagecache - min(pagecache/2, wmark_low);      /* 檔案快取至少留一半 */
reclaimable = NR_SLAB_RECLAIMABLE_B + NR_KERNEL_MISC_RECLAIMABLE;
available += reclaimable - min(reclaimable/2, wmark_low);  /* 可回收 slab 也只算一半 */
```

**RK3588 特有的一點**：`VmallocTotal` 高達 127 TiB，是因為 `CONFIG_ARM64_VA_BITS=48`
（核心線性映射區 128 TiB）；`CmaTotal=256MB` 來自 device tree 的 `reserved-memory/cma` 節點，
給 VOP2 / RGA / MPP 等 RK3588 多媒體 IP 做大塊連續 DMA 用：

```bash
ssh radxa@192.168.68.57 'ls /proc/device-tree/reserved-memory/'
# cma  drm-cubic-lut@0  drm-logo@0  minidump-mem@c000000  minidump-smem@1f0000  ramoops@110000
```

---

<a name="q3"></a>
## 3. 為什麼 `/proc/meminfo` 的 MemTotal 不等於（QEMU 虛擬機／實體板子）分配的內存大小？

### 結論

`MemTotal` 只統計**最終交給伙伴系統管理的頁面**（`_totalram_pages`，即各 zone 的 `managed_pages`），
而下面這幾塊在 memblock 階段就被 **reserve** 掉、從未進入伙伴系統：

1. **韌體佔用**：BL31/ATF、OP-TEE、SPL 等（RK3588 在 `0x0 ~ 0x200000` 這 2 MiB）
2. **核心映像本體**：`text/rodata/rwdata/bss` + `initrd` + `memblock` 自己的 bitmap + `struct page` 陣列
3. **device tree `reserved-memory`**：ramoops、minidump、drm-logo…
4. **CMA 池**（開機時 reserve，稍後才「還」給伙伴系統當 MIGRATE_CMA）

其中 `init` 段與 initrd 在開機末期會被釋放回伙伴系統，CMA 也會被 `init_cma_reserved_pageblock()`
用 `adjust_managed_page_count()` 加回 `managed`，所以 **最終 MemTotal ≠ 開機 log 的 available**。

> **書目**：奔跑吧 §6.1.2 問題（1)「為什麼 MemTotal 不等於 QEMU 虛擬機中分配的內存大小？」
> 書上用 1 GiB 的 QEMU 例子：`53400K reserved`，扣掉 `Freeing unused kernel memory: 4608K`，
> 剩 48792 KB，加上 MemTotal 正好 1 GiB。下面把同一套算法套在 RK3588 上，**逐項對帳完全成立**。

### 實機驗證（本機 8 GiB 的完整對帳）

```bash
ssh radxa@192.168.68.57
sudo journalctl -k -b | grep -E "Memory:|Reserved memory|Freeing"
sudo cat /proc/iomem | grep "System RAM"
grep -E "present|managed|spanned" /proc/zoneinfo
grep MemTotal /proc/meminfo
```

**(a) 開機日誌**

```
Reserved memory: created CMA memory pool at 0x0000000010000000, size 256 MiB
Memory: 7840408K/8386560K available (16768K kernel code, 3796K rwdata, 6856K rodata,
        7296K init, 769K bss, 284008K reserved, 262144K cma-reserved)
Freeing initrd memory: 19668K
Freeing unused kernel memory: 7296K
```

**(b) BIOS/韌體看得到的實體 RAM**（`/proc/iomem`）

```
00200000-efffffff : System RAM      → 0xEFE00000 = 3,930,112 kB
100000000-1ffffffff : System RAM    → 4 GiB      = 4,194,304 kB
2f0000000-2ffffffff : System RAM    → 256 MiB    =   262,144 kB
                                       合計       = 8,386,560 kB  ← 與 log 的分母一致
```

⇒ **實體 8 GiB (8,388,608 kB) 中，最低的 2 MiB (2048 kB) 根本沒交給核心**，被 RK3588 的
ATF/BL31 佔走（`0x0~0x200000`，`start_pfn: 512` 也印證了這點）。

**(c) 對帳**

```
開機 available          =  8,386,560 − 284,008 (reserved) − 262,144 (cma) = 7,840,408 kB  ✅ 與 log 相符
 + Freeing unused kernel memory (init 段)                        +  7,296 kB
 + Freeing initrd memory                                         + 19,668 kB
 + CMA 池歸還伙伴系統 (adjust_managed_page_count)                 + 262,144 kB
────────────────────────────────────────────────────────────────────────────
                                                        MemTotal = 8,129,516 kB  ✅ 完全吻合
```

**(d) 用 zoneinfo 交叉驗證**（不需要開機 log 也能算）

```
zone DMA    : present 982,528   managed   959,006 pages
zone Normal : present 1,114,112 managed 1,073,373 pages
────────────────────────────────────────────────────────
present 合計 = 2,096,640 pages × 4 kB = 8,386,560 kB   ← 交給核心的實體頁
managed 合計 = 2,032,379 pages × 4 kB = 8,129,516 kB   ← 正好等於 MemTotal ✅
```

**(e) 最終差額拆解**

```
8,388,608 (8 GiB) − 8,129,516 (MemTotal) = 259,092 kB
                  = 2,048   kB  (ATF/BL31 佔用的最低 2 MiB，核心看不到)
                  + 257,044 kB  (= 284,008 reserved − 7,296 init − 19,668 initrd)
```

> **面試加分點**：`present` 是「核心知道有這塊 RAM」，`managed` 是「伙伴系統真的能配」，
> 兩者的差就是 `reserved`。回答時直接用 `/proc/zoneinfo` 的 present/managed 差值，
> 比背開機 log 更可靠（開機 log 的 ring buffer 會被沖掉——本機 uptime 1 天多，
> `dmesg` 已經看不到開機訊息，只能靠 `journalctl -k -b`）。

---

<a name="q4"></a>
## 4. 為什麼 slab 要區分 SReclaimable 和 SUnreclaim？

### 結論

**分類依據**：建立 slab 描述符時是否帶 `SLAB_RECLAIM_ACCOUNT` 旗標。

```c
/* mm/slab_common.c / mm/slub.c : 配頁時 */
if (s->flags & SLAB_RECLAIM_ACCOUNT)
        mod_node_page_state(page_pgdat(page), NR_SLAB_RECLAIMABLE_B, ...);
else
        mod_node_page_state(page_pgdat(page), NR_SLAB_UNRECLAIMABLE_B, ...);
```

**區分的三個實際作用**：

1. **算出正確的 `MemAvailable`**。`si_mem_available()`（`mm/page_alloc.c`）只把
   `NR_SLAB_RECLAIMABLE_B` 算進可用量，而且還保守地只算一半：
   ```c
   reclaimable = global_node_page_state_pages(NR_SLAB_RECLAIMABLE_B) +
                 global_node_page_state(NR_KERNEL_MISC_RECLAIMABLE);
   available += reclaimable - min(reclaimable / 2, wmark_low);
   ```
   若不區分，`MemAvailable` 會把 `task_struct`、`vm_area_struct` 這種**永遠回收不了**的
   核心物件也算成「可用」，導致上層程式（如 JVM、資料庫）誤判可用記憶體而 OOM。

2. **決定頁面的遷移類型（migratetype）**。帶 `SLAB_RECLAIM_ACCOUNT` 的 cache 會設
   `__GFP_RECLAIMABLE`，配到的頁塊屬 `MIGRATE_RECLAIMABLE`；不帶的屬 `MIGRATE_UNMOVABLE`。
   把「可能被釋放」和「絕對釋放不了」的頁分開放，是**反碎片化（anti-fragmentation）**的核心手段——
   否則零星的 `task_struct` 會把整個 pageblock 釘死，讓 CMA / 巨頁再也湊不出連續空間。
   這在 RK3588 這種需要 256 MiB CMA 給 VOP2/RGA 的平台上特別關鍵。

3. **回收路徑**：`shrink_slab()` 走 shrinker 回呼（dentry/inode 的 `scan_objects`），
   以及 slab 自己的收割機（`cache_reap()`）會銷毀全空的 slab。

> **書目**：奔跑吧 §6.1.2 問題（3)「為什麼 slab 分配器要區分 SReclaimable 和 SUnreclaim？」
> ——書中明確指出「統計 SReclaimable 和 SUnreclaim 頁面的含義在於計算系統可用的總內存數量，
> 即 meminfo 中的 MemAvailable，詳見 `si_mem_available()` 函數」。

### 實機驗證

```bash
ssh radxa@192.168.68.57
# 直接讀每個 cache 的 SLAB_RECLAIM_ACCOUNT 旗標
sudo sh -c 'for d in dentry inode_cache ext4_inode_cache kmalloc-1k task_struct vm_area_struct; do
    printf "%-22s reclaim_account=%s\n" $d $(cat /sys/kernel/slab/$d/reclaim_account); done'
```

```
dentry                 reclaim_account=1   ← 可回收
inode_cache            reclaim_account=1   ← 可回收
ext4_inode_cache       reclaim_account=1   ← 可回收
kmalloc-1k             reclaim_account=0   ← 不可回收
task_struct            reclaim_account=0   ← 不可回收
vm_area_struct         reclaim_account=0   ← 不可回收
```

規律很清楚：**有 shrinker 撐腰的檔案系統快取（dentry/inode）才是 reclaimable，
描述行程與位址空間生命週期的物件一律 unreclaimable。**

本機 slab 用量（`sudo cat /proc/slabinfo`，欄位為 `<active_objs> <num_objs> <objsize>`）：

```
ext4_inode_cache  132206 132216   1504    →  ~190 MB，可回收
dentry            241130 241240    216    →  ~50  MB，可回收
buffer_head       492576 492576    128    →  ~60  MB
inode_cache        36522  36540    776    →  ~27  MB，可回收
task_struct          645    672   3968    →  ~2.6 MB，不可回收
vm_area_struct     29305  29400    136    →  ~4   MB，不可回收
```

meminfo 對應：

```
Slab:  562060 kB  =  SReclaimable: 430776 kB  +  SUnreclaim: 131284 kB   ✅
```

> 補充：`KReclaimable (430776) == SReclaimable (430776)` 是因為本機
> `NR_KERNEL_MISC_RECLAIMABLE == 0`（`/proc/vmstat` 可見），沒有註冊非 slab 的可回收核心記憶體。

---

<a name="q5"></a>
## 5. 為什麼 `Active(anon) + Inactive(anon) ≠ AnonPages`？

### 結論

因為 **shmem 是「另類頁面」**：它掛在**匿名 LRU 鏈**上，卻**不計入 `AnonPages`**。

- `Active(anon)+Inactive(anon)` = 匿名 LRU 鏈上的所有頁 = **可以被寫進 swap 的頁**
  （判定依據是 `PageSwapBacked()`，見 `mm/shmem.c` 配頁時的 `__SetPageSwapBacked(page)`）
- `AnonPages` = `NR_ANON_MAPPED` = **建立了 anon RMAP 且映射到使用者空間的傳統匿名頁**
  （`page_add_new_anon_rmap()` → `__mod_node_page_state(..., NR_ANON_MAPPED, nr)`）

shmem 有 `PG_swapbacked` 所以進匿名 LRU，但它的 `page->mapping` 指向 `inode->i_mapping`，
走的是 file rmap（`page_add_file_rmap()`），因此只計入 `NR_SHMEM` / `NR_FILE_MAPPED`，
不計入 `NR_ANON_MAPPED`。

**完整恆等式**（本機實測完全吻合）：

```
Active(anon) + Inactive(anon)  =  AnonPages + Shmem + SwapCached − Unevictable(中的 shmem/anon 部分)
```

> **書目**：奔跑吧 §6.1.2 問題（4)「為什麼 Active(anon)+Inactive(anon) 不等於 AnonPages？」
> 書中原話：「shmem 頁面一方面被添加到了匿名頁面的 LRU 鏈表裡，另一方面被統計到文件映射頁面的
> 計數中，真是個『另類的』頁面。」
> 另見 *Linux Memory Manager* `09_the_page_cache.txt` 對 shmem 雙重身分的說明。

### 實機驗證

實驗程式（放在裝置 `/tmp/lru_shmem.c`）：分配 512 MB `MAP_SHARED|MAP_ANONYMOUS`（= shmem）
與 512 MB `MAP_PRIVATE|MAP_ANONYMOUS`（= 傳統匿名頁），觀察 meminfo 各欄位的變化。

```bash
scp lru_shmem.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -o lru_shmem lru_shmem.c && ./lru_shmem'
```

```
                 anonLRU     fileLRU  | AnonPages   Shmem   Cached   Buffers   Mapped
baseline         1444760     3460744  |  1373324    99484   2933596   626404   496780
+512MB shmem     1968700     3460612  |  1373128   623428   3457636   626404  1020724
+512MB anon      1968612     3460624  |  1897120    99484   2933704   626404   496608
after munmap     1444476     3460624  |  1372976    99484   2933704   626404   496608
```

**讀法**：

| 動作 | anonLRU | AnonPages | Shmem | Cached | Mapped |
|------|---------|-----------|-------|--------|--------|
| +512MB **shmem** | **+523,940** ✅ | **−196（沒動）** ❗ | +523,944 | +524,040 | +523,944 |
| +512MB **anon** | +523,852 | **+523,992** ✅ | 沒動 | 沒動 | 沒動 |

shmem 讓 `anonLRU` 漲了 512 MB，卻**完全沒有動到 `AnonPages`** —— 這就是兩者不相等的直接原因。

程式最後印出的恆等式檢查（同一瞬間讀取，避免 per-CPU 計數漂移）：

```
[Q5] anonLRU(1444476) == AnonPages(1372976) + Shmem(99484) + SwapCached(20) − Unevictable(28004) = 1444476   ✅ 完全相等
```

> **注意**：如果你用 `cat /proc/meminfo` 分兩次讀，會有幾百 kB 的誤差
> （我另一次取樣得到 1444544 vs 1444808，差 264 kB），那是 per-CPU `vm_stat_diff[]`
> 還沒回寫到全域造成的**正常漂移**，不是公式錯。面試時提到這點會加分。

---

<a name="q6"></a>
## 6. 為什麼 `Active(file) + Inactive(file) ≠ Mapped`？

### 結論

這兩個量根本是**不同維度**的統計：

- `Active(file)+Inactive(file)` = 檔案 LRU 鏈上的**所有** page cache 頁
  （不管有沒有被誰 `mmap` 進頁表；`read()`/`readahead` 讀進來的也算）
- `Mapped` = `NR_FILE_MAPPED` = 「**目前被映射進某個行程頁表**」的 page cache 頁
  （在 `page_add_file_rmap()` 增加，`page_remove_rmap()` 減少）

也就是說 **`Mapped` 是檔案 LRU 的一個子集合的度量**，而且**還多算了 shmem**：
shmem 被映射時同樣走 `page_add_file_rmap()`，會加到 `NR_FILE_MAPPED`，
但它人在**匿名** LRU 鏈上。所以：

```
Mapped  ⊄  fileLRU  且  Mapped  ⊅  fileLRU     ← 互不包含，比較它們沒有意義
```

本機 `Mapped = 495436 kB` 只佔 `fileLRU = 3444204 kB` 的 14%，因為絕大部分是
`buffer_head` + ext4 檔案讀取產生的、沒有被 mmap 的快取。

> **書目**：奔跑吧 §6.1.2 問題（5)「為什麼 Active(file) + Inactive(file) 不等於 Mapped？」
> ——書中特別點出「有一個特殊情況需要考慮，就是 shmem 頁面。它會被計入 NR_FILE_MAPPED
> 計數值中，但是它會設置 PG_SwapBacked 標誌位，因此它會被計入匿名頁面。」

### 實機驗證

用同一支 `lru_shmem` 的輸出（見 [Q5](#q5)）：

```
                 anonLRU     fileLRU  | ... Mapped
baseline         1444760     3460744  | ... 496780
+512MB shmem     1968700     3460612  | ... 1020724     ← Mapped +523,944，fileLRU 卻沒動！
+512MB anon      1968612     3460624  | ...  496608     ← 兩者都沒動
```

**三個結論同時被證明**：

1. 映射 512 MB shmem → `Mapped` 暴增 512 MB，**`fileLRU` 紋風不動**
   ⇒ `Mapped` 裡有一大塊根本不在檔案 LRU 上。
2. 映射 512 MB 傳統匿名頁 → `Mapped` **完全沒變**
   ⇒ `NR_FILE_MAPPED` 只管 page cache，不管匿名頁（匿名頁看 `AnonPages`）。
3. `fileLRU`（3.4 GB）遠大於 `Mapped`（0.5 GB）
   ⇒ 大量 page cache 從未被 mmap。

想看是誰在貢獻 `Mapped`：

```bash
ssh radxa@192.168.68.57 "sudo awk '/^Rss:/{r+=\$2} END{print r\" kB total Rss\"}' /proc/*/smaps 2>/dev/null"
# 或針對單一行程
ssh radxa@192.168.68.57 'sudo grep -E "^(Rss|Private_Clean|Shared_Clean):" /proc/1/smaps_rollup'
```

---

<a name="q7"></a>
## 7. 為什麼 `Active(file) + Inactive(file) ≠ Cached`？

### 結論

兩者差在 **shmem** 和 **Buffers** 兩項，方向相反：

```c
/* fs/proc/meminfo.c:46 */
cached = global_node_page_state(NR_FILE_PAGES) - total_swapcache_pages() - i.bufferram;
```

| | 含 shmem？ | 含 Buffers？ |
|---|---|---|
| `Cached` | **含**（shmem 計入 `NR_FILE_PAGES`） | **不含**（明確扣掉） |
| `Active(file)+Inactive(file)` | **不含**（shmem 在匿名 LRU） | **含**（blockdev 的 page cache 頁在檔案 LRU 上） |

所以精確的換算關係是：

```
Active(file) + Inactive(file)  =  Cached − Shmem + Buffers
```

> **書目**：奔跑吧 §6.1.2 問題（6)「為什麼 Active(file) + Inactive(file) 不等於 Cached？」
> 書中給出 `Cached = NR_FILE_PAGES – swap_cache – Buffers` 並指出 shmem 是差異來源，
> 但沒有給出「+ Buffers」這一項；下面實機驗證補上這一項後，恆等式**逐位元完全相等**。

### 實機驗證

同樣來自 `lru_shmem` 程式在同一瞬間讀取的結果：

```
[Q7] fileLRU(3460624) == Cached(2933704) - Shmem(99484) + Buffers(626404) = 3460624   ✅ 完全相等
```

用開頭那份 `cat /proc/meminfo` 快照手算也一樣：

```
Active(file) 2,727,280 + Inactive(file) 716,924 = 3,444,204
Cached 2,917,288 − Shmem 99,476 + Buffers 626,392 = 3,444,204        ✅ 完全相等
```

順便驗證 `Cached` 自己的公式（`NR_FILE_PAGES` 從 `/proc/vmstat` 取）：

```
nr_file_pages 885,925 pages × 4 kB = 3,543,700 kB
3,543,700 − SwapCached 20 − Buffers 626,392 = 2,917,288 kB = Cached   ✅ 完全相等
```

```bash
# 一次抓齊三個節點自己驗算
ssh radxa@192.168.68.57 'grep -E "^(Cached|Buffers|Shmem|SwapCached|Active\(file\)|Inactive\(file\)):" /proc/meminfo;
                          grep -E "^nr_file_pages" /proc/vmstat'
```

---

<a name="q8"></a>
## 8. `/proc/PID/status` 中和記憶體相關的資訊各代表什麼？

### 結論

實作在 `fs/proc/task_mmu.c` 的 `task_mem()`，資料來源是 `mm_struct` 的各欄位與
`mm->rss_stat` 這四個計數器：

```c
/* include/linux/mm_types.h */
enum { MM_FILEPAGES, MM_ANONPAGES, MM_SWAPENTS, MM_SHMEMPAGES, NR_MM_COUNTERS };
```

| 欄位 | 來源 | 含義 |
|------|------|------|
| `VmPeak` | `mm->hiwater_vm` | 歷史最大**虛擬**位址空間 |
| `VmSize` | `mm->total_vm` | 目前**虛擬**位址空間大小（所有 VMA 長度總和，**不代表真的用了記憶體**） |
| `VmLck` | `mm->locked_vm` | 被 `mlock()` 鎖住、不可換出 |
| `VmPin` | `mm->pinned_vm` | 被 `pin_user_pages()` 釘住（DMA/GUP 用，連遷移都不行） |
| `VmHWM` | `mm->hiwater_rss` | 歷史最大**實體**駐留（high water mark） |
| `VmRSS` | = RssAnon+RssFile+RssShmem | 目前實體駐留 |
| `RssAnon` | `MM_ANONPAGES` | 匿名頁 |
| `RssFile` | `MM_FILEPAGES` | 檔案映射頁 |
| `RssShmem` | `MM_SHMEMPAGES` | shmem / tmpfs 頁 |
| `VmData` | `mm->data_vm` | 私有可寫資料段（heap、私有匿名映射） |
| `VmStk` | `mm->stack_vm` | 使用者堆疊 |
| `VmExe` | `mm->end_code − mm->start_code` | 程式碼段 |
| `VmLib` | `mm->exec_vm − VmExe` | 共享函式庫 |
| `VmPTE` | `mm->pgtables_bytes` | **本行程頁表佔用的實體記憶體** → 見 Ch7 Q12 |
| `VmSwap` | `MM_SWAPENTS` | 被換出到 swap 的**匿名**頁（**不含 shmem**）→ [Q9](#q9) |
| `HugetlbPages` | `mm->hugetlb_usage` | HugeTLB 用量 |

> **書目**：奔跑吧 §6.1.5「查看與行程相關的內存資訊」有完整逐項說明與 `mm_rss_stat` 定義。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'grep -E "^(Name|Pid|PPid|Threads|Vm|Rss|Hugetlb)" /proc/1/status'
```

PID 1（systemd）：

```
Name:   systemd        Pid: 1      PPid: 0     Threads: 1
VmPeak:   299548 kB    ← 曾經開到 292 MB 的虛擬空間（早期 mmap 過大量檔案）
VmSize:   168912 kB    ← 現在虛擬空間 165 MB
VmLck:         0 kB
VmPin:         0 kB
VmHWM:     12004 kB    ← 但實體最多只用過 11.7 MB
VmRSS:     11912 kB    ← 現在 11.6 MB   ← 虛擬 165MB vs 實體 11.6MB，差 14 倍！
RssAnon:    3568 kB
RssFile:    8344 kB
RssShmem:      0 kB    ← 3568 + 8344 + 0 = 11912 ✅ 等於 VmRSS
VmData:    19472 kB
VmStk:       132 kB
VmExe:        80 kB
VmLib:     15232 kB
VmPTE:       100 kB    ← 25 個頁表頁
VmSwap:        0 kB
```

**面試常考的三個坑**：

1. **`VmSize` 不是記憶體用量**。systemd 的 `VmSize=165MB` 但 `VmRSS=11.6MB`——
   虛擬位址空間是免費的（`MAP_NORESERVE`、未觸碰的 mmap、guard page），只有觸碰過才配實體頁。
2. **`VmRSS = RssAnon + RssFile + RssShmem`** 這個等式一定成立（本機 3568+8344+0=11912 ✅）。
3. **`VmRSS` 會重複計算共享頁**。父子行程 COW 共享的頁，兩邊的 RSS 都算，所以
   `Σ VmRSS > MemTotal` 是常態。要看「真實佔用」得用 `smaps` 的 `Pss`：
   ```bash
   ssh radxa@192.168.68.57 'sudo grep -E "^(Rss|Pss|Shared_Dirty|Private_Dirty):" /proc/1/smaps_rollup'
   ```

也可以看純數字版（`/proc/PID/statm`，單位是「頁」）：

```bash
ssh radxa@192.168.68.57 'cat /proc/1/statm'
# 42228 2978 2086 20 0 4901 0
#  size  rss shared text lib data dt      → 42228×4kB = 168912 kB = VmSize ✅
```

---

<a name="q9"></a>
## 9. 為什麼 S_swap（SwapTotal − SwapFree）≠ P_swap（Σ 各行程的 VmSwap）？

### 結論

**S_swap 統計的是「swap 分割區裡被佔用的 slot」，P_swap 統計的是「行程的 `MM_SWAPENTS` 計數」，
兩者的定義域根本不同。** 具體有五個來源：

| # | 原因 | 方向 |
|---|------|------|
| **1** | **shmem / tmpfs 被換出時不計入任何行程的 `VmSwap`** ← 最主要 | S_swap > P_swap |
| **2** | `SwapCached`：頁已換回記憶體但 swap slot 還沒釋放。此時 `try_to_unmap` 的效果已被 `do_swap_page()` 回復，`MM_SWAPENTS` 已減、但 slot 還在 | S_swap > P_swap |
| **3** | 行程已 exit、`mm` 已釋放，但 swap slot 仍被 swap cache / 其他共享者持有 | S_swap > P_swap |
| **4** | **共享的 swap entry 被重複計算**：fork 後父子共用同一個 swap entry，兩邊的 `VmSwap` 都算一次 | P_swap > S_swap |
| **5** | 掃描 `/proc/*/status` 需要時間，期間 swap 一直在變動（取樣競態） | 兩個方向都可能 |

其中 **第 1 點** 的機制：`try_to_unmap_one()` 裡只有 `PageAnon(page)` 為真才會做
`inc_mm_counter(mm, MM_SWAPENTS)`；shmem 的 `page->mapping` 指向 `inode->i_mapping`，
`PageAnon()` 為假，走的是 shmem 自己的 `shmem_writepage()` 路徑，把 swap entry 存進
`inode->i_mapping` 的 XArray，**完全不經過行程的 `mm_rss_stat`**。

> **書目**：奔跑吧 §6.1.6 專門一節「為什麼 S_swap 與 P_swap 不相等」，結論是
> 「在 `try_to_unmap_one()` 函數中，shmem 頁面並沒有被統計到行程的 `MM_SWAPENTS` 計數中，
> `/proc/PID/status` 節點中的 VmSwap 不包含被寫入交換分區的 shmem 頁面。」
> 另見 *Linux Memory Manager* `12_swap_memory.txt`。

### 實機驗證（一）：本機當下的差值

```bash
ssh radxa@192.168.68.57 'grep -E "SwapTotal|SwapFree" /proc/meminfo
  sudo awk "/^VmSwap:/{s+=\$2}END{print \"P_swap =\", s, \"kB\"}" /proc/[0-9]*/status'
```

```
SwapTotal:  4064756 kB
SwapFree:   4061172 kB
──────────────────────
S_swap    =    3584 kB
P_swap    =    2356 kB      ← 差 1228 kB
```

### 實機驗證（二）：用 `MADV_PAGEOUT` 直接證明 shmem 的差異

這是本題最有力的證據。程式 `/tmp/swap_shmem.c` 分配 256 MB，觸碰後用
`madvise(MADV_PAGEOUT)` **強制**把該區段換出（不需要製造全系統記憶體壓力），
然後同時讀 `/proc/self/status:VmSwap` 與 `/proc/meminfo:SwapFree`。

```bash
scp swap_shmem.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -o swap_shmem swap_shmem.c && ./swap_shmem anon; ./swap_shmem shmem'
```

**A) 傳統匿名頁（`MAP_PRIVATE|MAP_ANONYMOUS`）**

```
                        VmSwap      VmRSS   |  SwapFree     Shmem    AnonPages
before mmap                  0        744   |  4061172      99480     1373268
after touch                  0     263308   |  4061172      99480     1635416
after MADV_PAGEOUT      262144       1164   |  3799028      99484     1373200
                        ▲ +256MB              ▲ −256MB
>>> VmSwap = 262144 kB  (計入 P_swap)
```
⇒ **S_swap 增加 256 MB，P_swap 也增加 256 MB。兩邊同步。**

**B) shmem（`MAP_SHARED|MAP_ANONYMOUS`）**

```
                        VmSwap      VmRSS   |  SwapFree     Shmem    AnonPages
before mmap                  0        756   |  4060916      99484     1372972
after touch                  0     263320   |  4060916     361456     1372972
after MADV_PAGEOUT           0       1216   |  3798772      99484     1372980
                        ▲  不變               ▲ −256MB
>>> VmSwap = 0 kB       (完全不計入 P_swap)
```
⇒ **S_swap 增加 256 MB，但 P_swap 完全沒動。** 這 256 MB 就是 S_swap 與 P_swap 的差額來源。

**順帶證明了 [Q5](#q5)**：B 組 `after touch` 時 `Shmem` 從 99,484 → 361,456（+256MB），
而 `AnonPages` **完全沒動**。

### 補充：怎麼查「誰把 shmem 換出去了」

```bash
# shmem 換出的量（tmpfs 掛載點的 swap 使用）
ssh radxa@192.168.68.57 'cat /proc/meminfo | grep -E "Shmem|Swap"; df -h /dev/shm /run /tmp'
# zram 的實際壓縮狀況
ssh radxa@192.168.68.57 'cat /sys/block/zram0/mm_stat'
```

---

<a name="q10"></a>
## 10. 請簡述 `min_free_kbytes` 的含義和作用

### 結論

`min_free_kbytes` 決定**全系統最低警戒水位 `WMARK_MIN` 的總量**，也就是「**預留給高優先級
分配路徑的緊急備用金**」。普通優先級的分配（`GFP_KERNEL`、`GFP_USER`）碰到
`free < WMARK_MIN` 就必須進入直接回收甚至 OOM；只有帶
`__GFP_HIGH` / `__GFP_ATOMIC` / `__GFP_MEMALLOC`（或行程帶 `PF_MEMALLOC`，如 kswapd 自己）
的路徑才能動用這塊預留。

**為什麼需要它？** 回收記憶體這件事本身要花記憶體（配 `bio`、配 swap cache、配 skb）。
如果放任 free 掉到 0，回收路徑自己會配不到記憶體 → **死鎖**。`WMARK_MIN` 就是保證
「回收機制永遠有本錢啟動」的那筆錢。

**三條水位線**（`mm/page_alloc.c` `__setup_per_zone_wmarks()`）：

```
WMARK_HIGH  ── kswapd 回收到這裡就收工睡覺
WMARK_LOW   ── free 跌破 → 喚醒 kswapd 背景回收（不阻塞配置者）
WMARK_MIN   ── free 跌破 → 配置者自己下海做 direct reclaim（阻塞）
   ↓ 以下是預留區，只有 __GFP_HIGH/PF_MEMALLOC 能碰
```

**分攤公式**（`mm/page_alloc.c:8739 __setup_per_zone_wmarks()`）：

```c
unsigned long pages_min = min_free_kbytes >> (PAGE_SHIFT - 10);   /* kB → 頁 */
for_each_zone(zone) {
    tmp = (u64)pages_min * zone_managed_pages(zone);
    do_div(tmp, lowmem_pages);              /* 依各 zone 的 managed 比例分攤 */
    zone->_watermark[WMARK_MIN] = tmp;

    tmp = max_t(u64, tmp >> 2,              /* LOW/HIGH 的間距 */
                mult_frac(zone_managed_pages(zone), watermark_scale_factor, 10000));
    zone->_watermark[WMARK_LOW]  = min_wmark_pages(zone) + tmp;
    zone->_watermark[WMARK_HIGH] = low_wmark_pages(zone) + tmp;
}
```

**預設值**（`mm/page_alloc.c:8850 calculate_min_free_kbytes()`）：

```c
min_free_kbytes = clamp(int_sqrt(lowmem_kbytes * 16), 128, 262144);   /* 上限 256 MB */
```

即 `min_free_kbytes ≈ 4 × sqrt(lowmem_kbytes)`——**開根號**是刻意的：記憶體越大，
預留的**比例**越小（8 GB 機器預留約 0.14%，而不是線性放大）。

> **書目**：奔跑吧 §6.2.1「影響內存管理區水位的調優參數 min_free_kbytes」。
> 另見 *Linux Memory Manager* `11_reclaim_and_memory_pressure.txt`。

### 實機驗證（逐 zone 驗算）

```bash
ssh radxa@192.168.68.57 'cat /proc/sys/vm/min_free_kbytes /proc/sys/vm/watermark_scale_factor
  grep -E "^Node|pages free|boost|min |low |high |managed" /proc/zoneinfo'
```

```
min_free_kbytes        = 16384          → pages_min = 16384 >> 2 = 4096 頁
watermark_scale_factor = 10             → 間距 = managed × 10/10000 = 0.1%

Node 0, zone DMA                        Node 0, zone Normal
  pages free  604713                      pages free  9684
  boost       0                           boost       6463      ← 注意！見 Q13
  min         1932                        min         8626
  low         2891                        high        9699
  high        3850                        high       10772
  managed     959006                      managed  1073373
```

**驗算 ZONE_DMA**（boost = 0，可以直接對）：

```
WMARK_MIN  = 4096 × 959006 / 2032379 = 1932.7 → 1932       ✅ 完全吻合
間距 tmp   = max(1932>>2, 959006×10/10000) = max(483, 959) = 959
WMARK_LOW  = 1932 + 959 = 2891                              ✅ 完全吻合
WMARK_HIGH = 2891 + 959 = 3850                              ✅ 完全吻合
```

**驗算 ZONE_Normal**（顯示值含 boost，要先扣掉 6463）：

```
真實 WMARK_MIN  = 4096 × 1073373 / 2032379 = 2163.2 → 2163
間距 tmp        = max(2163>>2, 1073373×10/10000) = max(540, 1073) = 1073
真實 WMARK_LOW  = 2163 + 1073 = 3236
真實 WMARK_HIGH = 3236 + 1073 = 4309

顯示的 min  = 2163 + boost 6463 = 8626    ✅ 完全吻合
顯示的 low  = 3236 + boost 6463 = 9699    ✅ 完全吻合
顯示的 high = 4309 + boost 6463 = 10772   ✅ 完全吻合
```

**兩個 zone 的真實 WMARK_MIN 總和 = 1932 + 2163 = 4095 頁 ≈ 4096 = `min_free_kbytes >> 2`** ✅
（差 1 頁是兩次整數除法的截斷誤差）

> **這裡藏了一個非常容易踩的坑**：`/proc/zoneinfo` 印的 `min/low/high` 是
> `min_wmark_pages(zone)` 巨集，而
> ```c
> /* include/linux/mmzone.h:568 */
> #define min_wmark_pages(z) (z->_watermark[WMARK_MIN] + z->watermark_boost)
> ```
> **顯示值 = 真實水位 + watermark_boost**。如果直接拿 `/proc/zoneinfo` 的 min 去除
> `min_free_kbytes`，你會發現對不起來（本機 (1932+8626)×4 = 42232 kB ≠ 16384 kB），
> 然後懷疑人生。詳見 [Q13](#q13)。

> **本機的 `min_free_kbytes=16384` 從哪來？** 用公式算 `int_sqrt(8129516×16) = 11404`，
> 不是 16384。`/etc/sysctl.d/` 底下也沒有設定。16384 = 16 MiB 是個整數，
> 推測是 Radxa/Armbian 的 image 在 initramfs 或開機腳本裡設的
> （寫入時會走 `min_free_kbytes_sysctl_handler()` → `setup_per_zone_wmarks()` 重算水位）。

---

<a name="q11"></a>
## 11. 請簡述 `lowmem_reserve_ratio` 的含義和作用

### 結論

**保護低端 zone 不被「本來可以用高端 zone」的分配請求耗光。**

每個 zone 都有一個 `lowmem_reserve[]` 陣列（`/proc/zoneinfo` 印成 `protection:`），
`zone->lowmem_reserve[j]` 的意思是：

> 「當一個**原本可以落在 zone j**（j > 本 zone）的分配請求，退而求其次想從**本 zone** 拿頁時，
> 本 zone 必須額外保留 `lowmem_reserve[j]` 頁不給它。」

檢查點在 `zone_watermark_ok()`（`mm/page_alloc.c`）：

```c
long min = mark;                                   /* WMARK_LOW/MIN */
free_pages -= (1 << order) - 1;
if (free_pages <= min + z->lowmem_reserve[highest_zoneidx])
        return false;                              /* 不准配 */
```

**為什麼需要？** 低端 zone 是**稀缺資源**：ZONE_DMA 的頁能滿足所有請求（含 32-bit DMA 裝置），
而 ZONE_NORMAL 的頁只能滿足普通請求。如果讓 `GFP_HIGHUSER`（可以用任何 zone）
把 ZONE_DMA 吃光，之後真正需要低位址的 DMA 配置就會失敗。
在 RK3588 上這尤其現實——本機 **ZONE_DMA 涵蓋 0–4 GiB，占了一半以上的記憶體**
（`start_pfn: 512`, `spanned 1048064`），是 GMAC/USB/PCIe 等有 32/34-bit 定址限制的
控制器唯一能用的區域。

**計算公式**（`mm/page_alloc.c` `setup_per_zone_lowmem_reserve()`）：

```c
for (i = 0; i < MAX_NR_ZONES - 1; i++) {
    struct zone *zone = &pgdat->node_zones[i];
    int ratio = sysctl_lowmem_reserve_ratio[i];
    unsigned long managed_pages = 0;
    for (j = i + 1; j < MAX_NR_ZONES; j++) {
        managed_pages += zone_managed_pages(&pgdat->node_zones[j]);
        zone->lowmem_reserve[j] = managed_pages / ratio;   /* 注意是「除以」ratio */
    }
}
```

**ratio 是分母**，所以**數值越大 → 保留越少**。`256` 代表「保留高端 zone 總量的 1/256 ≈ 0.39%」。
設成 `0` 或 `1` 以下代表**完全不保留**（陣列清零）。

> **書目**：奔跑吧 §6.2.2「影響頁面分配的參數 lowmem_reserve_ratio」，
> 書中以 x86_64 為例說明「內存管理區的空閒頁面必須要大於 30484KB 才能滿足分配請求」，
> 並指出最終由 `setup_per_zone_lowmem_reserve()` 實現。

### 實機驗證（驗算完全吻合）

```bash
ssh radxa@192.168.68.57 'cat /proc/sys/vm/lowmem_reserve_ratio
  grep -E "^Node|protection|managed" /proc/zoneinfo'
```

```
lowmem_reserve_ratio = 256   256   32   0
                       ↑DMA  ↑DMA32 ↑Normal ↑Movable

Node 0, zone DMA      protection: (0, 0, 4192, 4192)     managed  959006
Node 0, zone DMA32    protection: (0, 0, 0, 0)           managed       0
Node 0, zone Normal   protection: (0, 0, 0, 0)           managed 1073373
Node 0, zone Movable  protection: (0, 0, 0, 0)           managed       0
```

**驗算 ZONE_DMA 的 `lowmem_reserve[ZONE_NORMAL]`**（zone index：DMA=0, DMA32=1, NORMAL=2, MOVABLE=3）：

```
managed(DMA32) + managed(NORMAL) = 0 + 1,073,373 頁
1,073,373 / 256 = 4192.86 → 4192                             ✅ 完全吻合 protection[2]
lowmem_reserve[MOVABLE] = (0 + 1,073,373 + 0) / 256 = 4192   ✅ 完全吻合 protection[3]
```

**實際效果**：一個 `GFP_HIGHUSER_MOVABLE` 的請求（`highest_zoneidx = ZONE_MOVABLE`）
想從 ZONE_DMA 配頁時，門檻是

```
WMARK_LOW(2891) + lowmem_reserve[3](4192) = 7083 頁 ≈ 27.7 MB
```

而同一個請求若指定 `GFP_DMA`（`highest_zoneidx = ZONE_DMA`），門檻只有
`WMARK_LOW + lowmem_reserve[0] = 2891 + 0 = 2891 頁`。**同一個 zone，兩種門檻**——
這就是 lowmem_reserve 的全部精髓。

**ZONE_Normal 的 protection 全是 0**，因為它已經是最高的有頁 zone，上面沒有更高端的 zone 可退讓。

---

<a name="q12"></a>
## 12. 請簡述 `zone_reclaim_mode` 的含義和作用

### 結論（本機的實驗結果很有意思）

> ### ⚠️ **這台 RK3588 上根本沒有 `/proc/sys/vm/zone_reclaim_mode` 這個節點。**

```bash
ssh radxa@192.168.68.57 'cat /proc/sys/vm/zone_reclaim_mode'
# cat: /proc/sys/vm/zone_reclaim_mode: No such file or directory

ssh radxa@192.168.68.57 'ls /proc/sys/vm/ | tr "\n" " "'
# admin_reserve_kbytes compaction_proactiveness compact_memory ... lowmem_reserve_ratio
# max_map_count min_free_kbytes ... watermark_boost_factor watermark_scale_factor
#                                     ↑ 完全沒有 zone_reclaim_mode
```

**原因**：`zone_reclaim_mode` 是 **NUMA 專屬**參數，其 sysctl 表項包在 `#ifdef CONFIG_NUMA` 裡
（`kernel/sysctl.c` / `mm/vmscan.c` 的 `node_reclaim_mode`）。RK3588 是單晶片 UMA SoC：

```bash
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep -E "CONFIG_NUMA"; ls /sys/devices/system/node/'
# # CONFIG_NUMA is not set
# ls: cannot access '/sys/devices/system/node/': No such file or directory
```

`/proc/zoneinfo` 也只有 `Node 0` 一個節點。所以這題在本平台上的正確答案是
**「不適用——本機沒有 NUMA，這個旋鈕不存在」**，這本身就是很好的面試回答。

### 參數本身的含義（在 NUMA 機器上）

當**本地 node** 的記憶體不足時，核心有兩個選擇：
（a）**回收本地 node** 的頁面（保住 NUMA locality，但要付回收代價）；
（b）**跨 node 去遠端配置**（配得快，但之後每次存取都是遠端延遲）。
`zone_reclaim_mode`（新名 `node_reclaim_mode`）就是在選 (a) 還是 (b)。

| 值 | 含義 |
|----|------|
| `0` | **關閉**（現代預設）。直接去別的 node 配，不做本地回收 |
| `1` | 開啟本地 node 回收 |
| `2` | 回收時**允許寫回髒頁** |
| `4` | 回收時**允許換出（swap）** |

（bit mask，可疊加，如 `3 = 1|2`。）

**為什麼現代預設是 0？** 早期核心預設 1，結果在資料庫 / 檔案伺服器上造成惡名昭彰的
「明明還有幾十 GB free，卻瘋狂回收 page cache 甚至 swap」的病態行為——
因為本地 node 一緊張就先殺自己的快取。Linux 3.16（commit `4f9b16a6`）改成預設 0。
**只有當跨 node 存取代價遠大於回收代價時**（例如某些 HPC 的 in-memory workload，
測得的 NUMA distance > 30）才建議開啟。

> **書目**：奔跑吧 §6.2.3「影響頁面回收的參數」。

### 本平台的替代品

RK3588 沒有 NUMA，但有**兩個 zone**（DMA 0–4G / Normal 4–12G），
跨 zone 的取捨改由 [`lowmem_reserve_ratio`](#q11) 和 [`watermark_*`](#q13) 負責。
另外 RK3588 是 big.LITTLE（4×A76 + 4×A55），**排程上的非對稱性**由 EAS/`sched_domain`
處理，跟記憶體的 NUMA 是兩回事，別在面試時混為一談。

---

<a name="q13"></a>
## 13. 請簡述 `watermark_boost_factor` 的含義和作用

### 結論

**這是「外部碎片化（external fragmentation）的緊急煞車」**，Linux 5.0 引入（commit `1c30844d2dfe`）。

**觸發時機**：當伙伴系統被迫做 **fallback**——某個 migratetype 的頁塊用完了，
要去「偷」另一個 migratetype 的整個 pageblock 時（`steal_suitable_fallback()` →
`boost_watermark()`），代表系統開始碎片化了。

**做什麼**：**臨時把該 zone 的所有水位往上抬** `watermark_boost` 頁，
於是 kswapd 被喚醒、並且會一路回收到「被抬高的 HIGH 水位」，**同時觸發 kcompactd 做規整**。
目的是**主動製造出成塊的空閒頁**，避免下次又要偷別人的 pageblock。

```c
/* mm/page_alloc.c:2705 */
static inline bool boost_watermark(struct zone *zone)
{
    unsigned long max_boost;
    if (!watermark_boost_factor)
        return false;                       /* 設 0 = 完全關閉這個機制 */
    if ((pageblock_nr_pages * 4) > zone_managed_pages(zone))
        return false;                       /* 太小的 zone 不玩，會直接 OOM */

    max_boost = mult_frac(zone->_watermark[WMARK_HIGH],
                          watermark_boost_factor, 10000);
    max_boost = max(pageblock_nr_pages, max_boost);
    zone->watermark_boost = min(zone->watermark_boost + pageblock_nr_pages, max_boost);
    ...
}
```

- **單位是萬分之一**：`15000`（預設）= HIGH 水位的 **150%**
- 每次 fallback 增加 `pageblock_nr_pages`（ARM64 4K 頁 = **512 頁 = 2 MB**），上限 `max_boost`
- **會自然衰減**：kswapd 每跑完一輪就 `zone->watermark_boost = 0`（`balance_pgdat()`），
  所以這是個「脈衝式」的暫時提高，不是永久的
- **設為 0 = 停用**。嵌入式小記憶體裝置常這麼做，因為抬高水位等於「憑空少了一塊可用記憶體」，
  在 512 MB 的板子上可能直接把系統推進 OOM

> **書目**：奔跑吧 §6.2.3「影響頁面回收的參數」，同節也說明了
> `watermark_scale_factor`（「警戒水位與低水位的差距是總內存的 0.1%，最大可設為 1000，
> 即兩個水位之間的差距最大為總內存的 10%」）。

### 實機驗證（本機的 boost 正在生效，而且已經飽和！）

```bash
ssh radxa@192.168.68.57 'cat /proc/sys/vm/watermark_boost_factor
  grep -E "^Node|boost|min |low |high |managed" /proc/zoneinfo'
```

```
watermark_boost_factor = 15000

Node 0, zone DMA                 Node 0, zone Normal
  boost    0                       boost    6463      ← 非零！正在 boost 中
  min      1932                    min      8626
  low      2891                    low      9699
  high     3850                    high    10772
  managed  959006                  managed 1073373
```

**驗算**（用 [Q10](#q10) 算出的 ZONE_Normal 真實 `WMARK_HIGH = 4309`）：

```
max_boost = mult_frac(WMARK_HIGH, watermark_boost_factor, 10000)
          = 4309 × 15000 / 10000
          = 6463.5 → 6463                    ✅ 與 /proc/zoneinfo 的 boost 完全相等
```

⇒ **本機 ZONE_Normal 的 watermark_boost 已經頂到 `max_boost` 上限**，
代表這個 zone 正在經歷**持續而嚴重的 fallback**。

**旁證**（`/proc/zoneinfo` + `/proc/pagetypeinfo`）：

```
Node 0, zone Normal
  pages free   9684        ← 只比 low(9699) 低一點點，kswapd 正被壓著跑
  nr_free_pages 9684

# sudo cat /proc/pagetypeinfo （Number of blocks type）
Node 0, zone      DMA    Unmovable 108   Movable 1571   Reclaimable 112   CMA 128
Node 0, zone   Normal    Unmovable 151   Movable 1933   Reclaimable  92   CMA   0

# Normal zone 的高階空閒頁塊（order 8/9/10）全是 0：
Node 0, zone Normal, type Movable   7  2  1  1  0  1  0 64  0  0  0
                                                          ↑order7 ↑order8~10 全空
```

ZONE_Normal 已經**完全湊不出 order-8 以上的連續頁**，這正是 watermark boost
被打到上限的直接原因。這台機器 uptime 1 天多、跑了大量 I/O（`nr_dirtied 4401014`），
page cache 把 Normal zone 撐滿並碎片化了。

**boost 造成的實際代價**：

```
ZONE_Normal 因為 boost 多凍結了 6463 頁 = 25.8 MB
kswapd 的回收目標從 free≥4309(17MB) 被抬高到 free≥10772(43MB)
```

> **面試加分點**：能講出「`/proc/zoneinfo` 顯示的 min/low/high 是
> `_watermark[] + watermark_boost`（`include/linux/mmzone.h:568`），
> 所以看到 min×zone 加總對不上 `min_free_kbytes` 時，先去看 boost 欄位」——
> 這是實際除錯時最常卡住的地方，本機就是活生生的例子。

---

<a name="q14"></a>
## 14. 影響髒頁回寫的參數有哪些？含義和作用分別是什麼？

### 結論

髒頁回寫由 `mm/page-writeback.c` 控制，兩組「量的門檻」+ 一組「時間的門檻」：

#### A. 量的門檻——背景回寫（不阻塞應用程式）

| 參數 | 本機值 | 含義 |
|------|--------|------|
| `dirty_background_ratio` | **1**（%） | 髒頁佔「可髒化記憶體」的比例超過此值 → **喚醒 writeback 執行緒**在背景回寫。應用程式的 `write()` **不受阻**。預設 10 |
| `dirty_background_bytes` | 0 | 同上但用絕對位元組。**與 ratio 互斥**：設了其中一個，另一個自動變 0 |

#### B. 量的門檻——強制回寫（**阻塞**應用程式）

| 參數 | 本機值 | 含義 |
|------|--------|------|
| `dirty_ratio` | **50**（%） | 髒頁比例達此值 → **`write()` 系統呼叫被阻塞**（`balance_dirty_pages()` 把呼叫者拖去做回寫）直到降下來。預設 20 |
| `dirty_bytes` | 0 | 同上但用絕對位元組，與 ratio 互斥 |

> 「可髒化記憶體」不是 MemTotal，而是 `free + file LRU − 各 zone 的保留`
> （`global_dirtyable_memory()`），所以髒頁上限會隨 page cache 大小浮動。

#### C. 時間的門檻

| 參數 | 本機值 | 含義 |
|------|--------|------|
| `dirty_writeback_centisecs` | **500** | writeback 執行緒**週期性醒來**的間隔，單位 1/100 秒 → 本機 **5 秒**。設 0 = 完全停用週期性回寫 |
| `dirty_expire_centisecs` | **3000** | 髒資料的**過期時間** → 本機 **30 秒**。執行緒醒來時，優先回寫「髒了超過 30 秒」的資料 |
| `dirtytime_expire_seconds` | **43200** | 只更新 atime 的 inode（lazytime 掛載）多久後才寫回 → 12 小時 |

#### D. 相關但常被一起問的

| 參數 | 本機值 | 含義 |
|------|--------|------|
| `drop_caches` | — | 手動釋放：`1`=page cache、`2`=可回收 slab（dentry/inode）、`3`=兩者。**只釋放乾淨的，先 `sync` 才有效** |
| `vfs_cache_pressure` | **500** | 回收 dentry/inode 快取的積極度。100=預設；本機 500 表示**5 倍積極**地回收 metadata 快取 |
| `swappiness` | **100** | 回收時匿名頁 vs 檔案頁的權重。本機 100（zram 場景的典型設定，換出到壓縮 RAM 很便宜） |
| `laptop_mode` | 0 | 非 0 時盡量把 I/O 攢在一起，讓硬碟多睡覺 |

> **書目**：奔跑吧 §6.2.4「影響髒頁回寫的參數」有完整逐項說明，包含
> 「`dirty_background_ratio` 是內存可以產生髒頁的百分比…而 `dirty_ratio` 的語義是髒頁的限制…
> 新的 I/O 請求（write 系統調用）將會被阻塞…這是造成 I/O 延遲的重要原因」。
> 另見 *Linux Memory Manager* `10_writeback.txt`。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'for f in dirty_background_ratio dirty_background_bytes dirty_ratio dirty_bytes \
   dirty_writeback_centisecs dirty_expire_centisecs dirtytime_expire_seconds swappiness vfs_cache_pressure; do
   printf "%-28s = %s\n" $f "$(cat /proc/sys/vm/$f)"; done'
```

```
dirty_background_ratio       = 1        ← 只要 1% 就開始背景回寫（很積極）
dirty_background_bytes       = 0
dirty_ratio                  = 50       ← 到 50% 才阻塞（很寬鬆）
dirty_bytes                  = 0
dirty_writeback_centisecs    = 500      ← 每 5 秒
dirty_expire_centisecs       = 3000     ← 髒了 30 秒就該寫
dirtytime_expire_seconds     = 43200
swappiness                   = 100
vfs_cache_pressure           = 500
```

**核心實際換算出來的門檻**（`/proc/vmstat`，單位是頁）：

```bash
ssh radxa@192.168.68.57 'grep -E "nr_dirty|nr_writeback" /proc/vmstat; grep -E "^Dirty|^Writeback" /proc/meminfo'
```

```
nr_dirty                        138      ← 目前只有 138 頁髒（0.5 MB）
nr_writeback                      0
nr_dirty_threshold           728053      ← 阻塞門檻：728053 頁 = 2.78 GB
nr_dirty_background_threshold 14219      ← 背景門檻:  14219 頁 = 55.5 MB
```

**驗算**（可髒化記憶體 ≈ `NR_FREE_PAGES + file LRU`）：

```
free 613,837 + fileLRU 870,318 ≈ 1,484,155 頁
× dirty_ratio 50%            = 742,077  ≈ nr_dirty_threshold 728,053      ✅ 量級吻合
× dirty_background_ratio 1%  =  14,841  ≈ nr_dirty_background_threshold 14,219  ✅ 量級吻合
```
（差幾個百分點是因為 `global_dirtyable_memory()` 還要扣掉各 zone 的
`totalreserve_pages`，且兩個節點不是同一瞬間取樣。）

**Radxa 這組設定的用意**（面試可以延伸的一段）：
`dirty_background_ratio=1` 搭 `dirty_ratio=50` 是**典型的 SD 卡 / eMMC 嵌入式調法**——
- **背景門檻壓到 1%**：讓髒資料「涓涓細流」持續寫出去，避免一次爆量寫入把慢速的
  eMMC/SD 佇列塞爆造成系統卡頓；
- **阻塞門檻抬到 50%**：給突發寫入（例如 `apt install`、編譯）留很大的緩衝，
  盡量不要真的去阻塞 `write()`。

**實測回寫行為**（安全，不改任何設定）：

```bash
# 觀察髒頁的產生與衰減（每 0.5 秒取樣，同時在背景寫檔）
ssh radxa@192.168.68.57 'dd if=/dev/zero of=/tmp/dirty.bin bs=1M count=200 2>/dev/null &
  for i in $(seq 12); do grep -E "^(Dirty|Writeback):" /proc/meminfo | tr "\n" " "; echo; sleep 0.5; done
  rm -f /tmp/dirty.bin'
```

累計統計（開機至今）：

```
nr_dirtied  4,401,014 頁   ← 累計弄髒了 16.8 GB
nr_written  4,121,267 頁   ← 累計寫出了 15.7 GB
```

---

## 附錄：本次用到的實驗程式

三支程式都放在裝置的 `/tmp/`，原始碼在
`/tmp/claude-1000/.../scratchpad/exp/`：

| 檔案 | 用途 | 對應題目 |
|------|------|---------|
| `lru_shmem.c` | 分配 512MB shmem / anon，比對 meminfo 各欄位與恆等式 | Q5, Q6, Q7 |
| `swap_shmem.c` | `MADV_PAGEOUT` 強制換出，比對 `VmSwap` 與 `SwapFree` | Q9 |

重跑方式：

```bash
scp lru_shmem.c swap_shmem.c radxa@192.168.68.57:/tmp/
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -o lru_shmem lru_shmem.c && ./lru_shmem'
ssh radxa@192.168.68.57 'cd /tmp && gcc -O0 -o swap_shmem swap_shmem.c && ./swap_shmem anon && ./swap_shmem shmem'
```
