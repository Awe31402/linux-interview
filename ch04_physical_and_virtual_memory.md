# 第 4 章 物理內存與虛擬內存 — 高頻面試題解答（46 題）

> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588），`192.168.68.57`
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`，8 GiB
> **關鍵組態**：`CONFIG_SLUB=y`、`CONFIG_SLAB_MERGE_DEFAULT=y`、`CONFIG_SLUB_CPU_PARTIAL=y`、
> `MAX_ORDER=11`、`pageblock_order=9`、`ARCH_KMALLOC_MINALIGN=128`、
> `CONFIG_ARM64_HW_AFDBM=y`、`# CONFIG_TRANSPARENT_HUGEPAGE is not set`
>
> **書目對照**
> - 《奔跑吧 Linux 內核》（第二版）卷 1 第 4 章 —
>   `books/running-linux-kernel/running-kernel-1-txt/12_第4章_物理内存与虚拟内存.txt`（下稱「奔跑吧 §4.x」）
> - *Linux Memory Manager* — `books/memory-manager/chapters/{02_physical_memory,04_process_memory,06_page_faults}.txt`
> - 核心程式碼路徑相對於本專案樹
>
> ## ⚠️ 本次實測發現的三項與書本不同之處
>
> | # | 書本／常見說法 | **本機實測** |
> |---|---------------|-------------|
> | [Q8-17](#q9) | 以經典 **SLAB** 描述（着色區、三種佈局、array_cache） | 本機是 **SLUB**：**沒有**着色、**沒有**三種佈局，freelist 寄生在空閒物件內 |
> | [Q8](#q8) | kmalloc 有 8/16/32/64 等小尺寸 | ARM64 上 `ARCH_KMALLOC_MINALIGN=128`，**最小的 cache 就是 `kmalloc-128`**，`kmalloc(8)` 實際吃 128 bytes |
> | [Q22](#q22) | VMA 用**紅黑樹 `mm_rb` + 鏈表** | 6.1 已改用 **maple tree `mm->mm_mt`**，紅黑樹與 `mm->mmap` 鏈表都被移除 |

---

## 目錄

| # | 題目 | 證據 | # | 題目 | 證據 |
|---|------|------|---|------|------|
| [1](#q1) | 伙伴系統如何分配連續頁 | 模組 | [24](#q24) | malloc 是否馬上配實體頁 | ★實測 |
| [2](#q2) | gfp_mask → zone | ★模組 | [25](#q25) | malloc 100 bytes 配多少 | 實測 |
| [3](#q3) | zone 掃描方向 | ★模組 | [26](#q26) | bufA/bufB 位址相同是否衝突 | — |
| [4](#q4) | GFP_KERNEL vs GFP_HIGHUSER_MOVABLE | 模組 | [27](#q27) | vm_normal_page() | 模組 |
| [5](#q5) | 中斷上下文能用 GFP_KERNEL 嗎 | 原始碼 | [28](#q28) | get_user_pages() | 原始碼 |
| [6](#q6) | 如何判斷 zone 滿足分配 | ★模組 | [29](#q29) | follow_page() | 原始碼 |
| [7](#q7) | 釋放時如何合併 | ★模組 | [30](#q30) | SYSCALL_DEFINE1 展開 | 原始碼 |
| [8](#q8) | 2ⁿ 分配的缺點 / slab | ★實測 | [31](#q31) | 使用者空間劃分 / brk | ★實測 |
| [9](#q9) | slab 如何分配小記憶體 | 模組 | [32](#q32) | 私有映射 vs 共享映射 | ★實測 |
| [10](#q10) | cache colouring | 說明 | [33](#q33) | MAP_FIXED 為何不報錯 | ★實測 |
| [11](#q11) | 空閒物件太多怎麼辦 | 指令 | [34](#q34) | ARM64 如何知道錯誤位址 | ★實測 |
| [12](#q12) | 對象緩衝池 | sysfs | [35](#q35) | 如何知道是讀還是寫 | ★實測 |
| [13](#q13) | slab 佔幾頁 / 幾個物件 | ★sysfs | [36](#q36) | 可修復 vs 不可修復 | ★實測 |
| [14](#q14) | 三種 slab 佈局 | 說明 | [37](#q37) | do_page_fault 要考慮什麼 | 原始碼 |
| [15](#q15) | 何時給 slab 配實體頁 | 原始碼 | [38](#q38) | major vs minor fault | ★實測 |
| [16](#q16) | freelist 如何管理空閒物件 | 說明 | [39](#q39) | 匿名頁缺頁判斷條件 | ★實測 |
| [17](#q17) | 多 CPU 並發性能 | sysfs | [40](#q40) | 檔案映射缺頁判斷條件 | ★實測 |
| [18](#q18) | kmalloc/vmalloc/malloc | ★模組 | [41](#q41) | 什麼是 COW 缺頁 | ★實測 |
| [19](#q19) | 如何管理使用者位址空間 | 實測 | [42](#q42) | COW：複用 vs 複製 | ★實測 |
| [20](#q20) | vm_flags → 硬體屬性 | ★實測 | [43](#q43) | DBM 與軟體的競爭 | 原始碼 |
| [21](#q21) | 如何保證位址不衝突 | 實測 | [44](#q44) | pte_offset_map 何時安全 | 原始碼 |
| [22](#q22) | 快速查詢/插入 VMA | ★原始碼 | [45](#q45) | 切換 PTE 前為何要刷 TLB | 原始碼 |
| [23](#q23) | find_vma 的查找條件 | 原始碼 | [46](#q46) | 多 CPU 同時缺頁 | 原始碼 |

**實驗程式**：`experiments/mm_probe.c`（核心模組）、`fault_types.c`、`esr_far.c`、
`mmap_vma.c`、`slub_info.sh`

---

## 實驗工具

```bash
# 核心模組（伙伴系統/gfp/zonelist/水位/SLUB/kmalloc-vmalloc）
ssh radxa@192.168.68.57 'mkdir -p ~/exp/armv8'
scp notes/experiments/mm_probe.c radxa@192.168.68.57:~/exp/armv8/
scp notes/experiments/Makefile.mod radxa@192.168.68.57:~/exp/armv8/Makefile
ssh radxa@192.168.68.57 'cd ~/exp/armv8 && make
  sudo dmesg -C; sudo insmod mm_probe.ko
  sudo dmesg | sed "s/^\[[^]]*\] //;s/^mm_probe: //"; sudo rmmod mm_probe'

# 使用者態
scp notes/experiments/{fault_types,esr_far,mmap_vma}.c notes/experiments/slub_info.sh \
    radxa@192.168.68.57:~/exp/
ssh radxa@192.168.68.57 'cd ~/exp
  for s in fault_types esr_far mmap_vma; do gcc -O2 -w -o $s $s.c; done
  ./fault_types; ./esr_far; ./mmap_vma; sudo ./slub_info.sh'
```

---

<a name="q1"></a>
## 1. 理想情況下頁面分配器如何分配出連續物理頁面？

### 結論

**伙伴系統（buddy system）**：每個 zone 把空閒頁按 2ⁿ 連續頁分成
`MAX_ORDER` 個等級，每級再按 migratetype 分成數條鏈表：

```c
struct zone {
    struct free_area free_area[MAX_ORDER];      /* MAX_ORDER = 11，order 0..10 */
};
struct free_area {
    struct list_head free_list[MIGRATE_TYPES];  /* 6 種遷移類型 */
    unsigned long    nr_free;
};
```

**分配 2^order 頁的流程**（`rmqueue()` → `__rmqueue_smallest()`，`mm/page_alloc.c`）：

1. 到 `free_area[order].free_list[mt]` 找，有就直接摘下；
2. 沒有 → 到 `order+1` 找，找到就**分裂（expand）**：一半給請求者，
   另一半（伙伴）掛回 `order` 級鏈表；
3. 逐級往上，直到 `MAX_ORDER-1`；
4. 該 migratetype 全部落空 → **fallback 到別的 migratetype**（見 [Ch5 Q46](./ch05_memory_management_advanced_topics.md#q46)）。

> **書目**：奔跑吧 §4.1「頁面分配之快速路徑」、§4.1.6「rmqueue() 函數」。

### 實機驗證

```bash
sudo insmod mm_probe.ko && sudo dmesg | grep -A4 "伙伴系統的分裂與合併"
```

```
MAX_ORDER=11  -> 最大一塊 = 2^10 頁 = 4096 KB
pageblock_order=9 -> 一個 pageblock = 512 頁 = 2 MB
```

模組配了一塊 **order-8（1 MB）**，觀察 `free_area` 的變化：

```
---- 配置前 ----  zone "DMA" free=491854 頁
     order:        0     1     2     3     4     5     6     7     8     9    10
     Unmovable     7    61    30     4     4     2     1     2     2     0     1
                                                             ↑ order-8 有 2 塊

  ---- 剛配了一塊 order-8 (1024 KB) ----
     PFN = 0x6b600  PA = 0x6b600000  zone="DMA"
     PFN 是否 order-8 對齊？ 0x6b600 % 256 = 0  是 ✔

---- 配置後 ----  zone "DMA" free=491598 頁          （491854 − 256 = 491598 ✅）
     Unmovable     7    61    30     4     4     2     1     2     1     0     1
                                                             ↑ 變成 1 塊
```

**order-8 鏈表從 2 塊變 1 塊、free 剛好減少 2⁸=256 頁**——
這次直接命中，不需要分裂。分配到的 PFN `0x6b600` **必然是 256 頁對齊**的，
這是伙伴系統的不變量。

---

<a name="q2"></a>
## 2. 如何從 gfp_mask 確定可以從哪些 zone 分配？

### 結論

`gfp_mask` 的四個 zone 修飾位（`__GFP_DMA`/`__GFP_DMA32`/`__GFP_HIGHMEM`/`__GFP_MOVABLE`）
經 **`gfp_zone()`** 查一張編譯期常數表 `GFP_ZONE_TABLE`（`include/linux/gfp.h`），
換算成一個 **`enum zone_type` 上限**，然後 `first_zones_zonelist()`
從 zonelist 中找到第一個 `zone_idx <= 上限` 的 zone 作為掃描起點。

**關鍵觀念**：`gfp_zone()` 給的是「**最高能碰到哪個 zone**」，不是「一定用這個 zone」。

> **書目**：奔跑吧 §4.1.2「分配掩碼」、§4.1.4「get_page_from_freelist() 函數」。

### 實機驗證（模組直接印出 `gfp_zone()` 的結果）

```bash
sudo dmesg | grep -A15 "gfp_mask -> 最高允許 zone"
```

```
gfp_mask                     gfp_zone()   zone 名稱      遷移類型
GFP_KERNEL                   2            ZONE_NORMAL   Unmovable
GFP_ATOMIC                   2            ZONE_NORMAL   Unmovable
GFP_NOWAIT                   2            ZONE_NORMAL   Unmovable
GFP_NOIO                     2            ZONE_NORMAL   Unmovable
GFP_NOFS                     2            ZONE_NORMAL   Unmovable
GFP_USER                     2            ZONE_NORMAL   Unmovable
GFP_HIGHUSER                 2            ZONE_NORMAL   Unmovable
GFP_HIGHUSER_MOVABLE         3            ZONE_MOVABLE  Movable      ← 唯一能碰 MOVABLE 的
GFP_DMA                      0            ZONE_DMA      Unmovable
GFP_DMA32                    1            ZONE_DMA32    Unmovable
GFP_KERNEL|__GFP_MOVABLE     2            ZONE_NORMAL   Movable
```

**兩個要點**：
1. **`GFP_HIGHUSER` 在 64 位系統上等同 `GFP_USER`**（都到 ZONE_NORMAL），
   因為 64 位沒有 highmem；差別只在 32 位。
2. **`__GFP_MOVABLE` 同時影響兩件事**：把 zone 上限抬到 `ZONE_MOVABLE`，
   以及把 **migratetype 設成 `MIGRATE_MOVABLE`**（後者才是本機真正有意義的效果，
   因為本機 `ZONE_MOVABLE` 是空的）。

---

<a name="q3"></a>
## 3. 頁面分配器按什麼方向掃描 zone？

### 結論

**從高 zone_idx 往低 zone_idx**，即
`ZONE_MOVABLE → ZONE_HIGHMEM → ZONE_NORMAL → ZONE_DMA32 → ZONE_DMA`。

由 `build_zonelists()`（`mm/page_alloc.c`）在開機時建好，存在
`pgdat->node_zonelists[ZONELIST_FALLBACK]`。

**為什麼？** 因為**低端 zone 是稀缺資源**：ZONE_DMA 的頁能滿足所有請求，
但 ZONE_NORMAL 的頁滿足不了 32-bit DMA 裝置。先用高端、把低端留到最後，
才不會讓普通分配把 DMA 專用區吃光。

> **書目**：奔跑吧 §4.1.4「get_page_from_freelist() 函數」。

### 實機驗證

```bash
sudo dmesg | grep -A6 "zonelist 的掃描順序"
```

```
node 0 的 ZONELIST_FALLBACK（回退列表）：
 [0] zone_idx=2  "Normal "  managed=1073373    free=9684
 [1] zone_idx=0  "DMA    "  managed=959006     free=491854
```

**`zone_idx` 由大到小排（2 → 0）** ✅
本機沒有 DMA32/Movable（present=0），所以 zonelist 只有兩項。

**實際效果**：`GFP_KERNEL` 的分配會**先試 Normal**；Normal 只有 9,684 頁空閒
（低於 low watermark 9,699！），所以會 fallback 到 DMA（491,854 頁空閒）。
這正是為什麼本機 [Ch6 Q13](./ch06_memory_management_case_studies.md#q13)
看到 Normal zone 的 watermark boost 被打到上限——**Normal 已經長期吃緊，
大部分分配其實落在 DMA zone**。

---

<a name="q4"></a>
## 4. 為使用者行程分配物理內存，該用 GFP_KERNEL 還是 GFP_HIGHUSER_MOVABLE？

### 結論

**`GFP_HIGHUSER_MOVABLE`**。原因不在「能不能用 highmem」（64 位沒有 highmem），
而在 **`__GFP_MOVABLE` 決定了頁面的 migratetype**：

| | `GFP_KERNEL` | `GFP_HIGHUSER_MOVABLE` |
|---|---|---|
| zone 上限 | ZONE_NORMAL | ZONE_MOVABLE |
| **migratetype** | **`MIGRATE_UNMOVABLE`** | **`MIGRATE_MOVABLE`** |
| 落在哪種 pageblock | 不可遷移區 | 可遷移區 |
| 能參與規整/遷移嗎 | **不能** | **能** |
| 能參與 CMA 嗎 | 不能 | 能 |

**使用者態頁面天生就是可遷移的**（有 rmap 可以找到所有 PTE、可以改映射），
把它們放進 `MIGRATE_MOVABLE` 的 pageblock，才能讓
[Ch5 Q28](./ch05_memory_management_advanced_topics.md#q28) 的**記憶體規整**
把它們搬走、湊出大塊連續記憶體。反過來，如果用 `GFP_KERNEL` 配使用者頁，
這些頁會散落在不可遷移區裡**把整個 pageblock 釘死**，是碎片化的元兇。

核心實際使用的路徑（`mm/memory.c`）：
```c
/* do_anonymous_page() */
folio = vma_alloc_zeroed_movable_folio(vma, vmf->address);   /* 帶 __GFP_MOVABLE */
```

> **書目**：奔跑吧 §4.1.2「分配掩碼」；配合 §5.9「內存碎片化管理」理解 migratetype 的意義。

### 實機驗證

```
GFP_KERNEL                   -> ZONE_NORMAL   遷移類型 Unmovable
GFP_HIGHUSER_MOVABLE         -> ZONE_MOVABLE  遷移類型 Movable      ★
```

**遷移的實證**：`migrate_compact` 實驗中，使用者的 32 MB 匿名記憶體有
**284 頁被規整搬動**（PFN 從 `0x26c9` 搬到 `0x7b331`），
證明使用者匿名頁確實落在可遷移區。詳見
[Ch5 Q23](./ch05_memory_management_advanced_topics.md#q23)。

---

<a name="q5"></a>
## 5. 中斷上下文能不能用 GFP_KERNEL？

### 結論

**絕對不能。**

```c
/* include/linux/gfp_types.h */
#define GFP_KERNEL  (__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define __GFP_RECLAIM  (__GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM)
```

`__GFP_DIRECT_RECLAIM` 表示「記憶體不夠時，**呼叫者自己下海回收**」——
這條路徑會掃 LRU、可能等磁碟 I/O、可能 `congestion_wait()`，**都會睡眠**。
而中斷上下文（硬中斷、softirq、持 spinlock、`preempt_disable()`）**禁止睡眠**。

核心自己會抓：`__alloc_pages()` 裡有
```c
might_alloc(gfp);        /* -> might_sleep_if(gfp_has_io_fs(gfp)) */
```
開了 `CONFIG_DEBUG_ATOMIC_SLEEP` 就會噴
`BUG: sleeping function called from invalid context`。

**該用什麼**：

| 遮罩 | 會睡眠？ | 能動用預留？ | 用途 |
|------|---------|-------------|------|
| `GFP_ATOMIC` | ✗ | ✓（`ALLOC_HIGH`，見 [Ch5 Q43](./ch05_memory_management_advanced_topics.md#q43)） | 中斷處理、持鎖 |
| `GFP_NOWAIT` | ✗ | ✗ | 失敗也無所謂的場合 |
| `GFP_NOIO` | ✓ | — | 區塊層自己（不能再發 I/O） |
| `GFP_NOFS` | ✓ | — | 檔案系統自己（不能再進 fs） |

> **書目**：奔跑吧 §4.1.2「分配掩碼」。

### 實機驗證

```bash
sudo dmesg | grep -E "GFP_ATOMIC|GFP_NOWAIT|GFP_NOIO|GFP_NOFS"
```

```
GFP_ATOMIC     -> ZONE_NORMAL   Unmovable
GFP_NOWAIT     -> ZONE_NORMAL   Unmovable
GFP_NOIO       -> ZONE_NORMAL   Unmovable
GFP_NOFS       -> ZONE_NORMAL   Unmovable
```

zone 上限都一樣，**差別完全在「回收行為」的位元上，不在 zone 上**。

---

<a name="q6"></a>
## 6. 如何判斷一個 zone 是否滿足分配需求？

### 結論

`__zone_watermark_ok()`（`mm/page_alloc.c`）——**兩個條件都要成立**：

```c
bool __zone_watermark_ok(struct zone *z, unsigned int order, unsigned long mark,
                         int highest_zoneidx, unsigned int alloc_flags, long free_pages)
{
    long min = mark;
    const bool alloc_harder = (alloc_flags & (ALLOC_HARDER|ALLOC_OOM));

    free_pages -= __zone_watermark_unusable_free(z, order, alloc_flags);  /* 扣掉 CMA/highatomic */

    if (alloc_flags & ALLOC_HIGH)   min -= min / 2;      /* 門檻砍半 */
    if (alloc_harder) {
        if (alloc_flags & ALLOC_OOM) min -= min / 2;     /* 再砍半 */
        else                         min -= min / 4;     /* 砍 1/4 */
    }

    /* 條件 (a)：總量夠嗎？（還要扣掉 lowmem_reserve） */
    if (free_pages <= min + z->lowmem_reserve[highest_zoneidx])
        return false;
    if (!order) return true;

    /* 條件 (b)：order>0 時，該階（或更高階）真的有塊嗎？ */
    for (o = order; o < MAX_ORDER; o++) {
        struct free_area *area = &z->free_area[o];
        if (!area->nr_free) continue;
        for (mt = 0; mt < MIGRATE_PCPTYPES; mt++)
            if (!free_area_empty(area, mt)) return true;
        ...
    }
    return false;                                        /* 只有外碎片，不算滿足 */
}
```

**條件 (b) 是關鍵**：總空閒頁再多，如果全是零散的 order-0，
order-8 的請求依然失敗——這就是**外碎片化**。

> **書目**：奔跑吧 §4.1.5「zone_watermark_fast() 函數」。

### 實機驗證

```bash
sudo dmesg | grep -A9 "zone 水位與是否滿足分配"
```

```
zone "DMA"  free=491854
   _watermark[MIN] =1932    +boost=0      -> 顯示值 1932
   _watermark[LOW] =2891                  -> 顯示值 2891
   _watermark[HIGH]=3850                  -> 顯示值 3850
   lowmem_reserve[] = 0 0 4192 4192
   nr_reserved_highatomic = 0 頁
   各階是否有空閒塊：o0=8 o1=69 o2=229 o3=109 o4=91 o5=118 o6=76 o7=43 o8=11 o9=3 o10=459

zone "Normal"  free=9684
   _watermark[MIN] =2163    +boost=6463   -> 顯示值 8626
   _watermark[LOW] =3236                  -> 顯示值 9699
   _watermark[HIGH]=4309                  -> 顯示值 10772
   lowmem_reserve[] = 0 0 0 0
   各階是否有空閒塊：o0=8 o1=2 o2=12 o3=95 o4=24 o5=3 o6=3 o7=64 o8=0 o9=0 o10=0
                                                            ↑ order 8/9/10 全是 0
```

**這張表把兩個條件都演出來了**：

| 請求 | 條件 (a) 總量 | 條件 (b) 有塊 | 結果 |
|------|--------------|--------------|------|
| Normal zone，order-0 | free 9684 vs low 9699 → **差 15 頁不過** | — | **不通過** → 喚醒 kswapd |
| Normal zone，order-8 | 不過 | `o8=0` **也不過** | **不通過** |
| DMA zone，order-8 | 491854 ≫ 2891 ✓ | `o8=11` ✓ | **通過** |

而且 `boost=6463` 把 Normal 的 min 從 2163 抬到 8626——**這 6463 頁是「被 boost 凍結」的**，
詳見 [Ch6 Q13](./ch06_memory_management_case_studies.md#q13)。

---

<a name="q7"></a>
## 7. 釋放頁面時如何合併空閒頁面？

### 結論

`__free_one_page()`（`mm/page_alloc.c`）用**異或找伙伴**：

```c
buddy_pfn = pfn ^ (1 << order);      /* 只翻轉第 order 位 */
```

合併條件（`page_is_buddy()`）**三個都要成立**：
1. 伙伴也是**空閒**的（`PageBuddy()`）
2. 伙伴的 **order 相同**（`buddy_order(buddy) == order`）
3. **同一個 zone**（且合併後不跨 pageblock 邊界時 migratetype 才不會混）

滿足就：把伙伴從鏈表摘除 → 兩塊合成一塊 `order+1`（起始 PFN 取較小者）→
`order++` → 重複，直到不能再合或到 `MAX_ORDER-1`。

**為什麼用 XOR？** 因為伙伴系統保證「order-n 的塊起始 PFN 一定是 2ⁿ 對齊」，
所以同一對伙伴的 PFN **只差第 n 位**。一條 XOR 指令 O(1) 找到，這是整個演算法的精髓。

> **書目**：奔跑吧 §4.1.7「釋放頁面」。

### 實機驗證

模組配了 order-8 的塊（PFN `0x6b600`）之後印出：

```
PFN = 0x6b600
PFN 是否 order-8 對齊？ 0x6b600 % 256 = 0  是 ✔
伙伴 PFN = pfn XOR (1<<8) = 0x6b700          ← 只差第 8 位
```

驗算：`0x6b600 = 0b0110_1011_0110_0000_0000`，第 8 位是 0；
XOR `0x100` 後第 8 位變 1 → `0x6b700` ✅

**釋放後的合併結果**：

```
---- 配置後 ---- Unmovable  o8=1   free=491598
---- 已釋放 ----
---- 釋放後 ---- Unmovable  o8=2   free=491854      ← 完全回到原狀 ✅
```

order-8 鏈表從 1 塊回到 2 塊，free 從 491,598 回到 491,854（+256）。
（這次伙伴 `0x6b700` 不空閒，所以停在 order-8 沒有繼續往上合併。）

---

<a name="q8"></a>
## 8. 早期以 2ⁿ 位元組分配的缺點？slab 如何克服？

### 結論

**純 2ⁿ 分配的三大缺點**：
1. **內部碎片巨大**：申請 65 bytes 給 128 bytes，浪費 49%
2. **不認識物件類型**：每次都得從裸記憶體重新初始化 `task_struct`/`inode` 這種複雜物件
3. **沒有 per-CPU 快取**，多核下鎖競爭嚴重

**slab/slub 的四項改進**：
1. **按物件實際大小建專屬 cache**（`kmem_cache`），不用湊到 2ⁿ
2. **釋放的物件保留在 freelist**，下次直接複用，省掉建構/解構
3. **per-CPU 快取**（[Q12](#q12)/[Q17](#q17)）
4. 通用請求退化成一組**比 2ⁿ 精細的尺寸階梯**（`kmalloc-96`、`kmalloc-192`…）

> **書目**：奔跑吧 §4.2.1「slab 分配器產生的背景」。

### 實機驗證（★ ARM64 上有個大坑）

```bash
sudo dmesg | grep -A12 "kmalloc 的尺寸階梯"
```

```
kmalloc(    8) -> ksize()=128     內部碎片  120 bytes (93%)
kmalloc(   16) -> ksize()=128     內部碎片  112 bytes (87%)
kmalloc(   24) -> ksize()=128     內部碎片  104 bytes (81%)
kmalloc(   32) -> ksize()=128     內部碎片   96 bytes (75%)
kmalloc(   65) -> ksize()=128     內部碎片   63 bytes (49%)
kmalloc(  100) -> ksize()=128     內部碎片   28 bytes (21%)
kmalloc(  192) -> ksize()=256     內部碎片   64 bytes (25%)
kmalloc(  200) -> ksize()=256     內部碎片   56 bytes (21%)
kmalloc( 1000) -> ksize()=1024    內部碎片   24 bytes ( 2%)
kmalloc( 4000) -> ksize()=4096    內部碎片   96 bytes ( 2%)
kmalloc( 8000) -> ksize()=8192    內部碎片  192 bytes ( 2%)
```

```bash
ssh radxa@192.168.68.57 'ls /sys/kernel/slab/ | grep -E "^kmalloc-[0-9]" | sort -t- -k2 -n'
```

```
kmalloc-128 kmalloc-256 kmalloc-512 kmalloc-1k kmalloc-2k kmalloc-4k kmalloc-8k
```

> ### ⚠️ **本機沒有 `kmalloc-8/16/32/64/96/192`——最小的就是 `kmalloc-128`！**
>
> 原因在 ARM64 的 DMA 對齊要求：
> ```c
> /* arch/arm64/include/asm/cache.h:26 */
> #define ARCH_DMA_MINALIGN  (128)
> /* include/linux/slab.h:228 */
> #define ARCH_KMALLOC_MINALIGN  ARCH_DMA_MINALIGN
> ```
> 因為 `kmalloc()` 回傳的記憶體**必須能直接拿去做 DMA**，
> 而 ARM64 的 cache line / DMA 對齊要求是 128 bytes（見
> [Ch2 Q13](./ch02_arm64_in_linux_kernel.md#q13) 的 `CTR_EL0.CWG=4`），
> 所以所有小於 128 的 kmalloc cache 都被砍掉了。
>
> **`kmalloc(8)` 在這台機器上實際吃掉 128 bytes，浪費 93%。**
> 這是嵌入式 ARM64 上很實際的一個坑——大量小物件請用專屬 `kmem_cache` 而不是 `kmalloc`。

---

<a name="q9"></a>
## 9. slab 分配器如何分配和釋放小記憶體塊？

### 結論

> ### ⚠️ 本機是 **SLUB** 不是經典 SLAB
> ```bash
> ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep -E "^CONFIG_SL(UB|AB|OB)"'
> # CONFIG_SLUB=y   CONFIG_SLUB_CPU_PARTIAL=y   CONFIG_SLUB_DEBUG=y
> ```
> 書中 Q9~Q17 是以**經典 SLAB** 描述的。下面先講書上的 SLAB，再對照 SLUB 的差異。

**經典 SLAB 的分配（`kmem_cache_alloc()` → `slab_alloc()`）三層**：
1. **per-CPU `array_cache`**：最近釋放的物件指標陣列，命中就直接彈出，無鎖
2. per-CPU 空了 → 從 `kmem_cache_node` 的 `slabs_partial`/`slabs_free` **批次搬** `batchcount` 個上來
3. 都滿了 → 向伙伴系統要 `1 << gfporder` 頁，切成新 slab

釋放相反：優先放回 per-CPU；滿了就批次歸還。

**SLUB 的差異**（`mm/slub.c`）：

| | 經典 SLAB | **SLUB（本機）** |
|---|---|---|
| per-CPU 結構 | `array_cache`（物件指標陣列） | `kmem_cache_cpu`（**單一 freelist 指標 + tid**） |
| freelist 存哪 | slab 頭部的獨立索引陣列 | **寄生在空閒物件自己的記憶體裡**（`c->offset` 處放下一個空閒物件的指標） |
| 快速路徑 | 關中斷 + 陣列彈出 | **`this_cpu_cmpxchg_double(freelist, tid)` 完全無鎖** |
| 部分空閒 slab | node 級 `slabs_partial` | **另有 per-CPU partial 鏈表**（`CONFIG_SLUB_CPU_PARTIAL`） |
| 着色 | 有 | **沒有** |

> **書目**：奔跑吧 §4.2.5「分配 slab 對象」、§4.2.6「釋放 slab 緩存對象」。

### 實機驗證

```bash
sudo dmesg | grep -A6 "kmem_cache_create"
```

```
kmem_cache_create("mm_probe_test", object_size=100) 連配兩個物件：
   obj1 = 0xffff0000564f5680   ksize=104
   obj2 = 0xffff0000564f5478   兩者相距 520 bytes
   同一個 slab 頁嗎？ 是 ✔   PFN=0x564f5 PageSlab=1
   slab 頁的 _refcount=1  在 LRU 上嗎=0
```

**兩個觀察**：
1. `object_size=100` → `ksize()=104`（對齊到 8）
2. **兩個相鄰配置的物件相距 520 bytes = 5 × 104**，**不是相鄰槽位**——
   因為 SLUB 的 freelist 是一條「散落在 slab 內」的鏈，
   釋放順序決定了下次分配的順序，**不保證位址遞增**。
   這正是 SLUB 與 SLAB「索引陣列」實作的可見差異。

---

<a name="q10"></a>
## 10. slab 的高速緩存着色（cache colouring）有什麼作用？

### 結論

**讓不同 slab 中「相同槽位」的物件，落在 CPU cache 的不同 set，避免衝突失效。**

問題：若每個 slab 都從頁首開始擺物件，那麼「所有 slab 的第 0 個物件」的位址
低位元完全相同 → **映射到 cache 的同一個 set** → 這些物件互相踢對方出 cache
（conflict miss），即使 cache 總容量還很空。

做法：每個新 slab 在物件陣列前面留一小段**着色區**，長度是
`colour_off × cache_line_size`，`colour_off` 在 `0 ~ colour-1` 之間循環遞增。
這樣不同 slab 的「第 0 個物件」位址錯開若干個 cache line，分散到不同 set。

代價：着色區本身不能放物件（用的是 slab 內原本就剩下的零頭，所以幾乎免費）。

> **書目**：奔跑吧 §4.2.3「slab 分配器的內存布局」。

### 實機驗證（本機沒有這個機制）

> ### ⚠️ **SLUB 沒有 cache colouring。**
>
> 這是 SLUB 相對 SLAB 的簡化之一。SLUB 的作者 Christoph Lameter 認為：
> 現代 CPU 的 cache 關聯度（associativity）已經夠高
> （本機 A76 L1d 是 4-way、L2 8-way、L3 16-way），
> colouring 帶來的效益不足以抵銷實作複雜度。

```bash
# SLAB 才有的 colour 欄位，SLUB 的 sysfs 完全沒有
ssh radxa@192.168.68.57 'sudo ls /sys/kernel/slab/dentry/ | tr "\n" " "'
```

```
aliases align cache_dma cpu_partial cpu_slabs ctor destroy_by_rcu hwcache_align
min_partial objects object_size objects_partial objs_per_slab order partial
poison reclaim_account red_zone sanity_checks shrink slabs slabs_cpu_partial
slab_size store_user total_objects trace usersize validate
                                        ↑ 沒有任何 colour 相關的節點
```

**回答這題的正確姿勢**：說明 SLAB colouring 的原理與目的，
然後指出「本機用的 SLUB 已經拿掉了這個機制」，並說明原因。

---

<a name="q11"></a>
## 11. slab 增長導致大量空閒物件，如何解決？

### 結論

**兩條路徑**：

1. **主動的定期回收**
   - 經典 SLAB：`cache_reap()` 由 workqueue 週期性喚醒，
     收縮 per-CPU array_cache、把 `slabs_free` 上待太久的 slab 還給伙伴系統
   - **SLUB**：沒有 `cache_reap()`。改成「`kmem_cache_shrink()` 按需呼叫」，
     以及 per-CPU partial 鏈表超過 `cpu_partial` 就把多的 slab 推回 node

2. **被動的記憶體壓力回收**：`shrink_slab()` 走 shrinker 回呼
   （dentry/inode 的 `scan_objects`），由 kswapd / direct reclaim 觸發。
   只有帶 `SLAB_RECLAIM_ACCOUNT` 的 cache 才有 shrinker
   （見 [Ch6 Q4](./ch06_memory_management_case_studies.md#q4)）

> **書目**：奔跑吧 §4.2.10「小結」；配合 §5.3「頁面回收」。

### 實機驗證

**(a) 手動觸發單一 cache 的收縮**（SLUB 的 `shrink` 節點）：

```bash
ssh radxa@192.168.68.57 'sudo sh -c "
  echo \"收縮前: slabs=$(cat /sys/kernel/slab/dentry/slabs) objects=$(cat /sys/kernel/slab/dentry/objects)\"
  echo 1 > /sys/kernel/slab/dentry/shrink
  echo \"收縮後: slabs=$(cat /sys/kernel/slab/dentry/slabs) objects=$(cat /sys/kernel/slab/dentry/objects)\""'
```

**(b) 觸發全系統的 shrinker**：

```bash
ssh radxa@192.168.68.57 'grep -E "^(Slab|SReclaimable|SUnreclaim):" /proc/meminfo
  sudo sh -c "sync; echo 2 > /proc/sys/vm/drop_caches"
  grep -E "^(Slab|SReclaimable|SUnreclaim):" /proc/meminfo'
```

本機當前 slab 用量（`sudo cat /proc/slabinfo`）：

```
ext4_inode_cache  132206 132216   1504      ← ~190 MB，有 shrinker
dentry            241130 241240    216      ← ~50  MB，有 shrinker
buffer_head       492576 492576    128      ← ~60  MB
task_struct          645    672   3968      ← 沒有 shrinker，回收不掉
```

`SReclaimable = 430,776 kB` 幾乎全來自前三者——**這就是「可以被收縮」的部分**。

---

<a name="q12"></a>
## 12. 什麼是對象緩衝池？

### 結論

**per-CPU 的物件快取層**，夾在「全域共享的 slab 結構」與「實際的分配請求」之間：

```
   分配請求
      │
      ▼
 ┌──────────────────────┐
 │ per-CPU 緩衝池        │  ← 99% 的請求在這裡完成，【無鎖】
 │ SLAB: array_cache     │
 │ SLUB: kmem_cache_cpu  │     freelist + tid + page + partial
 └──────────┬───────────┘
            │ 空了 / 滿了才往下（低頻）
            ▼
 ┌──────────────────────┐
 │ kmem_cache_node       │  ← 需要 list_lock
 │  partial / full slab  │
 └──────────┬───────────┘
            │ 都沒有才往下（更低頻）
            ▼
      伙伴系統 alloc_pages()
```

**價值**：
1. 消除多核搶同一把鎖造成的 **cache line bouncing**
2. 剛釋放的物件**還熱在該 CPU 的 L1/L2 裡**，馬上重用命中率高

> **書目**：奔跑吧 §4.2.2「創建 slab 描述符」（表 4.9 array_cache 的成員）。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'sudo sh -c "
for d in dentry kmalloc-128 task_struct; do
  printf \"%-14s cpu_slabs=%-6s slabs_cpu_partial=%-20s cpu_partial=%s\n\" \$d \
    \"\$(cat /sys/kernel/slab/\$d/cpu_slabs)\" \
    \"\$(cat /sys/kernel/slab/\$d/slabs_cpu_partial)\" \
    \"\$(cat /sys/kernel/slab/\$d/cpu_partial)\"; done"'
```

`cpu_slabs` 會列出**每顆 CPU 各自持有幾個 slab**——這就是 per-CPU 緩衝池的實體。

**per-CPU 上限**（`cpu_partial`，見 [Q17](#q17)）：

```
kmalloc-128      cpu_partial=120     ← 小物件，多快取一點
dentry           cpu_partial=120
vm_area_struct   cpu_partial=120
kmalloc-1k       cpu_partial=24      ← 大物件，少快取（省記憶體）
task_struct      cpu_partial=24
```

---

<a name="q13"></a>
## 13. 建立 slab 描述符時，如何確定佔幾頁、幾個物件、幾個着色區？

### 結論

**SLUB 的 `calculate_order()`（`mm/slub.c`）**：

```
目標：找一個 order，讓  浪費率 = (slab_size − objects × size) / slab_size
      足夠小，同時 order 不要太大（避免向伙伴系統要大塊連續頁）

策略（由寬到嚴逐步嘗試）：
  1. min_objects 從 (4 × log2(nr_cpu_ids)) 起跳，逐步減半
  2. 對每個 min_objects，fraction 從 16 起跳（浪費 ≤ 1/16）逐步放寬
  3. order 從 slab_order(size, min_objects, slub_max_order, fraction) 取
  4. 都不行 → 退到「一個 slab 至少放 1 個物件」的最小 order
```

`objects = slab_size / size`，資訊打包在 `kmem_cache->oo`
（高 16 位是 order，低 16 位是 objects）。

**着色區**：SLUB **沒有**（見 [Q10](#q10)）。
經典 SLAB 的公式是 `colour = 剩餘空間 / cache_line_size`。

> **書目**：奔跑吧 §4.2.2「創建 slab 描述符」（`calculate_slab_order()`）。

### 實機驗證（★ 直接從 sysfs 讀出來驗算）

```bash
ssh radxa@192.168.68.57 'sudo ./slub_info.sh'
```

```
kmalloc-128    object_size=128    slab_size=128    order=0 objs_per_slab=32   cpu_partial=120  align=128  reclaim=0
kmalloc-1k     object_size=1024   slab_size=1024   order=3 objs_per_slab=32   cpu_partial=24   align=1024 reclaim=0
dentry         object_size=216    slab_size=216    order=1 objs_per_slab=37   cpu_partial=120  align=8    reclaim=1
task_struct    object_size=3968   slab_size=3968   order=3 objs_per_slab=8    cpu_partial=24   align=64   reclaim=0
vm_area_struct object_size=136    slab_size=136    order=0 objs_per_slab=30   cpu_partial=120  align=8    reclaim=0
anon_vma       object_size=120    slab_size=128    order=0 objs_per_slab=32   cpu_partial=120  align=8    reclaim=0
```

**逐項驗算**（`slab 總大小 = PAGE_SIZE << order`）：

| cache | order | slab 總大小 | slab_size(每物件) | objs | 用掉 | **浪費** |
|-------|-------|------------|------------------|------|------|---------|
| `kmalloc-128` | 0 | 4096 | 128 | 32 | 32×128 = **4096** | **0 (0%)** ✅ |
| `vm_area_struct` | 0 | 4096 | 136 | 30 | 30×136 = **4080** | 16 (0.4%) ✅ |
| `anon_vma` | 0 | 4096 | 128 | 32 | 32×128 = **4096** | 0 (0%) ✅ |
| `dentry` | 1 | 8192 | 216 | 37 | 37×216 = **7992** | 200 (2.4%) ✅ |
| `task_struct` | 3 | 32768 | 3968 | 8 | 8×3968 = **31744** | 1024 (3.1%) ✅ |
| `kmalloc-1k` | 3 | 32768 | 1024 | 32 | 32×1024 = **32768** | 0 (0%) ✅ |

**每一個都對得上，浪費率全部 ≤ 3.1%**——這就是 `calculate_order()` 的成果。

注意 `dentry` **不用 order-0**：216 bytes 在 4096 裡只能放 18 個，浪費 208 bytes(5%)；
用 order-1 放 37 個只浪費 200 bytes(2.4%)，所以選了 order-1。

`anon_vma` 的 `object_size=120` 但 `slab_size=128`——**對齊到 8 之後又被拉到 128**，
因為它跟 `kmalloc-128` 被 **merge** 了（`CONFIG_SLAB_MERGE_DEFAULT=y`）。

---

<a name="q14"></a>
## 14. slab 的三種佈局（正常 / OBJFREELIST_SLAB / OFF_SLAB）有什麼區別？

### 結論

**這三種佈局是經典 SLAB 的概念，本機的 SLUB 沒有。**

| 佈局 | freelist 放哪 | 適用 | 代價 |
|------|--------------|------|------|
| **正常模式** | slab 內部、物件陣列**之前**的一段獨立區域 | 物件不大的常見情況 | freelist 佔掉 slab 內的空間 |
| **OFF_SLAB** | **另外用 `kmalloc()` 配一塊**，與物件資料實體分離 | 物件很大（一個就快佔滿一頁），slab 內擠不下 freelist | 多一次間接存取；freelist 本身也要管理 |
| **OBJFREELIST_SLAB** | **寄生在「空閒物件自己的記憶體」裡** | 物件大小/對齊允許時的最優解 | 需要物件夠大能放下一個索引 |

判定邏輯在 `set_objfreelist_slab_cache()` / `set_off_slab_cache()` / `set_on_slab_cache()`
（`mm/slab.c`）。

**SLUB 的做法**：**永遠是 OBJFREELIST 的思路**——
`freelist` 就是一條把「空閒物件」串起來的單向鏈表，
指標直接寫在物件內部偏移 `s->offset` 的位置：

```c
/* mm/slub.c */
static inline void *get_freepointer(struct kmem_cache *s, void *object)
{
        return freelist_dereference(s, object + s->offset);
}
```

所以 SLUB **不需要**額外的 freelist 區域，也就不需要三種佈局。

> **書目**：奔跑吧 §4.2.3「slab 分配器的內存布局」。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep -E "^CONFIG_SL(UB|AB)\b"'
# CONFIG_SLUB=y                    ← 沒有 CONFIG_SLAB
```

[Q9](#q9) 的實驗間接證明了 SLUB 的 freelist 就在物件裡：
兩個連續配置的物件**相距 520 bytes（5 個槽位）而非相鄰**，
說明 freelist 的順序取決於釋放順序、散落在 slab 內部，
而不是像 SLAB 那樣有一個「頭部索引陣列」按順序發放。

---

<a name="q15"></a>
## 15. 什麼時候給 slab 分配器分配物理內存？

### 結論

**惰性（lazy）、按需觸發**——只有在下面這條路徑走到底時才向伙伴系統要頁：

```
kmem_cache_alloc()
  └─ per-CPU freelist 有嗎？ 有 → 直接回傳（快速路徑，無鎖）
       └─ 沒有 → per-CPU partial 有嗎？ 有 → 換一個 slab 上來
            └─ 沒有 → node 的 partial 鏈表有嗎？ 有 → 搬上來
                 └─ 沒有 → ★ new_slab() → allocate_slab()
                            → alloc_pages(gfp, oo_order(s->oo))   ← 這時才配實體頁
```

**建立 `kmem_cache` 時完全不配頁**——`kmem_cache_create()` 只是登記一份描述符。

釋放方向也是惰性的：一個 slab 全空了不會馬上還，
會先留在 `partial` 鏈表當緩衝（`min_partial` 控制留幾個），
超過才 `discard_slab()` → `__free_pages()`。

> **書目**：奔跑吧 §4.2.7「slab 分配器和伙伴系統的接口函數」。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'sudo sh -c "
  echo \"min_partial(留幾個全空 slab 不還) = \$(cat /sys/kernel/slab/dentry/min_partial)\"
  echo \"partial (node 上的部分空閒 slab)  = \$(cat /sys/kernel/slab/dentry/partial)\"
  echo \"slabs   (總 slab 數)              = \$(cat /sys/kernel/slab/dentry/slabs)\"
  echo \"objects (總物件數)                = \$(cat /sys/kernel/slab/dentry/objects)\""'
```

從 `/proc/slabinfo` 也看得到惰性的證據：

```
dentry            241130 241240    216   37    2 : ... : slabdata   6520   6520      0
                  ↑活躍  ↑總數                              ↑active  ↑total
```

`active_slabs == total_slabs == 6520`，`active_objs(241130) ≈ num_objs(241240)`——
**幾乎沒有全空的 slab 被留著**，因為 `min_partial` 很小、記憶體壓力也大。

---

<a name="q16"></a>
## 16. slab 管理區 freelist 如何管理空閒物件？

### 結論

**經典 SLAB**：freelist 是一個**索引陣列**（`freelist_idx_t`，通常 1~2 bytes），
用陣列下標模擬指標串成單向鏈表：

```
分配：idx = slab->free;  slab->free = freelist[idx];  return obj_at(idx);
釋放：freelist[idx] = slab->free;  slab->free = idx;          （頭插法）
```

用**下標而非指標**的好處：省空間（1 byte vs 8 bytes），
而且物件槽位可以被外部資料完全佔滿——只有空閒時才被 freelist 追蹤。

**SLUB（本機）**：freelist 是一條**真指標鏈**，指標寫在**空閒物件自己內部**
偏移 `s->offset` 的位置：

```c
/* mm/slub.c 快速路徑 */
object = c->freelist;
next_object = get_freepointer_safe(s, object);   /* *(void **)(object + s->offset) */
this_cpu_cmpxchg_double(s->cpu_slab->freelist, s->cpu_slab->tid,
                        object, tid, next_object, next_tid(tid));
```

**為什麼 SLUB 敢用真指標？** 因為物件空閒時它的內容本來就沒人在乎，
拿頭 8 bytes 存指標是「免費的」。代價是需要 `CONFIG_SLAB_FREELIST_HARDENED`
之類的保護（把指標與位址、cache 的隨機數 XOR）來擋 heap 攻擊。

> **書目**：奔跑吧 §4.2.8「管理區」。

### 實機驗證

[Q9](#q9) 的實驗結果就是最好的證明：

```
obj1 = 0xffff0000564f5680
obj2 = 0xffff0000564f5478   兩者相距 520 bytes（= 5 × 104）
```

**兩次連續 `kmem_cache_alloc()` 拿到的不是相鄰槽位，而且 obj2 位址比 obj1 小。**
這說明 freelist 是「按釋放順序串起來的鏈」，
**不是**經典 SLAB 那種「按索引順序發放」的陣列。

檢查 hardening 有沒有開：

```bash
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep -E "SLAB_FREELIST"'
```

---

<a name="q17"></a>
## 17. slab 如何保證多 CPU 大型機的並發存取性能？

### 結論

**三層設計**：

1. **per-CPU 快取，完全無鎖**
   SLUB 用 `this_cpu_cmpxchg_double()` 一次原子更新 `freelist` + `tid`
   （tid 是遞增的事務號，用來偵測「中途被搶佔到別的 CPU」）。
   **沒有 spinlock、沒有關中斷**，多核完全不互相干擾。

2. **per-CPU partial 鏈表**（`CONFIG_SLUB_CPU_PARTIAL=y`）
   當前 slab 用完時，先從自己 CPU 的 partial 鏈表換一個上來，
   還是不用碰 node 的共享結構。

3. **只有跨過前兩層才拿 `kmem_cache_node->list_lock`**
   而且是「批次搬運」——一次搬一整個 slab，把高頻的單物件操作
   和低頻的批次操作解耦。

> **書目**：奔跑吧 §4.2.10「小結」。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'nproc; sudo ./slub_info.sh | head -6'
```

```
8                                        ← 8 核（4×A55 + 4×A76）

kmalloc-128    ... cpu_partial=120  ← per-CPU 最多快取 120 個物件份的 partial slab
kmalloc-1k     ... cpu_partial=24
dentry         ... cpu_partial=120
task_struct    ... cpu_partial=24
vm_area_struct ... cpu_partial=120
```

**`cpu_partial` 按物件大小分級**（小物件 120、大物件 24），
是「快取深度」與「記憶體佔用」之間的權衡。

**SLUB 不用經典 SLAB 的調優參數**——`/proc/slabinfo` 的 tunables 欄全是 0：

```bash
ssh radxa@192.168.68.57 'sudo grep -E "^(dentry|task_struct) " /proc/slabinfo'
```

```
dentry       241130 241240 216 37 2 : tunables    0    0    0 : slabdata 6520 6520 0
task_struct     645    672 3968 8 8 : tunables    0    0    0 : slabdata   84   84 0
                                                  ↑ limit/batchcount/sharedfactor 全 0
```

**這三個 0 就是「本機跑的是 SLUB 不是 SLAB」的鐵證**——
那三個參數是經典 SLAB 的 `array_cache` 調優鈕，SLUB 根本不用。

---

<a name="q18"></a>
## 18. kmalloc()、vmalloc()、malloc() 的區別？

### 結論

| | `kmalloc()` | `vmalloc()` | `malloc()` |
|---|---|---|---|
| 層級 | 核心態 | 核心態 | 使用者態（glibc） |
| **實體連續** | **是** ✔ | **否** ✘ | 不保證也不在乎 |
| 虛擬連續 | 是 | 是 | 是 |
| 位在哪個區 | **線性映射區**（`PAGE_OFFSET` 之後） | **vmalloc 區**（`VMALLOC_START` 之後） | 行程使用者空間 |
| `__pa()` 可用？ | **可以**（簡單加減） | **不可以**（要 `vmalloc_to_page()`） | 不適用 |
| 建立頁表？ | 不用（線性映射早就建好） | **要**，逐頁建 PTE | 缺頁時才建 |
| 大小上限 | `KMALLOC_MAX_CACHE_SIZE`（本機 8 KB，更大走 `alloc_pages`） | 幾乎無上限 | 受 `overcommit` 限制 |
| 速度 | 快（per-CPU 命中近 O(1)） | 慢（配頁 + 建頁表 + TLB） | 小塊快（使用者態池）；大塊走 mmap |
| 用途 | DMA buffer、硬體描述符、絕大多數核心小物件 | 模組程式碼、大型核心結構 | 一般應用程式 |

> **書目**：奔跑吧 §4.3「vmalloc()」。

### 實機驗證（★ 直接逐頁比對實體位址）

```bash
sudo dmesg | grep -A22 "kmalloc vs vmalloc 的實體連續性"
```

**kmalloc(64KB)**：

```
kmalloc(65536)  VA=0xffff00006b410000
   在線性映射區嗎？ 是 ✔
      page[ 0] VA=0xffff00006b410000  PA=0x6b410000
      page[ 1] VA=0xffff00006b411000  PA=0x6b411000
      page[ 2] VA=0xffff00006b412000  PA=0x6b412000
      page[ 3] VA=0xffff00006b413000  PA=0x6b413000
      page[15] VA=0xffff00006b41f000  PA=0x6b41f000
   不連續的頁數 = 0  -> ★ 實體完全連續 ✔
```

**vmalloc(64KB)**：

```
vmalloc(65536)  VA=0xffff80000e61d000
   在 vmalloc 區嗎？ 是 ✔ (VMALLOC_START=0xffff800008000000)
   __pa() 對它有效嗎？ __pa(v)=0x6a1d000  <- 不可信！
   必須用 vmalloc_to_page() 逐頁查頁表：
      page[ 0] VA=0xffff80000e61d000  PA=0x4cb63000  PFN=0x4cb63
      page[ 1] VA=0xffff80000e61e000  PA=0x4cb62000  PFN=0x4cb62   ← PFN 遞減！
      page[ 2] VA=0xffff80000e61f000  PA=0x340cd000  PFN=0x340cd   ← 跳到別的地方
      page[ 3] VA=0xffff80000e620000  PA=0x340cc000  PFN=0x340cc
      page[15] VA=0xffff80000e62c000  PA=0x6144c000  PFN=0x6144c
   不連續的頁數 = 15 / 16  -> ★ 實體不連續，只有虛擬位址連續 ✔
```

**16 頁裡有 15 頁跟前一頁不連續**，PFN 從 `0x4cb63` 跳到 `0x340cd` 再跳到 `0x6144c`——
散落在整個實體記憶體。而虛擬位址 `0xffff80000e61d000 ~ 0xffff80000e62c000`
是完美連續的。

**`__pa(v) = 0x6a1d000` 是垃圾值**（真正的 page[0] PA 是 `0x4cb63000`），
因為 vmalloc 區不在線性映射裡——這是核心開發常見的 bug 來源
（見 [Ch2 Q10](./ch02_arm64_in_linux_kernel.md#q10)）。

**使用者態的 `malloc()`**：

```bash
ssh radxa@192.168.68.57 'sudo ./pagemap_walk | grep -A1 "heap"'
```

```
heap  (malloc)  VA=0x0000aaaae1f1b2a0  present=1  PFN=0x6df23  PA=0x0006df232a0
```

`malloc()` 給的是**使用者虛擬位址**，實體頁由缺頁異常按需配（見 [Q24](#q24)）。

---

<a name="q19"></a>
## 19. Linux 內核如何管理行程的使用者態位址空間？

### 結論

**`mm_struct` + 一堆 `vm_area_struct`（VMA）+ 一棵 maple tree**：

```c
struct mm_struct {
        struct maple_tree mm_mt;        /* 6.1：所有 VMA 的容器（取代紅黑樹）*/
        pgd_t *pgd;                     /* 頁表根，會寫進 TTBR0 */
        unsigned long mmap_base;        /* mmap 區起點 */
        unsigned long start_code, end_code, start_data, end_data;
        unsigned long start_brk, brk, start_stack;
        unsigned long total_vm, locked_vm, pinned_vm, data_vm, exec_vm, stack_vm;
        struct mm_rss_stat rss_stat;    /* 見 Ch6 Q8 */
        ...
};

struct vm_area_struct {
        unsigned long vm_start, vm_end;      /* [start, end) */
        unsigned long vm_flags;              /* VM_READ/WRITE/EXEC/SHARED... */
        pgprot_t vm_page_prot;               /* 轉好的硬體屬性，見 Q20 */
        struct file *vm_file;                /* 檔案映射才有 */
        unsigned long vm_pgoff;              /* 在檔案中的頁偏移 */
        const struct vm_operations_struct *vm_ops;
        struct anon_vma *anon_vma;           /* 反向映射，見 Ch5 Q7 */
        ...
};
```

**兩層分工**：
- **VMA 說「這段位址允許做什麼」**（有沒有映射、什麼權限、對應哪個檔案）
- **頁表說「這一頁現在實際在哪」**（可能還沒配、可能被換出）

兩者不同步是常態——這就是**按需分頁**（見 [Q24](#q24)）。

> **書目**：奔跑吧 §4.4「虛擬內存管理之進程地址空間」、§4.4.2「mm_struct 數據結構」、
> §4.4.3「VMA 數據結構」。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'cat /proc/self/maps'
```

```
aaaad0e30000-aaaad0e34000 r-xp 00000000 103:02 ...  /usr/bin/cat     ← .text  (VM_READ|VM_EXEC)
aaaad0e43000-aaaad0e44000 r--p 00003000 103:02 ...  /usr/bin/cat     ← .rodata
aaaad0e44000-aaaad0e45000 rw-p 00004000 103:02 ...  /usr/bin/cat     ← .data
aaaaf0f8e000-aaaaf0faf000 rw-p 00000000 00:00 0     [heap]           ← brk 區，見 Q31
ffff8b2e0000-ffff8b4ce000 r-xp 00000000 103:02 ...  libc.so.6
...
ffffd4a2e000-ffffd4a4f000 rw-p 00000000 00:00 0     [stack]
ffffd4b5b000-ffffd4b5f000 r--p 00000000 00:00 0     [vvar]
ffffd4b5f000-ffffd4b61000 r-xp 00000000 00:00 0     [vdso]
```

**每一行就是一個 VMA。** 用核心模組直接看內部欄位（`mm_convert.ko`，見
[Ch3 Q3](./ch03_memory_management_prerequisites.md#q3)）：

```
[1] mm + vaddr -> VMA : find_vma(mm, 0xffffbd89c000)
    vma = 0xffff00005be7b6e8  範圍 [0xffffbd89c000, 0xffffbd89f000)  大小 12 KB
    vm_flags=0x100073 (rw-p)  vm_pgoff=0xffffbd89c  vm_file=無（匿名映射）
    mm->pgd = 0xffff000055dcd000   __pa = 0x55dcd000  <- 會被寫進 TTBR0_EL1
```

---

<a name="q20"></a>
## 20. 行程位址空間的屬性如何轉換成硬體能識別的屬性？

### 結論

**三層轉換**：

```
   軟體語意                中間表                    硬體位元
 vm_flags            protection_map[16]           PTE 的 bits
 VM_READ    ──┐      ┌──────────────┐        PTE_USER / PTE_RDONLY
 VM_WRITE   ──┼─索引→ │ 16 個 pgprot_t │ ──────→ PTE_UXN / PTE_PXN
 VM_EXEC    ──┤      └──────────────┘        PTE_AF / PTE_SHARED
 VM_SHARED  ──┘        （架構相關）              AttrIndx
```

1. **`vm_get_page_prot(vm_flags)`**（`mm/mmap.c`）
   用 `vm_flags` 的低 4 位（R/W/X/S）當索引查 `protection_map[]`
2. **`protection_map[]`** 由各架構定義。ARM64 在 `arch/arm64/include/asm/pgtable-prot.h`：
   ```c
   #define PAGE_NONE         __pgprot(...PTE_RDONLY | PTE_NG | PTE_PXN | PTE_UXN)
   #define PAGE_SHARED       __pgprot(_PAGE_SHARED)          /* rw-s */
   #define PAGE_READONLY     __pgprot(_PAGE_READONLY)        /* r--p */
   #define PAGE_READONLY_EXEC __pgprot(_PAGE_READONLY_EXEC)  /* r-xp */
   ```
3. **`mk_pte(page, vma->vm_page_prot)`** 把 PFN 與屬性位組合成最終 PTE

**這一層抽象的價值**：`do_page_fault()`、`handle_mm_fault()` 這些核心邏輯
完全用架構無關的 `vm_flags` 寫，只有 `protection_map[]` 是架構相關的。

> **書目**：奔跑吧 §4.4.4「VMA 的屬性」。

### 實機驗證（★ 從 VMA 一路追到 PTE 的硬體位元）

用 `armv8_dump.ko` 巡覽 `insmod` 行程的程式碼段：

```
software page-table walk: insmod 行程的使用者空間程式碼段 (TTBR0)
  VA = 0x0000aaaad26a0000
   L3 desc = 0x00200001f1290fc3
        AttrIndx=0  NS=0  AP=3(EL1 RO, EL0 RO)  SH=3(Inner Shareable)
        AF=1  nG=1  DBM=0  Contig=0  PXN=1  UXN=0
```

對照 `/proc/<pid>/maps` 該段是 **`r-xp`**：

| VMA 的 `vm_flags` | → PTE 的硬體位元 | 意義 |
|------------------|-----------------|------|
| `VM_READ` ✓ | `AP=3` (EL0 RO) | 使用者可讀 |
| `VM_WRITE` ✗ | `AP=3` 是唯讀 | 使用者不可寫 |
| `VM_EXEC` ✓ | **`UXN=0`** | 使用者可執行 |
| — | **`PXN=1`** | **核心不可執行**（防止核心誤跳到使用者程式碼，PXN 是 SMEP 的 ARM 版） |
| `VM_SHARED` ✗（私有） | — | |
| 使用者頁 | **`nG=1`** | non-global，綁 ASID（見 [Ch2 Q14](./ch02_arm64_in_linux_kernel.md#q14)） |

**對照核心的 `.text`**（同一次巡覽）：

```
_stext:  AP=2(EL1 RO, EL0 none)  PXN=0  UXN=1  nG=0
         ↑ 使用者完全碰不到      ↑核心可執行 ↑使用者不可執行 ↑global
```

**`vm_flags` → `pgprot` → PTE 三層轉換的結果，在頁表裡一位一位看得見。**

---

<a name="q21"></a>
## 21. 行程位址空間是離散的，內核如何保證不衝突？

### 結論

**靠「每次建立映射前都做區間查找 + 衝突檢測」的統一路徑**，不是靠運氣：

| 情況 | 做法 |
|------|------|
| 沒指定位址 | `arch_get_unmapped_area[_topdown]()` 在 maple tree 上找**足夠大的空洞** |
| 指定位址但沒 `MAP_FIXED` | 先試該位址；有衝突就**當作沒指定**，另找一個 |
| 指定位址 + **`MAP_FIXED`** | **強制用**，先 `do_munmap()` 掉重疊部分（見 [Q33](#q33)） |
| 指定位址 + **`MAP_FIXED_NOREPLACE`** | 有重疊就 **`-EEXIST`**（Linux 4.17 加的安全版本） |

資料結構層面，maple tree 本身就是「管理一組**不重疊**區間」的結構，
插入重疊區間會被 `mas_store_gfp()` 擋下。

> **書目**：奔跑吧 §4.4.6「插入 VMA」、§4.4.7「合併 VMA」。

### 實機驗證

見 [Q33](#q33) 的 `mmap_vma` 實驗——`MAP_FIXED_NOREPLACE` 確實回傳 `EEXIST`：

```
[3] mmap(同一位址, 4KB, MAP_FIXED_NOREPLACE)
    -> 0xffffffffffffffff   errno=17 (File exists)   ★ 正確地偵測到重疊並回報 EEXIST
```

**VMA 合併**也是保證「不重疊且不碎裂」的一環：

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./mmap_vma | sed -n "/VMA 會被自動合併/,/^$/p"'
```

```
--- [Q21] 相鄰且屬性相同的 VMA 會被自動合併 ---
    第 1 塊 4KB                  -> VMA 總數 17 (+1)
    第 2 塊（緊鄰、屬性相同）      -> VMA 總數 17      ← 沒有增加！
    第 3 塊（緊鄰、屬性相同）      -> VMA 總數 17      ← 還是沒增加
    三塊合併的結果：
      700100000000-700100003000 rw-p 00000000 00:00 0     ← ★ 3 塊變成 1 行

    第 4 塊（緊鄰但 PROT_READ）    -> VMA 總數 18   ★ 屬性不同就不能合併
    屬性不同無法合併：
      700100000000-700100003000 rw-p 00000000 00:00 0
      700100003000-700100004000 r--p 00000000 00:00 0     ← ★ 只好另開一個 VMA
```

**三次 `mmap` 相鄰位址、屬性相同 → `vma_merge()` 併成一個 VMA（總數不變）；
第四次屬性不同 → 無法合併，VMA 總數 +1。**

這就是核心維持「位址空間離散但不碎裂」的機制：
不只保證不重疊，還會**主動把能合的合起來**，
避免 maple tree 被幾萬個瑣碎的 VMA 撐爆（`max_map_count` 預設 65530）。

---

<a name="q22"></a>
## 22. Linux 內核如何實現行程位址空間的快速查詢和插入？

### 結論

> ### ⚠️ **本機 6.1 已經改用 maple tree，書上的紅黑樹已經被移除**

| | 書中（≤ 6.0） | **本機 6.1** |
|---|---|---|
| 結構 | **紅黑樹 `mm->mm_rb`** + 雙向鏈表 `mm->mmap` | **maple tree `mm->mm_mt`** |
| 查找 | `rb_search` O(log N) | `mas_walk()` O(log N)，但**分支因子大很多** |
| 走訪相鄰 | 靠 `vma->vm_next`/`vm_prev` 鏈表 | `mas_next()`/`mas_prev()`，**`vm_next`/`vm_prev` 已刪除** |
| 並發讀 | 需要 `mmap_lock` | **RCU 無鎖讀**（為 per-VMA lock 鋪路） |
| 記憶體 | 每個 VMA 帶 rb_node(24B) + 兩個指標 | **節點共用，更省** |

```c
/* include/linux/mm_types.h（6.1） */
struct mm_struct {
        struct maple_tree mm_mt;      /* ★ 沒有 mm_rb，也沒有 mmap 鏈表 */
        ...
};
```

**Maple Tree** 是 Liam Howlett 為 mm 子系統寫的 **B 樹變種**，
專門管理「一組不重疊的區間」，Linux 6.1（commit `524e00b36e8a`
"mm: remove rb tree"）正式取代紅黑樹。

> **書目**：奔跑吧 §4.4.5「查找 VMA」、§4.4.8「紅黑樹例子」——
> 書中整節在講紅黑樹，這是典型的「書本描述歷史實現、實機已經演進」的例子。

### 實機驗證

```bash
grep -n "struct maple_tree mm_mt\|struct rb_root mm_rb" include/linux/mm_types.h
```

```
include/linux/mm_types.h:  struct maple_tree mm_mt;
```

**`mm_rb` 完全查無此物** ✅

```bash
grep -rn "vm_next\|vm_prev" include/linux/mm_types.h | head
# （無比對——這兩個成員在 6.1 已刪除）

sed -n '/^struct vm_area_struct \*find_vma/,/^}/p' mm/mmap.c
```

```c
struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
        struct vm_area_struct *vma;
        MA_STATE(mas, &mm->mm_mt, addr, addr);      /* ★ maple tree 迭代器 */

        rcu_read_lock();                            /* ★ RCU 無鎖讀 */
        vma = mas_walk(&mas);
        rcu_read_unlock();
        return vma;
}
```

**回答這題的正確姿勢**：先講紅黑樹 + 鏈表的經典設計與它的問題
（鏈表與樹要同時維護、走訪要拿鎖），再說明 6.1 改用 maple tree 解決了什麼，
並用 `grep` 佐證。

---

<a name="q23"></a>
## 23. find_vma() 查找符合哪些條件的 VMA？

### 結論

**`find_vma(mm, addr)` 回傳「第一個滿足 `vma->vm_end > addr` 的 VMA」。**

⚠️ **它不保證 `addr` 落在回傳的 VMA 裡面！**

```
      addr
       │
  ─────┼──────────────────────────────
       │        ┌────────┐   ┌──────┐
       └────────│  VMA A │   │VMA B │
       空洞      └────────┘   └──────┘
                 ↑ find_vma() 回傳這個（vm_end > addr）
                   但 addr < A->vm_start，addr 其實在空洞裡
```

所以呼叫者**必須自己再檢查一次**：

```c
vma = find_vma(mm, addr);
if (!vma || addr < vma->vm_start) {
        /* addr 落在空洞裡 —— 可能是堆疊要增長，也可能是非法存取 */
}
```

`do_page_fault()` 就是這樣寫的（`arch/arm64/mm/fault.c`）：
落在空洞且 VMA 帶 `VM_GROWSDOWN` → 呼叫 `expand_stack()` 自動長堆疊；
否則送 SIGSEGV。

**相關的另外兩個介面**：
- `find_vma_intersection(mm, start, end)`：要求真的與 `[start,end)` 有交集
- `vma_lookup(mm, addr)`（5.18 新增）：**要求 `addr` 真的在 VMA 內**，否則回 NULL
  —— 就是為了避免上面這個坑而加的

> **書目**：奔跑吧 §4.4.5「查找 VMA」。

### 實機驗證

```bash
grep -n -A10 "^struct vm_area_struct \*find_vma" mm/mmap.c
grep -n -A8 "^struct vm_area_struct \*vma_lookup" mm/mmap.c 2>/dev/null || \
  grep -n -B2 -A8 "vma_lookup" include/linux/mm.h | head -20
```

`mm_convert.ko` 的輸出示範了正確的用法：

```c
vma = find_vma(mm, va);
if (!vma || va < vma->vm_start) {          /* ← 必須自己補這一檢查 */
        P("  [1] find_vma(mm, 0x%lx) 沒找到涵蓋此位址的 VMA\n", va);
        goto unlock;
}
```

```
[1] mm + vaddr -> VMA : find_vma(mm, 0xffffbd89c000)
    vma = 0xffff00005be7b6e8  範圍 [0xffffbd89c000, 0xffffbd89f000)
                               ↑ vm_start == addr，這次確實落在裡面
```

---

<a name="q24"></a>
## 24. malloc() 返回的記憶體是否馬上分配物理內存？

### 結論

**不會。這就是「按需分頁 / 惰性分配（demand paging）」。**

```
malloc()  →  brk() / mmap()  →  只建立/擴大 VMA
                                 【一個實體頁都沒配，一個 PTE 都沒建】
                                          │
                       程式第一次讀寫這段位址
                                          ▼
                            MMU 找不到 PTE → Translation Fault
                                          ▼
                do_page_fault() → handle_mm_fault() → do_anonymous_page()
                                          ▼
                         此時才 alloc_page() + 建立 PTE
```

**題目裡的 testA()/testB()**：
- `testA()`（配完馬上寫）→ **在那一行寫入時**觸發缺頁、配實體頁
- `testB()`（配完延後才存取）→ **在它自己第一次存取時**才配
- 如果 `testB()` 從頭到尾沒碰過 → **永遠不會配實體頁**

> **書目**：奔跑吧 §4.5「malloc()」、§4.7「缺頁中斷」。

### 實機驗證（★）

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./fault_types | head -12'
```

```
--- [Q24/Q25] 惰性分配 demand paging ---
mmap 前 RSS = 186 頁
mmap 16MB（完全不觸碰）           minor=0     major=0    (預期 0 頁)
mmap 後 RSS = 186 頁   <- 完全沒漲，一個實體頁都沒配

--- [Q39] 匿名頁「寫」缺頁：每頁一次 minor fault ---
寫入 16MB 匿名頁                  minor=4096  major=0    (預期 4096 頁)
RSS = 4381 頁
```

**`mmap` 16 MB 之後 RSS 完全沒變（186 → 186 頁），缺頁次數 0。**
一旦逐頁寫入，**剛好 4096 次 minor fault**（16 MB / 4 KB = 4096），
RSS 從 186 漲到 4381（+4195 ≈ 4096 + 零頭）。

**未觸碰的頁在 pagemap 裡 present=0**：

```bash
ssh radxa@192.168.68.57 'sudo ./pagemap_walk | grep -A3 "未觸碰"'
```

```
mmap 未觸碰   VA=0x0000ffffa9261000  present=0  PFN=0x0          PA=0x00000000000
mmap 觸碰後   VA=0x0000ffffa9261000  present=1  PFN=0x72c56      PA=0x00072c56000
```

---

<a name="q25"></a>
## 25. malloc() 分配 100 位元組，內核實際分配 100 位元組嗎？

### 結論

**不是。核心的最小單位是「頁」（本機 4096 bytes）。**

三個層面的「向上取整」：

1. **VMA 層面**：`mmap()`/`brk()` 的區間一定是**頁對齊**的
2. **實體頁層面**：缺頁時 `alloc_page()` 配的是**整整一頁 4096 bytes**
3. **glibc 層面**：`malloc(100)` 通常不會單獨去要一頁，
   而是從已有的堆裡切一塊（加上 8~16 bytes 的 chunk header，對齊到 16）

所以「使用者要 100 bytes」→「glibc 給 112 bytes 的 chunk」→
「核心配 4096 bytes 的頁」。剩下的 3984 bytes 會被**同一個行程後續的 malloc 複用**，
不會浪費——這正是 glibc 要在使用者態自己維護堆的原因。

> **書目**：奔跑吧 §4.5「malloc()」、§4.5.1「brk 系統調用」。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'cd ~/exp && cat > m100.c <<EOF
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
int main(void){
  void *a=malloc(100), *b=malloc(100), *c=malloc(100);
  printf("malloc(100) 三次： %p %p %p\n", a,b,c);
  printf("相鄰兩塊相距 %ld / %ld bytes\n", (char*)b-(char*)a, (char*)c-(char*)b);
  printf("malloc_usable_size(a) = %zu bytes  <- glibc 實際給的\n", malloc_usable_size(a));
  printf("三塊是否在同一個 4KB 頁？ %s\n",
     ((unsigned long)a>>12)==((unsigned long)c>>12) ? "是" : "否");
  return 0;}
EOF
gcc -O2 -o m100 m100.c && ./m100'
```

典型輸出：

```
malloc(100) 三次： 0xaaaae1f1b2a0 0xaaaae1f1b310 0xaaaae1f1b380
相鄰兩塊相距 112 / 112 bytes         ← glibc 給 112 (100 + header，對齊 16)
malloc_usable_size(a) = 104 bytes
三塊是否在同一個 4KB 頁？ 是         ← 三次 malloc 共用同一個實體頁
```

**三次 `malloc(100)` 只消耗一個實體頁**——
從 [Q24](#q24) 的實驗也看得出來：`mmap` 16 MB 完全不碰 → 0 個實體頁。

---

<a name="q26"></a>
## 26. bufA 和 bufB 指向的位址一樣，內核中兩個虛擬記憶體塊是否衝突？

### 結論

**要看生命週期有沒有重疊。**

- **不重疊**（`bufA` 已 `free()` 後才 `malloc()` 出 `bufB`）→ **不衝突**。
  這是**正常且必然**的位址複用：glibc 把剛釋放的 chunk 掛回 bin，
  下次同尺寸請求直接發同一塊。核心層面更是如此——`munmap` 後那段位址就空了，
  下次 `mmap` 完全可能拿到同一個位址。

- **重疊**（`bufA` 還在用就出現同位址的 `bufB`）→ **這是應用層的 bug**
  （use-after-free、double free、指標管理錯誤），
  但**不是核心的問題**：核心只保證「**任一時刻**同一個 `mm` 內不會有兩個重疊的 VMA」，
  從不保證「歷史上不同時間的分配位址不重複」。

**核心真正的不變量**：`mm->mm_mt` 裡的 VMA 兩兩不重疊（見 [Q21](#q21)）。

> **書目**：奔跑吧 §4.5「malloc()」相關討論。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'cd ~/exp && cat > reuse.c <<EOF
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
int main(void){
  void *a = malloc(100);  printf("bufA = %p\n", a);  free(a);
  void *b = malloc(100);  printf("bufB = %p  %s\n", b, a==b?"<- 位址相同（合法複用）":"");
  void *m1 = mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
  printf("mmap1 = %p\n", m1);  munmap(m1,4096);
  void *m2 = mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
  printf("mmap2 = %p  %s\n", m2, m1==m2?"<- 核心也複用了同一段虛擬位址":"");
  return 0;}
EOF
gcc -O2 -o reuse reuse.c && ./reuse'
```

```
bufA = 0xaaaaf1a2b2a0
bufB = 0xaaaaf1a2b2a0  <- 位址相同（合法複用）
mmap1 = 0xffff9a6d8000
mmap2 = 0xffff9a6d8000  <- 核心也複用了同一段虛擬位址
```

**連核心的 `mmap` 都會複用剛 `munmap` 掉的位址** —— 這是設計，不是 bug。

---

<a name="q27"></a>
## 27. vm_normal_page() 返回什麼頁面？為什麼需要它？

### 結論

**`vm_normal_page(vma, addr, pte)` 只回傳「能用常規 `struct page` 語意處理」的頁**，
碰到下面這些「特殊頁」就回傳 **NULL**：

| 特殊頁 | 為什麼不能當普通頁 |
|--------|-------------------|
| **`ZERO_PAGE`** | 全系統共用一個唯讀零頁，`PageReserved=1`，不該被計入 rmap/LRU/refcount |
| **`VM_PFNMAP` / `VM_MIXEDMAP` 的 MMIO 映射** | 那段實體位址**根本沒有 `struct page`**（不是 RAM），碰它會存取非法記憶體 |
| **保留頁**（`PageReserved`） | 核心啟動早期保留的、不受伙伴系統管理 |

**判定邏輯**（`mm/memory.c`）：

```c
struct page *vm_normal_page(struct vm_area_struct *vma, unsigned long addr, pte_t pte)
{
        unsigned long pfn = pte_pfn(pte);

        if (unlikely(vma->vm_flags & (VM_PFNMAP|VM_MIXEDMAP))) { ... return NULL; }
        if (is_zero_pfn(pfn))       return NULL;      /* ← 零頁 */
        if (unlikely(!pfn_valid(pfn))) return NULL;   /* ← 沒有 struct page */
        return pfn_to_page(pfn);
}
```

**為什麼需要**：`copy_page_range()`（fork）、`zap_pte_range()`（munmap）、
`follow_page()`、頁面回收掃描……全都要拿 `struct page*` 去做
`get_page()`/`page_add_rmap()`/加入 LRU。對著一個沒有 `struct page` 的 MMIO PFN
做這些事會直接踩壞記憶體。所以核心用一個統一的守衛函式把兩類情況分開。

> **書目**：奔跑吧 §4.7.5「系統零頁」、§4.7.6「文件映射缺頁中斷」。

### 實機驗證

```bash
sudo dmesg | grep -A3 "ZERO_PAGE"
```

```
ZERO_PAGE（唯讀零頁）  _refcount=2  _mapcount=0  PageAnon=0 PageLRU=0 PageSlab=0 mapping=0x0
    ^ PageReserved=1，vm_normal_page() 會對它回傳 NULL
    ZERO_PAGE PFN = 0x260b  PA = 0x260b000
```

**零頁的實證**（`fault_types` 的「只讀」測試）：

```
--- [Q39 變體] 匿名頁「只讀」缺頁：共用 ZERO_PAGE，不佔 RSS ---
只讀掃過 16MB 匿名頁     minor=4096  major=0
RSS 變化 = 0 頁  <- 幾乎不漲：全部指向同一個唯讀 ZERO_PAGE
```

**4096 次缺頁，但 RSS 一頁都沒漲** —— 4096 個虛擬頁全部映射到
同一個 PFN `0x260b`。如果 `vm_normal_page()` 不把它擋掉，
這一個零頁的 `_mapcount` 會被加到 4096，rmap 也會塞爆。

---

<a name="q28"></a>
## 28. get_user_pages() 的作用和實現流程

### 結論

**GUP = 在核心態安全地取得使用者虛擬位址對應的 `struct page*`，並「釘住」它們。**

**為什麼需要「釘住」**：核心拿到 `struct page*` 之後可能要做 DMA、
或交給硬體長時間使用；期間這些頁**絕對不能被換出、遷移、釋放**。
所以要 `get_page()` 增加 refcount（新介面 `pin_user_pages()` 用專門的 pin 計數）。

**流程**：

```
get_user_pages_fast(start, nr_pages, gup_flags, pages)
  │
  ├─ ① 快速路徑 internal_get_user_pages_fast()
  │     關中斷（或 RCU）→ 直接走軟體頁表 gup_pgd_range()
  │     PTE 存在 && 權限夠 → try_grab_folio() 增 refcount → 完成
  │     【不需要 mmap_lock，最快】
  │
  └─ ② 慢速路徑 __gup_longterm_locked() → __get_user_pages()
        拿 mmap_lock(read)
        for 每一頁:
           follow_page_mask()  → 有映射就拿
           沒有/權限不夠 → faultin_page() → handle_mm_fault()
                              （真的觸發缺頁：配匿名頁 / 讀檔案 / 做 COW）
           再 follow 一次拿到 page → get_page()
```

**兩個關鍵細節**：
1. **`FOLL_WRITE` 會強制做 COW**：如果只是要讀，別加 `FOLL_WRITE`，
   否則會把父子共享的頁提前拆開
2. **用完一定要 `put_page()`/`unpin_user_pages()`**，否則這些頁永遠回收不掉

> **書目**：奔跑吧 §4.5.6「get_user_pages() 函數」。

### 實機驗證

```bash
grep -n "EXPORT_SYMBOL.*get_user_pages" mm/gup.c
```

```
mm/gup.c:3141:EXPORT_SYMBOL_GPL(get_user_pages_fast);
mm/gup.c:3172:EXPORT_SYMBOL_GPL(pin_user_pages_fast);
```

**誰在用它**（本機實際載入的驅動）：

```bash
ssh radxa@192.168.68.57 'sudo grep -c "" /proc/vmstat; grep -E "^nr_foll_pin" /proc/vmstat'
```

```
nr_foll_pin_acquired 0
nr_foll_pin_released 0
```

本機目前沒有活躍的長期 pin（沒跑 RDMA/DPDK/vfio）。
這兩個計數器就是專門用來抓「GUP 洩漏」的——
**`acquired − released` 長期不歸零就代表有驅動忘了 unpin**。

---

<a name="q29"></a>
## 29. follow_page() 的作用和實現流程

### 結論

**`follow_page()` 是「唯讀查詢」：走一遍軟體頁表，有映射就回傳 `struct page*`，
沒有就回 NULL——絕不主動建立映射。**

| | `follow_page()` | `get_user_pages()` |
|---|---|---|
| 沒映射時 | **回 NULL** | **觸發缺頁去建立** |
| 需要 `mmap_lock` | 呼叫者自己持有 | 內部處理 |
| 增加 refcount | 只有 `FOLL_GET` 時才加 | 一定加（這是重點） |
| 用途 | 診斷、`/proc/pid/pagemap`、KSM 掃描、遷移前檢查 | DMA、direct I/O、RDMA |

**流程**（`mm/gup.c` 的 `follow_page_mask()`）：

```
pgd_offset() → p4d_offset() → pud_offset() → pmd_offset() → pte_offset_map()
   每一級都檢查 pXd_none() / pXd_bad()，任一級無效 → 回 NULL
最後檢查 pte_present()：
   有效   → vm_normal_page() 拿 struct page*（見 Q27）
   無效   → 可能是 swap entry / migration entry → 回 NULL
```

> **書目**：奔跑吧 §4.5.6「get_user_pages() 函數」內的相關討論。

### 實機驗證

`mm_convert.ko` 做的就是 `follow_page()` 的手工版本
（見 [Ch3 Q3](./ch03_memory_management_prerequisites.md#q3)）：

```
[2] VMA + vaddr -> page（逐級走頁表）
    pgd_offset()     = 0xffff000055dcdff8  val=0x800000055dcc003
    pud_offset()     = 0xffff000055dccff0  val=0x80000005caa1003
    pmd_offset()     = 0xffff00005caa1f60  val=0x80000005d34d003
    pte_offset_map() = 0xffff00005d34d4e0  val=0xe0000074969fc3
[7] PTE -> page : pte_page() = 0xfffffc0001d25a40   pte_pfn()=0x74969
```

**「沒有映射就回 NULL」的實證**（`pagemap` 是 `follow_page` 語意的使用者態版本）：

```
mmap 未觸碰   present=0  PFN=0x0     ← 頁表裡根本沒有這一項，回 NULL
mmap 觸碰後   present=1  PFN=0x72c56
```

`follow_page()` 對「未觸碰的頁」回 NULL，而 `get_user_pages()`
會在同樣情況下**主動觸發缺頁**、配一個實體頁再回傳——這就是兩者最大的差別。

---

<a name="q30"></a>
## 30. `SYSCALL_DEFINE1(brk, unsigned long, brk)` 這個巨集如何展開？

### 結論

`SYSCALL_DEFINEx` 的目的是**把「系統呼叫的實作」與「如何被分發機制正確呼叫」解耦**，
順便統一插入型別檢查、審計、對映錯誤的防護。

```c
/* include/linux/syscalls.h */
#define SYSCALL_DEFINE1(name, ...) SYSCALL_DEFINEx(1, _##name, __VA_ARGS__)

#define SYSCALL_DEFINEx(x, sname, ...)                          \
        SYSCALL_METADATA(sname, x, __VA_ARGS__)                 \
        __SYSCALL_DEFINEx(x, sname, __VA_ARGS__)

#define __SYSCALL_DEFINEx(x, name, ...)                                 \
        __diag_push();                                                  \
        asmlinkage long sys##name(__MAP(x,__SC_DECL,__VA_ARGS__));      \
        ALLOW_ERROR_INJECTION(sys##name, ERRNO);                         \
        static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__));     \
        static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__)); \
        asmlinkage long sys##name(__MAP(x,__SC_DECL,__VA_ARGS__))       \
                __attribute__((alias(__stringify(__se_sys##name))));    \
        static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__))      \
        {                                                               \
                long ret = __do_sys##name(__MAP(x,__SC_CAST,__VA_ARGS__)); \
                __MAP(x,__SC_TEST,__VA_ARGS__);                         \
                __PROTECT(x, ret,__MAP(x,__SC_ARGS,__VA_ARGS__));       \
                return ret;                                             \
        }                                                               \
        static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__))
```

`SYSCALL_DEFINE1(brk, unsigned long, brk) { ... }` 展開後產生**三個符號**：

| 符號 | 角色 |
|------|------|
| `sys_brk` | syscall table 用的入口（是 `__se_sys_brk` 的 alias） |
| `__se_sys_brk(long brk)` | **sign-extend 包裝層**：參數一律用 `long` 接（避免 32/64 位混用的漏洞），再轉型交給下一層 |
| `__do_sys_brk(unsigned long brk)` | **真正的實作**，緊跟在巨集後面的 `{ ... }` 就是它的函式體 |

**`__se_` 這層存在的理由**（CVE 級的安全考量）：使用者態傳進來的暫存器是 64 位，
但參數宣告可能是 `int`。若直接轉型，高 32 位的髒資料可能被利用。
`__SC_LONG` 強制先當 `long` 接、再顯式 `__SC_CAST` 轉型，堵掉這條路。

`__SC_TEST` 則在編譯期用 `BUILD_BUG_ON` 檢查**每個參數都不超過 `long` 的大小**。

> **書目**：奔跑吧 §4.5.1「brk 系統調用」、§4.5.3「__do_sys_brk() 函數」。

### 實機驗證

```bash
grep -n -A20 "^#define __SYSCALL_DEFINEx" include/linux/syscalls.h
grep -n "SYSCALL_DEFINE1(brk" mm/mmap.c
```

```
mm/mmap.c:  SYSCALL_DEFINE1(brk, unsigned long, brk)
```

**在符號表裡找到展開的結果**：

```bash
ssh radxa@192.168.68.57 'sudo grep -wE "sys_brk|__arm64_sys_brk" /proc/kallsyms'
```

```
ffff8000081f6234 T __arm64_sys_brk
```

ARM64 還多包一層 `__arm64_sys_brk(const struct pt_regs *regs)`
（`SYSCALL_DEFINE0`/`__SYSCALL_DEFINEx` 在 `arch/arm64/include/asm/syscall_wrapper.h` 被覆寫），
它負責**從 `pt_regs` 裡把參數挖出來**再呼叫 `__se_sys_brk`——
這是 Spectre 緩解（不讓 syscall 參數直接從暫存器流進來）的一部分。

**追一次實際的 brk**：

```bash
ssh radxa@192.168.68.57 'strace -e trace=brk /bin/true 2>&1 | head -3'
```

---

<a name="q31"></a>
## 31. ARM64 內核中使用者空間如何劃分？brk 區域的起止位址在哪？

### 結論

**使用者空間佈局**（本機 4KB 頁、48-bit VA、有 ASLR）：

```
0x0000000000000000  ┌──────────────────────┐
                    │  （保留，防 NULL 解參考）│  mmap_min_addr = 65536
                    ├──────────────────────┤
   0xaaaa........   │  ELF 映像 .text/.data │  ← PIE，位置隨機化
                    ├──────────────────────┤
                    │  [heap]  brk 區       │  ← start_brk ~ brk
                    │      ↓ 向上長          │
                    ├──────────────────────┤
                    │        （空洞）        │
                    ├──────────────────────┤
                    │      ↑ 向下長          │
   0xffff........   │  mmap 區（libc、匿名） │  ← mmap_base，topdown
                    ├──────────────────────┤
                    │  [stack]              │  ← 向下長
                    ├──────────────────────┤
                    │  [vvar] [vdso]        │
0x0000ffffffffffff  └──────────────────────┘
```

**brk 區的邊界**：
- **起點 `mm->start_brk`**：`load_elf_binary()` 在載入 ELF 時設定，
  緊接在 `.bss` 之後（有 `randomize_va_space` 時再加一段隨機偏移）
- **終點 `mm->brk`**：初始 == `start_brk`，每次 `brk()`/`sbrk()` 就改這個值

`SYSCALL_DEFINE1(brk, ...)`（`mm/mmap.c`）做的事：
1. 檢查新的 brk 是否低於 `mm->start_brk` → 拒絕
2. 檢查是否超過 `RLIMIT_DATA` → 拒絕
3. **檢查新區間會不會撞到既有的 VMA**（`find_vma_intersection()`）→ 撞到就拒絕增長
4. 沒問題 → `do_brk_flags()` 擴大那個 heap VMA 的 `vm_end`

> **書目**：奔跑吧 §4.5.2「用戶態地址空間劃分」、§4.5.3「__do_sys_brk() 函數」、
> §4.5.4「do_brk_flags() 函數」。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./mmap_vma 2>&1 | sed -n "/brk/,/^$/p"'
```

或直接觀察：

```bash
ssh radxa@192.168.68.57 'cat /proc/self/maps | grep -E "heap|stack|vdso"; echo "---"
  grep -E "^(VmData|VmStk|VmExe|VmLib):" /proc/self/status'
```

```
aaaaf0f8e000-aaaaf0faf000 rw-p 00000000 00:00 0   [heap]        ← start_brk ~ brk
ffffd4a2e000-ffffd4a4f000 rw-p 00000000 00:00 0   [stack]
ffffd4b5f000-ffffd4b61000 r-xp 00000000 00:00 0   [vdso]
```

**用 `sbrk()` 實際推動 `mm->brk` 並觀察 `[heap]` 變大**：

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./mmap_vma | sed -n "/使用者空間劃分/,/^$/p"'
```

```
--- [Q31] 使用者空間劃分 ---
  aaaada0e0000-aaaada0e3000 r-xp ... /home/radxa/exp/mmap_vma    ← .text  (r-xp)
  aaaada0ff000-aaaada100000 r--p ... /home/radxa/exp/mmap_vma    ← .rodata
  aaaada100000-aaaada101000 rw-p ... /home/radxa/exp/mmap_vma    ← .data/.bss
  aaab0a767000-aaab0a788000 rw-p ...                    [heap]  ← ★ brk 區
  ffff8d65b000-ffff8d65c000 r-xp ...                    [vdso]
  ffffc4e28000-ffffc4e49000 rw-p ...                    [stack]

  初始 brk (sbrk(0)) = 0xaaab0a788000       ← == [heap] 的結束位址 ✅
  sbrk(+1MB) 後      = 0xaaab0a888000   (差 1048576 bytes)
  擴大後的 [heap]：
    aaab0a767000-aaab0a888000 rw-p ...     [heap]
    ↑ 起點沒動           ↑ 終點跟著 brk 一起漲了 1 MB
  sbrk(-1MB) 後      = 0xaaab0a788000   <- mm->brk 可增可減，但 mm->start_brk 不變
```

> ### 🎯 三個對應關係一次驗證
> | 核心欄位 | 對應的觀察值 |
> |---------|------------|
> | `mm->start_brk` | `[heap]` 的**起始**位址 `0xaaab0a767000`——**全程不變** |
> | `mm->brk` | `[heap]` 的**結束**位址，也正是 **`sbrk(0)` 的回傳值** `0xaaab0a788000` ✅ |
> | `brk()` 系統呼叫 | 改的就是 `mm->brk`，連帶擴大/縮小那個 heap VMA 的 `vm_end` |
>
> 注意 `[heap]` 的起點 `0xaaab0a767000` 與執行檔的 `.data` 段
> `0xaaaada101000` **相差很遠**——這是 `randomize_va_space=2` 造成的
> **brk 隨機化**（`arch_randomize_brk()`），防止堆積溢位攻擊者猜到堆位址。

---

<a name="q32"></a>
## 32. 私有映射和共享映射的區別？

### 結論

| | **`MAP_PRIVATE`（私有）** | **`MAP_SHARED`（共享）** |
|---|---|---|
| 寫入對別人可見？ | ✗ | ✓ 立即可見 |
| 寫入會回寫檔案？ | ✗ | ✓（經 page cache，延遲回寫） |
| 實作機制 | **寫時複製 COW** | **直接共享同一個 page cache 頁** |
| 讀取內容 | 來自檔案/零頁 | 來自檔案/共享匿名頁 |
| 缺頁處理函式 | `do_cow_fault()` | `do_shared_fault()` |
| LRU 歸屬（匿名時） | 匿名 LRU | 匿名 LRU（shmem，見 [Ch6 Q5](./ch06_memory_management_case_studies.md#q5)） |
| 典型用途 | 載入執行檔/函式庫、`malloc` | IPC 共享記憶體、mmap 檔案讀寫 |

> **書目**：奔跑吧 §4.6.1「mmap 概述」（表：私有/共享 × 匿名/檔案 四種組合）。

### 實機驗證（★ 直接證明「回不回寫檔案」）

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./fault_types | tail -12'
```

```
--- [Q32] MAP_PRIVATE 檔案映射寫入 -> COW，不回寫檔案 ---
  寫入前 p[0] = 0xab（檔案內容）
寫 1 頁私有檔案映射              minor=1   major=0
  寫入後 p[0] = 0x5a（記憶體中的私有副本）
  msync 之後檔案第 0 byte = 0xab  <- 仍是 0xAB，證明私有映射不回寫檔案 ✓

--- [Q32] MAP_SHARED 檔案映射寫入 -> 直接改 page cache，會回寫 ---
  msync 之後檔案第 0 byte = 0x5a  <- 變成 0x5a，證明共享映射會寫回檔案 ✓
```

**同一個檔案、同一個 offset、同樣寫入 `0x5a`，然後同樣 `msync(MS_SYNC)`：**
- `MAP_PRIVATE` → 檔案還是 `0xAB`（寫入只停在私有副本）
- `MAP_SHARED` → 檔案變成 `0x5a`

**四種組合的完整實證**（`mmap_vma`）：

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./mmap_vma | sed -n "/四種 mmap/,/^$/p"'
```

```
  MAP_PRIVATE|MAP_ANONYMOUS   maps權限=rw-p  映射前=0x00 寫入後=0x5a
  MAP_SHARED |MAP_ANONYMOUS   maps權限=rw-s  映射前=0x00 寫入後=0x5a
  MAP_PRIVATE|檔案             maps權限=rw-p  映射前=0xab 寫入後=0x5a  檔案=0xab ★不回寫(COW)
  MAP_SHARED |檔案             maps權限=rw-s  映射前=0xab 寫入後=0x5a  檔案=0x5a ★會回寫
```

**兩個觀察**：

1. **`/proc/self/maps` 的第 4 個權限字元直接洩漏了映射類型**：
   `p` = private、`s` = shared。這是最快的判斷方法。
2. **檔案映射的「映射前內容 = 0xab」證明兩者都會先讀到檔案內容**；
   差別只在寫入之後——私有的停在記憶體副本，共享的傳到檔案。

`MAP_SHARED|MAP_ANONYMOUS` 的匿名共享映射就是 **shmem**——
maps 顯示 `rw-s`，它會被計入 `Shmem`、掛在**匿名 LRU** 上、
但**不計入 `AnonPages`**（見 [Ch6 Q5](./ch06_memory_management_case_studies.md#q5)）。

---

<a name="q33"></a>
## 33. 為什麼第二次 mmap 時內核沒有捕捉到位址重疊並返回失敗？

### 結論

**因為兩次都帶了 `MAP_FIXED`，而 `MAP_FIXED` 的語意就是「強制覆蓋」。**

`mmap(2)` 手冊寫得很明白：

> **MAP_FIXED**：… If the memory region specified by *addr* and *len* overlaps
> pages of any existing mapping(s), then the overlapped part of the existing
> mapping(s) will be **discarded**.

也就是說 `MAP_FIXED` **不是**「必須用這個位址，否則失敗」，
而是「**必須用這個位址；擋路的先拆掉**」——
核心會先對重疊區間做一次等效的 `do_munmap()`，再建立新映射。

實作在 `mmap_region()`（`mm/mmap.c`）：

```c
/* Unmap any existing mapping in the area */
if (do_vmi_munmap(&vmi, mm, addr, len, uf, false))
        return -ENOMEM;
```

**所以那段 strace 的結果是設計使然，不是 bug。**
第一個 819200 bytes 的映射會被裁成
「前面保留 + 中間被新的 4096 取代 + 後面保留」三段。

**安全的替代品**：Linux 4.17 加了 **`MAP_FIXED_NOREPLACE`**，
發現重疊就回 **`EEXIST`**，不做任何破壞。

> **書目**：奔跑吧 §4.6.1「mmap 概述」。

### 實機驗證（★）

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./mmap_vma | sed -n "/MAP_FIXED/,/^$/p"'
```

```
--- [Q33] MAP_FIXED 會靜默覆蓋，MAP_FIXED_NOREPLACE 才會報錯 ---
[1] mmap(hint, 800KB, MAP_FIXED)
    -> 0x700000000000   (VMA 總數 = 17)
    目前的 maps：
      700000000000-7000000c8000 rw-p 00000000 00:00 0

[2] mmap(同一位址, 4KB, MAP_FIXED)
    -> 0x700000000000   errno=0   ★ 成功！沒有回報任何錯誤
    覆蓋後的 maps：
      700000000000-7000000c8000 rw-p 00000000 00:00 0

[3] mmap(同一位址, 4KB, MAP_FIXED_NOREPLACE)
    -> 0xffffffffffffffff   errno=17 (File exists)   ★ 正確地偵測到重疊並回報 EEXIST

[4] mmap(同一位址, 4KB, 不帶 MAP_FIXED)
    -> 0xffff8d656000   ★ hint 只是建議，衝突時核心自動另尋位址
```

**四種行為一次看清楚**：`MAP_FIXED` 覆蓋、`MAP_FIXED_NOREPLACE` 回 `EEXIST`、
不帶 flag 時 hint 只是建議。

> ### 🎯 步驟 [2] 有個比「切成兩段」更值得講的細節
>
> 覆蓋之後 `/proc/self/maps` **看起來完全沒變**——還是一整段
> `700000000000-7000000c8000 rw-p`。
>
> 這不是沒發生事，而是：核心確實先 `do_vmi_munmap()` 把前 4 KB 拆掉、
> 再建了一個新 VMA，但因為新 VMA 的**屬性與旁邊完全相同**
> （`PROT_READ|PROT_WRITE` + `MAP_PRIVATE|MAP_ANONYMOUS`），
> `vma_merge()` 立刻把它跟後面那段**合併回去**了。
>
> **所以 `MAP_FIXED` 的覆蓋是「真・靜默」——連 `/proc/self/maps` 都看不出來。**
> 舊映射那 4 KB 的**內容已經被清空**（變成新的匿名頁），
> 但你從 maps 完全察覺不到。這正是 `MAP_FIXED` 危險的地方。
>
> （若新映射的屬性不同，例如改成 `PROT_READ`，就會看到 VMA 被切開——
> 見下面 [Q21](#q21) 的第 4 塊。）

---

<a name="q34"></a>
## 34. ARM64 在缺頁異常後如何找到異常類型和錯誤位址？

### 結論

處理器在進入異常向量之前，**硬體自動填好兩個系統暫存器**：

| 暫存器 | 內容 |
|--------|------|
| **`FAR_EL1`**（Fault Address Register） | **出錯的虛擬位址** |
| **`ESR_EL1`**（Exception Syndrome Register） | **異常的「症候群」**：類別、讀/寫、錯誤碼 |

`ESR_EL1` 的關鍵欄位（`arch/arm64/include/asm/esr.h`）：

```
 31    26 25 24        6  5    0
┌────────┬──┬──────────┬──┬──────┐
│   EC   │IL│    ISS   │WnR│ DFSC │
└────────┴──┴──────────┴──┴──────┘
  EC   = 異常類別：0x24 = Data Abort (lower EL)，0x20 = Instruction Abort
  WnR  = bit 6：1 = 寫造成，0 = 讀/取指造成           ← Q35
  DFSC = bits[5:0]：錯誤狀態碼
         0b0001LL = Translation fault, level LL   （頁表項無效）
         0b0010LL = Access flag fault, level LL   （AF=0）
         0b0011LL = Permission fault, level LL    （權限不符，COW 走這條）
         0b100001 = Alignment fault
```

核心的接法（`arch/arm64/kernel/entry.S` → `arch/arm64/mm/fault.c`）：

```c
static void __do_kernel_fault(unsigned long addr, unsigned long esr, struct pt_regs *regs);
void do_mem_abort(unsigned long far, unsigned long esr, struct pt_regs *regs)
{
        const struct fault_info *inf = esr_to_fault_info(esr);   /* 用 DFSC 查表 */
        unsigned long addr = untagged_addr(far);
        if (!inf->fn(far, esr, regs)) return;
        ...
}
```

`fault_info[]` 是一張 64 項的表，用 `esr & ESR_ELx_FSC` 直接索引，
把每種 DFSC 對應到處理函式與訊號。

> **書目**：奔跑吧 §4.7.1「ARM64 缺頁異常的底層處理流程」。

### 實機驗證（★ 從使用者態直接讀出 ESR/FAR）

`esr_far.c` 用 `sigaction(SA_SIGINFO)`，從 `siginfo_t` 拿 `si_addr`（核心從 `FAR_EL1` 抄的），
從 `ucontext` 的 `esr_context` 拿原始的 `ESR_EL1`：

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./esr_far'
```

```
┌─ 捕捉到 SIGSEGV：讀一個沒有任何 VMA 的位址 0x123456789000
│  si_addr (<- FAR_EL1) = 0x0000123456789000       ← 就是我們存取的位址 ✅
│  si_code              = 1 (SEGV_MAPERR 該位址沒有 VMA)
│  ESR_EL1              = 0x0000000092000004
│    EC   = 0x24  (Data Abort from lower EL)
│    WnR  = 0     (讀取/取指造成)
│    CM   = 0     ISV = 0
│    DFSC = 0x04  Translation fault, level 0
└─
```

**`FAR_EL1` 精準等於程式存取的位址，`ESR_EL1` 一位一位都能解碼。**

---

<a name="q35"></a>
## 35. 如何知道缺頁是因為讀還是寫？

### 結論

**看 `ESR_EL1` 的 bit 6（`WnR`, Write not Read）**：

- `WnR = 1` → **寫**造成
- `WnR = 0` → **讀**或**取指**造成

核心的用法（`arch/arm64/mm/fault.c` `do_page_fault()`）：

```c
if (is_write_abort(esr)) {                     /* esr & ESR_ELx_WNR && !esr & ESR_ELx_CM */
        vm_flags = VM_WRITE;
        mm_flags |= FAULT_FLAG_WRITE;
} else if (is_el0_instruction_abort(esr)) {
        vm_flags = VM_EXEC;
} else {
        vm_flags = VM_READ | VM_WRITE | VM_EXEC;   /* 讀 */
}
```

**這個位元決定了三件事**：
1. 檢查 VMA 權限時要比對 `VM_WRITE` 還是 `VM_READ`
2. 匿名頁缺頁時，**讀 → 給 ZERO_PAGE；寫 → 配新頁**（見 [Q39](#q39)）
3. 唯讀 PTE 上發生寫 → 走 **COW** 路徑（見 [Q41](#q41)）

> **書目**：奔跑吧 §4.7.2「do_page_fault() 函數」。

### 實機驗證（★ 同一個位址，只差讀/寫，ESR 只差 bit 6）

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./esr_far | head -25'
```

```
┌─ 讀一個沒有任何 VMA 的位址 0x123456789000
│  ESR_EL1 = 0x0000000092000004
│    WnR  = 0     (讀取/取指造成)
│    DFSC = 0x04  Translation fault, level 0
└─
┌─ 寫一個沒有任何 VMA 的位址 0x123456789000
│  ESR_EL1 = 0x0000000092000044
│    WnR  = 1     (寫入造成)
│    DFSC = 0x04  Translation fault, level 0
└─
```

> ### 🎯 **`0x92000004` vs `0x92000044` —— 只差 `0x40`，正好是 bit 6。**
> 同一個位址、同樣的錯誤碼（DFSC=0x04），**唯一的差別就是 WnR 這一個位元**。

「讀 vs 寫」造成的行為差異，在 `fault_types` 裡也看得很清楚：

```
只讀掃過 16MB 匿名頁     minor=4096  RSS 變化 = 0 頁      ← 讀 → 全指向 ZERO_PAGE
寫入 16MB 匿名頁         minor=4096  RSS 從 186 → 4381    ← 寫 → 真的配了 4096 頁
```

**同樣 4096 次缺頁，讀不配頁、寫才配頁。**

---

<a name="q36"></a>
## 36. 如何判斷發生異常的位址是可以修復的還是不能修復的？

### 結論

`do_page_fault()` 的判斷順序（`arch/arm64/mm/fault.c`）：

```
① 位址在核心空間，但錯誤發生在 EL0？        → 不可修復（送 SIGSEGV）
② 在中斷上下文 / 沒有 mm（核心執行緒）？     → 不可修復（走 __do_kernel_fault）
③ find_vma(mm, addr) 找不到 VMA？           → 不可修復，SEGV_MAPERR
④ 找到了但 addr < vma->vm_start：
     VMA 有 VM_GROWSDOWN 且是堆疊？          → 可修復（expand_stack）
     否則                                    → 不可修復，SEGV_MAPERR
⑤ VMA 權限與 vm_flags 不符？                → 不可修復，SEGV_ACCERR
⑥ 以上都過 → handle_mm_fault()              → ★ 可修復
     VM_FAULT_OOM     → OOM killer
     VM_FAULT_SIGBUS  → SIGBUS（例如超出檔案結尾）
     VM_FAULT_SIGSEGV → SIGSEGV
     成功             → 建立 PTE，返回使用者態重新執行那條指令
```

**核心態發生的缺頁**還有一條救命路徑：**exception table**（`__ex_table`）。
`copy_from_user()` 這類函式在存取指令旁邊登記了「若出錯就跳到這裡」的修復位址，
`fixup_exception()` 找得到就修復（回傳 `-EFAULT`），找不到才 Oops。

> **書目**：奔跑吧 §4.7.2「do_page_fault() 函數」、§4.7.13「小結」。

### 實機驗證（★ 六種不可修復 vs 可修復的對照）

**不可修復**（`esr_far` 全部觸發 SIGSEGV）：

| 情境 | `si_code` | `DFSC` | 判在哪一步 |
|------|-----------|--------|-----------|
| 讀沒有 VMA 的位址 | `SEGV_MAPERR`(1) | 0x04 Translation L0 | ③ |
| 寫沒有 VMA 的位址 | `SEGV_MAPERR`(1) | 0x04 Translation L0 | ③ |
| **寫 `PROT_READ` 的映射** | **`SEGV_ACCERR`(2)** | **0x0f Permission L3** | ⑤ |
| **讀 `PROT_NONE` 的映射** | **`SEGV_ACCERR`(2)** | 0x07 Translation L3 | ⑤ |
| 解參考 NULL | `SEGV_MAPERR`(1) | 0x04 Translation L0 | ③ |
| 使用者態讀核心位址 | `SEGV_MAPERR`(1) | 0x04 Translation L0 | ① |

> 🎯 **`si_code` 精準區分了兩類**：
> `SEGV_MAPERR` = 「**根本沒有 VMA**」（步驟 ③）；
> `SEGV_ACCERR` = 「**有 VMA 但權限不符**」（步驟 ⑤）。
> 這正好對應書上「不能修復」的兩種子情況。

**可修復**（`fault_types` 全部靜靜完成，沒有任何訊號）：

```
寫入 16MB 匿名頁          minor=4096  major=0     ← 正常 demand paging
讀 16MB 檔案映射（cache 熱）minor=256   major=0     ← page cache 命中
讀 16MB 檔案映射（cache 冷）minor=128   major=129   ← 讀磁碟
[子] fork 後寫 16MB        minor=4096  major=0     ← COW
```

**核心態的可修復**：`copy_from_user()` 碰到壞位址時靠 exception table 修復：

```bash
ssh radxa@192.168.68.57 'sudo grep -w "__ex_table\|fixup_exception" /proc/kallsyms | head -3'
```

---

<a name="q37"></a>
## 37. do_page_fault() 處理過程中需要考慮哪些情況？

### 結論

依 `arch/arm64/mm/fault.c` 的實際順序：

| # | 檢查 | 處理 |
|---|------|------|
| 1 | `kprobe_page_fault()` | 讓 kprobe 先看一眼 |
| 2 | **`is_el1_permission_fault()` 且核心存取使用者位址** | **PAN 保護**：核心誤存取使用者空間 → Oops |
| 3 | `faulthandler_disabled() \|\| !mm` | 原子上下文/核心執行緒 → `__do_kernel_fault()` |
| 4 | `user_mode(regs)` | 設 `FAULT_FLAG_USER` |
| 5 | `is_write_abort(esr)` | 決定 `vm_flags` = `VM_WRITE`/`VM_EXEC`/`VM_READ` |
| 6 | **`search_exception_tables()`** | 核心態存取使用者位址前要能修復 |
| 7 | `mmap_read_trylock()` 失敗 | 可能要 `might_sleep()`，重試 |
| 8 | `find_vma()` + 邊界檢查 | 找不到 → SEGV_MAPERR；堆疊 → `expand_stack()` |
| 9 | `vma->vm_flags & vm_flags` | 權限不符 → SEGV_ACCERR |
| 10 | **`handle_mm_fault()`** | 真正的處理 |
| 11 | `VM_FAULT_RETRY` | **釋放 `mmap_lock` 去等 I/O，然後重來**（避免長時間持鎖） |
| 12 | `VM_FAULT_OOM/SIGBUS/SIGSEGV` | 對應的訊號/OOM |

**幾個容易漏掉的**：
- **PAN（Privileged Access Never）**：ARM64 的硬體保護，`CONFIG_ARM64_PAN=y`（本機有），
  核心預設不能存取使用者位址，要用 `uaccess_enable()` 短暫開窗
- **`VM_FAULT_RETRY`**：major fault 要等磁碟時，先放掉 `mmap_lock` 再等，
  否則整個行程的位址空間會被卡住
- **`FAULT_FLAG_ALLOW_RETRY`** 只給第一次，避免無限重試

> **書目**：奔跑吧 §4.7.2「do_page_fault() 函數」、§4.7.12「缺頁異常引發的死鎖」。

### 實機驗證

```bash
sed -n '/^static int __kprobes do_page_fault/,/^}/p' arch/arm64/mm/fault.c | head -60
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep -E "ARM64_PAN|ARM64_SW_TTBR0_PAN"'
```

```
CONFIG_ARM64_PAN=y            ← 硬體 PAN 已啟用
```

**PAN 生效的證明**：`esr_far` 裡「使用者態讀核心位址」拿到 SEGV，
反過來「核心態讀使用者位址」在沒有 `uaccess_enable()` 時會 Oops——
這就是第 2 步在擋的東西。

---

<a name="q38"></a>
## 38. 主缺頁（major fault）和次缺頁（minor fault）有什麼區別？

### 結論

| | **minor fault（次缺頁）** | **major fault（主缺頁）** |
|---|---|---|
| 定義 | **不需要磁碟 I/O** 就能解決 | **必須發起磁碟 I/O** |
| 典型情境 | 匿名頁首次寫、page cache 命中、COW、fault-around | page cache miss 要讀檔案、從 swap 讀回 |
| 代價 | ~1 µs（配頁 + 建 PTE） | **~100 µs ~ 10 ms**（NVMe/eMMC 的延遲） |
| 計數 | `ru_minflt`、`/proc/vmstat: pgfault` | `ru_majflt`、`/proc/vmstat: pgmajfault` |
| 核心標記 | — | `ret \|= VM_FAULT_MAJOR; count_vm_event(PGMAJFAULT)` |

**差距有多大**：[Ch3 Q2](./ch03_memory_management_prerequisites.md#q2) 量到
DRAM 延遲 ~240 ns，而 NVMe 隨機讀是 50~100 µs——**200~400 倍**。
所以 major fault 是效能分析時最該盯的指標。

> **書目**：奔跑吧 §4.7「缺頁中斷」、§4.7.6「文件映射缺頁中斷」。

### 實機驗證（★）

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./fault_types | sed -n "/Q40/,/COW/p"'
```

```
--- [Q40] 檔案映射缺頁（page cache 命中 = minor）---
讀 16MB 檔案映射（cache 熱）    minor=256   major=0

--- [Q38] 主缺頁 major fault（page cache 被丟掉，要讀磁碟）---
讀 16MB 檔案映射（cache 冷）    minor=128   major=129
```

**同一個檔案、同樣讀 16 MB，只差有沒有先 `POSIX_FADV_DONTNEED`：**
- cache 熱：**major = 0**
- cache 冷：**major = 129**

> ### 🎯 為什麼只有 256 次缺頁而不是 4096 次？—— **fault-around**
>
> ```bash
> ssh radxa@192.168.68.57 'sudo cat /sys/kernel/debug/fault_around_bytes'
> # 65536
> ```
>
> `fault_around_bytes = 65536` = **16 頁**。核心的 `do_fault_around()`
> 在處理**唯讀檔案映射**的缺頁時，會**順便把周圍 16 頁的 PTE 一起建好**
> （只要那些頁已經在 page cache 裡）。
>
> **4096 / 16 = 256** —— 與實測的 `minor=256` 完全吻合 ✅
>
> 對照組：**匿名頁的缺頁沒有 fault-around**（`do_anonymous_page()` 沒這機制），
> 所以是紮紮實實的 4096 次。

**swap 造成的 major fault**（`reclaim_test`）：

```
重新讀一遍：pgmajfault +65529   pswpin +65529 頁
```

**65,529 次 major fault 精準等於 65,529 個從 swap 讀回的頁** ✅

---

<a name="q39"></a>
## 39. 匿名頁面缺頁異常的判斷條件是什麼？

### 結論

**判斷條件**（`handle_pte_fault()`，`mm/memory.c`）：

```c
if (!vmf->pte) {                                  /* PTE 表都還沒建 */
        if (vma_is_anonymous(vmf->vma))           /* ★ vma->vm_ops == NULL */
                return do_anonymous_page(vmf);    /* → 匿名頁缺頁 */
        else
                return do_fault(vmf);             /* → 檔案映射缺頁 */
}
```

**`vma_is_anonymous()` 的判定就是 `vma->vm_ops == NULL`** ——
匿名 VMA 沒有 `vm_ops`（沒有檔案可以呼叫 `->fault()`）。

**`do_anonymous_page()` 內部再分讀/寫**：

```c
/* 讀缺頁：給共用的唯讀零頁，不配實體頁 */
if (!(vmf->flags & FAULT_FLAG_WRITE) && !mm_forbids_zeropage(vma->vm_mm)) {
        entry = pte_mkspecial(pfn_pte(my_zero_pfn(vmf->address), vma->vm_page_prot));
        goto setpte;
}
/* 寫缺頁：真的配一個歸零的可遷移頁 */
folio = vma_alloc_zeroed_movable_folio(vma, vmf->address);
```

> **書目**：奔跑吧 §4.7.4「匿名頁面缺頁中斷」、§4.7.5「系統零頁」。

### 實機驗證（★ 讀 vs 寫的巨大差異）

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./fault_types | sed -n "/Q39/,/Q40/p"'
```

```
--- [Q39] 匿名頁「寫」缺頁：每頁一次 minor fault ---
寫入 16MB 匿名頁              minor=4096  major=0
RSS = 4381 頁                                       ← 真的配了 4096 個實體頁

--- [Q39 變體] 匿名頁「只讀」缺頁：共用 ZERO_PAGE，不佔 RSS ---
只讀掃過 16MB 匿名頁          minor=4096  major=0
RSS 變化 = 0 頁                                      ← ★ 一個實體頁都沒配！
```

> ### 🎯 **同樣 4096 次缺頁，RSS 差了 4096 頁（16 MB）。**
> 讀缺頁全部指向同一個 `ZERO_PAGE`（PFN `0x260b`，見 [Q27](#q27)），
> 寫缺頁才真的向伙伴系統要頁。
>
> 這就是為什麼 `calloc()` 大陣列後如果只讀不寫，**根本不吃記憶體**。

---

<a name="q40"></a>
## 40. 檔案映射頁面的缺頁異常判斷條件是什麼？

### 結論

**判斷條件**：`vma->vm_ops != NULL`（有檔案的 `vm_operations_struct`），
進入 `do_fault()`，再依映射類型三分：

```c
static vm_fault_t do_fault(struct vm_fault *vmf)
{
        if (!vma->vm_ops->fault)             ret = ...;           /* 沒有 ->fault */
        else if (!(vmf->flags & FAULT_FLAG_WRITE))
                ret = do_read_fault(vmf);    /* ① 唯讀 → 直接用 page cache 的頁 */
        else if (!(vma->vm_flags & VM_SHARED))
                ret = do_cow_fault(vmf);     /* ② 私有寫 → COW，複製一份 */
        else
                ret = do_shared_fault(vmf);  /* ③ 共享寫 → 直接改 page cache，標髒 */
}
```

| 路徑 | 條件 | 做什麼 |
|------|------|--------|
| `do_read_fault()` | 讀 | `__do_fault()` 從 page cache 拿頁 → 直接映射（唯讀）。**會做 fault-around** |
| `do_cow_fault()` | 寫 + `MAP_PRIVATE` | 拿 page cache 的頁 + **配一個新頁複製過去** → 映射私有副本 |
| `do_shared_fault()` | 寫 + `MAP_SHARED` | 直接映射 page cache 的頁（可寫）→ `set_page_dirty()` → 之後會回寫 |

`__do_fault()` → `vma->vm_ops->fault()` → 對 ext4 是 `filemap_fault()`：
page cache 命中 → **minor**；miss → `page_cache_ra_*()` 發起讀取 → **major**。

> **書目**：奔跑吧 §4.7.6「文件映射缺頁中斷」。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./fault_types | sed -n "/Q40/,/Q38/p"'
```

```
讀 16MB 檔案映射（cache 熱）    minor=256   major=0      ← do_read_fault + fault-around
讀 16MB 檔案映射（cache 冷）    minor=128   major=129    ← 同上但 page cache miss
```

**三條路徑的完整對照**（`fault_types` 的最後兩節）：

| 路徑 | 實驗 | 結果 |
|------|------|------|
| `do_read_fault()` | `mmap(PROT_READ, MAP_PRIVATE)` 讀 | minor=256（fault-around 生效） |
| `do_cow_fault()` | `mmap(PROT_RW, MAP_PRIVATE)` 寫 | minor=1，**檔案內容不變（0xAB）** |
| `do_shared_fault()` | `mmap(PROT_RW, MAP_SHARED)` 寫 | **檔案內容變成 0x5a** |

**`do_cow_fault` 與 `do_shared_fault` 的差別直接反映在檔案內容上** ✅

---

<a name="q41"></a>
## 41. 什麼是寫時複製類型的缺頁異常？判斷條件是什麼？

### 結論

**COW 缺頁 = 「PTE 存在且有效，但沒有寫權限，而這次是寫存取」。**

判斷條件（`handle_pte_fault()`）：

```c
if (vmf->flags & FAULT_FLAG_WRITE) {
        if (!pte_write(entry))              /* ★ PTE 存在但唯讀 */
                return do_wp_page(vmf);     /* → COW */
        else if (likely(vmf->flags & FAULT_FLAG_WRITE))
                entry = pte_mkdirty(entry);
}
```

**注意與 [Q39](#q39)/[Q40](#q40) 的區別**：
- Q39/Q40 是 **PTE 不存在**（`pte_none()`）→ Translation fault
- Q41 是 **PTE 存在但唯讀** → **Permission fault**

在 ARM64 的 `ESR_EL1` 上，兩者的 `DFSC` 完全不同（見 [Q34](#q34)）：
- Translation fault：`DFSC = 0b0001LL`
- **Permission fault：`DFSC = 0b0011LL`** ← COW 走這條

**COW 產生的三種來源**：
1. `fork()` 後父子共享的匿名頁（`copy_page_range()` 把雙方都設唯讀）
2. `MAP_PRIVATE` 的檔案映射被寫
3. **KSM 合併後的頁被寫**（見 [Ch5 Q37](./ch05_memory_management_advanced_topics.md#q37)）

> **書目**：奔跑吧 §4.7.7「寫時複製」。

### 實機驗證（★ Permission fault 的 ESR）

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./esr_far | sed -n "/PROT_READ/,/└/p"'
```

```
┌─ 捕捉到 SIGSEGV：寫一段 PROT_READ 的映射（有 VMA，權限不符）
│  si_addr (<- FAR_EL1) = 0x0000ffff89556000
│  si_code              = 2 (SEGV_ACCERR 有 VMA 但權限不符)
│  ESR_EL1              = 0x000000009200004f
│    WnR  = 1     (寫入造成)
│    DFSC = 0x0f  Permission fault, level 3    ← ★ COW 走的就是這種
└─
```

> 🎯 **`DFSC = 0x0f` = Permission fault level 3** ——
> 這正是 COW 缺頁在硬體層面的樣子。差別只在於：
> 這個例子的 VMA 本身就沒有 `VM_WRITE`（所以核心判定不可修復、送 SIGSEGV）；
> 而真正的 COW 是 **VMA 有 `VM_WRITE` 但 PTE 被刻意設成唯讀**，
> 核心一看就知道「這是我自己設的，該做複製了」。

**COW 的完整計數**（`fault_types`）：

```
父行程 fork 前 RSS = 4395 頁
[子] fork 後【讀】16MB（不觸發 COW）  minor=1      RSS 變化 = 0 頁
[子] fork 後【寫】16MB（每頁 COW）    minor=4096   ← 4096 次 Permission fault
```

**讀 1 次、寫 4096 次** —— 完美對應「只有寫才觸發 COW」。

---

<a name="q42"></a>
## 42. 寫時複製中，什麼情況下複用頁面、什麼情況下真的複製？

### 結論

**看 `_mapcount`（有幾個 PTE 指向這個頁）**：

```c
/* mm/memory.c do_wp_page() */
static vm_fault_t do_wp_page(struct vm_fault *vmf)
{
        ...
        if (folio && folio_test_anon(folio)) {
                if (!folio_test_ksm(folio) &&
                    (... wp_can_reuse_anon_folio(folio, vma))) {
                        /* ★ 只剩我一個用 → 直接改成可寫，不複製 */
                        wp_page_reuse(vmf);
                        return 0;
                }
        }
        /* 否則 → 真的複製 */
        return wp_page_copy(vmf);
}
```

`wp_can_reuse_anon_folio()` 的核心判斷是
**`folio_mapcount(folio) == 1` 且 refcount 也沒有別人持有**。

| 情況 | `_mapcount` | 走哪條 | 代價 |
|------|------------|--------|------|
| fork 後父子都在，任一方寫 | ≥ 2 | **`wp_page_copy()`** | 配新頁 + memcpy 4 KB |
| 子行程已退出，父行程再寫 | **1** | **`wp_page_reuse()`** | **只改 PTE 位元，不配頁不複製** |
| KSM 合併的頁被寫 | 任意 | **一定 copy**（`folio_test_ksm()` 擋住 reuse） | 配新頁 |

> **書目**：奔跑吧 §4.7.7「寫時複製」、§4.7.10「關於寫時複製的競爭問題」。

### 實機驗證（★ reuse vs copy 的直接對照）

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./fault_types | sed -n "/COW/,/MAP_PRIVATE/p"'
```

```
父行程 fork 前 RSS = 4395 頁

[子] fork 後【讀】16MB（不觸發 COW）      minor=1      major=0
[子] RSS 變化 = 0 頁
[子] fork 後【寫】16MB（每頁 COW=複製）   minor=4096   major=0     ← ★ copy 路徑

[父] 子行程結束後第 1 次寫（reuse 不複製） minor=4096   major=0
[父] RSS 變化 = 21 頁  <- 幾乎 0 就證明是 reuse 而非 copy ★
[父] 第 2 次寫（PTE 已可寫）              minor=0      major=0
  ^ 0 次缺頁：do_wp_page() 已把 PTE 改成可寫
```

**三段對照把 Q42 講完了**：

| 階段 | 缺頁次數 | RSS 變化 | 走哪條 |
|------|---------|---------|--------|
| 子行程寫（父子都在，mapcount=2） | 4096 | — | **`wp_page_copy()`** |
| 父行程寫（子已退出，mapcount=1） | **4096** | **+21 頁（雜訊）** | **`wp_page_reuse()`** ★ |
| 父行程再寫（PTE 已可寫） | **0** | 0 | 完全不缺頁 |

> ### 🎯 **關鍵證據**：第 2 段「發生了 4096 次缺頁，卻幾乎沒有配任何新記憶體」。
> 這就是 `wp_page_reuse()` —— **缺頁照樣發生（硬體必須報 Permission fault），
> 但核心一看 mapcount=1 就直接把 PTE 改成可寫，省掉 16 MB 的 memcpy。**

**smaps 的視角**（Ch7 的 `cow.c` 實驗）：

```
[parent] after memset (pre-fork)   Shared_Dirty: 0      Private_Dirty: 65536
[child ] right after fork          Shared_Dirty: 65536  Private_Dirty: 0
[child ] after WRITE sweep (COW)   Shared_Dirty: 0      Private_Dirty: 65536
```

**`Shared_Dirty` ↔ `Private_Dirty` 的來回轉換，就是 COW 的可視化。**

---

<a name="q43"></a>
## 43. ARMv8.1 硬體 DBM 機制下，如何避免軟體和 CPU 同時更新 DBM 位與 PTE_RDONLY？

### 結論

**背景**：ARMv8.1 的 **DBM（Dirty Bit Modifier）** 讓硬體自己維護「髒」狀態：
- PTE 帶 `DBM=1` 且 `AP[2]=1`（唯讀）時，
- CPU 執行寫入 → **硬體自動把 `AP[2]` 清成 0（變可寫）**，不觸發異常

這帶來一個競爭：**軟體正在改 PTE 的同時，硬體可能也在改同一個 PTE**。

```
   軟體（如 ptep_set_wrprotect 要設唯讀）      硬體（DBM 自動清 AP[2]）
   ─────────────────────────────────         ─────────────────────
   1. 讀出 pte                                 
                                               2. 寫入發生 → 清掉 AP[2]
   3. 寫回「修改過的 pte」                        ← 硬體的更新被覆蓋掉！
                                                  頁面實際被寫髒，但 PTE 說它乾淨
                                                  → 資料遺失
```

**解法：用 LDXR/STXR 的獨佔存取做原子的 read-modify-write**。
ARM64 的 `ptep_set_wrprotect()`（`arch/arm64/include/asm/pgtable.h`）：

```c
static inline void ptep_set_wrprotect(struct mm_struct *mm, unsigned long address,
                                      pte_t *ptep)
{
        pte_t old_pte, pte;

        pte = READ_ONCE(*ptep);
        do {
                old_pte = pte;
                pte = pte_wrprotect(pte);
                pte_val(pte) = cmpxchg_relaxed(&pte_val(*ptep),
                                               pte_val(old_pte), pte_val(pte));
                                               /* ★ cmpxchg：硬體改過就重來 */
        } while (pte_val(pte) != pte_val(old_pte));
}
```

`cmpxchg` 若發現 `*ptep` 已經不是讀出來的那個值（硬體改過了），
就**重讀、重算、重試**，直到成功——這樣硬體的更新永遠不會被覆蓋。

同理 `ptep_get_and_clear()`、`ptep_test_and_clear_young()` 也都用原子操作。

> **書目**：奔跑吧 §4.7.8「ARM64 硬體 DBM 機制導致的競爭問題」。

### 實機驗證

**(a) 確認本機硬體支援且核心啟用了 DBM**：

```bash
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep ARM64_HW_AFDBM'
```

```
CONFIG_ARM64_HW_AFDBM=y        ← Hardware Access Flag and Dirty Bit Management
```

**(b) 在頁表裡直接看到 `DBM` 位**（`armv8_dump.ko` 巡覽線性映射區）：

```
walk: PAGE_OFFSET+2MB (實體記憶體線性映射區)
   L3 desc = 0x0068000000200707
        AttrIndx=1  AP=0(EL1 RW, EL0 none)  SH=3(Inner Shareable)
        AF=1  nG=0  DBM=1  Contig=0  PXN=1  UXN=1
                    ↑★ 硬體 DBM 已啟用
```

**對照組**（唯讀的核心程式碼段）：

```
walk: _stext
   L3 desc = 0x0050000000410783
        AP=2(EL1 RO, EL0 none)  AF=1  nG=0  DBM=0  ← 唯讀頁不需要 DBM
```

**`DBM=1` 出現在可寫的線性映射頁上、`DBM=0` 在唯讀的程式碼頁上** ✅
——證明核心確實在用硬體 DBM。

**(c) 原始碼佐證**：

```bash
grep -n -B2 -A14 "^static inline void ptep_set_wrprotect" arch/arm64/include/asm/pgtable.h
```

---

<a name="q44"></a>
## 44. 什麼情況下可以安全地呼叫 pte_offset_map()？什麼情況下不行？

### 結論

`pte_offset_map(pmd, addr)` 做兩件事：
1. 從 `pmd` 取出 PTE 表的實體位址 → `__va()` 換成虛擬位址（見 [Ch2 Q23](./ch02_arm64_in_linux_kernel.md#q23)）
2. **在 32 位 highmem 系統上還要 `kmap_atomic()`**（所以有配對的 `pte_unmap()`）

**危險在於「取出 pmd 值」與「使用它」之間，pmd 可能被別人改掉**：
- THP 的 collapse/split 會把 pmd 從「指向 PTE 表」變成「巨頁 block」或清空
- `munmap` / `free_pgtables()` 會把整張 PTE 表釋放掉

**安全條件（三選一）**：

| 條件 | 說明 |
|------|------|
| **持有 `mmap_lock` 寫鎖** | 沒人能同時改 VMA/頁表 |
| **持有 `mmap_lock` 讀鎖 + 該 pmd 穩定** | 需要先 `pmd_trans_unstable()` / `pmd_none_or_trans_huge_or_clear_bad()` 檢查 |
| **持有 `pmd_lock()` 或 page table lock** | 針對該 pmd 的細粒度鎖 |

**核心的標準寫法**（`handle_pte_fault()` 之前）：

```c
/* mm/memory.c __handle_mm_fault() */
vmf.pmd = pmd_alloc(mm, vmf.pud, address);
...
/* ★ 先確認 pmd 是穩定的，才敢往下走 */
if (pmd_trans_unstable(vmf.pmd))
        return 0;                    /* 不穩定就直接返回，讓上層重試 */
```

以及 6.1 的 `handle_pte_fault()`：

```c
if (unlikely(pmd_none(*vmf->pmd))) {
        vmf->pte = NULL;             /* PTE 表還沒建 */
} else {
        if (pmd_devmap_trans_unstable(vmf->pmd))
                return 0;            /* ★ 不穩定，放棄這一輪 */
        vmf->pte = pte_offset_map(vmf->pmd, vmf->address);
        vmf->orig_pte = *vmf->pte;
        barrier();                   /* ★ 防止編譯器重排 */
        ...
}
```

**本機的簡化**：`# CONFIG_TRANSPARENT_HUGEPAGE is not set`，
所以沒有 THP collapse/split 的競爭，`pmd_trans_unstable()` 恆為 false。
但 `munmap` 併發釋放頁表的風險仍在，所以鎖還是要拿。

> **書目**：奔跑吧 §4.7.9「關於 pte_offset_map() 安全使用的問題」。

### 實機驗證

```bash
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep -E "TRANSPARENT_HUGEPAGE"'
```

```
# CONFIG_TRANSPARENT_HUGEPAGE is not set     ← 本機沒有 THP
```

```bash
grep -n -B3 -A12 "pmd_devmap_trans_unstable" mm/memory.c | head -25
grep -n "pte_offset_map\b" mm/memory.c | head
```

**我自己寫的模組也遵守了這個規則**（`mm_convert.c`）：

```c
mmap_read_lock(mm);                       /* ★ 先拿 mmap_lock 讀鎖 */
...
pte = pte_offset_map(pmd, va);
...
pte_unmap(pte);
mmap_read_unlock(mm);
```

而在**核心空間**（`armv8_dump.c` 巡覽 `swapper_pg_dir`）用的是
`pte_offset_kernel()`——核心頁表不會被併發拆除，所以不需要 kmap、也不需要 unmap。

---

<a name="q45"></a>
## 45. 切換新的頁表項之前，為什麼要先對頁表項清零並刷新 TLB？

### 結論

**為了關掉「舊 PTE 還在 TLB 裡」與「新頁面已經生效」之間的競爭視窗。**

以 `wp_page_copy()`（COW）為例，錯誤的做法：

```
  CPU0（做 COW）                         CPU1（同一行程的另一個執行緒）
  ─────────────────────                 ─────────────────────────
  1. 配新頁 new_page
  2. copy_user_page(old → new)
                                         3. 用【TLB 裡的舊 PTE】寫 old_page
                                            ★ 這次寫入寫進了舊頁！
  4. set_pte_at(new_page)                
  5. flush_tlb_page()
                                         6. 之後讀 new_page → 讀不到步驟 3 的資料
                                            ★ 資料遺失
```

**正確的做法**（`mm/memory.c` `wp_page_copy()`）：

```c
ptep_clear_flush_notify(vma, vmf->address, vmf->pte);
/*  ^^^^^^^^^^^^^^^^^^  ① 先把 PTE 清成 0
 *                      ② 立刻 flush TLB
 *   -> 從這一刻起，任何 CPU 存取這個位址都會【缺頁】，被擋在 pte lock 外面
 */
folio_add_new_anon_rmap(new_folio, vma, vmf->address);
folio_add_lru_vma(new_folio, vma);
set_pte_at_notify(mm, vmf->address, vmf->pte, entry);   /* ③ 才寫入新 PTE */
```

**關鍵**：清零 + flush 之後，這個位址就進入「PTE 無效」的狀態，
其他 CPU 再存取只會缺頁、然後卡在 `pte lock` 上等待——
**不可能再摸到舊頁**。等新 PTE 寫好、放鎖，它們才會看到新的映射。

**同樣的模式在 `ptep_get_and_clear()` 家族到處出現**：
`try_to_unmap_one()`（回收）、`migrate_page_move_mapping()`（遷移）、
`change_pte_range()`（mprotect）都是「先清 + flush，再設新值」。

> **書目**：奔跑吧 §4.7.11「為什麼要在切換頁表項之前刷新 TLB」。

### 實機驗證

```bash
grep -n -A20 "ptep_clear_flush_notify" mm/memory.c | head -30
grep -n "define ptep_clear_flush\b" -A12 include/asm-generic/tlb.h mm/pgtable-generic.c 2>/dev/null | head -20
```

**ARM64 的 TLB 失效指令**（`arch/arm64/include/asm/tlbflush.h`）：

```c
static inline void __flush_tlb_page_nosync(struct mm_struct *mm, unsigned long uaddr)
{
        unsigned long addr;
        dsb(ishst);                                  /* ① 確保 PTE 的寫入已完成 */
        addr = __TLBI_VADDR(uaddr, ASID(mm));
        __tlbi(vale1is, addr);                       /* ② Inner Shareable 廣播失效 */
        __tlbi_user(vale1is, addr);
        mmu_notifier_arch_invalidate_secondary_tlbs(mm, uaddr & PAGE_MASK,
                                                    (uaddr & PAGE_MASK) + PAGE_SIZE);
}
```

**三個細節值得講**：
1. **`dsb(ishst)` 在 `tlbi` 之前**：確保 PTE 的寫入對 MMU table walker 可見，
   否則 walker 可能還讀到舊值（見 [Ch2 Q17](./ch02_arm64_in_linux_kernel.md#q17)）
2. **`vale1is` 的 `is` = Inner Shareable**：**一條指令廣播到所有 8 個核**，
   不需要 IPI。這是 ARM64 相對 x86 的巨大優勢
   （x86 要靠 IPI 做 TLB shootdown）
3. **`ASID(mm)`**：只失效這個行程的表項，不影響別人（見 [Ch2 Q14](./ch02_arm64_in_linux_kernel.md#q14)）

本機還支援 **TLB range 失效**：

```bash
ssh radxa@192.168.68.57 'zcat /proc/config.gz | grep ARM64_TLB_RANGE'
# CONFIG_ARM64_TLB_RANGE=y     ← 一條指令失效一段連續位址（ARMv8.4 的 TLBI RANGE）
```

---

<a name="q46"></a>
## 46. 多核 SMP 系統中，多個 CPU 是否可能同時對同一個頁面發生缺頁異常？

### 結論

**完全可能，而且很常見。** 典型場景：

```
   同一個行程的兩個執行緒，跑在 CPU0 和 CPU1，同時第一次寫同一個匿名頁
   ────────────────────────────────────────────────────────────
   CPU0: do_anonymous_page()          CPU1: do_anonymous_page()
     配頁 A                             配頁 B
     ↓                                  ↓
     搶 pte lock ─────【贏】             搶 pte lock ──【等待】
     檢查 !pte_none(*pte) → 是 none      
     set_pte_at(A)                      
     放鎖                                拿到鎖
                                        ★ 再檢查一次：!pte_none(*pte) → 已經有了！
                                        → 把自己配的 B 丟掉，直接用 A
```

**核心的三道防線**：

1. **`pte lock`（`pte_offset_map_lock()`）**：per-PMD 的 spinlock，
   保證「檢查 + 設定 PTE」是原子的
2. **拿到鎖後【一定要再檢查一次】**：
   ```c
   /* mm/memory.c do_anonymous_page() */
   vmf->pte = pte_offset_map_lock(vma->vm_mm, vmf->pmd, vmf->address, &vmf->ptl);
   if (!pte_none(*vmf->pte)) {          /* ★ double-check */
           update_mmu_tlb(vma, vmf->address, vmf->pte);
           goto release;                /* 別人已經做好了，丟掉自己配的頁 */
   }
   ```
3. **`mmap_lock` 讀鎖**：保證 VMA 結構在處理期間不會被拆掉

**還有一種更微妙的情況**：`FAULT_FLAG_ALLOW_RETRY`。
major fault 要等磁碟時，會**先放掉 `mmap_lock`** 去等；
回來時世界可能已經變了，所以要帶 `FAULT_FLAG_TRIED` 重新走一遍完整流程。

> **書目**：奔跑吧 §4.7.10「關於寫時複製的競爭問題」、§4.7.12「缺頁異常引發的死鎖」。

### 實機驗證

```bash
grep -n -A8 "pte_offset_map_lock" mm/memory.c | grep -B2 -A6 "pte_none" | head -20
```

**併發缺頁的實際計數**（`update_mmu_tlb` 在「白做工」時被呼叫）：

```bash
ssh radxa@192.168.68.57 'grep -E "^(pgfault|thp_fault_fallback)" /proc/vmstat'
```

**做一個真正的併發缺頁測試**（`experiments/concurrent_fault.c`）：
8 個執行緒用 barrier 同步後**同時**寫同一塊 64 MB 匿名記憶體。

```bash
ssh radxa@192.168.68.57 'cd ~/exp && gcc -O2 -w -o concurrent_fault concurrent_fault.c -lpthread
  echo "=== 8 執行緒跑在 8 顆 CPU 上 ==="; ./concurrent_fault
  echo "=== 同一支程式綁在單一 CPU（對照組）==="; taskset -c 4 ./concurrent_fault'
```

```
=== 8 執行緒跑在 8 顆 CPU 上 ===
8 執行緒同時寫 64 MB（共 16384 頁）
實際 minor fault = 116913
單執行緒理論值   = 16384
多出來 = 100529 次（同時缺頁、被 double-check 擋掉的白工）

=== 同一支程式綁在單一 CPU（對照組）===
實際 minor fault = 16857
多出來 = 473 次
```

> ### 🎯 **8 顆 CPU：116,913 次缺頁；1 顆 CPU：16,857 次。差了 7 倍。**
>
> | | 缺頁次數 | 「必要」的 | **白工** |
> |---|---------|-----------|---------|
> | 8 CPU 併發 | **116,913** | 16,384 | **100,529（86%）** |
> | 1 CPU（同樣 8 執行緒） | 16,857 | 16,384 | 473（2.8%） |
>
> **86% 的缺頁是白工**——8 顆 CPU 同時對同一頁缺頁，
> 每顆都各自 `alloc_page()`、然後搶 `pte lock`，
> 只有一個贏家把頁裝上去，其餘 7 個在 double-check
> （`if (!pte_none(*vmf->pte)) goto release;`）發現「已經有人做好了」，
> 把自己剛配的頁**丟回伙伴系統**。
>
> **資料完全正確**（程式沒有任何錯誤），代價只是浪費 CPU——
> 這正是 `pte lock` + double-check 這個設計的效果：
> **用「白做工」換「絕不出錯」，而且完全不需要昂貴的全域鎖。**
>
> 對照組把 8 個執行緒擠在同一顆 CPU 上，它們就變成分時執行、
> 幾乎不會真正同時進入缺頁路徑，白工率掉到 2.8%。

---

## 附錄：本章實驗程式

| 檔案 | 用途 | 題目 |
|------|------|------|
| `experiments/mm_probe.c` | 核心模組：gfp_zone 表、zonelist、水位、伙伴 free_area、kmalloc vs vmalloc、SLUB、page->flags | Q1-Q7, Q8-Q9, Q13, Q18, Q27 |
| `experiments/fault_types.c` | 五種缺頁的精準計數（含 fault-around、ZERO_PAGE、COW reuse vs copy） | **Q24, Q25, Q32, Q38-Q42** |
| `experiments/esr_far.c` | 從訊號處理常式讀 ESR_EL1 / FAR_EL1 並解碼 | **Q34-Q36, Q41** |
| `experiments/mmap_vma.c` | MAP_FIXED / MAP_FIXED_NOREPLACE / 四種 mmap 組合 / brk | **Q21, Q31-Q33** |
| `experiments/slub_info.sh` | 從 sysfs 讀 SLUB 每個 cache 的 order/objs_per_slab/cpu_partial | **Q13, Q17** |
| `experiments/pagemap_walk.c` | 使用者態 VA→PFN | Q18, Q24, Q29 |
| `experiments/mm_convert.c` | mm/VMA/page/PFN 的九種轉換 | Q19, Q23, Q29 |

一鍵重跑見本文開頭的「實驗工具」。

---

## 跨章節關聯

| 本章 | 關聯 |
|------|------|
| [Q6](#q6) 水位 | [Ch6 Q10](./ch06_memory_management_case_studies.md#q10) min_free_kbytes、[Ch6 Q13](./ch06_memory_management_case_studies.md#q13) watermark boost |
| [Q7](#q7) 伙伴合併 | [Ch3 Q7](./ch03_memory_management_prerequisites.md#q7) 開機時如何加入伙伴系統 |
| [Q8](#q8) kmalloc 對齊 | [Ch2 Q13](./ch02_arm64_in_linux_kernel.md#q13) `CTR_EL0.CWG` = 128 bytes |
| [Q18](#q18) vmalloc | [Ch2 Q9](./ch02_arm64_in_linux_kernel.md#q9) vmalloc 區位置、[Ch2 Q10](./ch02_arm64_in_linux_kernel.md#q10) `__pa()` 不可用 |
| [Q20](#q20) PTE 屬性 | [Ch2 Q15](./ch02_arm64_in_linux_kernel.md#q15) MAIR、[Ch2 Q16](./ch02_arm64_in_linux_kernel.md#q16) SH |
| [Q34](#q34)-[Q36](#q36) 缺頁 | [Ch2 Q2](./ch02_arm64_in_linux_kernel.md#q2) 4 級頁表 |
| [Q41](#q41)-[Q42](#q42) COW | [Ch7 Q9](./ch07_process_management_basic_concepts.md#q9) COW 缺頁計數 |
| [Q45](#q45) TLB | [Ch2 Q14](./ch02_arm64_in_linux_kernel.md#q14) ASID、[Ch2 Q17](./ch02_arm64_in_linux_kernel.md#q17) 屏障 |
