# 《奔跑吧 Linux內核》（第二版）卷1 第3章 內存管理之預備知識 高頻面試題

> **說明**：收錄自第 3 章開篇「本章的高頻面試題」，共計 7 題。
> 答案結合書本理論、Linux 內核原始碼（本專案樹，版本 6.1.115）、
> 以及 Radxa ROCK 5B（RK3588）實機（`radxa@192.168.68.57`）的實測資料。

---

### 1. 请简述内存架构中UMA和NUMA的区别。

- **UMA（Uniform Memory Access，统一内存访问）**：系统中所有 CPU 核访问任意一块物理内存的延迟/带宽基本一致，逻辑上只有一整块内存，不存在"本地/远程内存"之分，操作系统只需一个统一的内存管理域（一个 `pg_data_t` 节点）即可。
- **NUMA（Non-Uniform Memory Access，非统一内存访问）**：系统划分为多个节点（node），每个节点有自己的本地 CPU 与本地内存，CPU 访问本地内存快、访问其它节点的远程内存慢，操作系统需要为每个节点维护独立的 `pg_data_t`，并做 NUMA 感知的调度与内存分配（详见第 1 章第 21 题）。

实测本设备为单 SoC、单一 DDR 控制器域：`numactl --hardware` 输出 `No NUMA available on this system`，内核配置 `# CONFIG_NUMA is not set`，`/sys/devices/system/node/` 目录不存在——即典型 **UMA** 架构；全系统只有一个 `pg_data_t`（`contig_page_data` 或等价的单节点结构），8 个核（4×A55+4×A76）访问同一块 8GB LPDDR4 的（近似）延迟一致。

### 2. CPU访问各级存储结构的速度是否一样？

不一样，存储层级（memory hierarchy）从快到慢、从小到大依次为：**寄存器 > L1 Cache > L2 Cache > L3 Cache > 主存（DRAM）> 辅存（Flash/eMMC/磁盘）**，每往下一级容量增大约一到几个数量级，访问延迟也相应增大一到几个数量级——这正是"存储层级金字塔"设计的核心权衡（用小容量高速存储 Cache 住热点数据，掩盖大容量低速存储的延迟）。

实测本设备各级差异（数量级示意）：
- **L1（A55: 32KB/A76: 64KB）**：延迟约几个时钟周期；
- **L2（A55: 128KB/A76: 512KB，均为核私有）**：延迟约十几个周期；
- **L3（3MB，8 核共享，`shared_cpu_list: 0-7`）**：延迟约几十周期；
- **主存（DDR，8GB，`/proc/iomem` 显示分为 3 段不连续物理区间）**：延迟通常是 L1 的百倍以上（一两百纳秒量级）；
- **辅存（本设备通过 NVMe/eMMC/SD 卡启动，`/proc/iomem` 可见 `nvme` 设备）**：延迟是主存的千倍到万倍以上（微秒~毫秒级）。

同一级存储内部（如不同核的 L1 vs 共享的 L3）速度也不同：核私有 Cache 比跨核共享 Cache 更快，这也是为什么 big.LITTLE 架构中 A76（大核）配置比 A55（小核）更大的私有 L1/L2，用更多面积/功耗换取更低的平均访存延迟。

### 3. 请绘制内存管理常用的数据结构的关系图。如mm_struct、VMA、 vaddr、page、PFN、PTE、zone、paddr和pg_data等，并思考如下转换关系。 如何由mm_struct和vaddr找到对应的VMA？ 如何由page和VMA找到vaddr？ 如何由page找到所有映射的VMA？ 如何由VMA和vaddr找出相应的page数据结构？ page和PFN之间如何互换？ PFN和paddr之间如何互换？ page和PTE之间如何互换？ zone和page之间如何互换？ zone和pg_data之间如何互换？

关系图：

```
task_struct --> mm_struct --+--> pgd (页表, 由 vaddr 经页表逐级查得 PTE)
                             |
                             +--> mmap / 红黑树(maple tree) --> VMA(vm_area_struct) [代表一段连续 vaddr 区间]
                                                                    |
                                                                    +--> vaddr 落在某个 VMA 内
                                                                            |
                                                                            v (查页表 PTE)
                                                                          PFN --(struct page*)--> page
                                                                            |
                                                                pg_data_t --+--> zone[] --> page[] (每个 zone 管理一段 PFN 范围)
                                                                            |
                                                                page <--anon_vma/i_mmap 反向映射--> 所有映射它的 VMA
```

各转换关系与对应内核接口：

