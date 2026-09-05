# 任務簡報：RK3588 NPU 入門教材

> 本檔是**任務定義**，不是教材本身。
> 教材等實體板子（Radxa Rock 5B）到手後再開始寫。
> 產出日期：2026-09-05

---

## 一句話

寫一份教材，讓一個沒寫過 Linux driver 的人，看完能回答：
**「我呼叫 `rknn_run()`，到 RK3588 的 NPU 硬體真的開始算，中間發生了什麼事？」**

---

## 欄位 1／5：目的（Purpose）

這份教材要讓一個**沒寫過 Linux driver** 的人，看完之後能自己打開 `drivers/rknpu/` 任一個檔，
讀懂每一段在幹嘛，並且知道它對應到 RK3588 TRM 第 36 章（RKNN）的哪個暫存器或哪個機制。

**涵蓋範圍**：從 RKNN SDK 的公開 API，一路到 RK3588 NPU 硬體暫存器。

**閉源的處理方式**：`librknnrt.so` 內部看不到。
用「從 ioctl 介面反推」的方式講 —— 說清楚它**必須**做什麼，不假裝知道它**怎麼**做。

**不是什麼**：不是「怎麼用 NPU 跑 AI 模型」的使用手冊（那是 RKNN SDK 文件的事）。

### 為什麼這個目的成立 —— 關鍵發現

`drivers/rknpu/include/rknpu_ioctl.h:216` 的 `struct rknpu_task`：

```c
struct rknpu_task {
	__u32 flags;
	__u32 op_idx;          // 第幾個運算子
	__u32 enable_mask;
	__u32 int_mask;        // 中斷遮罩
	__u32 int_clear;
	__u32 int_status;
	__u32 regcfg_amount;   // 要寫幾個暫存器
	__u32 regcfg_offset;
	__u64 regcmd_addr;     // ← 指向一串預編好的「暫存器寫入指令」
} __packed;
```

**SDK 把 AI 模型編譯成一長串「往 NPU 哪個暫存器寫什麼值」的清單。**
driver 幾乎不懂模型，它只負責把清單位址交給硬體，然後說「跑」。

而那些暫存器是什麼意思 —— **TRM 第 36 章就是在講這個**。
所以雖然 `.so` 閉源，這條路完全走得通。

---

## 欄位 2／5：現場狀況（Situation）

### 硬體
- Radxa Rock 5B，RK3588
- NPU **三核**，基底位址 `0xfdab0000` / `0xfdac0000` / `0xfdad0000`（各 0x10000）
- 中斷 `GIC_SPI 110/111/112`，名稱 `npu0_irq` / `npu1_irq` / `npu2_irq`
- 三個獨立電源域：`RK3588_PD_NPUTOP` / `RK3588_PD_NPU1` / `RK3588_PD_NPU2`
- 掛在自己的 IOMMU (`rknpu_mmu`, `iommu@fdab9000`, `compatible = "rockchip,iommu-v2"`) 底下
- 定義處：`arch/arm64/boot/dts/rockchip/rk3588s.dtsi:3450`（node）、`:3482`（OPP 表）、`:3712`（IOMMU）
- dtsi 裡 `status = "disabled"`，要板級 dtsi 打開

### TRM 記載的硬體規格（part1 ch36.1）
- 三顆 NPU CORE，可三核協作 / 雙核協作 / 各自獨立
- 每核內含 CNA（捲積）、DPU（Data Process Unit）、PPU（Planar Process Unit）
- 每核 **384KB** 內部緩衝
- AHB 介面只做設定，AXI 介面搬資料
- 支援 int4 / int8 / int16 / fp16 / bf16 / tf32
- int8 每 cycle 1024×3 MAC

### 軟體
- Rockchip **BSP** kernel 6.1，分支 `linux-6.1-stan-rkr4.1-buildroot`
- 跑在 Yocto 建置樹：`/home/awe/disk/yocto-rockchip-sdk/build/tmp/work-shared/rockchip-rk3588-rock-5b/kernel-source`
- `CONFIG_ROCKCHIP_RKNPU=y`（`rk3588_linux_defconfig:1208`、`rockchip_linux_defconfig:1208`）
- ⚠️ **上游 mainline kernel 完全沒有這個 driver**。網路上找 mainline 文件是找不到的。

### 一個一定要早點講清楚的怪事
`rknpu` 註冊成一個 **DRM driver**（繪圖子系統），但它**完全不畫圖**。
它只是借用 DRM 的記憶體管理（GEM）和同步機制（dma-fence）。
教材必須早點解釋這件事，否則讀者會一直卡在「為什麼 AI 加速器在圖形子系統裡」。

