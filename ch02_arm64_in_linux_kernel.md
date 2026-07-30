# 《奔跑吧 Linux內核》（第二版）卷1 第2章 ARM64 在 Linux 內核中的實現 高頻面試題

> **說明**：收錄自第 2 章開篇「本章的高頻面試題」，共計 23 題。
> 答案結合書本理論、Linux 內核原始碼（本專案樹 `arch/arm64/`，版本 6.1.115）、
> 以及 Radxa ROCK 5B（RK3588）實機（`radxa@192.168.68.57`）的實測資料。

---

### 1. ARM64处理器中有两个页表基地址寄存器TTBR0和TTBR1，处理器如何 使用它们？

ARM64 把 64 位虚拟地址空间一分为二，用地址的**第 63 位（VA[63]）**决定选用哪个页表基址寄存器：

- **VA[63] = 0** → 使用 **TTBR0_ELx**，覆盖用户空间地址范围（如 4 级页表下的 `0x0000000000000000 ~ 0x0000ffffffffffff`）；
- **VA[63] = 1** → 使用 **TTBR1_ELx**，覆盖内核空间地址范围（如 `0xffff000000000000 ~ 0xffffffffffffffff`）。

对应地，`TCR_ELx`（Translation Control Register）里有独立的 `T0SZ`/`T1SZ` 字段分别配置 TTBR0、TTBR1 各自管理的地址范围大小（有效虚拟地址位数），两者可以配置为不同大小，也可各自独立使能/禁用（`TCR.EPD0`/`EPD1`）。在 Linux 内核中：

- `swapper_pg_dir`（内核全局页表）的物理地址写入 **TTBR1**，且从不写入 TTBR0；
- 每个进程的用户态页表（`mm->pgd`）写入 **TTBR0**，进程切换（`switch_mm`）时只需要更换 TTBR0（连同 ASID），TTBR1 指向的内核页表在所有进程间保持不变，因此内核地址在所有地址空间中都能直接访问，无需切换。

实测该 6.1 内核源码 `arch/arm64/mm/context.c`、`arch/arm64/kernel/head.S` 中対 TTBR0/TTBR1 的写入正对应上述职责划分；本设备 `CONFIG_ARM64_VA_BITS=48`、`CONFIG_ARM64_PA_BITS=48`，用户/内核各自 48 位虚拟地址空间。

### 2. 请简述ARM64处理器的4级页表的映射过程，假设页面粒度为4KB，地 址宽度为48位。

4KB 页、48 位虚拟地址下，转换表分为 **L0（PGD）→ L1（PUD）→ L2（PMD）→ L3（PTE）**共 4 级，每级用 9 位地址位索引（每张表 512 项，`2^9 = 512`，每项 8 字节，正好一张表占一个 4KB 页），加上 12 位页内偏移：

```
|63    56|55    48|47    40|39    32|31    24|23    16|15     8|7      0|
 |                 |         |         |         |         v
 |                 |         |         |         |    [11:0]  页内偏移
 |                 |         |         |         +--> [20:12] L3(PTE) 索引
 |                 |         |         +------------> [29:21] L2(PMD) 索引
 |                 |         +----------------------> [38:30] L1(PUD) 索引
 |                 +--------------------------------> [47:39] L0(PGD) 索引
 +----------------------------------------------------------> [63] 选择 TTBR0/TTBR1
```

映射流程：
1. 处理器根据 VA[63] 选择 TTBR0 或 TTBR1，得到 L0 表的物理基址；
2. 用 VA[47:39] 索引 L0 表，取得一个表项，指向 L1 表物理基址；
3. 用 VA[38:30] 索引 L1 表项；此级除了指向下一级表外，也可能是 1GB 的**块（block）描述符**直接映射到物理页（跳过 L2/L3）；
4. 用 VA[29:21] 索引 L2 表项；同样可能是 2MB 块描述符直接映射；
5. 若未在 L1/L2 提前作为块映射结束，则用 VA[20:12] 索引 L3（PTE）表项，得到最终的 4KB **页描述符**（page descriptor），其中含物理页帧号（PFN，即 VA[47:12] 转换后对应的 PA 高位）及访问属性（AP、AF、SH、AttrIndx 等）；
6. 最终物理地址 = 页描述符中的 PFN（拼接 PA 高位）+ VA[11:0]（页内偏移）。

本设备 6.1 内核确认 `CONFIG_PGTABLE_LEVELS=4`、`CONFIG_ARM64_4K_PAGES=y`、`CONFIG_ARM64_VA_BITS_48=y`，与上述计算完全一致。

### 3. 在L0～L2页表项描述符中，如何判断一个页表项是块类型还是页表类 型？

用页表项（descriptor）的**最低 2 位 `[1:0]`**来区分：

