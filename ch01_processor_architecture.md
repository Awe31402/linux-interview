# 《奔跑吧 Linux內核》（第二版）卷1 第1章 處理器架構 高頻面試題

> **說明**：收錄自第 1 章開篇「本章的高頻面試題」，共計 34 題。
> 每題答案除書本理論外，均以實驗設備 **Radxa ROCK 5B（RK3588，8× Cortex-A55/A76，SSH: radxa@192.168.68.57）** 上取得的實測數據佐證。

---

### 1. 請簡述精簡指令集RISC和複雜指令集CISC的區別。

| | CISC（如 x86） | RISC（如 ARM） |
|---|---|---|
| 指令集 | 指令數量多、尋址方式多、單條指令可完成複雜操作（如記憶體操作數直接參與運算） | 指令數量少、格式規整，多為定長指令 |
| 執行方式 | 微碼（microcode）翻譯執行，單指令可能耗費多個週期 | 大部分指令一個時鐘週期內完成，易於流水線化 |
| 訪存 | 運算指令可直接訪問記憶體（register-memory / memory-memory） | 採用 Load/Store 架構，只有 Load/Store 指令可以訪問記憶體，運算指令只操作暫存器 |
| 硬體複雜度 | 譯碼/控制邏輯複雜，電晶體更多用於指令翻譯 | 硬體簡單，更多電晶體可用於流水線、亂序執行、Cache |
| 編譯器角色 | 編譯器只需選用高級指令 | 編譯器需要做更多指令排程、暫存器分配等優化工作 |

實測設備的 CPU 均為 ARM64（AArch64）指令集，`cat /proc/cpuinfo` 中 `CPU architecture: 8` 即 ARMv8-A，是典型 RISC 設計：Load/Store 架構、32 個通用暫存器、定長 32 位元指令（Thumb 除外）。

```c
/* x86 (CISC)：運算指令可直接帶記憶體運算元 */
add [ebx], eax        /* 一條指令完成「讀記憶體+加法+寫回記憶體」 */

/* ARM64 (RISC)：Load/Store 架構，運算指令只能操作暫存器 */
ldr x0, [x1]           // 先 load
add x0, x0, x2          // 純暫存器運算
str x0, [x1]            // 再 store
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 26 頁附近章節內文。

### 2. 請簡述數值0x1234 5678在大端和小端位元組序處理器的存儲器中的存儲方式。

設起始位址為 `0x1000`：

- **大端（Big-Endian）**：高位元組存於低位址。`0x1000:0x12 0x1001:0x34 0x1002:0x56 0x1003:0x78`
- **小端（Little-Endian）**：低位元組存於低位址。`0x1000:0x78 0x1001:0x56 0x1002:0x34 0x1003:0x12`

ARMv8 上電複位後預設按小端方式訪問（也可通過 `SCTLR_ELx.EE` 配置大端），Linux 內核也可編譯為 `CONFIG_CPU_BIG_ENDIAN`，但絕大多數發行版（含本設備）都使用小端。實測：

```
$ lscpu | grep "Byte Order"
Byte Order: Little Endian
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 26 頁附近章節內文。

### 3. 請簡述在你所熟悉的處理器（如雙核Cortex-A9）中一條存儲讀寫指令的 執行全過程。

以 store 指令 `str r0, [r1]` 為例（多核，含 Cache 一致性）：

1. **取指/譯碼**：CPU 流水線取出並譯碼 store 指令，計算出有效位址（EA）。
2. **TLB 尋找**：用虛擬位址查一級 TLB（micro-TLB），命中則得到實體位址與訪問屬性（Cacheable/Shareable 等）；未命中則觸發頁表walk（硬體 Page Table Walker 逐級查詢 L0~L3 頁表），並將結果填入 TLB。
3. **訪問一級 Cache（VIPT）**：用虛擬位址的 index 位元查 Cache set，用實體位址 tag 比較是否命中。
4. **一致性處理（MESI/MOESI）**：若是共享的 Cache 行，要先通過總線監聽協定（如 CCI-400 上的 ACE 協定）確認其他核該行狀態，將其失效或降級，本核獲得 Exclusive/Modified 權限後才能寫。
5. **寫入 Cache 行**：命中則更新該 Cache line 數據，若採用 write-back 策略，只標記該行為 Dirty，不立即寫回主記憶體；未命中則先分配（write-allocate）一行，從下一級 Cache/主記憶體搬入整行數據再寫入（或以 write-around 策略繞過 Cache 直寫緩衝）。
6. **寫緩衝/記憶體屏障語義**：數據先進入 Store Buffer/Write Buffer，異步寫回，除非遇到 DMB/DSB 等屏障指令或該位址為 Device 記憶體，才強制排空寫緩衝。
7. **退休（Retire）**：指令在重排序緩衝（ROB）中按序退休，異常處理精確對齊這一步。

```asm
// ARM64 一條 store 指令背後涉及的硬體動作
str x0, [x1]      // 1) 譯碼、算出有效位址 x1
                  // 2) 查 TLB 得到實體位址與屬性
                  // 3) 用實體位址查 L1 D-Cache（VIPT）
                  // 4) 依 MESI 取得 Exclusive/Modified 權限後寫入 Cache line
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 33 頁附近章節內文。

### 4. 請簡述記憶體屏障（memory barrier）產生的原因。

現代處理器為了提升性能，會對訪存指令做**亂序執行**、**寫緩衝（Store Buffer）**、**Cache 的非阻塞訪問**等優化，使得程序中指令實際到達記憶體系統的順序可能與程序順序（program order）不一致；此外編譯器也會為了優化重新排列指令。在單核單執行緒環境下這通常無害（處理器保證自身看到基於順序一致性假象的結果），但在多核系統中，一個核對記憶體的寫入順序，另一個核可能以不同順序觀察到，導致依賴數據同步的演算法（如自旋鎖、生產者-消費者的 flag/data 模式）出錯。因此需要顯式的記憶體屏障指令，強制規定屏障前後的訪存操作的完成/可見順序，彌合「處理器實際執行順序」與「程式員期望的順序」之間的差異。

```c
/* 典型的 flag/data 競態：沒有屏障時，CPU1 可能先看到 flag=1 卻讀到舊的 msg */
// CPU0
msg = 42;
flag = 1;          // 若被重排到 msg=42 之前，CPU1 就會讀到錯誤的 msg

// CPU1
while (!flag) {}
printf("%d\n", msg);  // 沒有屏障時可能印出未更新的舊值
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 32 頁附近章節內文。

### 5. ARM有幾條記憶體屏障指令？它們之間有什麼區別？

ARMv8（A64）提供 **3 條**記憶體屏障指令：

- **DMB（Data Memory Barrier，數據存儲屏障）**：保證 DMB 之前的所有存儲器訪問（讀/寫）都先於 DMB 之後的存儲器訪問完成（對同一 Shareability domain 可見），但不影響非存儲器指令的順序，也不保證屏障前指令本身已經「完成」（如寫緩衝可以仍未排空，只是順序被保證）。
- **DSB（Data Synchronization Barrier，數據同步屏障）**：更強，要求 DSB 之前的所有存儲器訪問都**真正完成**（寫操作數據已到達其目標、讀操作數據已返回）之後，才允許執行 DSB 之後的任何指令（包括非存儲器指令）。
- **ISB（Instruction Synchronization Barrier，指令同步屏障）**：清空流水線（flush pipeline），保證 ISB 之後取指得到的都是 ISB 之前所有對系統暫存器/頁表/Cache 等的修改**生效之後**的指令流，常用於修改 MMU/Cache 配置、寫 TTBR 或 ASID 之後。