| 转换 | 方法 |
|---|---|
| **mm_struct + vaddr → VMA** | `find_vma(mm, vaddr)`：在 `mm->mmap`（新版本内核为 `mm->mm_mt` maple tree，旧版本为红黑树 `mm_rb`）中按 vaddr 查找覆盖该地址的 `vm_area_struct` |
| **VMA + vaddr → page** | 先用 `mm->pgd` + `vaddr` 走页表（`pgd_offset`→`p4d_offset`→`pud_offset`→`pmd_offset`→`pte_offset_map`）取得 `pte_t`，再用 `pte_page(pte)` 或 `vm_normal_page()` 得到 `struct page *`（若缺页，则触发 `handle_mm_fault()` 建立映射） |
| **page + VMA → vaddr** | 若已知该 page 被此 VMA 映射：`vaddr = vma->vm_start + ((page->index - vma->vm_pgoff) << PAGE_SHIFT)`（文件映射场景），或匿名页通过反向映射结构里记录的偏移计算 |
| **page → 所有映射它的 VMA** | **反向映射（rmap）**：匿名页通过 `page->mapping`（低位打了 `PAGE_MAPPING_ANON` 标记）指向 `anon_vma`，再遍历 `anon_vma` 关联的 `anon_vma_chain` 找到所有共享该页的 VMA；文件页通过 `page->mapping`（`address_space`）+ `page->index`，配合 `address_space->i_mmap`（区间树）找到所有映射该文件同一偏移的 VMA。`rmap_walk()` 是统一入口 |
| **page ↔ PFN** | `page_to_pfn(page)` = `page - vmemmap`（或经 `SPARSEMEM` 的 section 换算）；`pfn_to_page(pfn)` = `vmemmap + pfn`（`struct page` 数组按 PFN 线性排布，见第 4 题的 VMEMMAP 区域） |
| **PFN ↔ paddr** | `paddr = pfn << PAGE_SHIFT`（`PFN_PHYS(pfn)`）；`pfn = paddr >> PAGE_SHIFT`（`PHYS_PFN(paddr)`），本设备 `PAGE_SHIFT=12`（4KB 页） |
| **page ↔ PTE** | `pte_page(pte)` 由 PTE 中的 PFN 字段转 `struct page*`（即 `pfn_to_page(pte_pfn(pte))`）；反向 `mk_pte(page, prot)` 由 `page_to_pfn(page)` 结合权限位组装出 PTE 值 |
| **zone ↔ page** | `page_zone(page)` 通过 `page->flags` 中编码的 zone 号，或由 PFN 落在哪个 zone 的 `[zone_start_pfn, zone_start_pfn+spanned_pages)` 区间判定；反向 `zone->zone_start_pfn` + 偏移即可枚举该 zone 内的 page |
| **zone ↔ pg_data** | `zone->zone_pgdat` 指回所属节点的 `pg_data_t`；`pg_data_t->node_zones[]` 数组正向持有该节点所有 zone（如 `ZONE_DMA`/`ZONE_DMA32`/`ZONE_NORMAL`/`ZONE_HIGHMEM`/`ZONE_MOVABLE`） |

实测本设备（UMA、单节点）只有一个 `pg_data_t`（对应 `NODE_DATA(0)`），因 `CONFIG_ARM64_PA_BITS=48` 且无 32 位外设 DMA 限制场景，一般包含 `ZONE_DMA32`（覆盖 0~4GB，供仅支持 32 位 DMA 地址的外设使用）与 `ZONE_NORMAL`（覆盖其余物理内存），且实测 `/proc/iomem` 显示 DTB `/memory` 节点给出 3 段不连续物理内存（`0x200000~0xefe00000`、`0x100000000~0x200000000`、`0x2f0000000~0x300000000`，见第 6 题），这些不连续区间在同一 zone 内仍以 PFN 连续编号（借助 `SPARSEMEM` 的 section 管理物理地址空洞）。

### 4. 在ARM64内核中，内核映像文件映射到内核空间的什么地方？

内核映像（vmlinux 的 `.text/.rodata/.data/.bss` 等段）被映射到内核虚拟地址空间中 **`KIMAGE_VADDR`** 开始的一段专用区域，而**不是**在 `PAGE_OFFSET` 开始的物理内存线性映射区域内（两者是两个独立、彼此位置解耦的映射）。在本设备实际运行的 6.1 内核源码中：

```c
#define KIMAGE_VADDR    (MODULES_END)
#define MODULES_END     (MODULES_VADDR + MODULES_VSIZE)
```

即内核镜像映射区紧跟在 modules（可加载模块代码）区域之后，位于内核地址空间高地址端；早期内核版本（书中所述）用固定常量 `KIMAGE_VADDR = 0xFFFF000010000000` + `TEXT_OFFSET` 偏移来描述同一位置，但当前版本已移除 `TEXT_OFFSET`，且 `KIMAGE_VADDR` 随 `VA_BITS`/modules 区域大小动态计算（详见第 2 章第 6、7 题）。

实测本设备 `/proc/kallsyms`（root）：`_stext = ffff800008010000`，与 `PAGE_OFFSET (0xffff800000000000)` 相差仅约 0x8010000（~128MB），说明本设备内核（因关闭了 `CONFIG_RANDOMIZE_BASE` 或该次启动未随机化）实际把内核镜像映射得非常接近 `PAGE_OFFSET`——这是因为 6.1 内核里 `KIMAGE_VADDR` 由 `MODULES_END`（=`_PAGE_END(VA_BITS_MIN)`，即内核地址空间起点附近）决定，而 `modules` 区域大小是固定的（`MODULES_VSIZE`，通常 128MB 量级），因此内核镜像紧接着 modules 区域之后、大致在 `PAGE_OFFSET` 之前的一段距离内，与实测数值吻合。