### 讀者
沒寫過 Linux driver。platform driver、device tree、DRM GEM、DMA-BUF、dma-fence、
IOMMU、devfreq —— 這些全部要從零講起。

### 板子狀態
**目前手上沒有 Rock 5B，之後會準備。**
→ 教材開工時機：等板子到手。
→ 但教材裡的「實機驗證」段落照樣要寫（指令、步驟寫好），輸出處標 `⏳ 待實機`。

---

## 欄位 3／5：素材（Inputs）

### 主要素材（一定要用，逐行對照）

| 素材 | 位置 | 規模 |
|---|---|---|
| NPU driver 原始碼 | `drivers/rknpu/` | 8478 行 |
| TRM RKNN 章 | `books/rk3588_trm/part1/chapter_36.txt` | 9172 行 |
| **交界面定義（最重要）** | `drivers/rknpu/include/rknpu_ioctl.h` | 330 行 |

driver 檔案清單（教材的走訪順序大致照這個）：

| 檔 | 行數 | 職責 |
|---|---|---|
| `rknpu_gem.c` | 1757 | 記憶體：配置、對映、給硬體看 |
| `rknpu_drv.c` | 1664 | 大門：probe、ioctl 分派、中斷處理 |
| `rknpu_job.c` | 1056 | 工作：排隊、送進硬體、等結果 |
| `rknpu_devfreq.c` | 808 | 動態調頻調壓 |
| `rknpu_iommu.c` | 619 | 位址翻譯 |
| `rknpu_debugger.c` | 605 | debugfs / procfs |
| `rknpu_mem.c` | 353 | 另一套記憶體介面（非 DRM 路徑） |
| `rknpu_mm.c` | 238 | SRAM 小配置器 |
| `rknpu_reset.c` | 158 | 掛掉時重置 |
| `rknpu_fence.c` | 80 | 同步旗標（dma-fence） |

ioctl 命令共 6 個：
`RKNPU_ACTION` / `RKNPU_SUBMIT` / `RKNPU_MEM_CREATE` /
`RKNPU_MEM_MAP` / `RKNPU_MEM_DESTROY` / `RKNPU_MEM_SYNC`

### 次要素材（需要時才查）

- `arch/arm64/boot/dts/rockchip/rk3588s.dtsi` — NPU 節點、IOMMU 節點、OPP 表
- TRM 其他章：
  - part1 ch02 CRU（時脈）
  - part1 ch07 PMU（電源域）
  - part1 ch08 MMU600
  - part1 ch11 GIC600（中斷）
- RKNN SDK：`/home/awe/disk/rknn-toolkit2/`
  - `rknpu2/runtime/Linux/librknn_api/include/rknn_api.h`（804 行，公開 API）
  - `rknpu2/runtime/Linux/librknn_api/include/rknn_matmul_api.h`
  - `rknpu2/runtime/Linux/librknn_api/include/rknn_custom_op.h`
  - `rknpu2/examples/` — C++ 範例，有原始碼
  - ⚠️ `librknnrt.so` 是閉源二進位，只能從介面反推
- 核心通用子系統原始碼（講基礎概念時引用）：
  - `drivers/gpu/drm/drm_gem.c`
  - `drivers/iommu/`
  - `drivers/devfreq/`

### 不准用
- 網路上的部落格、二手整理、模型記憶中的「我記得 Rockchip 是這樣」
- 對 `librknnrt.so` 內部實作的任何猜測
- `/home/awe/disk/linux-rockchip`（跟工作目錄是同一份 kernel，不用對照）

### ★ 最重要的一條規矩

> 每一個「這段程式碼對應 TRM 哪裡」的說法，**都必須真的在 `chapter_36.txt` 裡找到**。
> 找不到就明講「**TRM 沒寫，這是驅動自己的邏輯**」。
> **絕對不准掰一個聽起來合理的暫存器名字出來。**

理由：這正是最容易出錯、而且讀者最不容易發現的地方。
模型的預設行為是「回答」而不是「承認找不到」，必須明確覆蓋掉這個預設。

---

## 欄位 4／5：界線（Limits）

- **不改任何程式碼。** 純文件工作，一行 kernel code 都不動。
- **不教深度學習理論。** 不解釋捲積、池化、量化的數學。只講「硬體為了做這件事需要哪些零件」——
  剛好夠看懂 CNA / DPU / PPU 在幹嘛的量，不多。
