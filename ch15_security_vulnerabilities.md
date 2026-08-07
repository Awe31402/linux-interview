# 卷2 第 6 章（全書第 15 章）安全漏洞分析 — 高頻面試題解答

> **來源**：《奔跑吧 Linux內核》（第二版）卷2 第 6 章〈安全漏洞分析〉開篇「本章的高頻面試題」，共 9 題。
> 原文純文字檔：`books/running-linux-kernel/running-kernel-2-txt/13_第6章_安全漏洞分析.txt`
> （下稱「§6.x，行 N」，**行號一律以該純文字檔為準**）

---

## 本章面試題目列表

1. 请简述高速侧信道攻击的原理。
2. 在CPU熔断漏洞攻击中，攻击者在用户态访问内核空间时会发生异常，攻击者进程会被终止，那么如何解决这个问题？
3. 请简述熔断漏洞攻击的原理和过程。
4. 请简述KPTI方案的实现原理。
5. 在使能了KPTI方案的ARM64 Linux中，通过copy_to_user()/copy_from_user()访问用户空间地址时，CPU使用什么ASID去查询TLB？这对性能有什么影响？
6. 请简述分支预测的工作原理。
7. 请简述CPU"幽灵"漏洞变体1的攻击原理。
8. 请简述ARM64架构中新增的CSDB指令的作用。
9. 内核新增的接口函数array_index_nospec()是如何规避幽灵漏洞的？

---

## 這一章的實驗機：一台「熔斷免疫、但幽靈仍中」的真硬體

| | 書上（5.0 / x86 + 舊 ARM） | 本機實測（6.1.115 / RK3588） |
|---|---|---|
| 平台 | 泛指易受攻擊的處理器 | **Radxa ROCK 5B**：4×Cortex-A55(0xd05) + 4×Cortex-A76(0xd0b) |
| 熔斷 (Meltdown) | 可行，能讀核心資料 | **`meltdown: Not affected`**：A76 `ID_AA64PFR0_EL1.CSV3=1`、A55 在核心白名單 |
| 幽靈 v1 (Spectre-v1) | 可行 | **仍可行**：本文 PoC 在同一位址空間洩漏了 40 個位元組 |
| 幽靈 v2 (Spectre-v2/BHB) | 可行 | `Mitigation: CSV2, BHB`；**A76 掛 `__bp_harden_el1_vectors`、A55 用原始 `vectors`** |
| KPTI | x86 預設開；ARM64 需 `CONFIG_UNMAP_KERNEL_AT_EL0` | **編進去了但沒生效**：核心頁 `nG=0`（全域）、沒有 `tramp_vectors` 符號 |

> 也就是說，本機是一個**天然的對照組**：熔斷這條路被硬體堵死（Q3 的 PoC 拿不到資料、KPTI 因此沒被啟用），
> 但幽靈變體 1 這條路仍然通（Q7 的 PoC 完整偷出祕密字串）。這正好把「熔斷 vs 幽靈」的差別用實驗劃清。

實機資訊：

```
$ uname -a
Linux rock-5b 6.1.115+ #1 SMP Mon Apr 27 08:30:35 UTC 2026 aarch64 GNU/Linux

$ for f in /sys/devices/system/cpu/vulnerabilities/*; do printf "%-22s %s\n" "$(basename $f):" "$(cat $f)"; done
meltdown:              Not affected
spectre_v1:            Mitigation: __user pointer sanitization
spectre_v2:            Mitigation: CSV2, BHB
spec_store_bypass:     Mitigation: Speculative Store Bypass disabled via prctl
...

$ zcat /proc/config.gz | grep -E "UNMAP_KERNEL_AT_EL0|MITIGATE_SPECTRE_BRANCH_HISTORY"
CONFIG_UNMAP_KERNEL_AT_EL0=y
CONFIG_MITIGATE_SPECTRE_BRANCH_HISTORY=y
```

---

## 書是 5.0/QEMU 寫的，本機是 6.1.115/RK3588 —— 8 處差異