三者還可加 `ISH/OSH/NSH/SY`、`ST/LD` 後綴細分作用域（Shareability domain）和方向（只針對 store 或 load），如 `dmb ish`（內部共享域）、`dsb sy`（全系統）。

```asm
str x0, [x1]      // 寫 Msg
dmb ish           // Data Memory Barrier：確保上面的寫先於下面的寫對其他核可見
str x2, [x3]      // 寫 Flag
...
dsb sy             // Data Synchronization Barrier：等待前面的存取真正完成
isb                // Instruction Synchronization Barrier：清流水線，常接在改 TTBR/ASID 之後
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 36 頁附近章節內文。

### 6. 請簡述快取（cache）的工作方式。

Cache 利用**局部性原理**（時間局部性、空間局部性），在 CPU 與主記憶體之間插入區塊由 SRAM 構成的高速小容量存儲器。數據以固定大小的**Cache 行（line，本設備為 64 位元組）**為單位在 Cache 與主記憶體之間搬移。CPU 訪存時先查 Cache：

- **命中（hit）**：直接在 Cache 中完成讀/寫，速度遠快於訪問主記憶體；
- **缺失（miss）**：需要從下一級 Cache 或主記憶體載入整個 Cache 行到 Cache 中（可能需要先淘汰一個舊行，若被淘汰行為髒（dirty）則要先寫回主記憶體），再完成本次訪問。

Cache 採用組相聯映射，將位址劃分為 tag/index/offset 三部分查找對應的行；寫策略上有 write-through/write-back，替換策略上多用 LRU 近似演算法。實測本機三級 Cache 結構：L1（每核私有）、L2（每核/簇私有）、L3（全核共享）。

```bash
# 實測本機各層快取行大小（本設備皆為 64 位元組）
$ cat /sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size
64
$ cat /sys/devices/system/cpu/cpu0/cache/index0/size
32K
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 28 頁附近章節內文。

### 7. 快取的映射方式有全關聯（full-associative）、直接映射（directmapping）和組相聯（set-associative）3種方式，請簡述它們之間的區別。為什麼現代的處理器都使用組相聯的快取映射方式？

- **直接映射（direct-mapping）**：每個主記憶體區塊只能放到唯一固定的一個 Cache 行（`index = 位址 mod 行數`）。硬體簡單、查找快，但當多個經常訪問的位址恰好映射到同一行時會頻繁發生**Cache 抖動/衝突缺失（conflict miss）**，命中率低。
- **全相聯（full-associative）**：任何主記憶體區塊可放到任意一個 Cache 行，需要用位址的 tag 與**所有**行的 tag 同時比較（CAM，內容尋址存儲器）。命中率最高，但比較器數量隨容量線性增長，硬體代價與功耗過大，只適合極小容量（如 TLB 的部分結構）。
- **組相聯（set-associative）**：將 Cache 分成若干組（set），每組內有 N 路（way），位址的 index 位元決定落入哪一組，組內 N 路全相聯比較 tag。是直接映射（1 路）與全相聯（1 組）的折中。

現代處理器普遍採用組相聯（如本設備 L1 4-way、L2 4~8-way、L3 12-way），是因為它用**可控的硬體代價（每組只需比較 N 路而非全部）**換取了**接近全相聯的命中率**，在功耗、面積、時延與命中率之間取得最佳平衡點；同時組相聯也便於用軟體/硬體的 Cache 著色（cache coloring）、路預測等技術進一步優化。

```c
/* 32KB、4 路組相聯、64B 行的位址切分（對照本設備 A55 L1D）*/
#define OFFSET_BITS 6      // 64B  = 2^6
#define INDEX_BITS  7      // 32KB/4way/64B = 128 sets = 2^7
uint64_t offset = va & ((1<<OFFSET_BITS)-1);
uint64_t index  = (va >> OFFSET_BITS) & ((1<<INDEX_BITS)-1);
uint64_t tag    = va >> (OFFSET_BITS+INDEX_BITS);
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 41 頁附近章節內文。

### 8. 在一個32KB的4路組相聯的快取中，其中快取行為32位元組，請畫出這個快取的快取行（line）、路（way）和組（set）的示意圖。

計算：總容量 32KB，4 路，行大小 32B → 每路容量 = 32KB / 4 = 8KB；組數 = 8KB / 32B = 256 組（set）。

```
                    Way0        Way1        Way2        Way3
Set 0   [Tag|32B line] [Tag|32B line] [Tag|32B line] [Tag|32B line]
Set 1   [Tag|32B line] [Tag|32B line] [Tag|32B line] [Tag|32B line]
  ...          ...            ...           ...            ...