- `[1:0] = 0b11`（Valid=1, 表类型位=1）→ **表描述符（Table descriptor）**，指向下一级页表的物理基址，需要继续往下查；
- `[1:0] = 0b01`（Valid=1, 表类型位=0）→ **块描述符（Block descriptor）**，本级直接给出一段物理内存的映射，转换到此结束（仅 L1/L2 有效——L1 块对应 1GB，L2 块对应 2MB；L0 级只允许表描述符，不支持块映射）；
- `[1:0] = 0b00` 或 `0b10`（Valid=0）→ 该表项无效（Invalid），访问该地址触发转换错误（Translation Fault）。

而在最底层的 L3（PTE）表中，`[1:0] = 0b11` 表示有效的**页描述符（Page descriptor）**（此时 bit1 复用为"page"标志而非"table"标志，语义上是叶子节点），是转换的最终终点。

### 4. 在ARM64 Linux内核中，用户空间和内核空间是如何划分的？

依据内核官方文档 `Documentation/arm64/memory.rst`（本仓库繁中翻译版 `Documentation/translations/zh_TW/arm64/memory.txt`）：用地址第 63 位区分——**用户地址空间第 63:48 位为 0**，**内核地址空间对应位为 1**。以 4KB 页 + 4 级页表（48 位虚拟地址）为例：

```
起始地址              结束地址              大小     用途
0000000000000000  0000ffffffffffff  256TB    用户空间（TTBR0）
ffff000000000000  ffffffffffffffff  256TB    内核空间（TTBR1）
```

用户空间由每个进程各自的 `mm->pgd` 描述、装入 TTBR0；内核空间由全局唯一的 `swapper_pg_dir` 描述、装入 TTBR1，对所有进程共享。本设备内核配置 `CONFIG_ARM64_VA_BITS=48`，与该文档给出的 256TB/256TB 划分吻合；实测 `_stext` 地址为 `ffff800008010000`，落在内核空间范围内（`0xffff8...` 前缀正是 `PAGE_OFFSET` 之后的线性映射高位区）。

### 5. 在ARM64 Linux内核中，PAGE_OFFSET表示什么意思？

`PAGE_OFFSET` 是**物理内存在内核空间中做线性映射（linear/direct mapping）的起始虚拟地址**。内核会把（几乎）全部物理内存，从 `PAGE_OFFSET` 开始按 `虚拟地址 = 物理地址 + (PAGE_OFFSET - PHYS_OFFSET)` 的固定偏移关系一一映射到内核空间的这段区域，使内核可以用简单的加/减运算（而非查页表）在物理地址与该段虚拟地址之间转换（对应 `__va()`/`__pa()` 宏），以加速内核直接访问物理内存。

源码定义（`arch/arm64/include/asm/memory.h`）：

```c
#define _PAGE_OFFSET(va)    (-(UL(1) << (va)))
#define PAGE_OFFSET         (_PAGE_OFFSET(VA_BITS))
```

即取虚拟地址空间"内核部分"的最低地址。本设备 `VA_BITS=48` 时，`PAGE_OFFSET = 0xffff800000000000`，与书中 QEMU 示例（`VA_BITS=48` 时同为 `0xFFFF800000000000`）一致；实测 `_stext = ffff800008010000` 正好比 `PAGE_OFFSET` 大约 0x8010000，符合"内核镜像位于线性映射区域附近偏移处"的布局（详见第 9 题）。

### 6. KIMAGE_VADDR表示什么意思？

`KIMAGE_VADDR` 表示**内核镜像文件（vmlinux）被映射到内核虚拟地址空间的起始虚拟地址**。书中所述的旧版本内核将其定义为一个独立的固定常量（如 `0xFFFF000010000000`），但本设备实际运行的 6.1 内核源码已经改为：

```c
#define KIMAGE_VADDR    (MODULES_END)
#define MODULES_END     (MODULES_VADDR + MODULES_VSIZE)
#define MODULES_VADDR   (_PAGE_END(VA_BITS_MIN))
```

即**内核镜像映射区紧跟在 modules 区域（存放可加载内核模块代码的虚拟地址区间）之后**，两者共同构成内核地址空间高地址端的一段"非线性映射"专用区域，与 `PAGE_OFFSET` 开始的物理内存线性映射区分开存放——这样内核镜像本身的加载位置与物理内存大小、线性映射区大小解耦，也便于支持 KASLR（内核地址空间布局随机化）独立随机化内核镜像与线性映射的相对位置。这是新旧内核版本在同一概念上具体实现演进的一个典型例子。

### 7. TEXT_OFFSET表示什么意思？

在书中所述的早期内核版本里，`TEXT_OFFSET` 表示**内核镜像代码段（.text）相对于 `KIMAGE_VADDR`（及对应加载物理基址）的字节偏移量**，链接脚本 `vmlinux.lds.S` 中 `. = KIMAGE_VADDR + TEXT_OFFSET;` 决定了 `.text` 段真正的起始虚拟地址；默认值为 `0x00080000`（512KB），也可通过 `CONFIG_ARM64_RANDOMIZE_TEXT_OFFSET` 让 bootloader/内核在这个偏移上做随机化，弥补当时内核本身还不支持随机化基址（KASLR）的空缺，同时兼容不同 bootloader 对镜像头部的预留空间要求。

