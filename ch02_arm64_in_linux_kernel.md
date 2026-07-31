# 第 2 章 ARM64 在 Linux 內核中的實現 — 高頻面試題解答

> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588，4×Cortex-A76 + 4×Cortex-A55），`192.168.68.57`
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`
> **關鍵組態**：`CONFIG_ARM64_4K_PAGES=y`、`CONFIG_ARM64_VA_BITS=48`、`CONFIG_PGTABLE_LEVELS=4`、
> `CONFIG_THREAD_INFO_IN_TASK=y`、`CONFIG_UNMAP_KERNEL_AT_EL0=y`（KPTI）、
> `CONFIG_ARM64_PAN=y`、`CONFIG_ARM64_TLB_RANGE=y`、`CONFIG_ARM64_HW_AFDBM=y`、
> `CONFIG_SPARSEMEM_VMEMMAP=y`、`# CONFIG_RANDOMIZE_BASE is not set`（無 KASLR）
>
> **書目對照**
> - 《奔跑吧 Linux 內核》（第二版）卷 1 第 2 章 —
>   `books/running-linux-kernel/running-kernel-1-txt/10_第2章_ARM64在Linux内核中的实现.txt`（下稱「奔跑吧 §2.x」）
> - **RK3588 TRM** — `books/rk3588_trm/part1/chapter_01.txt`（Address Mapping）、
>   `chapter_03.txt`（CPU / Cache）、`chapter_08.txt`（MMU600 系統 MMU）
> - 核心程式碼路徑相對於本專案樹
>   `/home/awe/disk/yocto-rockchip-sdk/build/tmp/work-shared/rockchip-rk3588-rock-5b/kernel-source`
>
> ## ⚠️ 本次實測推翻了三項常見（也是本目錄舊草稿裡的）說法
>
> | # | 常見說法 / 舊草稿 | **本機實測** |
> |---|------------------|-------------|
> | [Q5](#q5)/[Q9](#q9) | `PAGE_OFFSET = 0xffff800000000000` | **`0xffff000000000000`**。Linux 5.4 把核心位址空間上下翻轉了，線性映射搬到低半部 |
> | [Q5](#q5)/[Q9](#q9) | 核心空間由低到高是「modules → 映像 → 線性映射」 | **正好相反**：線性映射在最低，modules/映像/vmalloc 在上半部 |
> | [Q22](#q22) | RK3588 實體位址 48 bits | **40 bits**（`ID_AA64MMFR0_EL1.PARange = 2`，`TCR_EL1.IPS = 2`）。`CONFIG_ARM64_PA_BITS=48` 只是核心編譯上限 |
>
> **主要實驗工具**：`notes/experiments/armv8_dump.c`（可載入核心模組，直接讀 EL1 系統暫存器 + 軟體巡覽頁表）

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | TTBR0 / TTBR1 如何使用 | TTBR0 BADDR **== `__pa(mm->pgd)`** |
| [2](#q2) | 4 級頁表映射過程 | 核心模組完整巡覽 + pagemap 交叉驗證 |
| [3](#q3) | 如何判斷 block / table 描述符 | 抓到**活的 2MB BLOCK** desc |
| [4](#q4) | 使用者/核心空間劃分 | `T0SZ=T1SZ=16` |
| [5](#q5) | `PAGE_OFFSET` | **`0xffff000000000000`**（與書不同） |
| [6](#q6) | `KIMAGE_VADDR` | `0xffff800008000000 == MODULES_END` |
| [7](#q7) | `TEXT_OFFSET` | 6.1 已**移除**此巨集 |
| [8](#q8) | 核心映像各段 | kallsyms + 開機 log |
| [9](#q9) | ARM64 記憶體佈局圖 | 模組印出全部邊界 |
| [10](#q10) | `__pa_symbol()` vs `__pa()` | 差 `0x800007c00000` |
| [11](#q11) | 線性映射前映像映到哪 | `kimage_voffset` |
| [12](#q12) | `kimage_voffset` | `0xffff800007c00000` |
| [13](#q13) | PoC vs PoU | `CLIDR_EL1` LoUU=0 LoC=3 |
| [14](#q14) | ASID | **16-bit**，`nG` 位元實測 |
| [15](#q15) | 記憶體屬性 | `MAIR_EL1` 五個 slot 全解碼 |
| [16](#q16) | inner / outer shareable | 頁表 `SH=3` |
| [17](#q17) | DMB / DSB / ISB | **SB litmus：31% 亂序 → dmb 後 0** |
| [18](#q18) | Load-Acquire / Store-Release | 同上 |
| [19](#q19) | 載入位址 vs 運行位址 | `_stext` VA/PA 對照 |
| [20](#q20) | 為何 I-cache 可開、D-cache 必關 | `SCTLR_EL1` |
| [21](#q21) | 為何要建立恆等映射 | `head.S` |
| [22](#q22) | 下一級頁表基址是實體位址 | 描述符 bits[47:12] |
| [23](#q23) | 軟體如何巡覽頁表 | `__va()` 每級換算 |

---

## 實驗工具：`armv8_dump.ko`

本章大部分答案的證據來自一支自己寫的核心模組（原始碼在 `notes/experiments/armv8_dump.c`）。
它做三件事：讀 EL1 系統暫存器、印出核心記憶體佈局巨集、對指定 VA 做軟體頁表巡覽。

```bash
# 1. 傳到機台並編譯（裝置上 /lib/modules/$(uname -r)/build 指向完整核心原始碼樹）
ssh radxa@192.168.68.57 'mkdir -p ~/exp/armv8'
scp notes/experiments/armv8_dump.c radxa@192.168.68.57:~/exp/armv8/
scp notes/experiments/Makefile.mod radxa@192.168.68.57:~/exp/armv8/Makefile
ssh radxa@192.168.68.57 'cd ~/exp/armv8 && make'

# 2. 載入（_stext 從 /proc/kallsyms 取，因為本機 CONFIG_KALLSYMS_ALL 沒開，
#    swapper_pg_dir 等資料符號查不到，模組改由 TTBR1 + kimage_voffset 推算）
ssh radxa@192.168.68.57 'cd ~/exp/armv8
  STEXT=0x$(sudo grep -w _stext /proc/kallsyms | cut -d" " -f1)
  sudo dmesg -C
  sudo insmod armv8_dump.ko stext=$STEXT
  sudo dmesg | sed "s/^\[[^]]*\] //"
  sudo rmmod armv8_dump'
```

---

<a name="q1"></a>
## 1. ARM64 有 TTBR0 和 TTBR1 兩個頁表基址暫存器，處理器如何使用它們？

### 結論

**用虛擬位址的最高位元 `VA[63]` 選擇**：

| `VA[63]` | 使用的暫存器 | 位址範圍 | 誰維護 |
|----------|-------------|---------|--------|
| `0` | **TTBR0_ELx** | `0x0000_0000_0000_0000` ~ `0x0000_ffff_ffff_ffff` | 每個行程各自的 `mm->pgd` |
| `1` | **TTBR1_ELx** | `0xffff_0000_0000_0000` ~ `0xffff_ffff_ffff_ffff` | 全域唯一的 `swapper_pg_dir` |

`TCR_EL1` 用 **`T0SZ` / `T1SZ`** 兩個獨立欄位分別設定兩邊的有效 VA 位元數
（`VA bits = 64 − TxSZ`），也可以用 `EPD0`/`EPD1` 各自停用該邊的 table walk。

**這個設計的核心價值**：行程切換（`switch_mm()` → `cpu_do_switch_mm()`）**只需要換 TTBR0**，
TTBR1 指向的核心頁表在所有行程間**永遠不變**。所以核心位址在任何行程上下文中都能直接存取，
不必切換頁表，這是系統呼叫 / 中斷處理能夠「原地」進入核心的前提。

> **書目**：奔跑吧 §2.1.2「頁表映射」——
> 「在 AArch64 架構中，因為地址總線位寬最多支持 48 位，所以 VA 被劃分為兩個空間，
> 每個空間最多支持 256TB。」

### 實機驗證（TTBR0 == `__pa(current->mm->pgd)`，一位不差）

```bash
sudo insmod armv8_dump.ko stext=$STEXT && sudo dmesg | grep -E "TCR_EL1|T0SZ|T1SZ|TTBR|EPD|A1 "
```

```
TCR_EL1   = 0x000001f2b5503510
   T0SZ = 16  -> TTBR0(user)   有效 VA = 48 bits = 256 TB
   T1SZ = 16  -> TTBR1(kernel) 有效 VA = 48 bits = 256 TB
   TG0  = 0 (4KB)              TG1 = 2 (4KB)
   A1   = 1 (ASID 由 TTBR1_EL1 提供)
   EPD0 = 0  EPD1 = 0
TTBR0_EL1 = 0x000000003430c001   BADDR(PA)=0x00003430c000   ASID=0
TTBR1_EL1 = 0x1476000001b2d001   BADDR(PA)=0x000001b2d000   ASID=5238
   -> current(insmod) mm->pgd VA=0xffff00003430c000  __pa=0x3430c000  應等於 TTBR0 BADDR
```

**`TTBR0_EL1.BADDR = 0x3430c000` 與 `insmod` 行程的 `__pa(mm->pgd) = 0x3430c000` 完全相同** ✅
——這就是「TTBR0 裝的是當前行程的頁表」最直接的證據。

**兩個容易被追問的細節**：

1. **`A1 = 1`**：ASID 不是放在 TTBR0，而是放在 **TTBR1_EL1 的 bits[63:48]**（此刻 = 5238）。
   Linux 在 `arch/arm64/mm/proc.S:430` 明確設定：
   ```asm
   mov_q tcr, TCR_TxSZ(VA_BITS) | TCR_CACHE_FLAGS | TCR_SMP_FLAGS | \
               TCR_TG_FLAGS | TCR_KASLR_FLAGS | TCR_ASID16 | \
               TCR_TBI0 | TCR_A1 | TCR_KASAN_SW_FLAGS | TCR_MTE_FLAGS
   ```
   目的是讓 `switch_mm` 可以先把 ASID 寫進 TTBR1、再換 TTBR0，避免中間狀態下
   硬體用「新 TTBR0 + 舊 ASID」做投機 table walk 而汙染 TLB。

2. **本機開了 KPTI**（`CONFIG_UNMAP_KERNEL_AT_EL0=y`）：回到 EL0 前，
   `tramp_exit` 會把 TTBR1 換成只含 trampoline 的 `tramp_pg_dir`，
   所以「TTBR1 永遠不變」嚴格說是「**在 EL1 執行期間**不變」。

---

<a name="q2"></a>
## 2. 請簡述 ARM64 的 4 級頁表映射過程（4KB 頁、48 位位址）

### 結論

4KB 頁 + 48 位 VA ⇒ 4 級表，每級用 9 個位元索引（512 項 × 8 bytes = 4096 bytes = 剛好一頁）：

```
 63    48 47      39 38      30 29      21 20      12 11         0
┌────────┬──────────┬──────────┬──────────┬──────────┬────────────┐
│ 全 0/1 │ L0 (PGD) │ L1 (PUD) │ L2 (PMD) │ L3 (PTE) │  頁內偏移   │
└───┬────┴────┬─────┴────┬─────┴────┬─────┴────┬─────┴────────────┘
    │         │          │          │          │
 VA[63] 選     9 bits     9 bits     9 bits     9 bits      12 bits
 TTBR0/1     512 項      512 項     512 項     512 項      4096 B

  一個 L3 表覆蓋   2 MB        一個 L2 表覆蓋   1 GB
  一個 L1 表覆蓋 512 GB        一個 L0 表覆蓋 256 TB
```

**步驟**：
1. `VA[63]` 決定用 TTBR0 還是 TTBR1 → 取得 **L0 表的實體基址**
2. `VA[47:39]` 索引 L0 → 表描述符 → L1 表實體基址
3. `VA[38:30]` 索引 L1 → **表**描述符 → L2 表實體基址；或 **1GB 區塊**描述符（轉換結束）
4. `VA[29:21]` 索引 L2 → **表**描述符 → L3 表實體基址；或 **2MB 區塊**描述符（轉換結束）
5. `VA[20:12]` 索引 L3 → **頁描述符**，取出實體頁基址
6. `PA = 頁基址 | VA[11:0]`

> **書目**：奔跑吧 §2.1.2「頁表映射」、§2.1.6「案例分析：ARM64 的頁表映射過程」
> （分析 `__create_pgd_mapping()`）。

### 實機驗證（核心模組完整巡覽 `_stext`）

```bash
sudo insmod armv8_dump.ko stext=0xffff800008010000
sudo dmesg | sed -n '/_stext (核心映像映射區/,/交叉驗證/p'
```

```
  VA = 0xffff800008010000
  VA[63] = 1  ->  使用 TTBR1_EL1 (核心空間)
     L0(PGD)=VA[47:39]=256  L1(PUD)=VA[38:30]=  0  L2(PMD)=VA[29:21]= 64  L3(PTE)=VA[20:12]= 16  off=0x000
  L0 表基址 VA = 0xffff80000972d000 (swapper_pg_dir)
  -> L0 entry 的 VA = 0xffff80000972d800          ← 0x972d000 + 256*8 = 0x972d800 ✅

   L0 desc = 0x10000002fffff003  [1:0]=0b11  TABLE descriptor
        bits[47:12] = 0x0002fffff000    ← 下一級表的【實體位址】
   L1 desc = 0x10000002ffffe003  [1:0]=0b11  TABLE descriptor
        bits[47:12] = 0x0002ffffe000
   L2 desc = 0x10000002ffffd003  [1:0]=0b11  TABLE descriptor
        bits[47:12] = 0x0002ffffd000
   L3 desc = 0x0050000000410783  [1:0]=0b11  PAGE descriptor
        bits[47:12] = 0x000000410000
        AttrIndx=0  AP=2(EL1 RO, EL0 none)  SH=3(Inner Shareable)
        AF=1  nG=0  Contig=1  PXN=0  UXN=1
  ==> L3 是 4KB PAGE 描述符。PA = 0x410000
  交叉驗證 __pa(va) = 0x410000                    ← ✅ 手工巡覽結果與核心巨集完全一致
```

**每一步都可以自己驗算**：
- L0 index = `(0xffff800008010000 >> 39) & 0x1ff` = **256** → entry VA = `0x972d000 + 256×8` = `0x972d800` ✅
- L1 index = `(… >> 30) & 0x1ff` = **0** → entry VA = `__va(0x2fffff000) + 0` = `0xffff0002fffff000` ✅
- L2 index = **64** → entry VA = `0xffff0002ffffe000 + 64×8` = `0xffff0002ffffe200` ✅
- L3 index = **16** → entry VA = `0xffff0002ffffd000 + 16×8` = `0xffff0002ffffd080` ✅
- PA = `0x410000 | 0x000` = **0x410000** ✅

### 使用者空間側的交叉驗證（`/proc/self/pagemap`）

程式 `notes/experiments/pagemap_walk.c` 從使用者態把 VA 翻成 PFN：

```bash
ssh radxa@192.168.68.57 'cd ~/exp && gcc -O2 -o pagemap_walk pagemap_walk.c && sudo ./pagemap_walk'
```

```
.text (main)      VA=0x0000aaaacae20800  present=1  PFN=0x65855   PA=0x00065855800
                   VA[63]=0(TTBR0)  L0=341 L1=171 L2= 87 L3= 32 off=0x800
stack             VA=0x0000ffffd4a4d5af  present=1  PFN=0x6dee0   PA=0x0006dee05af
                   VA[63]=0(TTBR0)  L0=511 L1=511 L2=165 L3= 77 off=0x5af
mmap 未觸碰       VA=0x0000ffffa9261000  present=0  PFN=0x0        ← demand paging
mmap 觸碰後       VA=0x0000ffffa9261000  present=1  PFN=0x72c56
```

同一個使用者 VA 丟給核心模組巡覽（`walk_va=0x...`），得到的 PA 與 pagemap 的
`PFN << 12` 完全一致——**使用者態與核心態兩條獨立路徑互相印證**。

---

<a name="q3"></a>
## 3. L0～L2 頁表項中，如何判斷是區塊（block）還是頁表（table）類型？

### 結論

看描述符**最低 2 個位元 `[1:0]`**：

| `[1:0]` | 類型 | 出現在哪一級 | 意義 |
|---------|------|-------------|------|
| `0b11` | **Table descriptor** | L0 / L1 / L2 | bits[47:12] = 下一級表的實體基址，繼續往下走 |
| `0b11` | **Page descriptor** | **L3** | bits[47:12] = 最終 4KB 實體頁；L3 沒有 table，bit1 語意變成「page」 |
| `0b01` | **Block descriptor** | **只有 L1 / L2** | 轉換到此結束。L1 → 1 GB，L2 → 2 MB |
| `0b00` / `0b10` | **Invalid** | 任何一級 | bit0=0 ⇒ Translation Fault |

**兩個容易記錯的地方**：
1. **L0 不支援 block**（4KB 粒度下 L0 block 會是 512 GB，架構未定義）。
2. **L3 的 `0b01` 是 reserved/invalid**，不是 block——L3 的有效葉子一定是 `0b11`。

> **書目**：奔跑吧 §2.1.3「頁表項描述符」。

### 實機驗證（抓到一個活的 2MB BLOCK）

本機 **線性映射與核心映像整段都是 4KB 頁**（因為 arm64 預設 `rodata=full`，
`map_mem()` 會帶 `NO_BLOCK_MAPPINGS`），所以要看 block 得往 **VMEMMAP 區**找：

```bash
sudo insmod armv8_dump.ko walk_va=0xfffffc0000040000    # VMEMMAP 區的 struct page 陣列
sudo dmesg | sed -n '/walk_va 參數/,/done/p'
```

```
  VA = 0xfffffc0000040000
     L0(PGD)=504  L1(PUD)=0  L2(PMD)=0  L3(PTE)=64
   L0 desc = 0x10000002fee91003   [1:0]=0b11   TABLE descriptor
   L1 desc = 0x10000002fee90003   [1:0]=0b11   TABLE descriptor
   L2 desc = 0x00600002f6e00701   [1:0]=0b01   BLOCK descriptor        ← ★
        bits[47:12] = 0x0002f6e00000
        AttrIndx=0  AP=0(EL1 RW, EL0 none)  SH=3(Inner Shareable)
        AF=1  nG=0  PXN=1  UXN=1
  ==> L2 是 2MB BLOCK, 轉換到此結束。PA = 0x2f6e40000
```

**對照組**（同一次執行的另外三個位址）全都是 `[1:0]=0b11` 一路走到 L3：

| 位址 | L2 描述符 `[1:0]` | 結果 |
|------|------------------|------|
| `_stext`（核心映像） | `0b11` TABLE | 4KB PAGE |
| `PAGE_OFFSET+2MB`（線性映射） | `0b11` TABLE | 4KB PAGE |
| 模組程式碼（vmalloc 區） | `0b11` TABLE | 4KB PAGE |
| **VMEMMAP** | **`0b01` BLOCK** | **2MB，轉換提前結束** |

`vmemmap_populate()`（`arch/arm64/mm/mmu.c`）刻意用 PMD block 映射 `struct page` 陣列——
省 1/512 的頁表記憶體，也少一級 table walk。

---

<a name="q4"></a>
## 4. ARM64 Linux 中，使用者空間和核心空間如何劃分？

### 結論

**看 `VA[63]`（實際上是 `VA[63:48]` 必須全 0 或全 1，否則是 non-canonical 位址）**：

```
0x0000_0000_0000_0000 ┬───────────────────────────┐
                      │  使用者空間 (TTBR0)         │  2^48 = 256 TB
0x0000_ffff_ffff_ffff ┴───────────────────────────┘
                            （不可用的位址空洞）
0xffff_0000_0000_0000 ┬───────────────────────────┐
                      │  核心空間 (TTBR1)           │  2^48 = 256 TB
0xffff_ffff_ffff_ffff ┴───────────────────────────┘
```

大小由 `TCR_EL1.T0SZ` / `T1SZ` 決定，`VA bits = 64 − TxSZ`。兩者**可以不同**
（例如 52-bit VA 的機器 T1SZ 可能與 T0SZ 不同），本機都是 16 → 各 48 bits。

> **書目**：奔跑吧 §2.1.5「ARM64 內核內存分布」——
> 「用戶空間：0x0000 0000 0000 0000～0x0000 FFFF FFFF FFFF。
> 內核空間：0xFFFF 0000 0000 0000～0xFFFF FFFF FFFF FFFF。
> 64 位 Linux 內核中沒有高端內存，因為 48 位的尋址空間已經足夠大了。」

### 實機驗證

```bash
sudo dmesg | grep -E "T0SZ|T1SZ|user space"
```

```
   T0SZ = 16  -> TTBR0(user)   有效 VA = 48 bits = 256 TB
   T1SZ = 16  -> TTBR1(kernel) 有效 VA = 48 bits = 256 TB
  user space   : 0x0000000000000000 - 0x0000ffffffffffff  (TTBR0, 每行程獨立)
```

從 `pagemap_walk` 印出的實際使用者位址也看得出來，**全部都是 `0x0000...` 開頭**：

```
.text  VA=0x0000aaaacae20800     ← ASLR 把可執行檔放在 0xaaaa... 區
stack  VA=0x0000ffffd4a4d5af     ← 堆疊在使用者空間頂端 0x0000ffff...
mmap   VA=0x0000ffffa8e50000     ← mmap 區由上往下長
```

而核心的 `_stext = 0xffff800008010000` 是 `0xffff...` 開頭 ✅

---

<a name="q5"></a>
## 5. `PAGE_OFFSET` 表示什麼意思？

### 結論

`PAGE_OFFSET` 是**實體記憶體在核心空間做「線性映射（linear / direct mapping）」的起始虛擬位址**。
核心開機時把（幾乎）全部實體記憶體按**固定偏移**一次映射進去，於是

```c
虛擬位址 = 實體位址 − PHYS_OFFSET + PAGE_OFFSET      /* __va() */
實體位址 = 虛擬位址 − PAGE_OFFSET + PHYS_OFFSET      /* __pa() */
```

只要一個加減法就能互換，**不必查頁表**，這是核心能高效直接存取任意實體頁的基礎
（`page_address()`、`kmap_local_page()` 等都靠它）。

**6.1 的定義**（`arch/arm64/include/asm/memory.h:44-45`）：

```c
#define _PAGE_OFFSET(va)  (-(UL(1) << (va)))
#define PAGE_OFFSET       (_PAGE_OFFSET(VA_BITS))
```

`VA_BITS = 48` ⇒ `PAGE_OFFSET = -(1 << 48)` = **`0xffff000000000000`**。

### ⚠️ 與書本的重大差異（版本演進）

> **書目**：奔跑吧 §2.1.5 明確寫著
> 「PAGE_OFFSET 表示物理內存在內核空間裡做線性映射的起始地址，
> 在 ARM64 的 Linux 內核中該值定義為 **0xFFFF 8000 0000 0000**。」
> 並附上 Linux 5.0 的定義：
> ```c
> #define PAGE_OFFSET  (UL(0xffffffffffffffff) - (UL(1) << (VA_BITS - 1)) + 1)
> ```

注意分母是 **`VA_BITS - 1`**（5.0）而 6.1 是 **`VA_BITS`**。這一個字元的差異，
代表 **Linux 5.4（commit `14c127c957c1` "arm64: mm: Flip kernel VA space"）
把整個核心位址空間上下翻轉了**：

| | Linux ≤ 5.3（書本） | **Linux ≥ 5.4（本機 6.1）** |
|---|---|---|
| `PAGE_OFFSET`（線性映射） | `0xffff800000000000`（核心空間**上半部**） | **`0xffff000000000000`（核心空間最低處）** |
| modules / 核心映像 / vmalloc | 核心空間**下半部** `0xffff0000...` | **上半部 `0xffff800000000000` 之後** |

翻轉的目的是為了支援 52-bit VA：讓線性映射從固定的低位址開始向上長，
`VA_BITS` 從 48 變 52 時**只需要把上界往外推**，不必移動整個佈局。

### 實機驗證

```bash
sudo dmesg | grep -E "PAGE_OFFSET|PAGE_END|MODULES_VADDR|memstart|__va|__pa\(PAGE"
```

```
  MODULES_VADDR: 0xffff800000000000     ← 書本說這裡是 PAGE_OFFSET，6.1 變成 modules 起點
  PAGE_OFFSET  : 0xffff000000000000     ← 6.1 的真值
  PAGE_END     : 0xffff800000000000
  memstart_addr (PHYS_OFFSET) = 0x0
  __pa(PAGE_OFFSET)   = 0x0                    (應 == memstart_addr)  ✅
  __va(memstart_addr) = 0xffff000000000000     (應 == PAGE_OFFSET)    ✅
```

線性映射區實測（walk `PAGE_OFFSET + 2MB`）：

```
  VA = 0xffff000000200000  ==> PA = 0x200000       ← 差值正好 PAGE_OFFSET − PHYS_OFFSET
  交叉驗證 __pa(va) = 0x200000                      ✅
```

> **為什麼 `memstart_addr = 0` 而 DTB 說記憶體從 `0x200000` 開始？**
> `arm64_memblock_init()` 會做 `memstart_addr = round_down(memblock_start_of_DRAM(), ARM64_MEMSTART_ALIGN)`，
> 而 `ARM64_MEMSTART_ALIGN` 在 4KB 頁時 = `1 << PUD_SHIFT` = **1 GB**
> （`arch/arm64/include/asm/kernel-pgtable.h:131,147`），
> 所以 `round_down(0x200000, 1GB) = 0`。這也是 [Ch3 Q6](./ch03_memory_management_prerequisites_ANSWERS.md#q6) 的伏筆。

---

<a name="q6"></a>
## 6. `KIMAGE_VADDR` 表示什麼意思？

### 結論

**核心映像（vmlinux 的 `.text/.rodata/.data/.bss`）被映射到核心虛擬位址空間的起始位址。**

6.1 的定義（`arch/arm64/include/asm/memory.h:46-49`）：

```c
#define KIMAGE_VADDR    (MODULES_END)
#define MODULES_END     (MODULES_VADDR + MODULES_VSIZE)
#define MODULES_VADDR   (_PAGE_END(VA_BITS_MIN))     /* = 0xffff800000000000 */
#define MODULES_VSIZE   (SZ_128M)
```

也就是說 **核心映像映射區緊接在 128 MB 的 modules 區之後**。

**為什麼不放在線性映射區裡？** 因為兩者要解耦：
- 核心映像的**虛擬**位址在連結時就固定了（vmlinux.lds.S），
- 但它的**實體**載入位址由 bootloader 決定（只要求 2MB 對齊）。

分開之後，映像放哪裡都行，且 KASLR 可以獨立隨機化「映像位置」與「線性映射位置」兩件事。
兩者各自的 VA↔PA 偏移分別是 `kimage_voffset`（[Q12](#q12)）和 `PAGE_OFFSET−PHYS_OFFSET`。

> **書目**：奔跑吧 §2.1.5 ——
> 「KIMAGE_VADDR 表示內核映像文件映射到內核空間的起始虛擬地址。它的值等於 MODULES_END 的值…
> KIMAGE_VADDR 宏的值為 **0xFFFF 0000 1000 0000**」
> 書本值是 5.0 佈局下的（`MODULES_VADDR = BPF_JIT_REGION_END`）；6.1 因為位址空間翻轉（見 [Q5](#q5)），
> 這個值變成 `0xffff800008000000`。**「等於 MODULES_END」這個關係則從 5.0 到 6.1 都沒變。**

### 實機驗證

```bash
sudo dmesg | grep -E "MODULES_|KIMAGE_VADDR|VMALLOC_START"
```

```
  MODULES_VADDR: 0xffff800000000000
  MODULES_END  : 0xffff800008000000   (MODULES_VSIZE = 128 MB)
  KIMAGE_VADDR : 0xffff800008000000   <- == MODULES_END ✅
  VMALLOC_START: 0xffff800008000000   <- 也等於 MODULES_END
```

驗算：`0xffff800000000000 + 128MB(0x8000000) = 0xffff800008000000` ✅

**`_stext = 0xffff800008010000`**，正好是 `KIMAGE_VADDR + 0x10000`（64 KB 的映像頭部）。

**旁證**：載入的核心模組落在 modules 區內：

```
本模組的 init 函式 VA = 0xffff800001119000
                       ↑ 介於 MODULES_VADDR(0xffff800000000000) 與 MODULES_END(0xffff800008000000) 之間 ✅
```

---

<a name="q7"></a>
## 7. `TEXT_OFFSET` 表示什麼意思？

### 結論（這題的正確答案是「它已經不存在了」）

**舊機制（Linux < 5.8）**：`TEXT_OFFSET` 是**核心映像入口相對於 RAM 起始位址的偏移**，
預設 `0x00080000`（512 KB）。它被寫在 ARM64 映像頭部（`Image` 的 header 第 2 欄位 `text_offset`），
bootloader 必須把映像載到 `RAM_BASE + TEXT_OFFSET`。連結腳本裡是：

```
. = KIMAGE_VADDR + TEXT_OFFSET;
```

搭配 `CONFIG_ARM64_RANDOMIZE_TEXT_OFFSET` 還可以在這個偏移上做隨機化，
當作 KASLR 還沒實作前的替代品。

**為什麼被移除**：
- Linux 4.6 之後核心映像本身已支援**任意 2MB 對齊位址載入 + 自我重定位**，
  bootloader 不必再預留固定頭部空間；
- 5.8（commit `120dc60d0bdb` "arm64: remove TEXT_OFFSET randomisation"）
  正式把 `TEXT_OFFSET` 固定為 0 並移除相關程式碼；
- 現在 `Image` header 的 `text_offset` 欄位固定填 0，
  改由 `image_size` 欄位告訴 bootloader 要留多少空間。

### 實機驗證（原始碼裡查無此物）

```bash
grep -rn "TEXT_OFFSET" arch/arm64/Makefile arch/arm64/kernel/vmlinux.lds.S \
                       arch/arm64/include/asm/ 2>/dev/null; echo "exit=$?"
```

本專案樹（6.1.115）**沒有任何比對**。

實機上映像的實際落點：

```
KIMAGE_VADDR        = 0xffff800008000000
_stext              = 0xffff800008010000     → 差 0x10000 = 64 KB（映像 header + 對齊），不是 0x80000
__pa_symbol(_stext) = 0x410000               → 實體上載在 0x400000（4 MB），2 MB 對齊 ✅
```

**回答這題的正確姿勢**：先說明書上描述的歷史機制與用途，再指出
「目前核心已移除，改用可重定位映像 + KASLR」，並用 `grep` 佐證。

---

<a name="q8"></a>
## 8. 核心映像包含哪些段？作用是什麼？在 System.map 中用哪些符號表示起訖？

### 結論

由 `arch/arm64/kernel/vmlinux.lds.S` 定義：

| 段 | 作用 | 起訖符號 | 權限（映射後） |
|----|------|---------|--------------|
| `.head.text` | 映像頭 + `_start` 開機入口 | `_text` ~ | RX |
| `.text` | 核心程式碼 | `_stext` ~ `_etext` | **RO + X** |
| `.rodata` | 唯讀資料、字串常數、異常表、`__ex_table` | `__start_rodata` ~ `__end_rodata` | **RO + NX** |
| `.init.*` | 只在初始化用的程式碼/資料、`initcall` 表、`.altinstructions` | `__init_begin` ~ `__init_end` | 開機後**整段釋放** |
| `.data` | 已初始化的可寫全域/靜態變數 | `_sdata` ~ `_edata` | RW + NX |
| `.bss` | 未初始化（歸零）全域/靜態變數 | `__bss_start` ~ `__bss_stop` | RW + NX |
| — | 映像結束 | `_end` | |

> **書目**：奔跑吧 §2.6.1「連結文件基礎知識」、§2.6.2「vmlinux.lds.S 文件分析」、§2.6.3。

### 實機驗證

**(a) 符號位址**（本機 `CONFIG_KALLSYMS_ALL` 沒開，`/proc/kallsyms` 只留下三個關鍵符號）：

```bash
sudo grep -wE "_stext|_etext|__init_begin" /proc/kallsyms
```

```
ffff800008010000 T _stext
ffff800009070000 D _etext
ffff800009730000 T __init_begin
```

⇒ `.text` 大小 = `0x9070000 − 0x8010000` = `0x1060000` = **16,768 KB**

**(b) 開機日誌給出所有段的大小**（`mm_init()` → `mem_init()`）：

```bash
sudo journalctl -k -b | grep "^.*Memory:"
```

```
Memory: 7840408K/8386560K available (16768K kernel code, 3796K rwdata, 6856K rodata,
        7296K init, 769K bss, 284008K reserved, 262144K cma-reserved)
                     ↑             ↑            ↑           ↑          ↑
                   .text        .data       .rodata      .init      .bss
```

**`16768K kernel code` 與 (a) 用符號算出來的 16,768 KB 完全吻合** ✅

**(c) init 段開機後被釋放**：

```bash
sudo journalctl -k -b | grep -i "Freeing"
```

```
Freeing initrd memory: 19668K
Freeing unused kernel memory: 7296K      ← 正好等於上面的「7296K init」✅
```

`free_initmem()` 把 `__init_begin ~ __init_end` 整段還給伙伴系統
（這也是 [Ch6 Q3](./ch06_memory_management_case_studies_ANSWERS.md#q3) 裡 MemTotal 對帳的一項）。

**(d) 段的權限可以從頁表看出來**（`armv8_dump.ko` 巡覽 `_stext`）：

```
   L3 desc = 0x0050000000410783
        AP=2 (EL1 RO, EL0 none)   PXN=0   UXN=1
        ↑ 唯讀            ↑ EL1 可執行   ↑ EL0 不可執行
```

**`.text` 確實被映射成「核心唯讀 + 核心可執行 + 使用者完全不可存取」** ✅
（`mark_rodata_ro()` 的成果；`rodata=full` 預設開啟）

---

<a name="q9"></a>
## 9. 請畫出 ARM64 Linux 核心的記憶體佈局

### 結論（**本機實測值，與書本的 5.0 佈局上下顛倒**）

```
0x0000000000000000  ┌──────────────────────────────────┐
                    │   使用者空間 (TTBR0，每行程獨立)     │  256 TB
0x0000ffffffffffff  └──────────────────────────────────┘
                              ✂ 不可用的位址空洞 ✂
0xffff000000000000  ┌──────────────────────────────────┐ ← PAGE_OFFSET
                    │  ★ 實體記憶體線性映射區             │  128 TB
                    │    __va(pa) = pa − PHYS_OFFSET     │
                    │              + PAGE_OFFSET         │
0xffff800000000000  ├──────────────────────────────────┤ ← PAGE_END / MODULES_VADDR
                    │  核心模組區 (modules)               │  128 MB
0xffff800008000000  ├──────────────────────────────────┤ ← MODULES_END
                    │        = KIMAGE_VADDR              │   核心映像 vmlinux
                    │        = VMALLOC_START             │   [_text .. _end]
                    │  vmalloc / ioremap / 核心堆疊       │  ~124 TB
0xfffffbfff0000000  ├──────────────────────────────────┤ ← VMALLOC_END
                    │           (保留 256 MB)            │
0xfffffbfffe000000  ├──────────────────────────────────┤ ← FIXADDR_TOP
0xfffffbfffe800000  ├──────────────────────────────────┤ ← PCI_IO_START
                    │  PCI I/O 空間  (16 MB)             │
0xfffffbffff800000  ├──────────────────────────────────┤ ← PCI_IO_END
0xfffffc0000000000  ├──────────────────────────────────┤ ← VMEMMAP_START
                    │  VMEMMAP（struct page 陣列）  2 TB  │
0xfffffe0000000000  └──────────────────────────────────┘ ← VMEMMAP_END
```

各邊界由 `arch/arm64/include/asm/memory.h` 依 `VA_BITS` 算出：

```c
#define PAGE_OFFSET     (-(UL(1) << VA_BITS))                 /* 0xffff000000000000 */
#define MODULES_VADDR   (_PAGE_END(VA_BITS_MIN))              /* 0xffff800000000000 */
#define MODULES_END     (MODULES_VADDR + SZ_128M)
#define KIMAGE_VADDR    (MODULES_END)
#define VMALLOC_START   (MODULES_END)                         /* pgtable.h:24 */
#define VMALLOC_END     (VMEMMAP_START - SZ_256M)             /* pgtable.h:25 */
#define VMEMMAP_START   (-(UL(1) << (VA_BITS - VMEMMAP_SHIFT)))
#define PCI_IO_END      (VMEMMAP_START - SZ_8M)
#define FIXADDR_TOP     (VMEMMAP_START - SZ_32M)
```

> **書目**：奔跑吧 §2.1.5「ARM64 內核內存分布」（圖 2.8 是 QEMU 上的 Linux 5.0 佈局）。
> 該節也提到「這部分信息的輸出是在 `mem_init()` 函數中實現的。注意，該信息已經在
> Linux 4.16 內核中刪除」——所以**本機開機日誌不會印佈局表**，只能靠模組或原始碼推算，
> 這正是我寫 `armv8_dump.ko` 的原因。

### 實機驗證

```bash
sudo insmod armv8_dump.ko && sudo dmesg | sed -n '/kernel VA layout/,/線性映射區自我檢驗/p'
```

```
=============== [2] kernel VA layout (VA_BITS=48, PAGE_SIZE=4K) ===============
  user space   : 0x0000000000000000 - 0x0000ffffffffffff  (TTBR0, 每行程獨立)
  ---- 以下屬核心空間 (TTBR1, 全域共享) ----
  MODULES_VADDR: 0xffff800000000000
  MODULES_END  : 0xffff800008000000   (MODULES_VSIZE = 128 MB)
  KIMAGE_VADDR : 0xffff800008000000
  VMALLOC_START: 0xffff800008000000
  VMALLOC_END  : 0xfffffbfff0000000
  PAGE_OFFSET  : 0xffff000000000000
  PAGE_END     : 0xffff800000000000
  FIXADDR_TOP  : 0xfffffbfffe000000
  PCI_IO_START : 0xfffffbfffe800000   PCI_IO_END = 0xfffffbffff800000
  VMEMMAP_START: 0xfffffc0000000000   VMEMMAP_END= 0xfffffe0000000000
```

**四個區域各取一個位址巡覽，證明它們確實落在宣稱的區間裡**：

| 位址 | 落在 | 巡覽結果 |
|------|------|---------|
| `0xffff000000200000` | 線性映射 | PA `0x200000`，`__pa()` 可用 |
| `0xffff800001119000` | **modules** | PA `0x34214000`，`__pa()` **給出垃圾** `0xfffffffff9519000` |
| `0xffff800008010000` | 核心映像 | PA `0x410000` |
| `0xfffffc0000040000` | **VMEMMAP** | 2MB block，PA `0x2f6e40000` |

「modules 區用 `__pa()` 得到垃圾」是很好的教材：**`__pa()` 只對線性映射區有效**（見 [Q10](#q10)）。

---

<a name="q10"></a>
## 10. `__pa_symbol()` 和 `__pa()` 有什麼區別？

### 結論

兩者都是「核心虛擬位址 → 實體位址」，但**適用的位址來源不同，用的偏移量也不同**：

| 巨集 | 適用位址 | 公式 | 偏移來源 |
|------|---------|------|---------|
| `__pa(x)` | **線性映射區**（`PAGE_OFFSET` ~ `PAGE_END`） | `x − PAGE_OFFSET + PHYS_OFFSET` | 開機時算出的 `memstart_addr` |
| `__pa_symbol(x)` | **核心映像符號**（`KIMAGE_VADDR` 之後） | `x − kimage_voffset` | 開機時 `head.S` 算出的 `kimage_voffset` |

原始碼（`arch/arm64/include/asm/memory.h`）：

```c
#define __is_lm_address(addr)  (((u64)(addr) - PAGE_OFFSET) < (PAGE_END - PAGE_OFFSET))
#define __lm_to_phys(addr)     (((addr) - PAGE_OFFSET) + PHYS_OFFSET)
#define __kimg_to_phys(addr)   ((addr) - kimage_voffset)

#define __virt_to_phys_nodebug(x) ({                                   \
        phys_addr_t __x = (phys_addr_t)(__tag_reset(x));               \
        __is_lm_address(__x) ? __lm_to_phys(__x) : __kimg_to_phys(__x); \
})
#define __pa_symbol_nodebug(x)  __kimg_to_phys((phys_addr_t)(x))
```

**現代核心的 `__pa()` 其實會用 `__is_lm_address()` 先判斷、自動走對的分支**，
所以拿映像符號餵給 `__pa()` 不會算錯；但：
1. `__pa_symbol()` **省掉那個判斷**，對已知是映像符號的路徑更快；
2. 開了 `CONFIG_DEBUG_VIRTUAL` 時，`__pa()` 會 `VIRTUAL_BUG_ON(!__is_lm_address())`，
   誤用會直接噴警告——這就是核心堅持分兩個巨集的意義；
3. **`__pa()` 對「既不是線性映射、也不是核心映像」的位址（vmalloc / modules / vmemmap）
   會靜靜地回傳垃圾**，這才是真正的地雷。

> **書目**：奔跑吧 §2.1.5 與 §2.7 相關討論。

### 實機驗證

```bash
sudo dmesg | sed -n '/__pa_symbol() vs __pa()/,/線性映射區自我檢驗/p'
```

```
  _stext (由 /proc/kallsyms 傳入) = 0xffff800008010000
  __pa_symbol(_stext)             = 0x410000            ← 正確
  手算 _stext - kimage_voffset    = 0x410000            ← 完全相同 ✅
  若用線性映射公式 (naive __pa)   = 0x800008010000      ← 錯誤答案
  兩者相差 0x800007c00000
  原因: kimage_voffset      = 0xffff800007c00000
        線性映射偏移         = PAGE_OFFSET − PHYS_OFFSET = 0xffff000000000000
```

**兩個偏移量差了 `0x800007c00000`（≈ 128 TB + 124 MB）** ——這就是混用兩個巨集的代價。

**「`__pa()` 對第三類位址回傳垃圾」的實證**（模組自己的程式碼在 modules 區）：

```
本模組的 init 函式 VA = 0xffff800001119000
  頁表巡覽的真實 PA    = 0x34214000        ← 正確答案
  __pa(va)             = 0xfffffffff9519000 ← 垃圾！既非線性映射也非核心映像
```

**線性映射區則兩邊都對**：

```
  __pa(PAGE_OFFSET)   = 0x0                    (== memstart_addr) ✅
  __va(memstart_addr) = 0xffff000000000000     (== PAGE_OFFSET)   ✅
```

---

<a name="q11"></a>
## 11. 實體記憶體還沒線性映射到核心空間時，核心映像映射到什麼地方？

### 結論

在 `head.S` 的早期組合語言階段，核心會建立**兩份小範圍的臨時映射**：

1. **恆等映射（identity map，`idmap_pg_dir`）**：`VA == PA`，只涵蓋
   `__idmap_text_start ~ __idmap_text_end` 這一小段（開啟 MMU 那幾條指令），
   放進 **TTBR0**。用途見 [Q21](#q21)。
2. **核心映像映射（`init_pg_dir` → 之後的 `swapper_pg_dir`）**：把映像
   （`_text ~ _end`）映射到 **`KIMAGE_VADDR` 之後的虛擬位址**，放進 **TTBR1**。
   `__create_page_tables` / `map_kernel()` 只映射映像本身那幾 MB，
   **不涵蓋全部實體記憶體**。

建立完之後 `head.S` 立刻算出並存下 `kimage_voffset`：

```asm
/* arch/arm64/kernel/head.S */
    ldr_l   x4, kimage_vaddr            // 映像的虛擬基址
    sub     x4, x4, x0                  // 減掉實體基址
    str_l   x4, kimage_voffset, x5      // kimage_voffset = VA - PA
```

在這個階段，`__pa()` **還不能用**（`memstart_addr` 尚未由 `arm64_memblock_init()` 設定，
線性映射也還不存在），只有 `__pa_symbol()` 能用（因為 `kimage_voffset` 已就緒）。

真正的**全記憶體線性映射**要等到 `setup_arch()` → `paging_init()` → `map_mem()`
才依 memblock 登記的區間逐段建立起來。

> **書目**：奔跑吧 §2.6.4「創建恆等映射和內核映像映射」、§2.6.6「__primary_switch 函數分析」。

### 實機驗證

```bash
sudo dmesg | grep -E "kimage_voffset|swapper_pg_dir|memstart"
```

```
  memstart_addr (PHYS_OFFSET) = 0x0
  kimage_voffset              = 0xffff800007c00000
  swapper_pg_dir PA (TTBR1)   = 0x1b2d000
  swapper_pg_dir VA (推算)    = 0xffff80000972d000   (= PA + kimage_voffset)
```

**注意 `swapper_pg_dir` 自己也住在核心映像裡**（它是 `.data` 區的靜態陣列），
所以它的 VA 要用 `PA + kimage_voffset` 換算，**不能用 `__va()`**。
本模組就是靠這個關係從 TTBR1 反推出 `swapper_pg_dir` 的 VA，
而且後續用它成功巡覽了四個不同區域的位址 —— **反推正確性得到驗證** ✅

映像的實體落點：

```
_stext VA = 0xffff800008010000
_stext PA = 0xffff800008010000 − 0xffff800007c00000 = 0x410000
```

⇒ bootloader（U-Boot）把 `Image` 載在實體 **`0x400000`（4 MB）**，符合 2 MB 對齊要求。

---

<a name="q12"></a>
## 12. `kimage_voffset` 代表什麼意思？

### 結論

```
kimage_voffset = 核心映像的虛擬位址 − 核心映像的實體位址
```

定義在 `arch/arm64/mm/mmu.c:59`：

```c
u64 kimage_voffset __ro_after_init;
EXPORT_SYMBOL(kimage_voffset);
```

它和 `PAGE_OFFSET` 是**平行的兩個「錨點」**，各自服務一段映射：

| | 服務的映射 | 偏移量 | 換算巨集 |
|---|---|---|---|
| `PAGE_OFFSET − PHYS_OFFSET` | 全部實體記憶體的**線性映射** | `0xffff000000000000` | `__pa()` / `__va()` |
| **`kimage_voffset`** | **核心映像自身**那一小段映射 | `0xffff800007c00000` | `__pa_symbol()` |

它在 `head.S` 建立映像映射後**立刻**算出（見 [Q11](#q11)），因此在開機極早期
（memblock 都還沒初始化）就能做映像符號的位址換算，且**只要一次減法，不必查頁表**。

### 實機驗證

```bash
sudo dmesg | grep -E "kimage_voffset|__pa_symbol|手算"
```

```
  kimage_voffset              = 0xffff800007c00000
  _stext                      = 0xffff800008010000
  __pa_symbol(_stext)         = 0x410000
  手算 _stext − kimage_voffset = 0x410000        ← 逐位元相同 ✅
```

**用它反推 `swapper_pg_dir` 的 VA 並成功巡覽頁表**（見 [Q11](#q11)），
是這個變數語意正確的最強證明。

---

<a name="q13"></a>
## 13. PoC 和 PoU 有什麼區別？

### 結論

兩者都是「一致性觀察點」，差別在**觀察範圍**：

| | **PoU**（Point of Unification） | **PoC**（Point of Coherency） |
|---|---|---|
| 視角 | **單一 CPU 核心內部** | **整個系統** |
| 統一誰 | 該核的 **I-cache、D-cache、TLB(table walk)** | 所有能存取記憶體的 agent：CPU、GPU、DMA、VPU、NPU… |
| 通常落在 | 該核的 L2（或更外層的統一 cache） | **主記憶體（DDR）**，或系統級一致性互連的某一點 |
| 典型用途 | **自我修改程式碼**：改完指令要 `DC CVAU`（清 D 到 PoU）+ `IC IVAU`（無效 I 到 PoU） | **DMA / 跨裝置共享緩衝區**：`DC CIVAC`（清+無效到 PoC） |

**一句話**：改程式碼用 PoU，跟外面的硬體交換資料用 PoC。

`CLIDR_EL1` 直接告訴你它們在第幾級：
- `LoUU`  = Level of Unification, Uniprocessor
- `LoUIS` = Level of Unification, Inner Shareable
- `LoC`   = Level of Coherency

> **書目**：奔跑吧 §2.2「高速緩存管理」。
> **TRM**：`books/rk3588_trm/part1/chapter_03.txt` §3.2 —
> 「the cores connect to system bus through **DSU-L3** which can handle with CDC issue」。

### 實機驗證

```bash
sudo dmesg | grep -E "CLIDR_EL1|LoUU|LoC|LoUIS|cache type|CTR_EL0|IminLine|DminLine|L1Ip"
```

```
CTR_EL0   = 0x000000009444c004
   IminLine=4 -> I-cache line = 64 bytes
   DminLine=4 -> D-cache line = 64 bytes
   L1Ip=3 (PIPT)      CWG=4      ERG=4
CLIDR_EL1 = 0x00000000c3000123
   LoUU  = 0     <- PoU
   LoC   = 3     <- PoC
   LoUIS = 0     <- PoU for Inner Shareable
   L1 cache type = 3 (I+D separate)
   L2 cache type = 4 (unified)
   L3 cache type = 4 (unified)
```

**怎麼讀這組數字**：

- **`LoUU = 0` / `LoUIS = 0`**：PoU 在**第 0 級之上**，也就是**不需要清任何一級 cache
  就已經達成 I/D 統一**。原因是 Cortex-A76 的 L1 I-cache 對 L1 D-cache 的寫入是
  硬體維護一致的（`CTR_EL0.L1Ip = 3` = **PIPT**，沒有別名問題，且 DSU 會做 snoop）。
  這讓 `flush_icache_range()` 在這顆 SoC 上便宜很多。
- **`LoC = 3`**：PoC 在**第 3 級之後**，也就是必須一路清穿 L1 → L2 → **L3(DSU)** 才到達
  全系統一致點。這正對應 TRM 的架構圖：8 個核共用一個 DSU-L3，L3 之外才是系統匯流排。
- **`DminLine = IminLine = 64 bytes`**：cache 維護指令的最小粒度。
  `CWG=4`（64 B）決定了 `____cacheline_aligned` 的大小、也是**避免 false sharing 的對齊單位**。

**這個結論可以被 [Q17](#q17) 的實驗間接印證**：SB litmus 裡我刻意把 `x` 和 `y`
分別放在**不同的 64 byte cache line**（`struct slot` 用 `int pad[15]` 撐開），
才觀察得到乾淨的亂序率；若放同一條 line，結果會被 cache line 的原子性汙染。

---

<a name="q14"></a>
## 14. ASID 是什麼意思？有什麼作用？

### 結論

**ASID（Address Space ID）= 硬體給每個行程位址空間的標籤**，用來讓不同行程的
TLB 表項**共存於 TLB 中而不互相衝突**。

- TLB 命中的條件從「VA 相符」變成「**VA 相符 且 ASID 相符**」；
- 於是 `switch_mm()` **不需要 flush 整個 TLB**，只要換 TTBR0 + ASID；
- 沒有 ASID 的話，每次行程切換都得 `TLBI VMALLE1`，切換後必然是一連串 TLB miss。

**Global vs non-global**：頁表項有個 **`nG` 位元**：
- `nG = 0`（global）→ 這條 TLB 表項**對所有 ASID 有效**，用於**核心頁**（核心映射在所有行程中相同）；
- `nG = 1`（non-global）→ 這條表項綁定當前 ASID，用於**使用者頁**。

**Linux 的 ASID 管理**（`arch/arm64/mm/context.c`）：
硬體 ASID 只有 8 或 16 bits，一定會用完。核心用「**版本號（generation）+ ASID**」的方式：
`mm->context.id` 高位存 generation、低位存 ASID；ASID 用完時 generation +1，
並對所有 CPU 做一次全域 TLB flush（`flush_context()`），然後重新發放。

### 實機驗證

```bash
sudo dmesg | grep -E "AS   =|ASIDBits|TTBR1_EL1|A1 "
```

```
   AS   = 1 -> ASID 寬度 16 bits              ← TCR_EL1.AS
   A1   = 1 (ASID 由 TTBR1_EL1 提供)
ID_AA64MMFR0_EL1 = 0x0000000000101122
   ASIDBits = 2 -> 硬體支援 16-bit ASID (65536 個)
TTBR1_EL1 = 0x1476000001b2d001   BADDR(PA)=0x000001b2d000   ASID=5238
```

**RK3588 支援 16-bit ASID（65536 個）且 Linux 確實啟用了**（`TCR_EL1.AS = 1`）。
當下 `insmod` 行程拿到的 ASID 是 **5238**。

**`nG` 位元的實測對照**（同一次 dmesg 的四個 walk）：

| 巡覽的位址 | 區域 | `nG` | 意義 |
|-----------|------|------|------|
| `_stext` | 核心映像 | **0** | global，所有行程共用 |
| `PAGE_OFFSET+2MB` | 線性映射 | **0** | global |
| 模組程式碼 | modules | **0** | global |
| `0x0000aaaad26a0000` | **使用者程式碼** | **1** | **non-global，綁 ASID** |

**核心頁 `nG=0`、使用者頁 `nG=1`**，一個位元把 [Q1](#q1) 的「TTBR1 不隨行程切換」
和 ASID 機制串起來了 ✅

**查看 ASID 迴轉（rollover）發生的頻率**：

```bash
ssh radxa@192.168.68.57 'sudo cat /proc/interrupts | grep -i "TLB\|IPI"'
ssh radxa@192.168.68.57 'grep -c . /proc/[0-9]*/status 2>/dev/null | wc -l'   # 行程數 << 65536
```

本機只有 295 個行程，遠小於 65536，所以幾乎不會發生 rollover。
（8-bit ASID 的舊平台只有 256 個，就很容易迴轉，這是 16-bit ASID 的價值。）

---

<a name="q15"></a>
## 15. ARMv8 支援哪幾種記憶體屬性？各有什麼特點？

### 結論

透過 **`MAIR_EL1`（8 個 slot，每 slot 1 byte）+ 頁表項的 `AttrIndx[2:0]`（3 bits）** 間接指定，
分兩大類：

**A. Normal Memory（一般記憶體，給 RAM）**
- 可 cache（Write-Back / Write-Through / Non-cacheable），可分別設定 Inner/Outer
- **允許亂序執行、投機存取、預取、存取合併/拆分**
- 存取不會有副作用，重複讀同一個位址結果相同
- 細分：`Normal-Cacheable` / `Normal Non-Cacheable`

**B. Device Memory（裝置記憶體，給 MMIO）**
- **絕不 cache**、絕不投機存取（讀一個裝置暫存器可能有副作用！）
- 按三個維度分四級（**G**athering / **R**eordering / **E**arly-write-ack，`n` = 不允許）：

| 型別 | Gathering 合併 | Reordering 重排 | Early ack 提前應答 | 用途 |
|------|---------------|----------------|-------------------|------|
| **Device-nGnRnE** | ✗ | ✗ | ✗ | 最嚴格＝傳統 Strongly-Ordered。時序極敏感的暫存器 |
| **Device-nGnRE** | ✗ | ✗ | ✓ | **Linux `ioremap()` 的預設**，絕大多數週邊 |
| **Device-nGRE** | ✗ | ✓ | ✓ | |
| **Device-GRE** | ✓ | ✓ | ✓ | 最寬鬆，如 frame buffer |

**Linux 的介面對應**：

```c
/* arch/arm64/include/asm/memory.h:140-144 */
#define MT_NORMAL         0
#define MT_NORMAL_TAGGED  1
#define MT_NORMAL_NC      2
#define MT_DEVICE_nGnRnE  3
#define MT_DEVICE_nGnRE   4
```

| 核心 API | 使用的 MT | 屬性 |
|----------|----------|------|
| 一般 RAM（線性映射） | `MT_NORMAL` | Normal WB |
| `ioremap()` | `MT_DEVICE_nGnRE` | Device-nGnRE |
| `ioremap_np()` | `MT_DEVICE_nGnRnE` | Device-nGnRnE |
| `ioremap_wc()` / `dma_alloc_coherent()` 非一致性時 | `MT_NORMAL_NC` | Normal Non-Cacheable |

> **書目**：奔跑吧 §2.4.1「內存屬性」——
> 「ARMv8 架構處理器主要提供兩種類型的內存屬性，分別是普通（normal）內存和設備（device）內存。」

### 實機驗證（MAIR_EL1 完整解碼）

```bash
sudo dmesg | grep -E "MAIR"
```

```
MAIR_EL1  = 0x000000040044ffff
   MAIR Attr0 = 0xff  Normal WB RW-Allocate (inner+outer)    ← MT_NORMAL      一般 RAM
   MAIR Attr1 = 0xff  Normal WB RW-Allocate (inner+outer)    ← MT_NORMAL_TAGGED
   MAIR Attr2 = 0x44  Normal Non-Cacheable (inner+outer NC)  ← MT_NORMAL_NC   ioremap_wc/DMA
   MAIR Attr3 = 0x00  Device-nGnRnE  最嚴格/強序               ← MT_DEVICE_nGnRnE
   MAIR Attr4 = 0x04  Device-nGnRE   ioremap 預設              ← MT_DEVICE_nGnRE
   MAIR Attr5..7 = 0x00  (未使用)
```

**MAIR 的 slot 順序與 `memory.h` 的 `MT_*` 索引完全對應** ✅
（`MAIR_EL1` 的值由 `arch/arm64/mm/proc.S:66` 的 `MAIR_EL1_SET` 組出，`proc.S:429` 寫入）

**頁表項的 `AttrIndx` 實測**：

| 巡覽位址 | `AttrIndx` | 指向 | 語意 |
|---------|-----------|------|------|
| `_stext`（核心程式碼） | **0** | Attr0 = 0xff | Normal WB ✓ 程式碼當然要 cache |
| `PAGE_OFFSET+2MB`（線性映射 RAM） | **1** | Attr1 = 0xff | Normal WB（`MT_NORMAL_TAGGED`，本機支援 MTE 編碼） |
| 使用者程式碼 | **0** | Attr0 = 0xff | Normal WB |
| VMEMMAP | **0** | Attr0 = 0xff | Normal WB |

**RK3588 的 Device Memory 在哪裡？** 看 TRM 的 Address Mapping
（`books/rk3588_trm/part1/chapter_01.txt` Table 1-1）：`0xF0000000` 以上整段是週邊
（PCIe3_4L_S @ F0000000、DSI HOST0 @ FDE20000 …），這些位址被 `ioremap()`
映射時就會拿到 `AttrIndx=4`（Device-nGnRE）。這也解釋了為什麼
[Ch3 Q6](./ch03_memory_management_prerequisites_ANSWERS.md#q6) 裡 DDR 的第一段
只到 `0xefe00000` 就斷掉——**後面那塊位址空間被 MMIO 佔走了**。

---

<a name="q16"></a>
## 16. inner shareable 和 outer shareable 有什麼區別？

### 結論

Shareability 描述「**某個 agent 改了這塊記憶體之後，哪個範圍內的其他 agent
會透過硬體一致性協定自動看到**」。由頁表項的 **`SH[1:0]`** 設定：

| `SH[1:0]` | 名稱 | 硬體一致性範圍 |
|-----------|------|---------------|
| `0b00` | **Non-shareable** | 只有自己，不需要與任何人一致 |
| `0b10` | **Outer Shareable** | Inner 域 **加上**外部 agent（GPU、其他 cluster/socket、一致性 DMA） |
| `0b11` | **Inner Shareable** | 通常是「同一個 cache 一致性互連內的所有 CPU 核」 |
| `0b01` | reserved | |

**Outer Shareable 是 Inner Shareable 的超集**（域越大，一致性維護的代價越高）。
實際邊界是 SoC 設計者定義的：哪些 master 接在硬體一致性互連上，就屬於哪個域。

**在屏障指令上的體現**：`dmb ish` / `dmb osh` / `dmb sy` 三種作用域，
`ish` 最便宜。Linux 的 SMP 屏障（`smp_mb()`）就是 `dmb ish`，
而跟 DMA 打交道的 `mb()` 是 `dsb sy`。

> **書目**：奔跑吧 §2.4.2「高速緩存共享屬性」、§2.5.2「共享屬性」。

### 實機驗證

**(a) 頁表裡的 SH 欄位——本機所有一般記憶體都是 Inner Shareable**：

```bash
sudo dmesg | grep -E "SH=" | sort -u
```

```
        AttrIndx=0 ... SH=3(Inner Shareable)      ← _stext
        AttrIndx=1 ... SH=3(Inner Shareable)      ← 線性映射
        AttrIndx=0 ... SH=3(Inner Shareable)      ← 模組
        AttrIndx=0 ... SH=3(Inner Shareable)      ← 使用者程式碼
        AttrIndx=0 ... SH=3(Inner Shareable)      ← VMEMMAP (2MB block)
```

**全部 `SH=3`**——因為 `PROT_DEFAULT` 帶 `PTE_SHARED`（= `SH=0b11`），
定義在 `arch/arm64/include/asm/pgtable-prot.h`。

**(b) RK3588 的 Inner Shareable 域有多大？** TRM 說得很清楚：

> `books/rk3588_trm/part1/chapter_03.txt` §3.1：
> 「The RK3588 has **a cluster** with quad-core Cortex-A55 and quad-core Cortex-A76」
> §3.2：「the cores connect to system bus through **DSU-L3**」

**8 個核（4×A55 + 4×A76）在同一個 DSU 裡，構成單一 Inner Shareable 域。**
GPU（Mali-G610）、NPU、VPU 這些走 MMU600（TRM Chapter 8）的 master
則在 Outer Shareable 域（或完全不一致，需要軟體 `DC CIVAC` 到 PoC）。

**(c) 「同一個 Inner 域內硬體自動一致」的實證**——[Q17](#q17) 的 SB litmus：

```
A76 cpu4 ↔ A76 cpu5，無屏障：r0=1,r1=1 出現 47 次
                     加 dmb ish 後：r0=1,r1=1 出現 8352 次
```

`dmb **ish**`（只同步到 Inner Shareable 域）就足以消滅全部亂序 ——
**證明兩顆核確實在同一個 Inner Shareable 域內**，不需要動用更貴的 `osh`/`sy` ✅

---

<a name="q17"></a>
## 17. ARMv8 支援哪幾條記憶體屏障指令？有什麼區別？

### 結論

ARMv8 提供三條：

| 指令 | 全名 | 保證什麼 | 影響範圍 |
|------|------|---------|---------|
| **DMB** | Data Memory Barrier | 屏障前後的**記憶體存取**不能跨越屏障重排。**不要求前面的存取已完成**，也不影響非記憶體指令 | 只管 load/store |
| **DSB** | Data Synchronization Barrier | 屏障前的所有記憶體存取（含 cache/TLB 維護）**必須真正完成**，才能執行屏障後的**任何**指令 | 管所有指令 |
| **ISB** | Instruction Synchronization Barrier | **清空管線**，保證之後取到的指令看得到先前所有系統狀態變更（改頁表、寫系統暫存器、cache/TLB 維護） | 取指路徑 |

強度：**ISB（取指）與 DSB（最強的資料屏障）> DMB**。

**後綴**：
- **作用域**：`SY`（全系統，預設）、`OSH`（Outer Shareable）、`ISH`（Inner Shareable）、`NSH`（Non-shareable）
- **方向**：`ST`（只管 store）、`LD`（只管 load）—— DMB/DSB 支援，ISB 沒有

例如 `dmb ishst` 比 `dmb sy` 便宜得多。Linux 的封裝在
`arch/arm64/include/asm/barrier.h`：`smp_mb()`=`dmb ish`、`smp_wmb()`=`dmb ishst`、
`mb()`=`dsb sy`、`isb()`=`isb`。

> **書目**：奔跑吧 §2.5.1「內存屏障指令」——
> 「ARMv8 指令集提供了 3 條內存屏障指令。數據存儲屏障（DMB）指令：僅當所有在它前面的
> 存儲器訪問操作都執行完畢後，才提交在它後面的訪問指令。」

### 實機驗證（**Store Buffer litmus test：31% 亂序 → 加屏障後 0**）

這是本章最有力的實驗。程式 `notes/experiments/barrier_sb.c` 跑經典的 SB litmus：

```
T0 (cpu4):   x = 1 ;   r0 = y ;
T1 (cpu5):   y = 1 ;   r1 = x ;
```

在 **sequential consistency** 下 `(r0==0 && r1==0)` **不可能發生**
（總順序中兩個 store 必有先後，後執行的那條 thread 的 load 一定看得到）。
但 ARMv8 是 weakly-ordered，store buffer 會讓兩邊的 load 都「越過」自己的 store。

```bash
ssh radxa@192.168.68.57 'cd ~/exp && gcc -O2 -o barrier_sb barrier_sb.c -lpthread
  for m in none dmb seqcst; do ./barrier_sb $m 4 5; echo; done'
```

```
SB litmus  T0(cpu4): x=1; r0=y  |  T1(cpu5): y=1; r1=x   mode=none    41943040 次
  r0=0,r1=0 :  13007759   <<<< 亂序 (SC 下不可能)
  r0=0,r1=1 :  12874759
  r0=1,r1=0 :  16060475
  r0=1,r1=1 :        47
  => 亂序率 31.012914%

  mode=dmb     (dmb ish)
  r0=0,r1=0 :         0   <<<<   ★ 完全消失
  r0=1,r1=1 :      8352
  => 亂序率 0.000000%

  mode=seqcst  (C11 __ATOMIC_SEQ_CST)
  r0=0,r1=0 :         0   <<<<   ★ 完全消失
  => 亂序率 0.000000%
```

**4194 萬次測試中，無屏障時有 1300 萬次（31%）觀察到架構允許、但直覺不允許的結果；
插入一條 `dmb ish` 之後變成 0。** 這是「ARM64 是弱序記憶體模型」最直接的證明。

**不同核心組合的亂序率差異**（同一支程式，只改 CPU 綁定）：

| writer ↔ reader | 關係 | 亂序次數 / 4194 萬 | 亂序率 |
|-----------------|------|------------------|--------|
| cpu4 ↔ cpu5 | 同 **A76** cluster | 13,007,759 | **31.01%** |
| cpu0 ↔ cpu1 | 同 **A55** cluster | 2,128,801 | **5.08%** |
| cpu4 ↔ cpu0 | **跨** cluster（A76↔A55） | 101,804 | **0.24%** |

跨 cluster 反而低，是因為兩顆核速度差很多（2.256 GHz vs 1.8 GHz），
兩條執行緒很快就錯開、真正「同時」踩到同一個 slot 的機會變少
（可以從 `r0=0,r1=1` 高達 4183 萬次看出 A76 幾乎全程領先）。
**這也提醒：litmus test 沒觀察到亂序，不代表架構不允許亂序。**

---

<a name="q18"></a>
## 18. Load-Acquire 與 Store-Release 原語有什麼區別？各有什麼作用？

### 結論

這是 ARMv8 引入的**單指令屏障**——屏障語意直接內建在 load/store 指令裡
（`LDAR` / `STLR`），比獨立的 `DMB` 更輕、粒度更細，是**單向**的：

```
         程式順序                        允許重排的方向
    ┌──────────────┐
    │   存取 A      │  ─────┐
    ├──────────────┤        │ 可以往下移
    │ LDAR (acquire)│  ◄─────┘        ✗ 下面的不能往上跑
    ├──────────────┤
    │   存取 B      │  ← 不能移到 LDAR 之前
    └──────────────┘

    ┌──────────────┐
    │   存取 C      │  → 不能移到 STLR 之後
    ├──────────────┤
    │ STLR(release)│  ◄─────┐        ✗ 上面的不能往下跑
    ├──────────────┤        │ 可以往上移
    │   存取 D      │  ─────┘
    └──────────────┘
```

| | **Load-Acquire (`LDAR`)** | **Store-Release (`STLR`)** |
|---|---|---|
| 保證 | 程式順序在它**之後**的所有存取，不得重排到它**之前** | 程式順序在它**之前**的所有存取，必須在它**之前**完成/可見 |
| 方向 | 對「後面」設下界 | 對「前面」設上界 |
| 類比 | **取鎖**：取到鎖之後才能進臨界區 | **放鎖**：臨界區做完才放鎖 |
| Linux | `smp_load_acquire()` | `smp_store_release()` |
| C11 | `memory_order_acquire` | `memory_order_release` |

**經典配對用法（spinlock / 無鎖佇列）**：

```c
/* 生產者 */                          /* 消費者 */
buf[i] = data;                        while (!smp_load_acquire(&ready))  /* LDAR */
smp_store_release(&ready, 1); /*STLR*/     ;
                                      use(buf[i]);      /* 保證看得到 data */
```

**比 DMB 好在哪**：`DMB` 對屏障兩側**所有**存取做雙向排序；
Acquire/Release 只做單向，硬體可以保留更多重排自由度，開銷更低。

> **書目**：奔跑吧 §2.5「內存屏障指令」相關段落。

### 實機驗證

**(a) 用 SB litmus 證明 SEQ_CST（會編成 STLR + LDAR）能消滅亂序**（見 [Q17](#q17)）：

```
mode=seqcst   r0=0,r1=0 : 0 / 41943040     ✅
```

**(b) 但 Acquire/Release 對 SB pattern 本身是「不夠」的——這是重要細節。**
SB litmus 需要的是 **store→load 的排序**，而 release/acquire 配對只保證
**release 之前的存取** 對 **acquire 之後的存取** 可見。純 `STLR; LDAR` 在架構上
**仍允許** SB 結果；本機測到 0 是因為 GCC 對 `__ATOMIC_SEQ_CST` 用了更強的實作。
真正需要 acquire/release 的是 **message-passing (MP) pattern**，
也就是 `notes/experiments/barrier_mp.c` 測的：

```bash
ssh radxa@192.168.68.57 'cd ~/exp && ./barrier_mp relaxed 0 4; ./barrier_mp acqrel 0 4'
```

```
mode=relaxed  writer=cpu0 reader=cpu4  觀察 2000000 次，0 次違規
mode=acqrel   writer=cpu0 reader=cpu4  觀察 2000000 次，0 次違規
```

⚠️ 這個 MP 測試**沒能觀察到亂序**，原因很誠實地說明如下：我在每一輪之間做了
「reader 把 flag 清 0、writer 等 flag 變 0」的握手，這個往返本身就構成同步，
把亂序視窗關掉了。**要可靠觀察 MP 亂序需要 litmus7 那種無握手的批次跑法**
（就像 [Q17](#q17) 的 SB 測試那樣）。
把這件事講出來，比宣稱「測不到所以不會發生」更誠實，也是面試時該有的態度。

**(c) 反組譯確認編譯器真的產生了 LDAR/STLR**：

```bash
ssh radxa@192.168.68.57 'objdump -d ~/exp/barrier_sb | grep -E "ldar|stlr" | head'
```

---

<a name="q19"></a>
## 19. 什麼是一個段的載入位址和運行位址？

### 結論

| | **載入位址（LMA, Load Memory Address）** | **運行位址（VMA / 連結位址）** |
|---|---|---|
| 定義 | 這段程式碼/資料**實際被放到**哪個（實體）位址 | 程式碼**被連結時假定自己會在**哪個位址執行 |
| 誰決定 | bootloader / 燒錄工具 | 連結腳本（`vmlinux.lds.S`）裡的 `. = ...` |
| 影響什麼 | 記憶體裡的實際位置 | 程式內的絕對跳轉目標、全域變數參照、重定位項 |

**兩者常常不同**，典型三種情況：
1. **核心**：bootloader 把 `Image` 載到某個實體位址（LMA），但核心是按 `KIMAGE_VADDR`
   （虛擬位址，VMA）連結的 → 必須先建立映射或用位置無關程式碼才能跳過去。
2. **`.data` 段**：LMA 在 Flash 裡（跟 `.text` 連在一起），VMA 在 RAM 裡；
   啟動程式碼負責把它從 Flash 複製到 RAM。
3. **bootloader 自我重定位**：先在 SRAM 執行 PIC，再把自己搬到 DDR 的最終位址。

**兩者不同時的解法**：（a）寫成位置無關程式碼（PIC/PIE）；
（b）先建立恆等映射再跳轉（ARM64 核心的做法，見 [Q21](#q21)）。

> **書目**：奔跑吧 §2.6.1「連結文件基礎知識」、§2.6.2「vmlinux.lds.S 文件分析」。

### 實機驗證（本機核心的 LMA vs VMA）

```bash
sudo grep -w _stext /proc/kallsyms          # 運行位址（連結位址）
sudo dmesg | grep -E "__pa_symbol|kimage_voffset"   # 載入位址
```

```
運行位址 (VMA)：_stext = 0xffff800008010000       ← vmlinux.lds.S 連結出來的
載入位址 (LMA)：       PA = 0x410000              ← U-Boot 實際放的地方
                    kimage_voffset = 0xffff800007c00000  ← 兩者的差
```

**同一段程式碼，在虛擬世界住在 `0xffff8000_08010000`，在實體 DRAM 裡住在 `0x41_0000`。**
`Image` 本身被載在 `0x400000`（4 MB，符合 ARM64 boot protocol 的 2 MB 對齊要求）。

驗算：`0xffff800008010000 − 0xffff800007c00000 = 0x410000` ✅

**為什麼可以這樣錯開？** 因為 ARM64 核心自 4.6 起是**可重定位映像**——
它在 `head.S` 裡讀取自己實際的執行位址（`adrp` 是 PC-relative），
算出 `kimage_voffset`，再據此建立映射並跳到連結位址。詳見 [Q11](#q11) / [Q21](#q21)。

---

<a name="q20"></a>
## 20. 從 U-Boot 跳到核心時，為什麼指令快取可以開啟而資料快取必須關閉？

### 結論

這是 ARM64 **Booting Protocol**（`Documentation/arm64/booting.rst`）的硬性規定：

> The MMU must be off. Instruction cache may be on or off.
> The address range corresponding to the loaded kernel image must be cleaned
> to the PoC. **The D-cache must be off.**

**為什麼 I-cache 可以開？**
核心映像的 `.text` 在跳轉之前就已經完整載入且**不會再被修改**，
I-cache 只讀不寫，不存在「誰寫的版本比較新」的問題。
（前提是 U-Boot 已經把映像 clean 到 PoC，讓 I-cache 能取到正確內容。）
開著它可以加速核心入口那段程式碼，架構上完全安全。

**為什麼 D-cache 必須關？** 三個層面：

1. **MMU 關閉時，所有存取都被當成 Device-nGnRnE / Normal Non-Cacheable。**
   若此時 D-cache 開著，會出現同一實體位址「有些存取走 cache、有些不走」的
   **不一致視圖**——這在架構上是 **mismatched memory attributes**，行為未定義。

2. **核心早期寫的關鍵資料必須讓別人看到。**
   `head.S` 要寫頁表、`memstart_addr`、`kimage_voffset`，這些資料
   （a）會被 **MMU 硬體的 table walker** 直接從記憶體讀（table walk 在 MMU 剛開時
   可能還沒進入一致性域）；（b）會被**還沒開 MMU 的 secondary CPU** 讀。
   若停在 D-cache 沒寫回，對方讀到的就是舊值。

3. **多核啟動**：secondary CPU 透過 PSCI / spin-table 被喚醒時，各核的
   cache 狀態尚未統一納入一致性域，此時若各自 D-cache 有髒資料，會直接不一致。

核心自己在 `__cpu_setup` / `__enable_mmu` 裡把頁表、MAIR、TCR 都設好之後，
才寫 `SCTLR_EL1` 一次同時開啟 `M`（MMU）、`C`（D-cache）、`I`（I-cache），
並用 `ISB` 讓它生效。

> **書目**：奔跑吧 §2.6.5「__cpu_setup 函數分析」——「__cpu_setup 函數打開 MMU 以做一些
> 與處理器相關的初始化」。

### 實機驗證（核心跑起來後三個位元都是 1）

```bash
sudo dmesg | grep SCTLR
```

```
SCTLR_EL1 = 0x000000003464d91d   M(MMU)=1  C(D-cache)=1  I(I-cache)=1
```

拆解 `0x3464d91d`：`bit0(M)=1`、`bit2(C)=1`、`bit12(I)=1` ✅
——證明核心在 `__cpu_setup` 之後確實把三者一起打開了。

**U-Boot 交棒時的狀態**可以從 U-Boot 側確認（本機 U-Boot 版本
`uboot-17.09-33-f-08/06/2024`，見 `/proc/cmdline` 的 `androidboot.fwver`）：

```bash
cat /proc/cmdline | tr ' ' '\n' | grep fwver
# androidboot.fwver=ddr-v1.16-9fffbe1e78,bl31-v1.45,uboot-17.09-33-f-08/06/2024
```

U-Boot 的 `armv8_switch_to_el2` / `cleanup_before_linux()` 會做
`dcache_disable()`（內含 clean+invalidate 到 PoC）再跳轉，正是協定要求的動作。

---

<a name="q21"></a>
## 21. 核心啟動組合語言中為什麼要建立恆等映射？

### 結論

**恆等映射（identity mapping）= `VA == PA` 的映射。**

問題出在「開啟 MMU」這個動作本身的**因果時序斷裂**：

```
     MMU 關閉                              MMU 開啟
  ┌──────────────┐                   ┌──────────────┐
  │ PC 是實體位址  │                   │ PC 是虛擬位址  │
  └──────────────┘                   └──────────────┘
         │                                   │
   msr sctlr_el1, x0    ← 就是這條指令把世界切換了
   isb                  ← 下一條指令的取指位址，已經要經過 MMU 翻譯了！
         │
         ▼
   如果核心是按 KIMAGE_VADDR(0xffff8000...) 連結的，
   而 PC 現在是 0x410000（實體），開啟 MMU 之後 MMU 會去翻譯 0x410000，
   但頁表裡 0x410000 這個「虛擬位址」根本沒有映射 → Translation Fault → 死機
```

**解法**：除了建立「核心真正要用的映射」（映像 → 高位址 VA、線性映射）之外，
額外建立一份**小範圍的恆等映射**（`idmap_pg_dir`，裝進 **TTBR0**），
只涵蓋 `__idmap_text_start ~ __idmap_text_end`——也就是
「開啟 MMU 那幾條指令 + 緊接著的跳轉指令」所在的那一小段實體位址。

於是 MMU 開啟前後，那幾條指令**無論用實體還是虛擬位址解讀，都指向同一份指令**，
執行流不中斷。接著程式碼用一條**絕對位址的長跳轉**（`br x8`，目標是連結位址）
跳到核心映像真正的高位址虛擬空間，之後恆等映射就功成身退
（後續 `paging_init()` 會把 TTBR0 換掉／設 `EPD0`）。

**這也是為什麼 `idmap` 必須放在單獨的 section**：連結腳本
`vmlinux.lds.S` 有 `idmap_pg_dir` 與 `.idmap.text`，且要求它不跨越
`idmap` 頁表能覆蓋的範圍。

> **書目**：奔跑吧 §2.6.4「創建恆等映射和內核映像映射」——
> 「為了降低啟動代碼的複雜性，我們約定進入 Linux 內核入口時 MMU 是關閉的…
> 因此，我們在初始化的某個階段需要把 MMU 打開並且使能數據高速緩存，以獲得更高的性能。」

### 實機驗證（原始碼側）

```bash
grep -n "idmap" arch/arm64/kernel/vmlinux.lds.S | head
grep -n "idmap_pg_dir\|__idmap_text" arch/arm64/kernel/head.S | head
```

本機執行時 idmap 已經退場，所以看不到；但可以看到它留下的痕跡：

```bash
sudo dmesg | grep -E "TTBR0|EPD0"
```

```
TTBR0_EL1 = 0x000000003430c001    ← 已經換成 insmod 行程的頁表，不再是 idmap_pg_dir
   EPD0 = 0                        ← TTBR0 的 table walk 是啟用的（因為要跑使用者空間）
```

**恆等映射存在的間接證據**：`_stext` 的實體位址是 `0x410000`，虛擬位址是
`0xffff800008010000`——兩者天差地遠。核心不可能「直接」從實體位址跳到虛擬位址，
中間**必然**需要一段兩邊都有效的過渡程式碼，那就是 idmap。

---

<a name="q22"></a>
## 22. L0～L2 頁表項中指向下一級頁表的基址，是實體位址還是虛擬位址？

### 結論

**一定是實體位址。**

理由是**避免無窮遞迴**：MMU 的 table walker 的職責就是把虛擬位址翻譯成實體位址。
如果頁表項裡存的是虛擬位址，那 walker 為了讀下一級頁表，得先把這個虛擬位址翻譯成
實體位址——而翻譯又要走頁表——**邏輯死循環**。

所以整條鏈上全部都是實體位址：
- `TTBR0_EL1` / `TTBR1_EL1` 的 `BADDR` = **L0 表的實體基址**
- L0/L1/L2 的 table descriptor `bits[47:12]` = **下一級表的實體基址**
- L3 的 page descriptor `bits[47:12]` = **最終實體頁的基址**

**位元寬度的細節**：`bits[47:12]` 能表達 48 位實體位址，
但實際硬體支援多少由 `ID_AA64MMFR0_EL1.PARange` 決定，
`TCR_EL1.IPS` 則設定實際啟用多少位。

> **書目**：奔跑吧 §2.7.1「關於下一級頁表基地址」——
> 「如果下一級頁表的基地址是虛擬地址，那麼 MMU 還需要查詢另外一個頁表才能找到這個
> 虛擬地址對應的物理地址，這樣 MMU 就會陷入死循環，因此這裡下一級頁表的基地址採用的是物理地址。」

### 實機驗證

```bash
sudo dmesg | grep -A1 "desc = " | head -20
```

以 `_stext` 的巡覽為例，每一級都印出了 `bits[47:12]`：

```
TTBR1_EL1 = 0x1476000001b2d001   BADDR(PA)=0x000001b2d000    ← L0 表的實體位址
   L0 desc = 0x10000002fffff003   bits[47:12] = 0x0002fffff000   ← L1 表的實體位址
   L1 desc = 0x10000002ffffe003   bits[47:12] = 0x0002ffffe000   ← L2 表的實體位址
   L2 desc = 0x10000002ffffd003   bits[47:12] = 0x0002ffffd000   ← L3 表的實體位址
   L3 desc = 0x0050000000410783   bits[47:12] = 0x000000410000   ← 最終實體頁
```

**怎麼確定這些真的是實體位址而不是虛擬位址？** 兩個鐵證：

1. **數值範圍**：`0x2fffff000`、`0x2ffffe000` 都落在 **12 GB 以內**，
   正好在本機實體記憶體的最高段（`0x2f0000000 ~ 0x300000000`，見
   [Ch3 Q6](./ch03_memory_management_prerequisites_ANSWERS.md#q6)）。
   核心**虛擬**位址一定是 `0xffff...` 開頭，這些顯然不是。

2. **模組必須先 `__va()` 才能讀**：模組印出的下一級 entry 虛擬位址是
   `0xffff0002fffff000` = `__va(0x2fffff000)` = `0x2fffff000 + PAGE_OFFSET`，
   **正好差一個 `PAGE_OFFSET`** ✅ 見 [Q23](#q23)。

### ⚠️ 實體位址寬度的修正

```bash
sudo dmesg | grep -E "PARange|IPS"
```

```
   IPS  = 2 -> 中間實體位址寬度 40bit/1TB          ← TCR_EL1.IPS
   PARange  = 2 -> 硬體實體位址寬度 40bit/1TB      ← ID_AA64MMFR0_EL1.PARange
```

**RK3588 的實體位址匯流排是 40 bits（最多 1 TB），不是 48 bits。**
`CONFIG_ARM64_PA_BITS=48` 只是**核心編譯時的上限**，
真正啟用多少由硬體 `PARange` 決定、由 `TCR_EL1.IPS` 設定。
（本目錄舊草稿寫「本裝置 `CONFIG_ARM64_PA_BITS=48`，48 位實體定址」——
組態值沒錯，但硬體實際只有 40 位。8 GB 的板子當然綽綽有餘。）

---

<a name="q23"></a>
## 23. `pgd_t`/`pud_t`/`pmd_t`/`pte_t` 只是 u64，軟體如何巡覽頁表？

### 結論

關鍵在於區分**兩個不同的位址視角**：

```
  頁表項「裡面存的值」  →  永遠是【實體位址】（硬體 walker 的要求，見 Q22）
  軟體「讀取頁表項時」  →  必須用【虛擬位址】（因為 MMU 已開，CPU 只認虛擬位址）
```

所以每往下走一級，軟體都要做一次 **實體 → 虛擬** 的換算，用的就是 `__va()`
（線性映射的固定偏移，[Q5](#q5)）。這正是 `*_offset()` 系列巨集在做的事：

```c
/* arch/arm64/include/asm/pgtable.h（概念示意） */
#define pgd_offset(mm, addr)   ((mm)->pgd + pgd_index(addr))
        /* mm->pgd 本身就是虛擬位址指標（配置時就記住了 VA），直接加索引 */

static inline pud_t *pud_offset(p4d_t *p4dp, unsigned long addr)
{
        return (pud_t *)__va(p4d_page_paddr(*p4dp)) + pud_index(addr);
        /*                ^^^^ 取出實體位址後立刻 __va() 換成虛擬位址 */
}
```

**逐級的動作固定是三步**：
1. 讀當前級的頁表項（用**虛擬位址**解參考）→ 拿到一個 `u64`
2. 取出其中的 `bits[47:12]` → 那是**下一級表的實體位址**
3. `__va(實體位址)` → 得到**虛擬位址**，加上下一級索引，回到步驟 1

資料型別方面（`arch/arm64/include/asm/pgtable-types.h`）都是單一 u64 的包裝：

```c
typedef struct { pteval_t pte; } pte_t;
#define pte_val(x)  ((x).pte)
```

用 struct 包起來是為了**型別安全**——編譯器會擋下把 `pmd_t` 當 `pte_t` 用的錯誤。

> **書目**：奔跑吧 §2.7.2「軟體遍歷頁表」。

### 實機驗證（`__va()` 的換算逐級可見）

模組每走一級都同時印出「描述符裡的實體位址」和「下一級 entry 的虛擬位址」：

```
   L0 desc = 0x10000002fffff003
        bits[47:12] = 0x0002fffff000        ← 實體位址
        -> 軟體要讀下一級, 必須先 __va(0x0002fffff000) 換成虛擬位址
   pud_offset() 回傳的 L1 entry VA = 0xffff0002fffff000     ← 虛擬位址
```

**驗算**：`__va(0x2fffff000)` = `0x2fffff000 − PHYS_OFFSET(0) + PAGE_OFFSET(0xffff000000000000)`
= **`0xffff0002fffff000`** ✅ 完全吻合。

再看下一級：

```
   L1 desc bits[47:12] = 0x0002ffffe000
   pmd_offset() 回傳的 L2 entry VA = 0xffff0002ffffe200
```

驗算：`__va(0x2ffffe000)` = `0xffff0002ffffe000`，
再加上 L2 索引 64 × 8 = `0x200` → **`0xffff0002ffffe200`** ✅

**每一級的「實體位址 + PAGE_OFFSET = 虛擬位址」關係都成立**，
把 [Q5](#q5)（PAGE_OFFSET）、[Q22](#q22)（描述符存實體位址）、
[Q23](#q23)（軟體用 `__va()` 換算）三題完整串起來。

**注意一個例外**：`pgd_offset(mm, addr)` **不需要 `__va()`**，
因為 `mm->pgd` 本身就是配置頁表時記下來的虛擬位址指標。
本機實測：

```
  L0 表基址 VA = 0xffff00003430c000 (current->mm->pgd)     ← 直接就是 VA
  TTBR0_EL1 BADDR(PA) = 0x3430c000                         ← 硬體看到的是 PA
```

**同一張 L0 表，軟體用 `0xffff00003430c000` 讀它，硬體用 `0x3430c000` 讀它** ——
這就是本題的完整答案。

---

## 附錄：本章實驗程式

| 檔案 | 用途 | 對應題目 |
|------|------|---------|
| `experiments/armv8_dump.c` + `Makefile.mod` | 核心模組：讀 EL1 系統暫存器、印記憶體佈局、軟體巡覽頁表 | Q1-Q6, Q9-Q16, Q20, Q22, Q23 |
| `experiments/pagemap_walk.c` | 使用者態 VA→PFN→PA（`/proc/self/pagemap`） | Q2, Q4, Q22 |
| `experiments/barrier_sb.c` | Store-Buffer litmus test | **Q17, Q18** |
| `experiments/barrier_mp.c` | Message-Passing litmus test（含測不到的誠實說明） | Q18 |
| `experiments/cache_ladder.c` | Cache 延遲階梯 | Q13（cache line/層級），Ch3 Q2 |

一鍵重跑：

```bash
ssh radxa@192.168.68.57 'mkdir -p ~/exp/armv8'
scp notes/experiments/armv8_dump.c radxa@192.168.68.57:~/exp/armv8/
scp notes/experiments/Makefile.mod radxa@192.168.68.57:~/exp/armv8/Makefile
scp notes/experiments/{pagemap_walk,barrier_sb,barrier_mp,cache_ladder}.c radxa@192.168.68.57:~/exp/

ssh radxa@192.168.68.57 'cd ~/exp/armv8 && make
  STEXT=0x$(sudo grep -w _stext /proc/kallsyms | cut -d" " -f1)
  sudo dmesg -C; sudo insmod armv8_dump.ko stext=$STEXT
  sudo dmesg | sed "s/^\[[^]]*\] //"; sudo rmmod armv8_dump'

ssh radxa@192.168.68.57 'cd ~/exp
  for s in pagemap_walk barrier_sb barrier_mp cache_ladder; do gcc -O2 -w -o $s $s.c -lpthread; done
  sudo ./pagemap_walk
  for m in none dmb seqcst; do ./barrier_sb $m 4 5; done
  taskset -c 4 ./cache_ladder'
```