Set 255 [Tag|32B line] [Tag|32B line] [Tag|32B line] [Tag|32B line]
```

位址劃分（假設實體位址）：`offset` 占 5 位元（2^5=32B），`index` 占 8 位元（2^8=256 組），其餘高位元為 `tag`：

```
| ... tag (高位元) ... | index[12:5] (8bit) | offset[4:0] (5bit) |
```

對照本設備實測的 A55 L2（`size=128K ways=4`）：每路 32KB，行 64B → 組數 = 32KB/64B = 512 組，index 占 9 位元、offset 占 6 位元，原理相同，只是參數不同。

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 41 頁附近章節內文。

### 9. 快取重名問題和同名問題是什麼？

這兩個問題都源於使用**虛擬位址**的部分位元來做 Cache 的 index/tag，而虛擬位址與實體位址之間是多對一或一對多的映射：

- **別名/重名問題（Cache aliasing，又稱同義 synonym）**：**多個不同的虛擬位址映射到同一個實體位址**（如共享記憶體、mmap 同一檔案），若這些虛擬位址的 index 位元不同，會在 Cache 中產生該同一份實體數據的**多個副本**，分處不同的 Cache 行；一旦其中一份被修改而未同步，就會造成數據不一致（多行程看到不同內容）。
- **同名問題（也稱歧義 homonym）**：**同一個虛擬位址在不同行程（不同頁表/位址空間）中，映射到不同的實體位址**。若 Cache 只用虛擬位址做 tag（如 VIVT），發生行程切換後，新行程可能命中了上一個行程遺留、tag 相同但內容完全不相關的 Cache 行，讀到錯誤數據。

```c
/* 別名(aliasing)示意：v1、v2 兩個不同虛擬位址映射同一實體頁 */
mmap(v1, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
mmap(v2, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
/* 若 v1、v2 的 cache index 位元不同，VIVT/寬鬆 VIPT 下會產生同一份資料的
 * 兩份不同步 cache 副本 */
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 44 頁附近章節內文。

### 10. ARM9處理器的數據快取組織方式使用虛擬索引虛擬標籤（Virtual Index Virtual Tag，VIVT）方式，而在Cortex-A7處理器中使用物理索引物理標籤 （Physical Index Physical Tag，PIPT），請簡述PIPT與VIVT相比的優勢。

PIPT 的 index 和 tag 都使用實體位址，天然不存在別名（aliasing）和同名（homonym）問題：因為 Cache 中的一行始終只與唯一的實體位址對應，不同虛擬位址映射同一實體位址時會命中同一 Cache 行，行程切換也不會讀到舊行程遺留的錯誤行。因此 PIPT 不需要軟體在行程切換、mmap/munmap、fork 共享記憶體等場景下額外維護 Cache 一致性（無需軟體按虛擬位址主動 flush/invalidate），大幅簡化了作業系統（Linux）Cache 管理程式碼，正確性更好；代價是需要先做位址轉換（TLB 尋找）才能索引 Cache，通常需要將 TLB 訪問置於 Cache 訪問的關鍵路徑上（VIPT 的一種是可以讓 index 部分與 offset 一樣在頁內、不需要先轉換位址，從而兼顧速度與 PIPT 的正確性）。

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 42 頁附近章節內文。

### 11. VIVT類型的快取有什麼缺點？請簡述作業系統需要做什麼事情來 克服這些缺點。

**缺點**：
1. 存在第 9 題所述的**別名（aliasing）**和**同名（homonym）**問題；
2. 行程切換、位址空間變化（如 `mmap`/`munmap`、寫時複製、`exec`）後，Cache 中殘留的舊虛擬位址數據可能不再有效或產生歧義。

**作業系統的應對**（以早期 ARM Linux 為例）：
- 行程切換（`switch_mm`）時，若新舊行程 ASID/位址空間不同且硬體無 ASID 支援，需要**flush 整個 Cache**（如 `flush_cache_mm`），代價高昂；
- 對共享映射（多個 VMA 映射同一檔案/實體頁）需要檢測**虛擬位址是否滿足「Cache 一致性同餘」條件（即兩個虛擬位址對 Cache 組大小取模相同，才允許共享而不衝突）**，Linux 的 `SHMLBA`、`flush_dcache_page()`、`flush_cache_range()` 等介面正是為此設計；
- 修改可執行頁（如動態連結、`ptrace`、JIT）後需要顯式 `flush_icache_range()`，保證指令 Cache 與數據一致；
- 因此內核充滿了大量與 VIVT 平台相關的 Cache 維護回呼（`cpu_cache.dcache_clean_area` 等），這也是 ARMv7 之後處理器逐漸轉向 PIPT/VIPT（且限制 VIPT 的 index 落在頁內）的原因——從根本上減少軟體維護負擔。

```c
/* 早期 ARM Linux 對付 VIVT 的典型作法：進程切換時整片 flush */
void switch_mm(struct mm_struct *prev, struct mm_struct *next)
{
    if (cache_is_vivt() && prev != next)
        flush_cache_mm(prev);   /* VIPT/PIPT（如本設備 A55/A76）則完全不需要 */
    ...
}
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 42 頁附近章節內文。

### 12. 虛擬索引物理標籤（Virtual Index Physical Tag，VIPT）類型的快取 在什麼情況下會出現快取重名問題？

VIPT 用虛擬位址查 index、實體位址比較 tag。若 **index 所用的位元數超出了頁內偏移（page offset）範圍**，即 `index` 位元跨越了虛擬位址到實體位址轉換不變的「頁內偏移」部分，那麼兩個映射到同一實體位址的不同虛擬位址（別名）就可能落在**不同的 index（不同的組）**，產生該實體數據同時存在於多個 Cache 組的重名（aliasing）問題——因為它們的實體 tag 相同、但位於不同的 set，Cache 硬體按 tag 比較時只在各自的 set 內尋找，無法偵測到彼此，導致同一份數據出現多個不同步的副本。

判斷條件：當 `Cache 總容量 / 路數（每路大小） > 頁大小（4KB）` 時，index 位元會超出頁內偏移範圍，就有別名風險；這也是為什麼很多處理器把**每路（way）容量限制為不超過一個頁大小**（如 4KB×N 路），使 VIPT 的 index 完全落在頁內偏移中，從而在效果上等價於 PIPT，同時保留了「無需位址轉換即可先行索引」的速度優勢。

```c
/* VIPT 別名風險判斷：只要 (每路大小 > 頁大小) 就可能發生 */
bool vipt_alias_risk(size_t way_size, size_t page_size) {
    return way_size > page_size;   /* index 位元跨出頁內偏移範圍 */
}
/* 本設備 A76 L2：512KB/8way = 每路 64KB > 4KB 頁 → 若是 VIPT 需特別設計避免別名 */
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 45 頁附近章節內文。

### 13. 請畫出在二級頁表架構中虛擬位址到實體位址查詢頁表散列過程。

以典型 32 位元、二級頁表（如 ARMv7 short-descriptor 或經典 x86 二級頁表）為例，虛擬位址被劃分為 `PGD index | PTE index | Offset` 三段：

```
虛擬位址(32bit)
+----------------+----------------+------------------+
| PGD index(高位) |  PTE index     |  Page Offset(低位) |
+----------------+----------------+------------------+
        |                 |                  |
        v                 v                  |
   [TTBR 指向 PGD 基址]                       |
        |                                    |
        v                                    |
  PGD[index] --> 指向二級頁表(PTE表)基址        |
                     |                        |
                     v                        |
              PTE[index] --> 頁幀號(PFN，實體頁基址)
                     |                        |
                     +----------+-------------+
                                v
                        實體位址 = PFN << 12 | Offset
```

流程：MMU 取虛擬位址高位段作為一級頁表（PGD）的索引，從 TTBR 暫存器指向的 PGD 基址加上該索引查得一個頁表項，該項指向二級頁表（PTE 表）的基址；再用虛擬位址中間段作為二級索引查得 PTE，其中包含實體頁幀號（PFN）及訪問屬性（可讀寫、可執行、Cache 屬性等）；最後將 PFN 與虛擬位址最低位的頁內偏移拼接，得到最終實體位址。若某一級頁表項無效則觸發缺頁異常（page fault）。

（本設備的 ARM64 實際使用 4 級頁表，48 位元虛擬位址、4KB 頁，詳見第 2 章第 2 題。）

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 50 頁附近章節內文。

### 14. 在多核處理器中，快取的一致性是如何實現的？請簡述MESI協定的含義。

多核下每個 CPU 都有私有 Cache（本設備每核私有 L1、A55 私有 L2/A76 私有 L2），當多個核快取了同一實體位址的數據副本、其中一個核修改後，必須保證其它核不再使用過期數據——這就是**Cache 一致性（coherency）**問題。常見解決方式是**總線監聽（snooping）協定**：所有核的 Cache 控制器都監聽共享總線/互連（本設備為 CCI/AMBA ACE 互連）上的讀寫事務，根據這些事務動態調整自己 Cache 行的狀態。

**MESI** 協定是最常用的一種監聽協定，為每個 Cache 行維護 4 種狀態（用 2 bit 編碼）：

- **M（Modified，已修改）**：該行只存在於本 Cache 中，且與主記憶體不一致（髒），本核可任意讀寫；
- **E（Exclusive，獨佔）**：該行只存在於本 Cache 中，但內容與主記憶體一致（乾淨），可直接轉為 M 而無需總線通知；
- **S（Shared，共享）**：該行可能同時存在於多個核的 Cache 中，內容與主記憶體一致，只能讀，寫入前須先廣播使其它副本失效；
- **I（Invalid，無效）**：該行數據無效，必須從主記憶體/其它 Cache 重新載入。

當某核要寫一個 S 或 I 狀態的行時，會在總線上發出 `BusRdX`（或 Invalidate）訊號，其它核收到後將各自副本置為 I；當某核要讀一個在其它核為 M 狀態的行時，擁有者需要先把數據寫回（或直接轉發），並將自身狀態降為 S。ARM/x86 處理器廣泛採用 MESI 或其變種（如 MOESI，增加 Owned 狀態避免寫回主記憶體，本設備 CCI-500/DSU 互連即支援類似語義）來實現 Cache 一致性。

```
CPU0: BusRd A  → 主存回應 → CPU0: I→E
CPU1: BusRd A  → CPU0 偵測到，E→S；CPU1: I→S
CPU1: BusRdX A(欲寫) → CPU0: S→I；CPU1: S→M
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 54 頁附近章節內文。

### 15. 快取在Linux內核中有哪些應用？

- **DMA 一致性管理**：`dma_map/unmap_single()`、`dma_sync_*` 系列介面，在設備 DMA 訪問前後主動 flush/invalidate Cache，保證 CPU 與外設看到一致數據（尤其在非 Cache 一致的 DMA 場景）；
- **可執行程式碼的自修改場景**：`flush_icache_range()`（模組載入、內核態程式碼patch、JIT、ptrace 寫斷點）保證 I-Cache 與 D-Cache 一致；
- **使用者態/內核態映射同一實體頁時的 flush_dcache_page()**：如檔案頁 cache 被內核寫入後，使用者態尚未映射的可執行檔案需要同步；
- **寫時複製（COW）與遷移頁面**：`copy_user_highpage()`、頁遷移（`migrate_page`）時需要維護目標頁的 Cache 狀態；
- **CPU 熱插拔/低功耗（cpuidle）**：核下電前要 flush 該核私有 Cache（如 `flush_cache_all()`），防止數據丟失；
- **`cache_line_size()`/`____cacheline_aligned` 等對齊巨集**：內核數據結構按 Cache 行對齊，避免偽共享（見第 29 題）；
- **`slab`/`slub` 分配器的著色（cache coloring）**：讓不同物件在 Cache 中錯開起始偏移，減少組衝突；
- **NUMA 感知的排程/記憶體分配**：結合 Cache 拓樸（`sched_domain`、`cacheinfo`）做負載均衡與就近記憶體分配。

實測：`cat /sys/devices/system/cpu/cpu*/cache/index*/*` 就是內核 `drivers/base/cacheinfo.c` 把每個 CPU 的 Cache 拓樸（level/size/ways/line size/shared_cpu_list）匯出到 sysfs 供使用者態與排程器使用的介面。

```c
// mm/slab.c：slab 配置器直接使用 cache_line_size() 做快取著色
cachep->colour_off = cache_line_size();
// mm/vmscan.c：依快取行大小決定一次批次操作的 PTE 數
int n = clamp_t(int, cache_line_size() / sizeof(pte_t), 2, 8);
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 53 頁附近章節內文。

### 16. 請簡述ARM big.LITTLE架構，包括總線連接和快取管理等。

big.LITTLE 是 ARM 提出的異構多核架構：將一組高性能大核（如 Cortex-A76/A75/A72）與一組低功耗小核（如 Cortex-A55/A53）封裝在同一晶片上，根據負載動態排程任務到合適的核簇，兼顧性能與功耗。典型連接方式：

- **總線互連**：大、小核簇各自通過 ACE（AXI Coherency Extensions）介面連接到 **CCI（Cache Coherent Interconnect，如 CCI-400/CCI-500/CCI-550）**或更新的 **DSU/CMN** 互連上，CCI 負責跨簇維護 Cache 一致性（監聽/轉發 MESI 相關總線事務）並提供簇間共享記憶體訪問；
- **GIC（Generic Interrupt Controller）**：統一管理跨核簇的中斷路由，使排程器可以把中斷/任務遷移到任意核；
- **Cache 層級**：每個核有私有 L1（及部分架構每核或每簇私有 L2），跨簇一致性由 CCI 保證；也可能存在跨簇共享的 L3/系統快取（SLC）。
- **排程模式**：早期有 cluster migration（整簇切換）、CPU migration（大小核一一對應切換）、**全局任務排程（Global Task Scheduling / HMP）**——現代 Linux 用 **EAS（Energy Aware Scheduling）** 結合 `sched_domain`、`arch_scale_cpu_capacity()` 實現任意核間任務遷移。

實測本設備 RK3588 即為 4×Cortex-A55（小核）+ 4×Cortex-A76（大核）的 big.LITTLE：

```
$ cat /proc/cpuinfo | grep -E "processor|CPU part"
processor 0~3 → CPU part: 0xd05 (Cortex-A55, variant 0x2)
processor 4~7 → CPU part: 0xd0b (Cortex-A76, variant 0x4)
```

Cache 拓樸（`/sys/devices/system/cpu/cpuN/cache/`）顯示：A55 每核私有 L1D/L1I 各 32KB(4-way)、私有 L2 128KB(4-way)；A76 每核私有 L1D/L1I 各 64KB(4-way)、私有 L2 512KB(8-way)；而 **L3 3MB(12-way) 由全部 8 個核共享**（`shared_cpu_list: 0-7`），說明 RK3588 用 DSU 把 8 核納入同一一致性域並共享一份系統級 L3。

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 65 頁附近章節內文。

### 17. 快取一致性（cache coherency）和一致性記憶體模型（memory consistency）有什麼區別？

- **Cache 一致性（coherency）**：針對**同一個**記憶體位址，保證所有處理器核在任意時刻看到的（最終會看到的）都是該位址**最新寫入的唯一值**——即多個 Cache 副本不能同時呈現不同數據。這是「單位址」的正確性問題，由 MESI 等總線監聽協定解決，對程式員通常透明。
- **記憶體一致性模型（consistency model）**：針對**不同位址**之間的訪存操作，規定這些操作對其它核而言「可被觀察到的順序」，即多個處理器同時對不同變數讀寫時，各處理器觀察到的**全域順序**是否與程序順序一致。它關心的是「多位址、多操作之間的順序」，由處理器的記憶體模型（強序 x86 TSO、弱序 ARM/RISC-V weak ordering）決定，程式員需要用記憶體屏障指令來顯式約束順序。

簡言之：coherency 保證「同一份數據不會分裂」，consistency 保證「多份數據/多次訪問的相對順序符合預期」；一致性協定是一致性記憶體模型的必要非充分條件——即使 Cache 完全一致，弱序處理器仍可能因為亂序執行/寫緩衝讓其它核看到「錯誤順序」的多變數更新，這正是需要記憶體屏障的原因（見第 4、5 題）。

```c
/* Coherency 保證看到的是同一份資料；但 Consistency 才保證「順序」正確 */
// CPU0                      // CPU1
data = 1;                    while (flag == 0) {}
dmb ish;   // 若無此屏障，   assert(data == 1); // 即使 data 的 Cache 完全一致，
flag = 1;  // CPU1 仍可能先看到 flag=1 卻還沒看到 data=1（亂序執行/寫緩衝所致）
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 35 頁附近章節內文。

### 18. 請簡述快取的回寫策略。

指 Cache 中的數據被修改後何時/如何寫回主記憶體，主要兩種：

- **寫通過（write-through）**：CPU 每次寫 Cache 命中時，同時把數據寫回下一級/主記憶體，保證 Cache 與主記憶體隨時一致；實現簡單、不會丟數據，但每次寫都要訪問較慢的主記憶體（通常配合寫緩衝 write buffer 緩解），頻寬壓力大。
- **寫回（write-back）**：CPU 寫命中時只更新 Cache 行內容，將該行標記為「髒（Dirty）」，暫不寫回主記憶體；只有當該行被**替換淘汰**、或被其它核/DMA 請求、或顯式 flush 時才一次性寫回。優點是大幅減少對主記憶體的寫次數（多次寫同一行只需最後寫回一次），是現代處理器（含本設備 Cortex-A55/A76）L1/L2/L3 普遍採用的策略；缺點是實現複雜（需要額外 Dirty 位元與寫回邏輯），且斷電/異常時若未及時寫回可能丟失數據（因此設備驅動、檔案系統在斷電場景要考慮 Cache 落盤/`flush`）。

配合寫策略還有**寫分配（write-allocate）**：寫未命中時先把整行數據從主記憶體載入進 Cache 再寫入（常與 write-back 搭配）；以及**非寫分配（no-write-allocate/write-around）**：寫未命中時直接寫主記憶體、不佔用 Cache 行（常與 write-through 搭配）。

```c
/* write-back：只標記 dirty，延後真正寫回 */
if (cache_hit) {
    cacheline.data = new_data;
    cacheline.dirty = true;          // 不立即寫主存
} else if (cacheline_evicted && cacheline.dirty) {
    write_back_to_memory(cacheline); // 只有被淘汰時才寫回
}
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 67 頁附近章節內文。

### 19. 請簡述快取行的替換策略。

當發生 Cache 缺失、且該組已存滿（組相聯情況下 N 路已用完）時，需要挑選一路淘汰騰出空間，常見策略：

- **隨機替換（Random）**：隨機挑選一路替換，硬體實現最簡單，性能中規中矩，避免了某些規律訪問模式下的病態最壞情況。
- **最近最少使用（LRU, Least Recently Used）**：淘汰最長時間未被訪問的一路，命中率通常最好，符合時間局部性原理；但真 LRU 需要為每路維護訪問時間戳/排序鏈表，路數越多硬體代價越高，實際處理器多用**偽 LRU（Pseudo-LRU，如樹形 PLRU、位元計數近似）**折中實現。
- **先進先出（FIFO）**：按照進入 Cache 的先後順序淘汰最早進入者，實現比 LRU 簡單，但不一定反映真實的訪問熱度。
- **輪詢/時鐘（Round-Robin / Clock）**：介於 FIFO 與 LRU 之間的近似演算法。

替換策略只與組相聯（或全相聯）Cache 有關；直接映射 Cache 由於每個位址唯一對應一路，沒有替換策略可言（新數據直接覆蓋舊數據）。ARM Cortex-A 系列（含本設備 A55/A76）L1/L2 通常採用硬體偽隨機或偽 LRU 策略，具體演算法由實現定義（IMPLEMENTATION DEFINED），軟體通常無法配置。

```c
/* 組相聯下的替換策略示意（實際由硬體 IMPLEMENTATION DEFINED 決定）*/
enum { RANDOM, LRU, FIFO, PLRU } policy;
int victim_way = pick_victim(set, policy); // ARM Cortex-A55/A76 多用硬體偽隨機/偽LRU
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 69 頁附近章節內文。

### 20. 多行程間頻繁切換對轉換旁視緩衝（Translation Look-aside Buffer， TLB）有什麼影響？現代的處理器是如何解決這個問題的？

TLB 快取的是「虛擬位址→實體位址」的轉換結果，而不同行程擁有**不同的頁表**（同一虛擬位址在不同行程中可能映射到不同實體位址，即「同名/homonym」問題，參見第9題）。若 TLB 項不區分行程，行程切換後舊行程殘留的 TLB 項就可能被新行程錯誤命中，導致位址轉換錯誤；早期做法是**每次行程切換（`switch_mm`）時無條件flush 整個 TLB**，這樣雖然正確，但新行程執行初期會頻繁發生 TLB miss（需要重新走頁表），如果行程切換非常頻繁（如高併發、容器/執行緒密集場景），TLB miss 帶來的頁表 walk 開銷會顯著拖累性能。

現代處理器（包括 ARMv8）引入**ASID（Address Space ID）**方案：為每個行程位址空間分配一個 ID，TLB 項除了虛擬位址外還攜帶該 ASID（本設備 TLB 項中 ASID 存於 Bit[63:48]），尋找 TLB 命中條件變為「虛擬位址比對 **且** ASID比對」。這樣行程切換時無需 flush 整個 TLB，只需切換 TTBR/ASID 暫存器，不同行程各自的 TLB 項可以共存、互不干擾，大幅減少了行程切換後的 TLB miss，只有當 ASID 數值空間耗盡發生「回卷（rollover）」時才需要做一次全局 flush 與重新分配。x86 也有類似的 PCID（Process-Context Identifier）機制。

```c
// arch/arm64/mm/context.c：ASID 讓進程切換不必 flush 整個 TLB
void check_and_switch_context(struct mm_struct *mm)
{
    ...
    old_active_asid = atomic64_read(&per_cpu(active_asids, cpu));
    if (old_active_asid && asid_gen_match(asid) &&
        atomic64_cmpxchg_relaxed(&per_cpu(active_asids, cpu),
                                  old_active_asid, asid))
        goto switch_mm_fastpath;   /* ASID 版本仍有效，免 flush TLB */
    ...
}
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 44 頁附近章節內文。

### 21. 請簡述NUMA架構的特點。

NUMA（Non-Uniform Memory Access，非統一記憶體訪問）從 SMP 演化而來：系統由多個「節點（node）」組成，每個節點包含一組 CPU 和一塊本地實體記憶體（以及可能的本地 I/O），節點間通過高速互連（如 QPI/UPI、AMD Infinity Fabric）連接。特點是：

- CPU 訪問**本節點本地記憶體**延遲低、帶宽高；訪問**其它節點的遠端記憶體**需要經過節點互連，延遲明顯更高、頻寬更低——即「記憶體訪問延遲不均一」；
- 邏輯上仍是一個統一的共享位址空間（區別於純粹的分散式/訊息傳遞系統），軟體可以像 SMP 一樣編程，但為了性能，作業系統和應用需要做 **NUMA 感知的排程與記憶體分配**（盡量讓任務在本地節點分配記憶體、就近排程，如 Linux 的 `numactl`、`mbind()`、`autonuma`）；
- 常見於多路伺服器 CPU（如 Intel 至強多路互聯、AMD EPYC、ARM 的 ThunderX2 等）。

實測本設備為**單晶片、單一 DDR 控制器域的 SoC**，`numactl --hardware` 返回 `No NUMA available on this system`，內核配置 `# CONFIG_NUMA is not set`，`/sys/devices/system/node/` 不存在——即典型的 **UMA（統一記憶體訪問）**架構：所有 8 個核訪問同一塊 DDR 記憶體的（近似）延遲一致，不存在節點劃分，這與多路伺服器的 NUMA 形成鮮明對比。

```bash
# 實測本設備：單一 SoC、單一記憶體控制器域，屬 UMA
$ numactl --hardware
No NUMA available on this system
$ grep CONFIG_NUMA /boot/config-$(uname -r) 2>/dev/null || zcat /proc/config.gz | grep CONFIG_NUMA
# CONFIG_NUMA is not set
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 70 頁附近章節內文。

### 22. ARM從Cortex系列開始性能有了質的飛躍，如CortexA8/A15/A53/A72，請指出Cortex系列在晶片設計方面的重大改進。

- **流水線深度與超標量/亂序執行**：從 Cortex-A8 的順序雙發射，到 A9/A15 引入亂序執行（Out-of-Order），A72/A76 在保持高效能的同時進一步加深流水線、提升發射寬度與執行單元數量（本設備 A76 為 4 發射亂序核）；
- **多核與 Cache 一致性互連**：引入 SMP 多核簇結構，配合 CCI/CoreLink 互連實現跨核 Cache 一致性；
- **big.LITTLE 異構計算**：A7/A15、A53/A57、A55/A76 等大小核組合，兼顧性能與功耗（見第16題）；
- **64 位元架構（ARMv8-A）**：從 A15/A7 的 32 位元 ARMv7-A 過渡到 A53/A57/A72/A76 的 64 位元 ARMv8-A，擴充暫存器數量（31個通用暫存器）、虛擬位址空間（48/52位元）、新增 NEON/加密指令擴充（AES/SHA/CRC32，本設備 `Features` 中可見 `aes pmull sha1 sha2 crc32`）；
- **更深/更大的 Cache 層級與 TLB**、硬體預取器改進；
- **更精細的電源管理**：DVFS（動態電壓頻率調節，本設備 `cpufreq` 顯示 A55 408MHz~1.8GHz、A76 408MHz~2.256GHz）、細粒度電源域/功耗門控；
- **原子操作與記憶體模型增強**：ARMv8.1 引入 LSE（Large System Extensions）原子指令（本設備 Feature 含 `atomics`），減少多核自旋鎖/原子操作開銷。

```bash
# 實測 RK3588 big.LITTLE 各簇 CPU part 編號（Cortex 系列辨識）
$ grep -E "processor|CPU part" /proc/cpuinfo | head -4
processor : 0
CPU part  : 0xd05   # Cortex-A55（小核）
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 71 頁附近章節內文。

### 23. 若對非對齊的記憶體進行讀寫，處理器會如何操作？

不同架構處理方式不同：
- 部分嚴格架構會直接觸發**對齊異常（Alignment Fault）**，交由軟體（內核）模擬完成非對齊訪問後再恢復執行，代價高昂；
- ARMv7 及以後（包括 ARMv8-A）**普通記憶體（Normal Memory）區域預設支援非對齊訪問**：處理器（在 `SCTLR_ELx.A` 位元未置位時）會自動將一次非對齊訪存拆分成兩次（或多次）對齊的總線傳輸，硬體內部處理拼接，程式員通常無感知，但相比對齊訪問會有額外的性能開銷（多一次總線週期，且如果跨越 Cache 行/頁邊界還可能觸發額外的 Cache 缺失或兩次頁表查詢）；
- 但對 **Device / Strongly-ordered 記憶體類型**（如 MMIO 暫存器空間）通常**不允許非對齊訪問**，會直接觸發 Data Abort 異常；對某些原子的 Load/Store-Exclusive、SIMD 大數據寬度訪問，架構也可能要求必須對齊，否則觸發異常。

Linux 內核可以通過 `SCTLR_EL1.A` 關閉非對齊訪問自動處理來 debug（或用 `/proc/cpu/alignment` 之類的介面在舊架構上統計/處理未對齊異常）。

```c
struct __attribute__((packed)) unaligned_t { char c; int i; };
struct unaligned_t *p = (struct unaligned_t *)addr;
int v = p->i;   /* ARMv8 Normal Memory 下硬體自動拆成兩次對齊的匯流排傳輸完成 */
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 78 頁附近章節內文。

### 24. 若兩個不同行程都能讓處理器的使用率達到100%，它們對處理器的功 耗影響是否一樣？

不一樣。CPU 利用率達到 100% 只說明「核在忙、沒有空閒進入 idle」，但功耗還取決於具體在執行**什麼樣的指令/微架構行為**：

- 若行程主要執行**密集型運算（如大量整數/浮點/SIMD 指令，尤其是高功耗的 NEON/SVE 向量運算或頻繁觸發 Cache miss、DRAM 訪問）**，會啟動更多功能單元、總線/記憶體控制器活動更頻繁，動態功耗（與開關活動 activity factor 成正比）顯著更高；
- 若行程主要是**忙等待/自旋（如空迴圈、頻繁但簡單的分支跳轉、命中 Cache 的小工作集）**，雖然利用率也是 100%，但活躍的電路規模、記憶體頻寬佔用小得多，功耗相對更低；
- 此外，不同利用率 100% 的負載可能觸發不同的 **DVFS 頻率/電壓**（如本設備 governor 會根據負載特徵選擇不同 OPP 點），頻率越高動態功耗隨頻率、電壓平方近似增長（`P ∝ f·V²`），因此即使兩者都跑滿 CPU，若被排程到不同頻率點，功耗也會不同；
- 還要看是被排程到大核（A76）還是小核（A55）——同樣 100% 利用率，A76 滿載功耗遠高於 A55。

因此「CPU 利用率」是排程/時間維度的指標，不能直接等價於功耗，需要結合具體指令組合、微架構活動、執行頻率/核類型綜合評估（這也是 Linux **EAS（Energy Aware Scheduling）**引入 per-CPU 能耗模型的原因）。

```bash
# 實測本機 DVFS：同樣 100% 使用率，跑在不同頻率/核心功耗差異很大
$ cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq   # A55
$ cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq   # A76
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 75 頁附近章節內文。

### 25. 為什麼頁表存放在主記憶體中而不是存放在晶片內部的暫存器中？

- **容量原因**：頁表需要覆蓋整個虛擬位址空間到實體位址空間的映射，條目數量龐大（現代 64 位元系統頁表可能達到 GB 級），晶片內部暫存器（觸發器）成本高、面積/功耗代價極大，不可能整合如此大容量的存儲；而主記憶體（DRAM）單位成本低、容量大，天然適合存放這種「大而稀疏訪問」的數據結構。
- **可分頁式的層級結構**：多級頁表設計允許按需分配（未使用的位址區間不必分配頁表頁），進一步利用了主記憶體「按需付費」的特性；
- **晶片內**只保留**最近使用過的少量轉換結果**——也就是 TLB（作為頁表的 Cache），用少量高速暫存器/CAM 存放熱點轉換，兼顧容量與速度：miss 時才去主記憶體中查完整頁表（頁表走訪，可由硬體 Page Table Walker 或軟體完成）。這本質上和「為什麼數據不能全放暫存器、要用 Cache+主記憶體的層級結構」是同一個道理。

```c
/* 32 位、4KB 頁的一級（線性）頁表需要的暫存器/記憶體量估算 */
#define ENTRIES (1UL << (32-12))   // 2^20 = 1,048,576 項
#define TABLE_BYTES (ENTRIES * 4)  // = 4MB，遠超晶片內暫存器可承受的容量
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 49 頁附近章節內文。

### 26. 為什麼頁表要設計成多級頁表？直接使用一級頁表是否可行？多級頁 表又引入了什麼問題？

**直接使用一級（線性/單級）頁表的問題**：假設 32 位元位址空間、4KB 頁，一級頁表需要 `2^32/4KB = 2^20` 個頁表項，若每項 4 位元組，則需要 4MB 連續記憶體；到了 64 位元位址空間（如 ARM64 的 48 位元虛擬位址）則需要 `2^48/4KB` 個項，頁表本身就要佔用 TB 級空間——而且**每個行程都要有一份完整頁表**，即使該行程實際只用到很少一部分位址空間，也必須提前分配整個頁表，浪費巨大。

**多級頁表的思路**：把線性頁表按虛擬位址高位切分成「頁目錄 → 頁表 → 頁幀」的樹狀結構，只有真正被該行程使用到的位址區間才需要分配對應的下級頁表頁，未使用區間對應的上級頁表項直接標記為無效（不存在），從而把頁表所佔空間從「和位址空間大小成正比」降低為「和實際映射的記憶體量成正比」，節省大量記憶體。

**多級頁表引入的問題**：
1. **位址轉換需要多次訪存**（本設備 ARM64 4 級頁表最壞情況需要連續訪問 4 次頁表 + 1 次實際數據，共 5 次記憶體訪問），比單級頁表慢，因此必須依賴 TLB 快取命中來彌補這一開銷；
2. 增加了硬體頁表走訪（或軟體 walk）邏輯的複雜度；
3. 多級結構下缺頁處理、頁表本身的分配/回收管理（如何建立/銷毀中間級頁表頁）也比單級複雜。

```c
/* 單級 vs 多級頁表所需空間對比（32 位、4KB 頁）*/
size_t single_level = (1UL<<20) * 4;      // 4MB，且每個進程都要「整份」配置
size_t two_level_worst = 1024*4 + 1024*(1024*4); // PGD 4KB + 最多 1024 個 PTE 表
/* 多級頁表只需替「實際用到的區間」配置下級頁表，可遠小於 4MB */
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 50 頁附近章節內文。

### 27. 記憶體管理單元（Memory Management Unit，MMU）查詢頁表的目的是找到虛擬位址對應的實體位址，頁表項中有指向下一級頁表基位址的指標，那它 指向的是下一級頁表基位址的實體位址還是虛擬位址？

是**實體位址**。因為 MMU 硬體頁表走訪器（Page Table Walker）本身工作在「MMU 尚未（對這次轉換而言）完成」的語境下——它的職責就是把虛擬位址轉換為實體位址，如果頁表項裏存的是下一級頁表的虛擬位址，那就成了「需要先做一次位址轉換才能拿到做位址轉換所需的輸入」的先有雞還是先有蛋的死循環。因此架構規定：TTBR 暫存器存放頂級頁表的**實體**基位址，頁表中每一級頁表項（如 ARM64 的 Table descriptor）裏存的下一級頁表基位址也都是**實體位址**，硬體頁表走訪器全程直接用實體位址訪存（通常還會繞過/直接訪問實體記憶體，不經過一般數據訪存的 Cache 一致性域），只有最終查到的葉子頁表項中的「實體頁幀號」才是最終答案。

軟體層面（Linux 內核）操作頁表時是通過內核的線性映射虛擬位址去讀寫頁表內容的（比如 `pgd_offset()` 返回的是虛擬位址指標），但頁表項**裡面存的內容**（即寫入到那些記憶體單元的值）永遠是實體位址，這是軟體視角與硬體視角的區別（詳見第2章第23題，Linux 如何用虛擬位址走訪、頁表項內容卻是實體位址）。

```c
// arch/arm64/include/asm/pgtable-types.h：頁表項是單純的 u64，存的是「下一級的物理位址」
typedef struct { pteval_t pte; } pte_t;
typedef struct { pgdval_t pgd; } pgd_t;
#define pgd_val(x)  ((x).pgd)   // MMU 硬體頁表遍歷器全程只認實體位址
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 51 頁附近章節內文。

### 28. 假設系統中有4個CPU，每個CPU都有各自的一級快取，處理器內部實現的是MESI協定，它們都想訪問相同位址的數據A，大小為64位元組，這4個 CPU的快取在初始狀態下都沒有快取數據A。在T0時刻，CPU0訪問數據A。 在T1時刻，CPU1訪問數據A。在T2時刻，CPU2訪問數據A。在T3時刻，CPU3想 更新數據A的內容。請依次說明，T0～T3時刻，4個CPU中快取行的變化情況。

| 時刻 | 事件 | CPU0 | CPU1 | CPU2 | CPU3 |
|---|---|---|---|---|---|
| 初始 | — | I | I | I | I |
| T0 | CPU0 讀 A（缺失，其它核都無該行）| **E**（獨佔，從主記憶體載入，無人共享） | I | I | I |
| T1 | CPU1 讀 A（發出 BusRd，CPU0 偵聽到並響應/共享） | **S**（由 E 降級為共享） | **S**（從 CPU0 或主記憶體獲得數據） | I | I |
| T2 | CPU2 讀 A（發出 BusRd，CPU0/CPU1 均已是 S，繼續共享） | **S** | **S** | **S**（加入共享） | I |
| T3 | CPU3 要寫 A（發出 BusRdX/Invalidate，使其它所有副本失效，自己獲得數據並修改）| **I**（被 CPU3 的 BusRdX 使無效） | **I** | **I** | **M**（獨佔且已修改，與主記憶體不一致） |

要點：第一個讀者直接進 E（因為讀之前該行在所有 Cache 中都是 I，沒有其他持有者），後續讀者的加入會把已持有該行的核（們）從 E/S 降級為 S 並共享；任何一個核要寫（更新數據）時都必須先通過總線廣播使其餘所有副本失效，自己獨佔該行並置為 M，寫回主記憶體前該行內容僅在 CPU3 私有 Cache 中是最新的。

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 60 頁附近章節內文。

### 29. 什麼是快取偽共享？請闡述快取偽共享發生時快取行狀態變化情況，以及軟體應該如何避免快取偽共享。

**快取偽共享（false sharing）**：多個 CPU 各自頻繁讀寫**不同的變數**，但這些變數在記憶體佈局上恰好落在**同一條 Cache 行**內。由於 MESI 協定是以「整條 Cache 行」為一致性維護粒度而非按位元組，即使兩個 CPU 操作的是行內互不相幹的位元組，只要其中一方寫了該行，就會觸發另一方持有的副本失效，造成大量不必要的 Cache 一致性流量與缺失，性能顯著下降——「共享」其實並未發生在邏輯數據上，只是實體上共享了同一條 Cache 行，故稱「偽共享」。

**狀態變化舉例**（CPU0 頻繁寫變數 a，CPU1 頻繁寫同一行內的變數 b）：
1. CPU0 寫 a：該行由 I/S 轉為 CPU0 獨佔 **M**；
2. CPU1 要寫 b：即使 b 與 a 無關，因為同行，CPU1 發出 `BusRdX`，CPU0 的該行被迫寫回並置為 **I**，CPU1 獲得數據後寫入變為 **M**；
3. CPU0 再寫 a：又要發 `BusRdX` 使 CPU1 該行失效為 **I**，自己轉為 **M**；
4. 如此在 CPU0、CPU1 之間反覆「乒乓」（ping-pong），行不斷在 M↔I 之間跳動，產生大量總線流量與延遲。

**軟體規避方法**：
- 讓不同執行緒/CPU 頻繁讀寫的（邏輯上無關的）變數在記憶體中**分開存放到不同 Cache 行**，常用**按 Cache 行大小對齊/填充（padding）**，如 Linux 內核的 `____cacheline_aligned_in_smp`、C11/C++17 的 `alignas(std::hardware_destructive_interference_size)`；
- 將 per-CPU 頻繁寫的數據組織為**per-CPU 變數**（如內核的 `DEFINE_PER_CPU`），從數據結構層面天然隔離到不同 Cache 行/不同記憶體區域；
- 調整結構體成員順序，把唯讀、跨核共享的欄位與各自私有、高頻寫的欄位分組隔離，避免它們混排進同一 Cache 行。

```c
/* 經典假共享(false sharing)範例與修正 */
struct { int a; int b; } shared;              // a, b 落在同一 cache line → 假共享
struct { int a; char pad[60]; int b; } fixed;  // 補齊到不同 cache line（64B）即可避免
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 62 頁附近章節內文。

### 30. CPU和快取之間，快取和主記憶體之間，主記憶體和輔存之間數據交換的單位分別是什麼？

- **CPU ↔ Cache**：以**字（word）**為單位（通常等於暫存器寬度，如 32/64 位元），CPU 的 Load/Store 指令按字（或位元組/半字，具體到指令粒度）讀寫；
- **Cache ↔ 主記憶體（DRAM）**：以**Cache 行（line）**為單位（本設備為 64 位元組），一次缺失/寫回都是整行搬運；
- **主記憶體 ↔ 輔存（磁碟/Flash 等外存）**：以**頁（page）**為單位（如 Linux 常見 4KB，本設備 `getconf PAGE_SIZE` = 4096），換頁/交換（swap）、檔案頁快取的讀寫都是整頁操作。

三者體現了存儲層級中「離 CPU 越近，交換粒度越小、速度越快、容量越小；離 CPU 越遠，交換粒度越大、速度越慢、容量越大」的一般規律。

```c
#define WORD_SIZE   8    // CPU  <-> Cache：以「字」為單位（本設備 64-bit）
#define LINE_SIZE  64    // Cache <-> 主存：以「Cache line」為單位（實測 64B）
#define PAGE_SIZE 4096   // 主存 <-> 輔存：以「頁」為單位（getconf PAGE_SIZE）
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 64 頁附近章節內文。

### 31. 作業系統選擇大粒度的頁面有什麼好處？選擇小粒度頁面有什麼好處？

**大頁（如 2MB/1GB 大頁，或 64KB 頁）的好處**：
- 同樣大小的位址空間需要的**頁表項數量更少**，頁表本身佔用記憶體更小、走訪層級更淺；
- **TLB 覆蓋的位址範圍更大**（TLB 項數量有限，每項覆蓋的記憶體更大意味著更少 TLB miss），顯著降低大記憶體工作集（如資料庫、大型記憶體計算）程序的位址轉換開銷；
- 減少缺頁異常次數（一次缺頁處理的數據量更大，攤薄了異常處理的固定開銷）。

**小頁（如 4KB）的好處**：
- **記憶體碎片/內部碎片更小**：行程實際請求的記憶體往往不是頁大小的整數倍，頁越小，最後一頁浪費的空間（內部碎片）越小；也更容易湊出連續大頁所需的實體連續性；
- **粒度更細，便於精細化的記憶體管理**：寫時複製（COW）、按需換頁（demand paging）、記憶體回收（reclaim）、權限設置（讀/寫/執行）都可以以更小單位操作，減少不必要的數據複製/換入換出，適合記憶體緊張、多任務、記憶體映射粒度要求高的場景（如載入可執行檔案的不同段）；
- 更容易在實體記憶體緊張時找到可用的空閒頁（大頁要求實體連續，容易因碎片而分配失敗，小頁要求低）。

因此現代作業系統（含 Linux/本設備）通常以小頁（4KB）為預設粒度，同時提供**大頁（HugePage/Transparent HugePage）**作為可選優化，讓程序按需在兩者之間取捨。

```bash
# 大頁 vs 小頁的取捨：本設備預設 4KB，但保留 Hugepage 介面供選用
$ getconf PAGE_SIZE
4096
$ cat /proc/meminfo | grep Hugepagesize
Hugepagesize:       2048 kB
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 48 頁附近章節內文。

### 32. 引入分頁機制的虛擬記憶體是為了解決什麼問題？

- **位址空間隔離與保護**：每個行程擁有獨立的虛擬位址空間和頁表，天然隔離行程之間的記憶體訪問，一個行程無法直接讀寫另一個行程的實體記憶體，提高系統安全性與穩定性；
- **簡化編程模型/重定位問題**：行程可以假設自己獨佔從 0 開始的連續位址空間，無需關心程序實際載入到實體記憶體的哪個位置，連結器/編譯器可以使用固定的虛擬位址，避免了早期「程序需要根據載入位址重定位」的複雜性；
- **突破實體記憶體容量限制**：通過按需調頁（demand paging）與磁碟交換（swap），行程可以使用的虛擬位址空間總量可以超過實際實體記憶體大小，作業系統按需把暫不使用的頁換出到磁碟；
- **高效的記憶體共享與去重**：多個行程可以將各自的虛擬位址映射到同一實體頁（共享庫、`mmap` 共享記憶體、寫時複製 `fork`），既節省實體記憶體又實現行程間通信；
- **細粒度的訪問權限控制**：頁表項可以為不同頁設置唯讀/讀寫/不可執行（NX/XN）等屬性，實現記憶體保護（如防止程式碼段被修改、數據段被執行）；
- **減少外部碎片**：以固定大小的頁為單位分配實體記憶體，不要求行程佔用連續實體記憶體，避免了早期「分段式」記憶體管理容易產生的外部碎片問題（代價是頁內的內部碎片，見第31題）。

```c
/* 分頁機制：邏輯位址與物理位置解耦，允許「假裝」擁有連續大空間 */
void *p = mmap(NULL, 1UL<<30, PROT_READ|PROT_WRITE,
               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0); // 1GB 虛擬位址，實體頁按需分配
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 48 頁附近章節內文。

### 33. 缺頁異常相比一般的中斷存在哪些區別？

- **同步 vs 異步**：一般外部中斷是**異步**的，與當前執行的指令流無關，隨時可能到來；缺頁異常是**同步異常（synchronous exception）**，由當前正在執行的具體指令（訪存指令）直接觸發，與該指令強相關。
- **精確定位與可恢復性（restartable）**：缺頁異常發生時，處理器必須精確保存「引發異常的那條指令」的上下文（如 ARM64 的 `ELR_ELx` 指向該指令本身，而不是下一條），使得內核在缺頁處理程序（`do_page_fault`/`handle_mm_fault`）分配好實體頁、建立好映射之後，可以**從異常前那條指令重新執行**，彷彿缺頁從未發生過；而一般中斷處理完成後是從**被中斷的下一條指令**或獨立的中斷服務流程返回，不需要「重新執行原指令」。
- **處理內容依賴 CPU 現場**：缺頁異常處理需要讀取導致異常的**具體位址（Fault Address Register，如 `FAR_ELx`）**及**訪問類型（讀/寫/取指、權限錯誤還是頁不存在）**等 CPU 相關的現場資訊來決定如何修復（分配頁、COW、換入 swap、還是觸發 SIGSEGV），而普通中斷處理通常只需知道中斷號，不依賴被中斷指令的具體語義；
- **可能涉及阻塞與排程**：缺頁處理可能需要等待磁碟 I/O（換入頁面）等耗時操作，處理過程中可以睡眠、被排程切換；而中斷上下文（尤其是硬中斷處理部分）通常不允許睡眠，必須儘快返回或轉交給可睡眠的下半部（softirq/tasklet/workqueue）處理。

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 50 頁附近章節內文。

### 34. 快取設計中，如何實現更高的性能？

- **提高命中率**：更大容量、更高相聯度（組相聯路數）、更優的替換演算法（偽LRU）、軟體/硬體預取（prefetch，根據訪存模式提前取入即將使用的行）、Cache 著色減少衝突缺失；
- **降低訪問延遲**：多級 Cache 層級設計（L1 小而快、L2/L3 逐級增大但延遲增加），讓絕大多數訪問在最快的 L1 命中；關鍵路徑上採用 VIPT（可與 TLB 尋找並行）而非串行的先轉換位址再查 Cache；
- **提高頻寬/併發度**：非阻塞 Cache（non-blocking / lockup-free cache，缺失後不阻塞後續無關請求，用 MSHR—Miss Status Holding Register 跟蹤多個在途缺失）、多埠/多存儲體（bank）設計允許同週期多次訪問、寫緩衝（write buffer）與合併寫（write combining）減少寫操作對流水線的阻塞；
- **降低一致性開銷**：更高效的互連（CCI/CCN/DSU/CMN 等）、目錄式（directory-based）一致性協定（大規模多核下比全廣播監聽更省頻寬）、MOESI 的 Owned 狀態減少不必要的寫回；
- **減少軟體維護成本換取硬體正確性**：採用 PIPT/受限 VIPT 避免別名問題（第10~12題），減少軟體 Cache flush 的頻率與範圍；
- **針對負載做訂製策略**：如根據訪問模式選擇合適的替換/預取策略、結合大頁減少 TLB miss 從而間接提升 Cache 訪問效率（TLB miss 期間無法繼續訪存流水線）。

綜合來看，Cache 性能優化是「命中率 × 單次訪問延遲 × 併發吞吐」三者的整體權衡，實際處理器設計（如本設備 Cortex-A76 相比 A55 有更大 L1/L2、更高相聯度、更複雜的亂序執行與預取器）正是圍繞這一權衡在成本、功耗與性能之間做出的取捨。

```c
/* 非阻塞 Cache（lockup-free）示意：缺失時仍可處理後續不相關請求 */
struct mshr { uint64_t addr; bool valid; } mshr_table[MAX_OUTSTANDING_MISS];
/* 命中率(組相聯/著色/預取) x 單次延遲(PIPT/多級) x 併發吞吐(非阻塞+MSHR) 三者權衡 */
```

> 📖 **書籍出處**：《奔跑吧 Linux核心》(第二版) 卷1 第1章〈處理器架構〉，原書 PDF 第 44 頁附近章節內文。