### 5. 在ARM64内核中，内核空间和用户空间是如何划分的？

同第 2 章第 4 题：用虚拟地址第 63 位区分——用户空间地址第 `63:48` 位为 0，内核空间对应位为 1。4KB 页 + 4 级页表（`VA_BITS=48`）下：

```
0000000000000000 ~ 0000ffffffffffff  256TB  用户空间 (TTBR0，每进程 mm->pgd 独立)
ffff000000000000 ~ ffffffffffffffff  256TB  内核空间 (TTBR1，全局 swapper_pg_dir 共享)
```

实测本设备内核配置 `CONFIG_ARM64_VA_BITS=48`，与该 256TB/256TB 对半划分一致；`TCR_EL1` 的 `T0SZ`/`T1SZ` 字段各自独立配置 TTBR0/TTBR1 管理的地址范围大小，使得用户和内核空间可以各自独立设定有效地址位数（本设备两者相同均为 48 位）。

### 6. 在系统启动时，ARM64 Linux内核如何知道系统有多大的物理内存？

内核启动早期（`setup_arch()` → `arm64_memblock_init()` 之前）会解析 **设备树（Device Tree Blob, DTB）** 中的 **`/memory` 节点**：其 `reg` 属性以 `(base, size)` 一组或多组的形式（受 `#address-cells`/`#size-cells` 决定每个字段占几个 32 位 cell）描述系统实际存在的物理内存区间（可能不止一段，中间可能有 MMIO 地址空洞）。解析函数 `early_init_dt_scan_memory()`（`drivers/of/fdt.c`）读取每个 `reg` 条目，调用 **`memblock_add(base, size)`** 把这段物理内存注册进 `memblock` 这个启动期的临时内存分配器/内存区间登记表中，之后内核所有的内存布局决策（zone 划分、`sparse_init`、buddy 系统初始化）都基于 `memblock` 登记的这些区间进行。

（除 DTB 外，UEFI/ACPI 启动路径则是解析 UEFI Memory Map / `EFI System Table` 获取内存区间，同样最终汇总进 `memblock`；也可以通过内核命令行 `mem=` 参数覆盖/裁剪。）

实测本设备通过 `/proc/device-tree/memory/reg` 读出（`#address-cells=2 #size-cells=2`，每组 base/size 各占 8 字节）3 组物理内存区间：

```
base=0x0000000000200000  size=0x00000000efe00000   (~3.75GB，低地址段，避开 0xf0000000 起的 MMIO/PCIe 空洞)
base=0x0000000100000000  size=0x0000000100000000   (4GB)
base=0x00000002f0000000  size=0x0000000010000000   (256MB)
```

三段相加共约 8GB，与实机规格（8GB LPDDR4）、`free -h` 显示的 `Mem: 7.8Gi total` 吻合（差值是内核代码/页表/reserved/CMA 等占用），也与 `/proc/iomem` 中的 `System RAM` 条目完全对应。这也解释了为什么该 SoC 的物理内存在地址空间上不是单一连续区间——中间被 MMIO（GIC、PCIe config space、各类外设寄存器等，见 `/proc/iomem` 中 `f0000000~` 之后大量外设地址）打断，需要 `memblock`/`SPARSEMEM` 支持多段不连续物理内存的管理。

### 7. 物理内存页面如何添加到伙伴系统中，是一页一页添加，还是以2n来添 加呢？

是**以 2ⁿ（幂次对齐的连续块）为单位批量添加**，而不是逐页添加。内核源码 `mm/memblock.c` 中的 `__free_pages_memory()` 函数（在 `memblock_free_all()` 释放启动期 `memblock` 托管的空闲内存、正式交给伙伴系统时调用）实现如下：

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

逻辑是：在待释放的 `[start, end)` 这段 PFN 区间里，每一步都取"**当前 `start` 地址对齐所允许的最大阶数**"（`__ffs(start)`，即 `start` 二进制表示中末尾连续 0 的个数，决定它最高能对齐到 2 的多少次方）与 `MAX_ORDER-1`（伙伴系统支持的最高阶）中的较小值作为初始阶数，再不断减小 `order` 直到 `2^order` 大小的块不超出剩余区间 `end`；然后调用 `memblock_free_pages()` 把这一整块（`1<<order` 个连续页）一次性交给伙伴系统对应阶的空闲链表，而不是拆成一页一页分别调用 `__free_page()`。

**这样做的原因**：伙伴系统本身就是按 2ⁿ 阶（order）组织空闲块的，如果启动时把整段连续内存拆成最小的单页（order-0）逐个 free，之后运行时反而需要靠"伙伴合并（buddy merging）"机制一步步把相邻的 order-0 块合并回 order-1、order-2……效率低且过程繁琐；而在明确知道这段内存本来就是连续的启动阶段，直接按最大可能的对齐阶数一次性喂给伙伴系统对应阶的链表，可以立刻得到大阶数的空闲块，减少后续内存分配时因缺少大块连续内存而失败或需要合并的情况，初始化效率也更高。