- **不重寫 TRM。** 不是把 9172 行翻譯成中文。只挑「程式碼真的碰到的」講，其他跳過。
- **不猜閉源的部分。** 只能說「從 ioctl 看得出它必須做 X」，不能說「它是這樣做的」。
- **不寫效能調校指南。** 怎麼讓模型跑更快、三核怎麼分配 —— 那是另一份文件。
- **只講 RK3588。** RK3562/3566/3568 的 NPU 是不同世代，程式碼裡的 `if (rk356x)` 分支
  一律標註「這段不是給我們的晶片」然後跳過。
- **不假設有板子。** 動手做的部分標「等有板子再做」，不當成理解的前提。

---

## 欄位 5／5：完成條件（Done-when）

### 放哪
```
notes/rknpu/
├── BRIEF.md          ← 本檔
├── ch01_*.md
├── ch02_*.md
├── ...
└── experiments/      ← 實驗用 .c / .sh
```
不塞進現有的 `notes/ch01~ch15`（那是另一本書的筆記）。

### 長什麼樣
跟 `notes/ch11_interrupt_management.md` 同一個模子：

- 繁體中文
- 每章開頭：**目錄** + **一鍵重現全部實驗**（可貼上執行的 shell 區塊）
- 每一節先 `### 結論` 給答案，再 `### 實機驗證（N）：...` 用真機證明
- 明確標註「TRM 說是 X，程式碼做的是 Y」這類差異
- 實驗程式碼放 `notes/rknpu/experiments/`
- 沒板子時：`### 實機驗證` 段落照寫指令和步驟，輸出處標 `⏳ 待實機`

### 規模
大約 8～10 章。

### ★ 第一個檢查點（開工後的第一件事）

> **先只交大綱，不寫內文。**
>
> 大綱要包含：
> - 每章標題
> - 每章 2～3 句說明在講什麼
> - 每章要對照哪幾個 `.c` 檔
> - 每章要對照 TRM 第 36 章的哪幾小節
>
> 使用者點頭之後，才開始寫內文。

理由：方向錯了，在大綱階段改一句話就好；寫完 8 章才發現就要整個重來。

### 後續節奏
大綱通過後，**一次寫一章**，寫完給使用者看，確認後再寫下一章。

### 可選的加值（先不做）
NPU 三核架構圖、資料流程圖 → 之後可另外做成互動式 artifact 網頁。
等文字內容穩定後再說。

---

## 附錄 A：實驗規劃（Experiments）

教材沿用 `notes/` 既有風格：每節先 `### 結論`，再 `### 實機驗證（N）` 用真機證明。
本附錄先把「有哪些觀察點」和「要做哪些實驗」定下來，板子到手直接照做。

### A.1 driver 提供的觀察點（已從原始碼確認）

**debugfs / procfs** — `drivers/rknpu/rknpu_debugger.c:304`
根目錄 `/sys/kernel/debug/rknpu/`（另有 procfs 鏡像）：

| 節點 | 讀 | 寫 | 內容 |
|---|---|---|---|
| `version` | ✓ | | 驅動版本 |
| `load` | ✓ | | **三核各自的負載百分比**（`rknpu_load_show`，`Core%d: NN%`） |
| `power` | ✓ | ✓ | 電源域開關 |
| `freq` | ✓ | ✓ | 目前頻率 |
| `volt` | ✓ | | 目前電壓 |
| `delayms` | ✓ | ✓ | `power_put_delay`，閒置多久自動關電 |
| `reset` | ✓ | ✓ | 手動觸發重置 |
| `mm` | ✓ | | SRAM 配置狀況（需 `CONFIG_ROCKCHIP_RKNPU_SRAM`） |

**模組參數** — `drivers/rknpu/rknpu_drv.c:66`，權限 `0644`，可執行時修改：

- `bypass_irq_handler` — 關掉中斷處理
- `bypass_soft_reset` — 關掉軟重置

> 這兩個參數是實驗的金礦。「把它關掉會怎樣」是最有力的證明方式。

**核心通用觀察面**：`/proc/interrupts`、ftrace / kprobe、
`/sys/class/devfreq/fdab0000.npu/`、`/proc/device-tree/npu@fdab0000/`、`devmem`。

### A.2 實驗清單