实测本仓库（6.1 内核）中已经**不存在 `TEXT_OFFSET` 这个宏/Makefile 变量**（`grep -rn "TEXT_OFFSET" arch/arm64/Makefile arch/arm64/kernel/vmlinux.lds.S` 无匹配），这是因为自 Linux 4.6 起（commit "arm64: kernel: Update the kernel image..." 系列）内核镜像本身已支持在任意 2MB 对齐地址加载并做重定位/KASLR，不再需要 bootloader 预留固定的 `TEXT_OFFSET` 头部空间，`.text` 现在直接从镜像的起始处（偏移 0）开始链接。这体现了 ARM64 Linux 启动机制随版本演进而简化的过程——回答此题时应说明书本描述的是历史机制，并指出目前内核已经移除该概念。

### 8. 内核映像文件包含哪些段？这些段的作用是什么？在Sysmtem.map文件 中它们分别使用哪些符号来表示段的开始和结束？

主要段及作用：

| 段 | 作用 | System.map / kallsyms 起止符号 |
|---|---|---|
| `.text` | 内核代码段（可执行指令） | `_text` ~ `_etext` |
| `.rodata` | 只读数据（字符串常量、`const` 数据、异常表等） | `__start_rodata` ~ `__end_rodata` |
| `.data` | 已初始化的可读写全局/静态数据 | `_sdata` ~ `_edata` |
| `.bss` | 未初始化（默认清零）的全局/静态数据 | `__bss_start` ~ `__bss_stop` |
| `__init` | 仅初始化阶段使用、启动完成后即可释放的代码/数据（如 `__init` 函数、`initcall` 表） | `__init_begin` ~ `__init_end` |
| `_end` | 整个内核镜像的最终结束地址 | `_end` |

实测本设备 `/proc/kallsyms`（root 权限读取）：
```
ffff800008010000 T _stext
ffff800009070000 D _etext
ffff800009730000 T __init_begin
```
与 dmesg 启动日志中的段大小统计一致：
```
Memory: 7840408K/8386560K available (16768K kernel code, 3796K rwdata,
6856K rodata, 7296K init, 769K bss, 284008K reserved, 262144K cma-reserved)
```
即内核代码 16768K、可读写数据 3796K、只读数据 6856K、init 段 7296K、bss 769K，启动流程 (`free_initmem`) 结束时会把 `__init_begin~__init_end` 区间整体释放归还给伙伴系统（dmesg 中 `Freeing unused kernel memory: 7296K` 正对应此操作）。

### 9. 请画出ARM64 Linux内核的内存布局。

以本设备实际配置（4KB 页、4 级页表、`VA_BITS=48`）为例，结合内核文档 `Documentation/arm64/memory.rst`：

```
0x0000000000000000 +----------------------------+
                    |         用户空间 (TTBR0)     |  256TB
                    |   0 ~ 0x0000ffffffffffff    |
0x0000ffffffffffff +----------------------------+
        ...                (未使用的地址空洞，VA[63:48] 需一致)                
0xffff000000000000 +----------------------------+ <- 内核空间起点
                    |  modules 区 (MODULES_VADDR) |
                    |----------------------------|
                    | 内核镜像映射区 KIMAGE_VADDR   |  <-- KIMAGE_VADDR = MODULES_END
                    |   [_stext .. _end]           |
0xffff800000000000 +----------------------------+ <- PAGE_OFFSET
                    |   物理内存线性映射区           |
                    |   (__va(pa) = pa + 常数偏移)  |
                    |----------------------------|
                    |   vmalloc 区域                |
                    |----------------------------|
                    |   VMEMMAP（struct page 数组） |
                    |----------------------------|
                    |   PCI I/O 空间 / fixmap       |
0xffffffffffffffff +----------------------------+
```

（各子区域的精确边界由 `MODULES_VADDR/END`、`PAGE_OFFSET`、`VMEMMAP_START/END`、`PCI_IO_START/END`、`FIXADDR_TOP` 等宏在 `arch/arm64/include/asm/memory.h` 中依据 `VA_BITS` 计算得出，随内核版本可能有细节差异；本机启动日志因未开启 `CONFIG_DEBUG_VM`/ptdump 而不再打印详细分区表，可用 `zcat /proc/config.gz` 结合源码宏计算精确边界。）

### 10. __pasymbol() 宏和\_pa()宏有什么区别？

两者都用于把**内核虚拟地址转换为物理地址**，但适用的虚拟地址来源不同：