| # | 書上 | 本機實測 | 題號 |
|---|------|------|------|
| 1 | 熔斷可行，KPTI 是必要修復 | **A76 `CSV3=1`、A55 白名單** → `meltdown: Not affected`；植入已知祕密 `0x5a` 到核心，signal 法探測 2000 次**一個位元組都偷不到** | Q2、Q3 |
| 2 | `CONFIG_UNMAP_KERNEL_AT_EL0` 開了就有 KPTI | 本機**開了但沒啟用**：`_stext` 末級 PTE `nG(bit11)=0`（全域）、`/proc/kallsyms` 裡**沒有 `tramp_vectors`**。因為硬體免疫，`unmap_kernel_at_el0()` 判定不需要 | Q4、Q5 |
| 3 | 「KPTI 前用 TTBR0 存 ASID，KPTI 後用 TTBR1」（行 518-525） | 本機 `TCR_EL1.A1=1` → **ASID 一律由 TTBR1_EL1 提供**（現代 arm64 的預設，與是否 KPTI 無關） | Q5 |
| 4 | KPTI 下核心偶數 ASID、使用者奇數 ASID | 本機 KPTI 沒生效 → **一個行程只有一個 ASID**，沒有奇偶配對；偶/奇分離的答案要在「KPTI 生效」的前提下講 | Q5 |
| 5 | Spectre-v2 用 `tramp_ventry` 裡的 `bl/b .`（行 481-485） | 本機 v2 緩解走 **`__bp_harden_el1_vectors`（BHB 迴圈/`clearbhb`）**，而且**只掛在 A76 上**；in-order 的 A55 不受 BHB 影響仍用原始 `vectors` | Q6、Q7 |
| 6 | `array_index_mask_nospec()` 反組譯是 `cmp/sbc/csdb`（行 686-696） | 本機核心 `invoke_syscall` 裡**原封不動找到這三條**：`cmp`→`sbc(ngc)`→`csdb(0xd503229f)` | Q8、Q9 |
| 7 | 沒提時鐘怎麼來 | 本機 EL0 只能讀 24 MHz 的 `CNTVCT`，量不出 cache 延遲；本文用 **`perf_user_access=1` + `config1=0x2`** 拿到週期級 `PMCCNTR_EL0`（見[附錄](#appendix)） | Q1、Q3、Q7 |
| 8 | 沒提探針陣列步長 | 4096 步長會讓所有探針**撞進同一個 L1 cache set**（A76 L1d 4-way）而失真；本文用 **4160 步長**把每個候選值散到不同 cache set（見[附錄](#appendix)） | Q1、Q3、Q7 |

---

## 目錄

| # | 題目 | 實機關鍵證據 |
|---|------|-------------|
| [1](#q1) | 高速側信道攻擊原理 | Flush+Reload 直方圖：**命中 52 週期、未命中 399/470 週期**，兩峰完全分離；隱蔽通道把 `"benshushu"` **9/9** 位元組傳出來 |
| [2](#q2) | 異常後如何繼續攻擊 | SIGSEGV handler + `siglongjmp` 與 fork 兩法，探測 **2000/2000 次進程都存活** |
| [3](#q3) | 熔斷原理與過程 | 核心植入已知祕密 `0x5a`，signal 法 **2000 次還原不出**；A76 `CSV3=1`、A55 白名單 → 硬體免疫 |
| [4](#q4) | KPTI 實作原理 | 書上原理逐條對照原始碼；本機 **KPTI 沒生效**（`nG=0`、無 `tramp_vectors`），因為硬體免疫 |
| [5](#q5) | copy_*_user 用哪個 ASID | `TCR_EL1.A1=1` → ASID 來自 TTBR1；KPTI 生效時是**偶數(核心)ASID**查使用者頁 → TLB miss → 效能損失 |
| [6](#q6) | 分支預測工作原理 | 已排序 vs 未排序同一迴圈：**A76 2.29×、A55 1.74×** 的誤判懲罰（週期級量測） |
| [7](#q7) | 幽靈變體1攻擊原理 | Spectre-v1 PoC **越過 `if(x<size)` 邊界檢查，洩漏 40/40 位元組**："The Magic Words are Squeamish Ossifrage." |
| [8](#q8) | CSDB 指令的作用 | 在核心 `invoke_syscall` 反組譯出 `cmp/sbc/csdb`；`csdb = 0xd503229f` |
| [9](#q9) | array_index_nospec() 如何規避 | 遮罩表：`index<size → 0xff..ff`、`index>=size → 0`；核心 syscall 表存取處實測用到 |

---

## 一鍵重現

```bash
cd notes/experiments && ./ch15_run_all.sh radxa@192.168.68.58
```

| 檔案 | 跑在哪 | 用途 | 題號 |
|------|--------|------|------|
| [`pmu_user.c`](./experiments/pmu_user.c) | 模組 | 打開 EL0 直讀 `PMCCNTR_EL0`（週期級時鐘的備援路徑） | Q1/Q3/Q7 |
| [`sidechannel.h`](./experiments/sidechannel.h) | 共用 | Flush+Reload 原語 + perf 自我監控週期計數器 | Q1/Q3/Q7 |
| [`sec_probe.c`](./experiments/sec_probe.c) | 模組 | 讀 CSV2/CSV3、TCR.A1/ASID、核心頁 `nG`、每 CPU VBAR、反組譯 `csdb`、植入核心祕密 | Q3/Q4/Q5/Q7/Q8 |
| [`flush_reload.c`](./experiments/flush_reload.c) | 使用者態 | cache 命中/未命中延遲直方圖 + 隱蔽通道 | **Q1** |
| [`meltdown_test.c`](./experiments/meltdown_test.c) | 使用者態 | 熔斷 PoC + 兩種例外抑制（signal/fork） | **Q2/Q3** |
| [`branch_pred.c`](./experiments/branch_pred.c) | 使用者態 | 分支誤判懲罰（已排序 vs 未排序） | **Q6** |
| [`spectre_v1.c`](./experiments/spectre_v1.c) | 使用者態 | 幽靈變體1 PoC（越界檢查繞過） | **Q7** |
| [`nospec_mask.c`](./experiments/nospec_mask.c) | 使用者態 | `array_index_mask_nospec` 遮罩 + `csdb` 機器碼 | **Q8/Q9** |
| [`ch15_run_all.sh`](./experiments/ch15_run_all.sh) | 主機 | 一鍵佈署與重現 | 全部 |

---

<a name="q1"></a>
## Q1：請簡述高速側信道攻擊的原理

### 書上怎麼說（§6.1，行 26-104）
側信道攻擊利用「加密設備運行時的時間/功率/電磁洩漏」來破解（行 27-29）。熔斷與幽靈用的是**時間側信道**：
cache 命中 vs 未命中的存取延遲差（行 30-40，「兩者的時間差異非常明顯，大約有 300 個時鐘週期以上」）。
書上偽代碼（行 50-57）：`clflush` 把探針陣列 `user_probe[]` 沖出 cache → 讀被攻擊值 → 用該值當索引存取
`user_probe[index]` → 最後**以 cache line 為步長遍歷 `user_probe[]` 量每個存取時間**，時間短者反推出祕密。

### 本機實測

用 [`flush_reload.c`](./experiments/flush_reload.c) 對同一條 cache line，分別在「已 flush」與「已載入」下量延遲：

```
$ taskset -c 4 ./flush_reload          # 綁 A76 大核
時鐘來源：perf mmap (PMCCNTR_EL0)
[cache 延遲量測 @ 200000 次取樣]
  命中(cached)   平均 = 52.0 週期
  未命中(flushed)平均 = 470.3 週期
  差異 = 418.3 週期  <-- 這就是側信道能利用的時間差

[延遲直方圖]
  命中(cached) 範圍 [52..70]：
    52 cyc |################################################## 199994
  未命中(flush) 範圍 [399..399]：
   399 cyc |################################################## 200000
  自動判別門檻 = 261 週期
```

兩個峰**完全分離**（52 vs 399），中間拿 261 當門檻，判別零重疊——這就是側信道成立的物理基礎。
接著把它當成**隱蔽通道**：sender 以「祕密值」當索引把 `user_probe[secret]` 拉進 cache，receiver 掃描找出變快的
index，完整還原祕密字串（完全對應書上偽代碼行 53-78）：

```
[隱蔽通道] 用 Flush+Reload 傳字串 "benshushu"（門檻 261 週期）
  祕密位元組 -> 側信道還原：benshushu
  正確還原 9/9 個位元組
```

> **重點**：側信道本身不「偷」資料，它只是把「祕密值」編碼成「哪條 cache line 被載入」這個可觀測的物理狀態；
> 攻擊者再用「存取延遲短=命中」把它讀回來。熔斷與幽靈的差別只在**怎麼讓 CPU 去載入那條 line**（見 Q3、Q7）。
> 詳見 §6.1 行 30-40（延遲差）、行 72-78（遍歷測量）。

---

<a name="q2"></a>
## Q2：異常後進程被終止，如何繼續側信道攻擊？

### 書上怎麼說（§6.1，行 81-100）
「若運行於用戶態的進程訪問特權頁面……用戶進程收到段錯誤信息而被終止」（行 81-82）。解法：**在攻擊者進程中設置
異常處理信號**，`SIGSEGV` 發生時調用回調函式（`sigsegv()`），`return` 回來而不是讓進程死掉（行 82-100 給了
`sigaction(SIGSEGV, ...)` 的範例碼）。書上還提到另一條路是用 Intel TSX 的交易記憶體把例外「吞掉」。

### 本機實測

[`meltdown_test.c`](./experiments/meltdown_test.c) 實作了**兩種**例外抑制，用 `METHOD=` 選：

- `signal`（對應書上）：`sigaction(SIGSEGV, ...)` + `siglongjmp()`，讀非法位址觸發例外後跳回，進程不死；
- `fork`：每次探測 `fork()` 一個子進程去「踩地雷」，父進程只做 Reload（x86 世界常用來取代 TSX 的做法）。

拿 Q3 植入核心的已知祕密位址當靶，探測 2000 次：

```
$ KADDR=0xffff00014b39a000 METHOD=signal taskset -c 4 ./meltdown_test
進程存活 2000/2000 次探測（例外抑制成功 = Q2 得證）
...
$ KADDR=0xffff00014b39a000 METHOD=fork taskset -c 4 ./meltdown_test
進程存活 2000/2000 次探測（例外抑制成功 = Q2 得證）
```

兩種方法都讓進程在**2000 次非法存取後照樣活著**，攻擊迴圈得以持續——這就是 Q2 的答案。
（至於「能不能偷到資料」是 Q3 的事：本機硬體免疫，抑制了例外也偷不到，見 Q3。）

> **重點**：例外抑制（signal/TSX/fork）只是「讓攻擊程式不死」的工程手段，和「能不能洩漏」是兩回事。
> 書上原理見 §6.1 行 81-100。

---

<a name="q3"></a>
## Q3：請簡述熔斷漏洞攻擊的原理和過程

### 書上怎麼說（§6.2，行 106-148）
熔斷利用**亂序執行的副作用**破壞位址空間隔離（行 107-109）。三個關鍵（§6.2.1）：
1. **亂序執行**（行 113-134）：Tomasulo/保留站，指令「順序發車、亂序超車、順序歸隊」；
2. **異常處理**（行 135-148）：異常指令帶著標記到保留站，出口被封鎖、異常指令及其後指令**不提交**；
   **但**亂序執行時後面的訪存指令**已經把物理資料預載到 cache**了（行 146-148）——這就是後門；
3. **地址空間**（行 149-156）：分頁隔離，但 TLB/MMU 在拿到物理位址那一刻**不檢查權限**（§6.2.2 行 161-164）。

過程（= Q1 偽代碼）：用戶態讀核心位址 → 觸發例外但亂序已預取核心資料進 cache → 用該值當索引汙染探針陣列
→ 例外用 signal 吞掉（Q2）→ Flush+Reload 反推核心資料。

### 本機實測：這台機器對熔斷免疫

用 [`sec_probe.ko`](./experiments/sec_probe.c) 讀每顆 CPU 的 `ID_AA64PFR0_EL1.CSV3`（CSV3≥1 = 對
「rogue data cache load / 熔斷」免疫）：

```
sec_probe:   CPU0 Cortex-A55  MIDR=0x412fd050  CSV2=0  CSV3=0 (ID未宣告，靠核心白名單判定)
sec_probe:   CPU4 Cortex-A76  MIDR=0x414fd0b0  CSV2=1  CSV3=1 (熔斷免疫)
```

- **A76**：`CSV3=1`，硬體直接宣告熔斷免疫；
- **A55**：`CSV3=0`（沒在 ID 暫存器宣告），但 A55 是**順序（in-order）核心**，本來就不做「先預取再回退」那套，
  Linux 把它放進 `kpti_safe_list` 白名單（`arch/arm64/kernel/cpufeature.c`），所以 `/sys` 仍報 `meltdown: Not affected`。

為了不只是「讀旗標」，`sec_probe` **在核心空間 kmalloc 一頁、寫入已知祕密 `0x5a('Z')`、印出它的核心 VA**，
再用 `meltdown_test` 去攻擊這個**值我們早就知道**的位址：

```
sec_probe: secret VA = 0xffff00014b39a000  值 = 0x5a ('Z')
$ KADDR=0xffff00014b39a000 METHOD=signal taskset -c 4 ./meltdown_test
[校準] 命中 53 / 未命中 469 -> 門檻 261 週期
進程存活 2000/2000 次探測（例外抑制成功 = Q2 得證）
Flush+Reload 最高票 index = 0xee，得票 1/2000
結論：無明顯洩漏 -> 本機硬體對熔斷免疫（CSV3=1，與 /sys 一致）。
```

探測 2000 次，最高票 index 只拿到 1 票的雜訊，**`0x5a` 從頭到尾沒被還原出來**。對照 Q7 的幽靈 PoC 在同機
「40/40、每位元組 1000/1000 票」——差別一目了然：**熔斷這條路被硬體封死，幽靈那條沒有**。

> **重點**：熔斷 = 亂序執行 + 例外延後 + TLB/MMU 不檢權限 三者疊加的副作用（§6.2.1）。
> 本機 A76 用 `CSV3=1`、A55 用「順序核心 + 核心白名單」把這條路堵死。
> **誠實補充**：`meltdown_test` 的 `fork` 法若拿「核心 *程式碼段* `_stext`」當靶，會出現一個穩定的
> `index=0x04` 假陽性（那是 fork/COW/exit 路徑對共享頁的雜訊，並非真的讀到 `_stext`）；
> 改用「植入的已知祕密 `0x5a`」當靶，signal/fork 兩法都乾淨地報「無洩漏」，證明那確實是假陽性。

---

<a name="q4"></a>
## Q4：請簡述 KPTI 方案的實現原理

### 書上怎麼說（§6.2.2，行 158-529）
KPTI（Kernel Page-Table Isolation）把**每個進程的一張頁表拆成兩張**——核心頁表與使用者頁表（行 167-173）：
- 用戶態跑**使用者頁表**；陷入核心時，經一小段**跳板（trampoline）**把頁表切成核心頁表；返回用戶態再切回。
- 用戶態時核心頁表**只映射跳板頁**，其他核心空間都是無效映射 → 亂序執行時 MMU 查到無效映射，**預取不到核心資料**。

ARM64 的實作要點：
1. `CONFIG_UNMAP_KERNEL_AT_EL0` 打開 KPTI（行 198-199）；
2. 把核心頁從「全域 TLB」改成「進程獨有」——**核心頁也加 `PTE_NG`**（`PTE_MAYBE_NG`，行 200-217）；
3. 核心頁表也配一個 ASID，**偶數給核心、奇數給使用者**，成對配（行 218-239）；
4. 建 `.entry.tramp.text` 段、`tramp_pg_dir` 跳板頁表、`tramp_vectors` 跳板向量表（行 273-529）；
   `tramp_map_kernel`/`tramp_unmap_kernel` 靠加減 `PAGE_SIZE + RESERVED_TTBR0_SIZE` 與 `USER_ASID_FLAG`
   在 TTBR1 裡切換核心/使用者頁表 + 偶/奇 ASID。

### 本機實測：KPTI「編進去了但沒生效」

`CONFIG_UNMAP_KERNEL_AT_EL0=y`，但因為硬體對熔斷免疫，`unmap_kernel_at_el0()` 判定**不需要啟用**。
三個獨立證據（[`sec_probe.ko`](./experiments/sec_probe.c)）：

**證據1：核心頁的 `nG` 位 = 0（全域，不是進程獨有）**——軟體巡覽 `_stext` 的末級描述符：

```
sec_probe: _stext(0xffff800008010000) 末級描述符 = 0x0050000000410783
sec_probe:   nG(bit11) = 0 -> global：核心頁是全域 TLB（KPTI 未在此頁生效）
```

若 KPTI 生效，核心頁會被加上 `PTE_NG`（bit11=1，書上行 200-203）；本機是 0，代表核心頁仍是**全域 TLB**。

**證據2：`/proc/kallsyms` 裡根本沒有 `tramp_vectors` / `__entry_tramp_text_start`**——跳板從沒被建起來：

```
$ sudo grep -iE "tramp_vectors|entry_tramp" /proc/kallsyms   # 空
```

**證據3：VBAR_EL1 指向 `vectors` 或 `__bp_harden_el1_vectors`，都不是 `tramp_vectors`**：

```
sec_probe:   CPU0 Cortex-A55  VBAR_EL1=0x...010800 = vectors (原始)
sec_probe:   CPU4 Cortex-A76  VBAR_EL1=0x...012800 = __bp_harden_el1_vectors (BHB/v2 硬化)
```

若 KPTI 生效，用戶態時 VBAR 會被設成 `tramp_vectors`（書上行 361、404）；本機不是。

> **重點**：KPTI 的原理（雙頁表 + 跳板 + 核心頁改 nG + 偶/奇 ASID）在 §6.2.2 講得很完整；
> 但**本機是一個「開了 config 卻沒啟用」的活教材**——`unmap_kernel_at_el0()` 會看 CPU 是否真的需要
> （熔斷是否可行、KASLR 是否要求），本機硬體免疫所以核心頁維持全域、跳板不建。
> 這也印證書上一句被忽略的前提：KPTI **不是「編了就有」**，要 CPU 真的有熔斷風險才會被打開。

---

<a name="q5"></a>
## Q5：KPTI 下 copy_to_user()/copy_from_user() 用什麼 ASID 查 TLB？效能影響？

### 書上怎麼說（§6.2.2，行 256-267、518-525）
KPTI 下核心態帶**偶數 ASID**。當核心透過 `copy_to_user()/copy_from_user()` 存取使用者位址時，
「依然帶著偶數 ASID 來查詢 TLB，導致 TLB 未命中，因為當前 CPU 只有一個 ASID 在使用，即分配給核心空間的偶數
ASID」（行 263-265）——使用者頁的 TLB 表項是**奇數 ASID**，配不上，所以 miss，只能走 MMU 翻頁，
**「這會有一點點性能損失」**（行 265）。ARM64（截至 v8.4）**不能同時用兩個 ASID 查 TLB**（行 266-267）。
另外書上提醒（行 518-525）：ASID 存在 TTBR0 還是 TTBR1，由 `TCR_EL1.A1` 決定；KPTI 前用 TTBR0，
KPTI 後改在 TTBR1 設 ASID。

### 本機實測

[`sec_probe.ko`](./experiments/sec_probe.c) 讀 `TCR_EL1.A1` 與兩個 TTBR 的 ASID 欄位：

```
sec_probe: TCR_EL1.A1 = 1  -> ASID 由 TTBR1_EL1 提供
sec_probe: TTBR0_EL1 = 0x0000000103fe8001  ASID=0 (TTBR0 的 ASID 欄位被忽略)
sec_probe: TTBR1_EL1 = 0x5492000001b2d001  ASID=21650
```

- **`TCR_EL1.A1 = 1`**：本機**ASID 一律由 TTBR1_EL1 提供**（這是現代 arm64 的預設，和書上「KPTI 後才用 TTBR1」
  的敘述不同——6.1 不管有沒有 KPTI 都把 A1 設 1）；
- 因為本機 KPTI **沒生效**（Q4），所以**一個行程只有一個 ASID**（這裡是 21650），**沒有偶/奇配對**。

所以 Q5 要分兩層回答：

1. **書上問的「KPTI 生效」情境**（標準答案）：核心態帶**偶數 ASID**。`copy_*_user()` 存取使用者位址時仍用這個
   偶數(核心)ASID 去查 TLB，但使用者頁的 TLB 是奇數 ASID → **TLB miss → 走 MMU 翻頁 → 有效能損失**。
   根因：ARMv8.4 前**無法同時用兩個 ASID 查 TLB**（§6.2.2 行 266-267）。
2. **本機實況**：KPTI 未啟用，核心頁是全域 TLB（Q4 的 `nG=0`），核心態存取使用者頁用的是行程當前那個唯一 ASID，
   **不存在偶/奇不匹配的懲罰**——這也是「硬體免疫 → 不開 KPTI → 省掉這筆效能損失」的直接好處。

> **重點**：標準答案是「偶數(核心)ASID → 查使用者頁必然 miss → MMU 翻頁 → 一點效能損失」，
> 根因是 ARMv8.4 前不能同時用兩個 ASID。本機因為沒啟用 KPTI，反而沒有這筆開銷。書上出處 §6.2.2 行 256-267。

---

<a name="q6"></a>
## Q6：請簡述分支預測的工作原理

### 書上怎麼說（§6.3.1，行 538-606）
超標量流水線在取指階段就得決定「下一週期取哪」，遇到條件跳轉時用**分支預測單元**猜測（行 539-543）：
猜對→流水線滿載；**猜錯→丟棄所有工作、從正確分支重新取指、填流水線，招致嚴重懲罰**（行 542-543）。
兩件事要預測（行 545-558）：
- **方向**：跳/不跳。最簡單是用上次結果；後來用**分支歷史表 BHT**（PC 後 12 位當索引，4KB 表記錄跳轉歷史，
  行 559-565）；再進化成局部/全域歷史緩衝器（GHB，行 571-574）。
- **目標位址**：直接跳轉用 **BTB（分支目標緩衝器）**（行 575-585）；`call/return` 這種間接跳轉用
  **RSB（返回棧緩衝器，LIFO）**（行 587-606）。ARM Cortex-A 系列實作了 GHB/BTB/RSB（行 556-558）。

### 本機實測：量出「誤判懲罰」

[`branch_pred.c`](./experiments/branch_pred.c) 跑同一個 `if (data[i] >= 128)` 迴圈，比較**已排序**（分支結果
一長串 0 再一長串 1，BHT 幾乎全中）vs **未排序**（隨機，命中率≈50%，每次誤判都清流水線）。兩者**做的算術完全一樣**，
唯一差別就是分支可不可預測：

```
$ taskset -c 4 ./branch_pred     # A76 大核（週期級量測）
  未排序（分支隨機，常誤判）：每次迭代 10.386 週期
  已排序（分支規律，幾乎全中）：每次迭代 4.534 週期
  比值 = 2.29x  <-- 這個差距就是「分支誤判懲罰」

$ taskset -c 0 ./branch_pred     # A55 小核
  未排序：9.610 週期   已排序：5.538 週期   比值 = 1.74x
```

- **A76**（亂序、深流水線）誤判懲罰更大：**2.29×**；
- **A55**（順序、淺流水線）較小：**1.74×**。

這正好呼應「深流水線猜錯要丟棄更多工作」（行 542-543），也解釋了為什麼**幽靈漏洞主要打亂序大核**（A76）
而不是 A55。

> **重點**：分支預測 = 方向（BHT/GHB）+ 目標（BTB / RSB）兩件事；猜對省時間、**猜錯清流水線是重罰**。
> 本機把這個「重罰」量成了 A76 2.29×、A55 1.74× 的實數。書上出處 §6.3.1 行 538-606。

---

<a name="q7"></a>
## Q7：請簡述 CPU「幽靈」漏洞變體1的攻擊原理

### 書上怎麼說（§6.3.2，行 608-666）
幽靈變體1 = **繞過邊界檢查**。偽代碼（行 628-640）：

```c
if (x < arr1->length) {                     // 邊界檢查
    value  = arr1->data[x];                 // 被推測執行的越界讀取
    index2 = (value & 1) * 0x100;
    value2 = share_data[index2];            // 用祕密值汙染 cache
}
```

攻擊步驟（行 646-659）：用**合法的 x**反覆訓練分支預測器學會「條件成立」→ flush → 餵一個**越界的 x** →
預測器仍預測成立，於是**推測地**讀了越界資料（雖然架構上最終被丟棄）並用它汙染 `share_data` →
Flush+Reload 反推。最常見場景是瀏覽器 JavaScript（行 660-662）。

### 本機實測：A76 上完整偷出祕密字串

[`spectre_v1.c`](./experiments/spectre_v1.c) 把偽代碼原封搬過來（同一位址空間、`the_secret[]` 供 PoC 用）：

```c
unsigned array1_size = 16;
uint8_t  array1[160] = {1..16};
uint8_t  array2[256 * 4160];
static void victim(size_t x) { if (x < array1_size) sink = array2[array1[x] * 4160]; }
```

Kocher 手法：每輪先 5 次合法索引訓練、第 6 次餵 `malicious_x = &secret - &array1`（無分支選擇避免破壞預測），
`flush(&array1_size)` + 延遲讓邊界檢查真的 stall、拉長推測窗，最後 Flush+Reload 掃 `array2`：

```
$ taskset -c 4 ./spectre_v1      # A76 大核
[校準] 命中 53 / 未命中 487 週期 -> 門檻 270
[Spectre v1] 從邊界檢查後方推測洩漏 "The Magic Words are Squeamish Ossifrage."
 byte  0:   最佳猜測 0x54 'T' (1000 票)  次高 0x00 (0 票)
 byte  1:   最佳猜測 0x68 'h' (1000 票)  次高 0x00 (0 票)
 ...
還原結果："The Magic Words are Squeamish Ossifrage."
正確 40/40 個位元組
```

**40/40 位元組、每個位元組 1000/1000 票**——即使有 `if (x < array1_size)` 邊界檢查，推測執行仍越過它洩漏了
本不該讀到的資料。這與 Q3 的熔斷 PoC（同機、同一套 Flush+Reload、卻一個位元組都偷不到）形成鮮明對照：
**幽靈變體1 用的是「分支預測」這條路，硬體的 CSV3（熔斷免疫）擋不住它**。

本機對 v1 的緩解是**核心層面的 `__user pointer sanitization`**（`/sys` 顯示 `spectre_v1: Mitigation:
__user pointer sanitization`，即 Q8/Q9 的 `array_index_nospec`），保護的是「核心裡的邊界檢查」；
使用者態自己寫的 gadget（本 PoC）不受保護，所以照樣中招。

另外 v2/BHB 的緩解在本機**只掛在 A76 上**（Q4 的 VBAR 證據）：A76 用 `__bp_harden_el1_vectors`
（含 BHB 清除迴圈），in-order 的 A55 不受 BHB 影響仍用原始 `vectors`。

> **重點**：幽靈 v1 = 訓練分支預測器 → 越界推測讀取 → cache 側信道洩漏。本機 A76 **實測可行（40/40）**。
> 書上出處 §6.3.2 行 608-666。

---

<a name="q8"></a>
## Q8：請簡述 ARM64 架構中新增的 CSDB 指令的作用

### 書上怎麼說（§6.3.3，行 671-696）
CSDB（Consume Speculative Data Barrier，消費預測資料屏障）的意思是：**在 CSDB 之後，任何指令都不會使用
「預測的值」來執行**（行 672-674）。通常和 CSEL 搭配繞過邊界檢查（行 674-696）：

```asm
cmp   untrusted_value, limit
b.hs  label
csel  tmp, untrusted_value, wzr, lo   ; 越界時 tmp=0
csdb                                   ; 保證 csel 沒用到「預測執行的結果」
ldrb  val, [array, tmp]
```

### 本機實測：在真核心裡抓到 CSDB

`array_index_mask_nospec()` 的組語版（§6.3.3 行 770-786）是 `cmp / sbc / csdb`。用 [`sec_probe.ko`](./experiments/sec_probe.c)
反組譯**正在跑的核心**的 `invoke_syscall`（系統呼叫分派，用 `array_index_nospec` 保護 syscall 表索引）：

```
sec_probe: 掃描 invoke_syscall @ 0xffff800008024f7c，csdb 機器碼 = 0xd503229f
sec_probe:   +0x60: 2a1303e0   ; mov  w0, w19        (scno)
sec_probe:   +0x64: eb15001f   ; cmp  x0, x21        (scno - sc_nr，設定 PSTATE.C)
sec_probe:   +0x68: da1f03e0   ; sbc/ngc x0,xzr,xzr  (mask = 0-0-!C)
sec_probe:   +0x6c: d503229f   ; CSDB  <-- 消費預測資料屏障
```

`csdb` 的機器碼固定是 **`0xd503229f`**。在使用者態程式 [`nospec_mask.c`](./experiments/nospec_mask.c) 也看得到同一條：

```
$ objdump -d ./nospec_mask | grep -E "cmp|ngc|csdb"
 740: f100403f   cmp x1, #0x10
 744: da1f03e3   ngc x3, xzr        ; = sbc x3, xzr, xzr
 748: d503229f   csdb
```

（`ngc` 是 `sbc Xd, xzr, xzr` 的別名，反組譯器習慣印成 `ngc`。）

> **重點**：CSDB 是一道**只擋「預測資料被消費」的專用屏障**——它保證前面 `csel/sbc` 算出來的遮罩，不會被後面
> 用「推測值」搶跑。它比 `dsb/isb` 輕量得多（只針對推測資料相依），這樣把 `array_index_nospec` 的
> 開銷壓到最低。書上出處 §6.3.3 行 671-696。

---

<a name="q9"></a>
## Q9：array_index_nospec() 如何規避幽靈漏洞？

### 書上怎麼說（§6.3.3，行 697-797）
`array_index_nospec(index, size)` 用一個**遮罩**把索引夾在 `[0, size)`（行 701-723）：

```c
static inline unsigned long array_index_mask_nospec(unsigned long index, unsigned long size) {
    return ~(long)(index | (size - 1UL - index)) >> (BITS_PER_LONG - 1);
}
#define array_index_nospec(index, size) ((index) & array_index_mask_nospec(index, size))
```

`index < size` 時遮罩 = 全 1（`0xFFFF...FFFF`）→ 放行原值；`index >= size` 時遮罩 = 0 → 索引被夾成 0
（行 721-723）。組語版（行 770-786）用 `cmp/sbc/csdb`：`cmp` 設定進位 C，`sbc mask, xzr, xzr` 得到
`index<size ? -1 : 0`，再 `csdb`（Q8）確保遮罩先於後續使用。

例子（行 736-750）：核心用它保護 **syscall 表**的索引 `syscall_table[array_index_nospec(scno, sc_nr)]`，
即使分支預測誤判 `if (scno < sc_nr)` 成立而推測執行，`scno & mask` 也已被**架構化地**限制住，
推測執行讀到的位址不會越界。

### 本機實測

[`nospec_mask.c`](./experiments/nospec_mask.c) 同時跑「純 C 版」與「和核心 `barrier.h` 一模一樣的組語版」，
`size=16`：

```
$ ./nospec_mask
index                  mask_c               mask_asm
0                      0xffffffffffffffff   0xffffffffffffffff  -> nospec(x)=0
1                      0xffffffffffffffff   0xffffffffffffffff  -> nospec(x)=1
15                     0xffffffffffffffff   0xffffffffffffffff  -> nospec(x)=15
16                     0x0000000000000000   0x0000000000000000  -> nospec(x)=0
17                     0x0000000000000000   0x0000000000000000  -> nospec(x)=0
100                    0x0000000000000000   0x0000000000000000  -> nospec(x)=0
18446744073709551615   0x0000000000000000   0x0000000000000000  -> nospec(x)=0
```

- `index < 16` → 遮罩 `0xffff...ffff` → 放行原值；
- `index >= 16`（含 `~0UL`）→ 遮罩 `0` → **夾成 0**。

而且這不是紙上談兵——Q8 已經證明**正在跑的核心** `invoke_syscall` 裡就是用這套 `cmp/sbc/csdb` 保護
syscall 表索引。`/sys` 的 `spectre_v1: Mitigation: __user pointer sanitization` 指的正是這類保護。

> **重點**：`array_index_nospec` 的精髓是**把「安全的邊界」變成一個資料相依的遮罩**，再用 `csdb` 擋住推測
> 消費——這樣就算分支預測誤判、推測執行了 `array[index]`，`index` 也早被夾進 `[0,size)`，
> **推測路徑上就讀不到越界位址**，側信道自然無從洩漏。書上出處 §6.3.3 行 697-797。

---

<a name="appendix"></a>
## 附錄：兩個讓側信道實驗「量得準」的關鍵

側信道實驗成敗全在**時鐘夠不夠細**與**探針陣列會不會自我干擾**。本章踩了兩個坑，記錄如下。

### A. 週期級時鐘：`perf_user_access=1` + `config1=0x2`

ROCK 5B 的 EL0 只能讀 24 MHz 的 `CNTVCT_EL0`（一 tick≈41.6 ns），而 L1 命中(52 週期≈22 ns) vs
DRAM(~400 週期≈170 ns) 的差只有幾個 tick，量不準。要用 PMU 週期計數器 `PMCCNTR_EL0`，但它預設只有 EL1 能讀。

兩條路（[`sidechannel.h`](./experiments/sidechannel.h) 會自動挑）：

1. **perf 自我監控（推薦）**：`perf_event_open(CPU_CYCLES, pid=0)` + `mmap`，讀 mmap 頁的 `index` 後直接
   `mrs pmccntr_el0`。arm64 授予 EL0 直讀的**三個必要條件**（`arch/arm64/kernel/perf_event.c`）：
   - `sudo sysctl kernel.perf_user_access=1`；
   - 事件 `attr.config1 |= 0x2`（`armv8pmu_event_want_user_access()` 檢查的 rdpmc 格式位，**最容易漏**）；
   - `attr.exclude_kernel=1`（只算使用者態）。三者齊了 `cap_user_rdpmc=1`、`index=32`（專屬週期計數器）。
2. **`pmu_user.ko`（備援）**：`on_each_cpu` 寫 `PMUSERENR_EL0` 打開 EL0 讀取。缺點是**深度 idle 電源崩塌後
   權限會被清掉**（PMU 狀態遺失），所以本文優先走 perf 那條（驅動會在情境切換/idle 後幫忙維持）。

### B. 探針步長：4096 會撞進同一個 cache set

書上偽代碼用「以 cache line 為步長」，但如果探針陣列用 **4096（page）步長**，在 A76 上
`(slot*4096) & 0xfc0`（L1d set index = 位址 [11:6]）**永遠是 0**——256 個候選值全部撞進 **cache set 0**，
而 A76 L1d 只有 **4-way**，掃描時彼此驅逐，側信道嚴重失真（實測隱蔽通道從 9/9 掉到 0/9）。

修法：步長改 **4160（=0x1040）**，`(slot*4160)>>6 mod 256 = slot*65 mod 256` 是一個**排列**，
讓每個候選值落在**不同的 L1 cache set**，同時仍錯開 page 以躲開硬體預取器。改完隱蔽通道立刻回到 **9/9**、
Spectre PoC 回到 **40/40**。這一條書上沒提，是把側信道從「玩具」變成「穩定可還原」的關鍵工程細節。

### 清理

```bash
ssh radxa@192.168.68.58 '
  sudo rmmod sec_probe pmu_user 2>/dev/null
  sudo sysctl kernel.perf_user_access=0'      # 收回 EL0 直讀權限
```
