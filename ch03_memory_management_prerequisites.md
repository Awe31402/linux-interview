# 《奔跑吧 Linux核心》（第二版）卷1 第3章 記憶體管理之預備知識 高頻面試題

> **說明**：收錄自第 3 章開篇「本章的高頻面試題」，共計 7 題。
> 答案結合書本理論、Linux 核心原始碼（本專案樹，版本 6.1.115）、
> 以及 Radxa ROCK 5B（RK3588）實機（`radxa@192.168.68.57`）的實測資料。

---

### 1. 請簡述記憶體架構中 UMA 和 NUMA 的區別。

- **UMA（Uniform Memory Access，統一記憶體存取）**：系統中所有 CPU 核存取任意一塊實體記憶體的延遲/頻寬基本一致，邏輯上只有一整塊記憶體，不存在「本地/遠端記憶體」之分，作業系統只需一個統一的記憶體管理域（一個 `pg_data_t` 節點）即可。
- **NUMA（Non-Uniform Memory Access，非統一記憶體存取）**：系統劃分為多個節點（node），每個節點有自己的本地 CPU 與本地記憶體，CPU 存取本地記憶體快、存取其它節點的遠端記憶體慢，作業系統需要為每個節點維護獨立的 `pg_data_t`，並做 NUMA 感知的排程與記憶體分配（詳見第 1 章第 21 題）。

實測本設備為單 SoC、單一 DDR 控制器域：`numactl --hardware` 輸出 `No NUMA available on this system`，核心組態 `# CONFIG_NUMA is not set`，`/sys/devices/system/node/` 目錄不存在——即典型 **UMA** 架構；全系統只有一個 `pg_data_t`（`contig_page_data` 或等價的單節點結構），8 個核（4×A55+4×A76）存取同一塊 8GB LPDDR4 的（近似）延遲一致。

### 2. CPU 存取各級儲存結構的速度是否一樣？

不一樣，儲存階層（memory hierarchy）從快到慢、從小到大依次為：**暫存器 > L1 Cache > L2 Cache > L3 Cache > 主記憶體（DRAM）> 輔助記憶體（Flash/eMMC/磁碟）**，每往下一級容量增約一到幾個數量級，存取延遲也相應增大一到幾個數量級——這正是「儲存階層金字塔」設計的核心權衡（用小容量高速儲存 Cache 住熱點資料，掩蓋大容量低速儲存的延遲）。

實測本設備各級差異（數量級示意）：
- **L1（A55: 32KB/A76: 64KB）**：延遲約幾個時脈週期；
- **L2（A55: 128KB/A76: 512KB，均為核私有）**：延遲約十幾個週期；
- **L3（3MB，8 核共享，`shared_cpu_list: 0-7`）**：延遲約幾十週期；
- **主記憶體（DDR，8GB，`/proc/iomem` 顯示分為 3 段不連續實體區間）**：延遲通常是 L1 的百倍以上（一兩百奈秒量級）；
- **輔助記憶體（本設備透過 NVMe/eMMC/SD 卡啟動，`/proc/iomem` 可見 `nvme` 設備）**：延遲是主記憶體的千倍到萬倍以上（微秒~毫秒級）。

同一級儲存內部（如不同核的 L1 vs 共享的 L3）速度也不同：核私有 Cache 比跨核共享 Cache 更快，這也是為什麼 big.LITTLE 架構中 A76（大核）配置比 A55（小核）更大的私有 L1/L2，用更多面積/功耗換取更低的平均訪存延遲。

### 3. 請繪製記憶體管理常用的資料結構的關係圖。如 mm_struct、VMA、vaddr、page、PFN、PTE、zone、paddr 和 pg_data 等，並思考如下轉換關係。 如何由 mm_struct 和 vaddr 找到對應的 VMA？ 如何由 page 和 VMA 找到 vaddr？ 如何由 page 找到所有對映的 VMA？ 如何由 VMA 和 vaddr 找出相應的 page 資料結構？ page 和 PFN 之間如何互換？ PFN 和 paddr 之間如何互換？ page 和 PTE 之間如何互換？ zone 和 page 之間如何互換？ zone 和 pg_data 之間如何互換？