- **`__pa(x)`**：假定 `x` 是**线性映射区（`PAGE_OFFSET` 之后）**内的虚拟地址，转换公式基于 `PAGE_OFFSET`/`PHYS_OFFSET` 的固定偏移，即 `pa = (va & ~PAGE_OFFSET) + PHYS_OFFSET`，适用于通过 `__va()`/`page_address()` 等获得的、指向"物理内存直接映射区"的地址；
- **`__pa_symbol(x)`**：假定 `x` 是**内核镜像本身的符号地址**（即 `KIMAGE_VADDR + ...` 区域，如各种全局变量、函数指针取地址得到的 `&some_kernel_symbol`），这类地址位于内核镜像映射区而非线性映射区，不能直接套用 `PAGE_OFFSET` 公式，而要通过 `kimage_voffset`（内核镜像虚拟地址与物理地址之间的固定偏移，运行时计算得出）做减法：`pa = addr - kimage_voffset`。

源码（`arch/arm64/include/asm/memory.h`）：
```c
#define __pa_symbol(x)          __phys_addr_symbol(RELOC_HIDE((phys_addr_t)(x), 0))
#define __pa_symbol_nodebug(x)  __kimg_to_phys((phys_addr_t)(x))
#define __kimg_to_phys(addr)    ((addr) - kimage_voffset)
```
若把本该用 `__pa_symbol()` 的镜像符号地址误用 `__pa()` 计算（或反之），在内核镜像与线性映射区偏移不同（这是常态，二者是两个独立映射）时会得到错误的物理地址，因此内核对这两类地址来源做了严格区分。

### 11. 在物理内存还没有线性映射到内核空间时，内核映像文件映射到什么 地方？

在内核启动的最早期（汇编阶段 `head.S` 执行、尚未建立面向 `PAGE_OFFSET` 的完整线性映射之前），内核会先建立一个**小范围、专门针对内核镜像自身的映射**：把内核镜像通过**块映射（block mapping，用 L1/L2 块描述符，2MB 粒度）**方式映射到 `KIMAGE_VADDR + TEXT_OFFSET`（书中所述版本）起始的虚拟地址上，映射范围通常只覆盖内核镜像大小本身（几 MB），并不覆盖全部物理内存。此时该映射区的虚拟地址与物理地址之间的固定差值就被记录到全局变量 **`kimage_voffset`** 中（`arch/arm64/kernel/head.S`：`str_l x4, kimage_voffset, x5`），供尚未建立完整线性映射前的阶段使用 `__pa_symbol()` 转换地址。直到后续 `paging_init()`/`map_mem()` 建立起从 `PAGE_OFFSET` 开始覆盖全部物理内存的线性映射后，`__pa()`/`__va()` 才可用于线性映射区地址的转换。

### 12. 在ARM Linux内核中，kimage_voffset代表什么意思呢？

`kimage_voffset` 是一个内核启动早期就计算并保存的**全局变量**，代表"内核镜像的虚拟地址"与"内核镜像的物理地址"之间的固定偏移量，即：

```
kimage_voffset = 内核镜像的虚拟地址 - 内核镜像的物理地址
```

它的作用类似于线性映射区的 `PAGE_OFFSET`——只不过 `PAGE_OFFSET` 针对的是"全部物理内存的线性映射"，而 `kimage_voffset` 针对的是"内核镜像自身这一小段特殊映射"（见第 11 题）。凡是要把**内核镜像内部符号**的虚拟地址换算为物理地址（如 `__pa_symbol()`），都通过 `addr - kimage_voffset` 这个简单减法完成，而不必查页表，从而在内核启动的很早阶段（页表还未完全建立、也没有建立通用线性映射时）就能高效地做地址转换。源码定义：`arch/arm64/mm/mmu.c: u64 kimage_voffset __ro_after_init;`，并在 `head.S` 中于建立恒等映射/内核镜像映射之后立即计算写入。

### 13. 在ARMv8架构中，高速缓存管理的PoC和PoU有什么区别？

- **PoC（Point of Coherency，一致性点）**：站在**整个系统**的角度看 Cache 一致性——系统中所有能够访问内存的代理（不仅是 CPU 核，还包括 GPU、DMA 控制器等）都能观察到同一份最新数据的点。通常这个观察点就是**主存（DDR）本身**，因为像 GPU 这类不一定在 CPU 的 Cache 一致性域（Cache coherent interconnect）内的代理，唯一能确保双方都看到同一份数据的地方就是主存。
- **PoU（Point of Unification，统一点）**：站在**单个处理器核**的角度看——该核的指令 Cache（I-Cache）、数据 Cache（D-Cache）、TLB 与其"下一级共享的、能同时被 I 端和 D 端访问到"的存储层级之间达成一致的观察点，通常是该核的**L2 Cache**（如果 I-Cache/D-Cache 都能看到同一份 L2 数据即视为已统一）。