| 組 | # | 實驗 | 證明什麼 | 對應章節 |
|---|---|---|---|---|
| 1 靜態結構 | 1.1 | `devmem` 直接讀 NPU 暫存器，對照 TRM 第 36 章 offset | TRM 描述的暫存器真的在那個位址上 | 硬體概觀 |
| | 1.2 | `/proc/interrupts` 找 `npu0_irq`/`npu1_irq`/`npu2_irq` | 三核三中斷，對得上 dtsi | 硬體概觀 |
| | 1.3 | `/proc/device-tree/npu@fdab0000/` 看實際生效的 reg / clocks / power-domains | device tree 到 driver 的交接 | probe 流程 |
| | 1.4 | `/sys/class/devfreq/fdab0000.npu/available_frequencies` 對照 dtsi OPP 表 | OPP 表怎麼進到 devfreq | 電源與頻率 |
| 2 三核分工 | 2.1 | 跑推論同時 `watch cat .../rknpu/load` | 三核負載分配 | 工作提交 |
| | 2.2 | SDK 指定單核 vs 三核，比較 `load` 與 `/proc/interrupts` 增量 | `core_mask` 是在 driver 決定分工的 | 工作提交 |
| 3 中斷鏈路 | 3.1 | ftrace 抓中斷處理完整呼叫鏈 | 從硬體中斷到喚醒等待者的路徑 | 中斷與完成 |
| | 3.2 | `/proc/interrupts` 前後差值 vs 送出的 job 數 | 一個 job 對應幾次中斷 | 中斷與完成 |
| | 3.3 | **`bypass_irq_handler=1` 後跑推論** | 會 timeout → 證明中斷就是「算完了」的唯一通知 | 中斷與完成 |
| 4 記憶體路徑 | 4.1 | kprobe / ftrace 追 `MEM_CREATE` → GEM 物件 → IOMMU 對映 | 使用者的緩衝區怎麼變成硬體看得懂的位址 | 記憶體 |
| | 4.2 | `MEM_SYNC` 前後的快取行為觀察 | cache coherency 為什麼需要顯式同步 | 記憶體 |
| | 4.3 | `rknpu/mm` 看 SRAM 配置 | SRAM 小配置器在幹嘛（`rknpu_mm.c`） | 記憶體 |
| 5 regcmd 本體 | 5.1 | **把 submit 下來的 `regcmd` buffer dump 出來，逐筆對照 TRM 第 36 章暫存器表** | 親眼看到模型被編成暫存器寫入序列 | ★ 核心章 |
| | 5.2 | 同一個模型換不同輸入尺寸，比較 regcmd 差異 | 哪些欄位是模型決定的、哪些是輸入決定的 | ★ 核心章 |
| 6 電源與調頻 | 6.1 | `echo` 改 `freq`，量 `volt` 跟著動 | devfreq / OPP 電壓頻率連動 | 電源與頻率 |
| | 6.2 | `delayms` 改小，量 `power` 自動掉下來的時間 | 閒置自動關電機制 | 電源與頻率 |
| | 6.3 | 跑推論時觀察 devfreq 升頻 | 負載驅動的動態調頻 | 電源與頻率 |
| 7 錯誤路徑 | 7.1 | `bypass_soft_reset=1` 觸發 timeout | 有無軟重置的復原行為差異 | 重置與錯誤 |
| | 7.2 | `echo 1 > rknpu/reset` 手動重置後確認仍可用 | 重置流程做了哪些事（`rknpu_reset.c`） | 重置與錯誤 |

### A.3 實驗 5.1 是重點

如果 dump 出來的 `regcmd` 內容真的能跟 TRM 第 36 章的暫存器 offset 表逐筆對上，
整份教材的論點就從「我說它是這樣」變成「你自己看」。

這也是最可能卡住的一項（可能需要加 kprobe 或臨時修改 debugger 來取得 buffer 內容）。
**開工時優先驗證這一項可不可行**，因為它決定核心章能寫到多深。

### A.4 實驗程式碼的擺放

沿用既有慣例：`notes/rknpu/experiments/`，內含 `.c`、`.sh`，
每章開頭提供一段「一鍵重現全部實驗」的可貼上 shell 區塊。

沒板子期間：實驗的指令與步驟照寫，輸出處標 `⏳ 待實機`。

---

## 開工前的檢查清單

- [ ] 板子（Radxa Rock 5B）到手
- [ ] 板子能開機、能跑 RKNN SDK 範例
- [ ] 工作目錄含 `/home/awe/disk/rknn-toolkit2`（本次已加）
- [ ] 第一步：產出大綱，等使用者確認
- [ ] 開工後優先驗證實驗 5.1（regcmd dump）可不可行