關係圖：

```
task_struct --> mm_struct --+--> pgd (頁表, 由 vaddr 經頁表逐級查得 PTE)
                             |
                             +--> mmap / 紅黑樹(maple tree) --> VMA(vm_area_struct) [代表一段連續 vaddr 區間]
                                                                    |
                                                                    +--> vaddr 落在某個 VMA 內
                                                                            |
                                                                            v (查頁表 PTE)
                                                                          PFN --(struct page*)--> page
                                                                            |
                                                                pg_data_t --+--> zone[] --> page[] (每個 zone 管理一段 PFN 範圍)
                                                                            |
                                                                page <--anon_vma/i_mmap 反向對映--> 所有對映它的 VMA
```

各轉換關係與對應核心介面：

| 轉換 | 方法 |
|---|---|
| **mm_struct + vaddr → VMA** | `find_vma(mm, vaddr)`：在 `mm->mmap`（新版本核心為 `mm->mm_mt` maple tree，舊版本為紅黑樹 `mm_rb`）中按 vaddr 尋找涵蓋該位址的 `vm_area_struct` |
| **VMA + vaddr → page** | 先用 `mm->pgd` + `vaddr` 走頁表（`pgd_offset`→`p4d_offset`→`pud_offset`→`pmd_offset`→`pte_offset_map`）取得 `pte_t`，再用 `pte_page(pte)` 或 `vm_normal_page()` 得到 `struct page *`（若缺頁，則觸發 `handle_mm_fault()` 建立對映） |
| **page + VMA → vaddr** | 若已知該 page 被此 VMA 對映：`vaddr = vma->vm_start + ((page->index - vma->vm_pgoff) << PAGE_SHIFT)`（檔案對映情境），或匿名頁透過反向對映結構裡記錄的偏移計算 |
| **page → 所有對映它的 VMA** | **反向對映（rmap）**：匿名頁透過 `page->mapping`（低位元打了 `PAGE_MAPPING_ANON` 標記）指向 `anon_vma`，再走訪 `anon_vma` 關聯的 `anon_vma_chain` 找到所有共享該頁的 VMA；檔案頁透過 `page->mapping`（`address_space`）+ `page->index`，配合 `address_space->i_mmap`（區間樹）找到所有對映該檔案同一偏移的 VMA。`rmap_walk()` 是統一入口 |
| **page ↔ PFN** | `page_to_pfn(page)` = `page - vmemmap`（或經 `SPARSEMEM` 的 section 換算）；`pfn_to_page(pfn)` = `vmemmap + pfn`（`struct page` 陣列按 PFN 線性排布，見第 4 題的 VMEMMAP 區域） |
| **PFN ↔ paddr** | `paddr = pfn << PAGE_SHIFT`（`PFN_PHYS(pfn)`）；`pfn = paddr >> PAGE_SHIFT`（`PHYS_PFN(paddr)`），本設備 `PAGE_SHIFT=12`（4KB 頁） |
| **page ↔ PTE** | `pte_page(pte)` 由 PTE 中的 PFN 欄位轉 `struct page*`（即 `pfn_to_page(pte_pfn(pte))`）；反向 `mk_pte(page, prot)` 由 `page_to_pfn(page)` 結合權限位元組裝出 PTE 值 |
| **zone ↔ page** | `page_zone(page)` 透過 `page->flags` 中編碼的 zone 號，或由 PFN 落在哪個 zone 的 `[zone_start_pfn, zone_start_pfn+spanned_pages)` 區間判定；反向 `zone->zone_start_pfn` + 偏移即可列舉該 zone 內的 page |
| **zone ↔ pg_data** | `zone->zone_pgdat` 指回所屬節點的 `pg_data_t`；`pg_data_t->node_zones[]` 陣列正向持有該節點所有 zone（如 `ZONE_DMA`/`ZONE_DMA32`/`ZONE_NORMAL`/`ZONE_HIGHMEM`/`ZONE_MOVABLE`） |