一句话区分：**PoC 是全系统范围的一致性观察点（通常落在主存），PoU 是单核内部 I/D 路径的一致性观察点（通常落在该核的 L2）**。两者决定了 Cache 维护指令（如 `DC CIVAC`、`IC IVAU`）需要清理/失效到多深的层级：例如加载内核模块后 patch 代码需要 `flush_icache_range()`，其内部按 PoU 清理 D-Cache（让即将被取指的新代码从 L2 可见）、按 PoU 使 I-Cache 失效，而涉及跨 CPU 一致性域访问设备内存（如 DMA 缓冲区）则要清理到 PoC。

### 14. 在ARMv8架构中，ASID是什么意思？有什么作用？

**ASID（Address Space ID，地址空间标识符）**是 ARMv8 为区分不同进程地址空间而设计的硬件标签。TLB 项除缓存"虚拟地址→物理地址"映射外，还携带产生该映射时所属进程的 ASID（本设备 TLB 项中 ASID 存放于 Bit[63:48]），查 TLB 命中的条件变成"虚拟地址匹配 **且** ASID 匹配"。

**作用**：解决第 1 章第 20 题所述的"进程频繁切换导致 TLB 大量失效"问题——不同进程的 TLB 项可以依靠不同的 ASID 共存于 TLB 中而不互相冲突/污染，进程切换（`switch_mm`）时只需要切换 TTBR0 与当前 ASID，无需 flush 整个 TLB，显著减少切换后立即发生大量 TLB miss 的开销。

Linux 在软件层面为每个 `mm_struct` 维护一个软件 ASID 计数（`mm->context.id` 的低若干位），支持的硬件 ASID 位宽可为 **8 位（256 个）或 16 位（65536 个，需要 `ID_AA64MMFR0_EL1` 报告支持且软件/`TCR_EL1.AS` 置位 `TCR_ASID16`）**；当 8/16 位 ASID 空间用尽发生"翻卷（rollover）"时，内核会分配新一轮 ASID 版本号并对所有 CPU 做一次全局 TLB flush（`arch/arm64/mm/context.c` 中的 `asid_bits`、`ASID_FIRST_VERSION`、`NUM_USER_ASIDS` 等即实现此逻辑）。此外 ARMv8 虚拟化扩展中还有类似机制的 **VMID（Virtual Machine ID）**，用于区分不同虚拟机的 Stage-2 页表 TLB 项。

### 15. 在ARMv8架构中支持哪几种内存属性？它们都有哪些特点？

ARMv8 通过 `MAIR_ELx`（Memory Attribute Indirection Register）配合页表项中的 `AttrIndx` 字段，将内存划分为两大类：

- **Normal Memory（普通内存）**：用于常规 RAM，特点是：
  - 允许指令/数据 Cache（可配置 Cacheable/Non-cacheable，及 Write-Back / Write-Through）；
  - 允许**乱序执行、预取、投机访问、地址合并/拆分（如把非对齐访问拆成多次总线传输）**；
  - 可进一步指定内部/外部（Inner/Outer）Cache 属性与 Shareability（见第 16 题）；
  - 又细分为 **Normal-Cacheable** 与 **Normal Non-cacheable**。
- **Device Memory（设备内存）**：用于 MMIO 寄存器等外设地址空间，**不可 Cache**，并按严格程度进一步分为四种（`nGnRnE`/`nGnRE`/`nGRE`/`GRE`，G=Gathering 是否允许合并多次访问、R=Reordering 是否允许重排、E=Early Write Acknowledgement 是否允许提前应答写完成）：
  - **Device-nGnRnE**（最严格）：不允许合并、不允许重排、不允许提前应答——即传统"强序（Strongly-ordered）"内存，每次访问都必须严格按程序顺序、独立地完成，常用于对时序极敏感的寄存器；
  - **Device-nGnRE**：不合并、不重排，但允许提前应答写完成；
  - **Device-nGRE**：不合并，允许重排与提前应答；
  - **Device-GRE**（最宽松）：允许合并、重排、提前应答，是设备内存中限制最少的一种。

Linux 内核通过 `ioremap()` 系列接口（`ioremap()`默认 nGnRE、`ioremap_wc()` 对应 Normal Non-cacheable 类似写合并、`ioremap_np()` 对应更严格的 nGnRnE）来选用合适的属性，MMIO 驱动代码需要根据外设时序要求选择正确的映射类型。

### 16. 在ARMv8架构中，高速缓存共享属性有内部共享（inner shareable）和 外部共享（outer shareable），它们有什么区别？

Shareability（共享属性）描述的是"当一个处理器/代理修改某内存位置后，系统中哪个范围内的其它代理需要通过 Cache 一致性协议自动看到这次修改"，分为几个层级（由页表项的 `SH[1:0]` 字段配置）：