實測本設備（UMA、單節點）只有一個 `pg_data_t`（對應 `NODE_DATA(0)`），因 `CONFIG_ARM64_PA_BITS=48` 且無 32 位元週邊設備 DMA 限制情境，一般包含 `ZONE_DMA32`（涵蓋 0~4GB，供僅支援 32 位元 DMA 位址的週邊設備使用）與 `ZONE_NORMAL`（涵蓋其餘實體記憶體），且實測 `/proc/iomem` 顯示 DTB `/memory` 節點給出 3 段不連續實體記憶體（`0x200000~0xefe00000`、`0x100000000~0x200000000`、`0x2f0000000~0x300000000`，見第 6 題），這些不連續區間在同一 zone 內仍以 PFN 連續編號（借助 `SPARSEMEM` 的 section 管理實體位址空洞）。

### 4. 在 ARM64 核心中，核心影像檔對映到核心空間的什麼地方？

核心影像（vmlinux 的 `.text/.rodata/.data/.bss` 等區段）被對映到核心虛擬位址空間中 **`KIMAGE_VADDR`** 開始的一段專用區域，而**不是**在 `PAGE_OFFSET` 開始的實體記憶體線性對映區域內（兩者是兩個獨立、彼此位置解耦的對映）。在本設備實際執行的 6.1 核心原始碼中：

```c
#define KIMAGE_VADDR    (MODULES_END)
#define MODULES_END     (MODULES_VADDR + MODULES_VSIZE)
```

即核心影像對映區緊跟在 modules（可載入模組程式碼）區域之後，位於核心位址空間高位址端；早期核心版本（書中所述）用固定常數 `KIMAGE_VADDR = 0xFFFF000010000000` + `TEXT_OFFSET` 偏移來描述同一位置，但目前版本已移除 `TEXT_OFFSET`，且 `KIMAGE_VADDR` 隨 `VA_BITS`/modules 區域大小動態計算（詳見第 2 章第 6、7 題）。

實測本設備 `/proc/kallsyms`（root）：`_stext = ffff800008010000`，與 `PAGE_OFFSET (0xffff800000000000)` 相差僅約 0x8010000（~128MB），說明本設備核心（因關閉了 `CONFIG_RANDOMIZE_BASE` 或該次啟動未隨機化）實際把核心影像對映得非常接近 `PAGE_OFFSET`——這是因為 6.1 核心裡 `KIMAGE_VADDR` 由 `MODULES_END`（=`_PAGE_END(VA_BITS_MIN)`，即核心位址空間起點附近）決定，而 `modules` 區域大小是固定的（`MODULES_VSIZE`，通常 128MB 量級），因此核心影像緊接著 modules 區域之後、大致在 `PAGE_OFFSET` 之前的一段距離內，與實測數值吻合。

### 5. 在 ARM64 核心中，核心空間和使用者空間是如何劃分的？

同第 2 章第 4 題：用虛擬位址第 63 位元區分——使用者空間位址第 `63:48` 位元為 0，核心空間對應位元為 1。4KB 頁 + 4 級頁表（`VA_BITS=48`）下：

```
0000000000000000 ~ 0000ffffffffffff  256TB  使用者空間 (TTBR0，每行程 mm->pgd 獨立)
ffff000000000000 ~ ffffffffffffffff  256TB  核心空間 (TTBR1，全域 swapper_pg_dir 共享)
```

實測本設備核心組態 `CONFIG_ARM64_VA_BITS=48`，與該 256TB/256TB 對半劃分一致；`TCR_EL1` 的 `T0SZ`/`T1SZ` 欄位各自獨立配置 TTBR0/TTBR1 管理的位址範圍大小，使得使用者和核心空間可以各自獨立設定有效位址位元數（本設備兩者相同均為 48 位元）。

### 6. 在系統啟動時，ARM64 Linux 核心如何知道系統有多大的實體記憶體？