- **Non-shareable（不可共享）**：该内存位置只被该处理器自身访问，不需要与任何其它代理保持一致（通常用于私有、未共享的内存，如某核独占使用的临时缓冲区）；
- **Inner Shareable（内部共享域）**：一致性范围覆盖"内部共享域"内的代理，典型对应**同一 Cache 一致性互连（如 CCI/CCN/DSU）连接的所有 CPU 核**——即片上的多核 CPU 集群会自动通过硬件监听协议保持一致，软件无需手动 flush；
- **Outer Shareable（外部共享域）**：一致性范围进一步扩大到"外部共享域"，通常覆盖**CPU 集群之外、但仍在系统级一致性范围内的代理**，如系统中的 GPU、其它 Socket 上的 CPU（多路服务器场景）等，只要它们也被纳入同一 Outer Shareable 域，也能自动获得一致性。

简言之：**Inner Shareable 是"CPU 集群内部自动保持一致"的范围，Outer Shareable 是把这个一致性范围扩展到集群之外更多种类/更大规模的代理**——外部共享域包含（是超集于）内部共享域。具体划分是 SoC 设计者根据互连拓扑（哪些代理接入了硬件一致性互连）来定义的实现细节。内存屏障指令的 `ish`/`osh`/`sy` 后缀（如 `dmb ish`）正是用于精确指定屏障需要同步到哪一个共享域，域越小屏障开销越低。

### 17. 在ARMv8架构中，支持哪几条内存屏障指令？它们都有什么区别？

同第 1 章第 5 题：ARMv8（A64 指令集）提供 **DMB、DSB、ISB** 三条基础内存屏障指令：

- **DMB（数据存储屏障）**：保证屏障前后的存储器访问相对顺序，但不要求屏障前的访问已经"完成"（数据到达终点），也不影响非存储器指令；
- **DSB（数据同步屏障）**：更强，要求屏障之前的所有存储器访问都**已经真正完成**才允许执行屏障之后的**任何**指令（不只是存储器指令）；
- **ISB（指令同步屏障）**：清空流水线，保证之后取指得到的都是本条指令之前所有系统状态变更（如页表、系统寄存器、Cache/TLB 维护操作）生效后的指令流。

三者都可以附加**作用域后缀**（`OSH`=外部共享域、`ISH`=内部共享域、`NSH`=不可共享域、`SY`=全系统，默认）以及**方向后缀**（`ST`=只针对 Store、`LD`=只针对 Load，DMB/DSB 支持，ISB 不支持），如 `dmb ishst`（仅针对内部共享域内的 Store 操作定序）比全量 `dmb sy` 开销更低，Linux 内核会依据具体同步需求（如自旋锁 vs. 跨核 IPI vs. 修改页表）选用不同强度/范围的屏障变体（对应 `arch/arm64/include/asm/barrier.h` 中的 `dmb()`、`dsb()`、`isb()` 宏封装）。

### 18. 加载-获取屏障原语与存储-释放屏障原语有什么区别？分别有什么作 用？

这是 ARMv8 引入的**单指令级内存屏障**（相比 DMB/DSB 是独立的屏障指令，Load-Acquire/Store-Release 是**内建在具体的 Load/Store 指令语义中**），用于高效实现临界区/无锁数据结构的同步：

- **加载-获取（Load-Acquire，如 `LDAR`）**：该 Load 指令保证——**在程序顺序上位于它之后的所有存储器访问，都不能被重排到这条 Load-Acquire 之前执行**（即它对后面的访问设置了一个"下界"，像获取锁一样，获取之后的操作不能"提前"到获取锁之前发生）；
- **存储-释放（Store-Release，如 `STLR`）**：该 Store 指令保证——**在程序顺序上位于它之前的所有存储器访问，都必须在这条 Store-Release 完成之前完成/可见**（即它对前面的访问设置了一个"上界"，像释放锁一样，释放之前的所有操作必须先完成，才能让"释放"这个信号对外可见）。

两者常常配对使用来实现**无需完整 DMB 的轻量级同步**：例如自旋锁的 `unlock` 用 Store-Release 写标志位（保证临界区内所有访问都先于解锁可见），而其它 CPU 用 Load-Acquire 读该标志位判断锁是否可用（保证读到"锁已释放"之后才能开始执行临界区代码），从而只需一对 Acquire/Release 指令即可正确同步，而不必像 DMB 那样对屏障两侧所有访问做全局排序，开销更低、粒度更细，也是 C11/C++11 `memory_order_acquire`/`memory_order_release` 语义在 ARM64 上的天然硬件映射（Linux 原子操作 `arch/arm64/include/asm/atomic_lse.h` 与 `smp_load_acquire()`/`smp_store_release()` 宏即基于此实现）。

### 19. 什么是一个段的加载地址和运行地址？

- **加载地址（Load Address，LMA, Load Memory Address）**：该代码/数据段**实际被存放（烧录/拷贝）到的物理存储位置**——比如 bootloader 把内核镜像从 Flash/eMMC/网络加载到 DRAM 中的某个物理地址，这个"被放到哪里"的地址就是加载地址。
- **运行地址（运行时地址 / VMA, Virtual Memory Address，链接地址）**：该代码在**运行时被 CPU 实际访问、按照代码里的绝对跳转/取数地址所期望**的（虚拟或物理）地址——即链接器（`ld`）在链接脚本中为这段代码设定的地址，代码内部的分支目标、全局变量引用等都是按这个地址编译生成的。

两者**通常一致**（直接在最终地址执行），但在很多嵌入式/内核启动场景中**不一致**：例如内核镜像被 bootloader 加载到某个物理地址（加载地址），但内核代码本身是按照 `KIMAGE_VADDR`（虚拟地址，运行地址）链接的；类似地，很多 bootloader 自身也是先被加载到一处物理地址执行一段"位置无关代码"，再重定位/拷贝自身到最终运行地址。当两者不一致时，必须先建立好从"当前执行位置"到"运行地址"的地址转换（如 ARM64 head.S 中的恒等映射，见第 21 题），或者代码本身写成位置无关代码（PIC/PIE），才能正确完成从加载地址到运行地址的跳转/切换。

### 20. 从U-boot跳转到内核时，为什么指令高速缓存可以打开而数据高速缓存 必须关闭？

- **指令 Cache 可以打开**：内核镜像的代码（.text）在被加载完成、且执行入口之前是**静态不变的只读数据**，U-Boot 与内核对同一段物理指令内容的理解是一致的，即使打开 I-Cache 提前缓存了部分指令，也不会因为"谁在什么时候写了什么"而产生歧义/不一致问题（不涉及写操作、也不存在多个执行者对同一地址产生不同数据版本的风险）；打开 I-Cache 能加速内核入口这段代码本身的取指执行，没有正确性风险。

- **数据 Cache 必须关闭**：因为此时**MMU 通常还未使能**（或者即将建立/切换页表），如果打开 D-Cache，会出现两个层面的风险：
  1. 内核启动早期需要对**页表、`memstart_addr`、`kimage_voffset` 等关键数据结构**做写入并被后续代码（可能运行在另一个核，或 MMU 使能之后用不同地址访问）正确看到，若 D-Cache 打开且这些写入停留在 Cache 中未及时写回，一旦发生 Cache 失效/被其它路径以非 Cache 一致的方式（如 MMU 关闭时对同一物理地址的直接访问，或其它尚未参与一致性域的核）读取内存，就会读到过期数据；
  2. 在 MMU 关闭阶段，D-Cache 若是 VIPT/PIPT 但当前又没有有效的地址转换语境，容易与"MMU 使能后、以虚拟地址正常访问同一物理内存"产生的 Cache 状态发生冲突（比如同一物理地址在 MMU 开关前后被不同"虚拟寻址上下文"下的 Cache 行记录，产生别名问题，见第 1 章第 9~12 题）；
  3. 多核启动（Secondary CPU 通过 `spin-table`/PSCI 等待、被唤醒后跳转执行）场景下，各核之间在 MMU/Cache 尚未统一建立前，如果各自的 D-Cache 缓存了不一致的数据且未强制同步，会导致核间数据不一致。

因此 ARM64 启动约定（Booting AArch64 Linux Kernel 文档）明确要求：跳转内核入口时 **MMU 关闭、D-Cache 关闭（或已妥善 clean/invalidate）、I-Cache 状态不限（可开可关，架构上是安全的）**，只有等内核自身在 `head.S`/`__enable_mmu` 中正确建立好页表、打开 MMU 之后，才会显式打开 D-Cache。

### 21. 在Linux内核启动汇编代码中，为什么要建立恒等映射？

**恒等映射（identity mapping）**指虚拟地址等于物理地址的映射（`VA == PA`）。原因在于：内核启动汇编代码执行"打开 MMU"这个动作本身是有**因果时序问题**的——在 `MMU` 打开之前，处理器正在以物理地址直接取指执行（PC 就是物理地址）；一旦执行了打开 MMU 的那条指令，从下一条指令起，处理器立刻开始把 PC 当成**虚拟地址**去查页表转换。如果内核代码本身是按照最终的运行虚拟地址（`KIMAGE_VADDR` 等，通常与当前物理执行地址不同）来链接的，那么"打开 MMU 那条指令"和"紧随其后的下一条指令"之间就会出现地址不连续的断裂：CPU 还在按物理地址执行到打开 MMU 那一条，下一条却要按照一个尚未生效、或者生效了但还没跳转过去的虚拟地址取指，会导致取指错乱/异常。