核心啟動早期（`setup_arch()` → `arm64_memblock_init()` 之前）會解析 **設備樹（Device Tree Blob, DTB）** 中的 **`/memory` 節點**：其 `reg` 屬性以 `(base, size)` 一組或多組的形式（受 `#address-cells`/`#size-cells` 決定每個欄位占幾個 32 位元 cell）描述系統實際存在的實體記憶體區間（可能不止一段，中間可能有 MMIO 位址空洞）。解析函式 `early_init_dt_scan_memory()`（`drivers/of/fdt.c`）讀取每個 `reg` 項目，呼叫 **`memblock_add(base, size)`** 把這段實體記憶體註冊進 `memblock` 這個啟動期的臨時記憶體分配器/記憶體區間登記表中，之後核心所有的記憶體佈局決策（zone 劃分、`sparse_init`、buddy 系統初始化）都基於 `memblock` 登記的這些區間進行。

（除 DTB 外，UEFI/ACPI 啟動路徑則是解析 UEFI Memory Map / `EFI System Table` 獲取記憶體區間，同樣最終彙整進 `memblock`；也可以透過核心命令列 `mem=` 參數覆蓋/裁剪。）

實測本設備透過 `/proc/device-tree/memory/reg` 讀出（`#address-cells=2 #size-cells=2`，每組 base/size 各占 8 位元組）3 組實體記憶體區間：

```
base=0x0000000000200000  size=0x00000000efe00000   (~3.75GB，低位址段，避開 0xf0000000 起的 MMIO/PCIe 空洞)
base=0x0000000100000000  size=0x0000000100000000   (4GB)
base=0x00000002f0000000  size=0x0000000010000000   (256MB)
```

三段相加共約 8GB，與實機規格（8GB LPDDR4）、`free -h` 顯示的 `Mem: 7.8Gi total` 吻合（差值是核心程式碼/頁表/reserved/CMA 等佔用），也與 `/proc/iomem` 中的 `System RAM` 項目完全對應。這也解釋了為什麼該 SoC 的實體記憶體在位址空間上不是單一連續區間——中間被 MMIO（GIC、PCIe config space、各類週邊設備暫存器等，見 `/proc/iomem` 中 `f0000000~` 之後大量週邊設備位址）打斷，需要 `memblock`/`SPARSEMEM` 支援多段不連續實體記憶體的管理。

### 7. 實體記憶體頁面如何添加到夥伴系統中，是一頁一頁添加，還是以 2ⁿ 來添加呢？

是**以 2ⁿ（冪次方對齊的連續塊）為單位批次添加**，而不是逐頁添加。核心原始碼 `mm/memblock.c` 中的 `__free_pages_memory()` 函式（在 `memblock_free_all()` 釋放啟動期 `memblock` 託管的空閒記憶體、正式交給夥伴系統時呼叫）實作如下：

```c
static void __init __free_pages_memory(unsigned long start, unsigned long end)
{
    int order;
    while (start < end) {
        order = min(MAX_ORDER - 1UL, __ffs(start));
        while (start + (1UL << order) > end)
            order--;
        memblock_free_pages(pfn_to_page(start), start, order);
        start += (1UL << order);
    }
}
```

邏輯是：在待釋放的 `[start, end)` 這段 PFN 區間裡，每一步都取「**目前 `start` 位址對齊所允許的最大階數**」（`__ffs(start)`，即 `start` 二進位表示中末尾連續 0 的個數，決定它最高能對齊到 2 的多少次方）與 `MAX_ORDER-1`（夥伴系統支援的最高階）中的較小值作為初始階數，再不斷減小 `order` 直到 `2^order` 大小的塊不超出剩餘區間 `end`；然後呼叫 `memblock_free_pages()` 把這一整塊（`1<<order` 個連續頁）一次性交給夥伴系統對應階的空閒鏈表，而不是拆成一頁一頁分別呼叫 `__free_page()`。

**這樣做的原因**：夥伴系統本身就是按 2ⁿ 階（order）組織空閒塊的，如果啟動時把整段連續記憶體拆成最小的單頁（order-0）逐個 free，之後執行時反而需要靠「夥伴合併（buddy merging）」機制一步步把相鄰的 order-0 塊合併回 order-1、order-2……效率低且過程繁瑣；而在明確知道這段記憶體本來就是連續的啟動階段，直接按最大可能的對齊階數一次性餵給夥伴系統對應階的鏈表，可以立刻得到大階數的空閒塊，減少後續記憶體分配時因缺少大塊連續記憶體而失敗或需要合併的情況，初始化效率也更高。