解决办法是：在建立"内核真正想用的映射"（内核镜像映射到高地址虚拟空间、线性映射覆盖全部物理内存）**之外**，额外建立一个小范围的**恒等映射**——把"当前正在执行、打开 MMU 那几条指令所在的物理地址"也映射为**相同数值的虚拟地址**（即 `VA == PA`）。这样打开 MMU 前后，紧邻打开 MMU 指令的那一小段代码（通常只是`head.S` 里紧挨着写 `SCTLR_ELx.M` 那几条指令）无论按物理地址还是按虚拟地址解释，取到的都是同一份指令/同一个地址，执行流不会中断；随后代码再显式跳转（长跳转 `br`/绝对地址跳转）到内核镜像真正的高地址虚拟空间，切换到最终的运行地址继续执行，之后才不再需要这段恒等映射（可以在后续被丢弃/覆盖）。

### 22. 在ARMv8架构中，在L0～L2页表项中包含了指向下一级页表的基地 址，那么这个下一级页表基地址是物理地址还是虚拟地址？

**物理地址**。这与第 1 章第 27 题的道理完全一致：MMU 硬件页表遍历器（Table Walk Unit）本身的职责就是完成"虚拟地址 → 物理地址"的转换，若页表项中存的是下一级页表的虚拟地址，就需要"先转换地址才能拿到转换地址所需的输入"，形成逻辑死循环。因此 TTBR0/TTBR1 存放顶层（L0）页表的物理基地址，L0~L2 每一级的**表描述符（Table descriptor）**中，指向下一级页表的字段（bit[47:12] 左右，取决于具体地址宽度）存的也都是**物理地址**，硬件全程用物理地址直接访存完成整个 4 级页表的遍历，只有最终 L3 叶子页表项中的"物理页帧号"才是本次转换要求的最终答案，与页内偏移拼接后得到目标数据的物理地址。

### 23. MMU可以遍历页表，Linux内核也提供了软件遍历页表的函数，如 walk_pgd()、__create_pgd_mapping()、follow_page()等。从软件的视角，Linux内 核的pgd_t、pud_t、pmd_t以及pte_t数据结构中并没有存储一个指向下一级页表 的指针（即从CPU角度来看，CPU访问这些数据结构时是以虚拟地址来访问 的），它们是如何遍历的呢？pgd_t、pud_t、pmd_t以及pte_t数据结构是u64类型 的变量。

关键在于：这些数据结构（`pgd_t`/`pud_t`/`pmd_t`/`pte_t`，本仓库 `arch/arm64/include/asm/pgtable-types.h` 定义均为单个 `u64` 值的包装结构，如 `typedef struct { pteval_t pte; } pte_t;`）里存的数值确实是（硬件要求的）**物理地址**（即下一级页表的物理基址，对应第 22 题），但**内核软件本身要去访问"下一级页表"这块内存、读取其中内容时，走的是内核自己的虚拟地址访存路径**（因为 CPU 执行内核代码时，MMU 已经打开，所有访存指令的地址都要经过 CPU 当前的地址转换）——软件遍历函数并不是"直接把页表项里的物理地址塞给 CPU 访存"，而是先把该物理地址通过 **`__va()`（基于 `PAGE_OFFSET` 线性映射的固定偏移，见第 5 题）转换为对应的内核虚拟地址**，再用这个虚拟地址去解引用、读取下一级页表的内容。

具体到函数命名与实现（如 `pgd_offset()`、`pud_offset()`、`pmd_offset()`、`pte_offset_kernel()`）：
1. `pgd_offset(mm, addr)` 返回 `mm->pgd`（这本身已经是一个内核虚拟地址指针，因为 `pgd` 页在分配时就记录了其虚拟地址）加上由 `addr` 计算出的索引，从而**直接以虚拟地址**解引用得到 `pgd_t` 的值（该值内部存的是物理地址）；
2. 要往下一级走时，`pud_offset(pgd, addr)` 内部会先用 `pgd_val(*pgd)`（或 `pgd_page_paddr()`）取出该 `pgd_t` 存储的物理地址，再调用 `phys_to_virt()`/`__va()` 把它转换为内核虚拟地址，才能对这个虚拟地址做指针运算、加上 `pud` 索引，进而解引用读出 `pud_t` 的内容；
3. 如此逐级重复：每次都是"读取当前级页表项的值（物理地址）→ `__va()` 换算成虚拟地址 → 用虚拟地址访问，取得下一级页表项的内容"，直至到达叶子 `pte_t`。

因此结论是：**MMU 硬件页表遍历始终使用物理地址，而 Linux 软件页表遍历函数在每一步"读取页表项内容"时用的是虚拟地址（当前进程/内核正常的访存路径），但在"从上一级页表项的值计算下一级页表的访问地址"这一步，都要先做一次 `物理地址(页表项里存的值) → 虚拟地址(通过 __va()/phys_to_virt())` 的转换**，这正是软件遍历与硬件遍历"看起来都在做同一件事，但工作在不同地址视角"的关键衔接点。
