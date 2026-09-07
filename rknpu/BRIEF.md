# 任務簡報：RK3588 NPU 入門教材

> 本檔是**任務定義**，不是教材本身。
> 產出日期：2026-09-05
> **更新：2026-09-07 —— 板子到手，開工前的驗證全部跑完。大綱見 `OUTLINE.md`。**

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

> ✅ **2026-09-07 實機驗證通過。** 用 `tools/rkspy.c` 攔下 `SUBMIT`，把 `regcmd_addr` 指到的
> 內容整包挖出來，解出格式後逐筆對回 TRM ——
> `0x1040`→`RKNN_cna_cbuf_con0`、`0x1020`→`RKNN_cna_data_size0`、`0x4004`→`RKNN_dpu_s_pointer`…
> **一個都沒漏。**
>
> regcmd 每筆 8 bytes（TRM 未記載，實測解出）：
> `[15:0]`=暫存器 offset、`[47:16]`=32-bit 值、`[63:48]`=區塊標籤
> （`0x0201`→CNA `0x1xxx`、`0x1001`→DPU `0x4xxx`、`0x2001`→DPU_RDMA `0x5xxx`，
> 與 TRM **36.4.1** 的位址表完全吻合）。
>
> 而且 TRM **36.3.5 Register File Fetch Unit** 自己就寫了這句：
> *"fetch register configuration from external system memory through AXI interface"*。
> 論點不是我們推的，是 Rockchip 自己講的。

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

### 板子狀態（2026-09-07 更新）
**板子已到手，隨時可用。** `ssh radxa@192.168.68.58`（金鑰已設，免密碼；sudo 密碼 `radxa`）。

| 項目 | 實機值 |
|---|---|
| kernel | `6.1.115+` |
| rknpu driver | **v0.9.8**，與本樹 `rknpu_drv.h:33-35` **相同** |
| 走哪道門 | **DRM GEM** — `/dev/dri/renderD129`（driver=RKNPU），`/dev/rknpu` 不存在 |
| RKNN runtime | `librknnrt.so.2.0.0b0` |
| SDK / 範例 / 模型 | 板子上都有，含編好的 `rknn_create_mem_demo` + RK3588 mobilenet_v1 |

實測跑通：mobilenet_v1 推論 **2.94 ms / 339 FPS**，狗辨識正確（class 156）。
閒置 `power=off` → 送 job `power=on` → 停 3 秒又 `off`（`delayms=3000` 生效）。
預設**只有 Core0 在動**（`core_mask=0x0`，由 driver 決定分工）。

**已開/未開的 config**（決定哪些實驗現在做得了）：

```
CONFIG_ROCKCHIP_RKNPU=y
CONFIG_ROCKCHIP_RKNPU_DRM_GEM=y        ← Kconfig 的 default 生效，猜對了
CONFIG_ROCKCHIP_RKNPU_DEBUG_FS=y
CONFIG_ROCKCHIP_IOMMU=y
# CONFIG_ROCKCHIP_RKNPU_SRAM    is not set   → 少了 debugfs 的 mm 節點
# CONFIG_ROCKCHIP_RKNPU_FENCE   is not set   → dma-fence 實驗做不了，實測 fence_fd=-1
# CONFIG_ROCKCHIP_RKNPU_PROC_FS is not set
```

可用的觀察工具：`strace`、`ftrace`、`CONFIG_KPROBES`、`CONFIG_DYNAMIC_FTRACE` 全部都有。

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
- ~~**不假設有板子。**~~ 2026-09-07 起板子已到手，實驗直接做。
  但**沒板子也要看得懂**這條仍然成立：實機輸出是佐證，不是理解的前提。

---

## 欄位 5／5：完成條件（Done-when）

### 放哪
```
notes/rknpu/
├── BRIEF.md          ← 本檔
├── OUTLINE.md        ← 大綱（2026-09-07 產出，待確認）
├── ch00_*.md … ch09_*.md
├── tools/            ← 跨章共用的工具
│   └── rkspy.c       ← LD_PRELOAD ioctl 攔截器
└── experiments/      ← 各章專屬的 .c / .sh
```

> `tools/` 是 2026-09-07 增補的。原本只規劃 `experiments/`，
> 但 `rkspy.c` 被第 4～7 章共用，不屬於任何單一實驗。
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
| `reset` | ✓ | ✓ | ⚠️ **不只是觸發重置**：寫 `1` 觸發、寫 `on`/`off` 切換 `bypass_soft_reset`；讀出來的是「功能開著嗎」。詳見 [ch09](./ch09_reset.md) §5 |
| ~~`mm`~~ | | | SRAM 配置狀況 —— **實機沒有**，`CONFIG_ROCKCHIP_RKNPU_SRAM` 沒開 |

> ⚠️ 2026-09-07 實測補充：這些節點 `ls -l` 顯示 `-r--r--r--`（`0444`），**看起來像唯讀**，
> 但 root 照樣寫得進去（`CAP_DAC_OVERRIDE` 蓋過權限位元）。已驗證 `delayms` 可寫。
> 這個反直覺點本身值得寫進教材。

實機讀出來長這樣：

```
version   RKNPU driver: v0.9.8
load      NPU load:  Core0:  0%, Core1:  0%, Core2:  0%,
power     off
freq      1000000000
volt      825000
delayms   3000
```

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

### A.3 實驗 5.1 是重點 —— ✅ 2026-09-07 已驗證通過

原本擔心這項會卡住（本來想用 kprobe 或改 debugger）。實際做法更乾淨：
**`LD_PRELOAD` 攔在 `librknnrt.so` 跟核心中間**，完全不動 kernel。

作法（原始碼：`tools/rkspy.c`，127 行）：

| 攔什麼 | 記下什麼 |
|---|---|
| `MEM_CREATE` 回傳 | `handle` / `obj_addr` / `dma_addr` / `size` |
| `MEM_MAP` 回傳 | 假 offset |
| `mmap` **和 `mmap64`** | 用 offset 綁到使用者位址 |
| `SUBMIT` 進去前 | 用 `task_obj_addr` 找 task 陣列；用 `regcmd_addr` 落在哪塊 buffer 的 dma 區間找 regcmd |

> 🕳️ 踩到的坑：aarch64 的 glibc 走 **`mmap64`**，只攔 `mmap` 會漏掉全部對映。兩個都要攔。

挖出來的結果（mobilenet_v1，一次推論配 5 塊記憶體、送 2 次 SUBMIT、共 120 個 task）：

```
--- task[0] op_idx=1 regcfg_amount=126 regcmd_addr=0xffc35bc0
    raw=40 10 1b 00 00 00 01 02  off=0x1040 val=0x0000001b tag=0x0201
    raw=0c 10 00 a0 00 60 01 02  off=0x100c val=0x6000a000 tag=0x0201
    raw=04 40 0e 00 00 00 01 10  off=0x4004 val=0x0000000e tag=0x1001
```

拿 TRM **36.4.3** 的位元定義去讀 `0x1020`（`datain_width` 在 `[26:16]`、
`datain_height` 在 `[10:0]`），整個 MobileNet 的形狀就浮出來了：

| op_idx | 寬 | 高（分批） |
|---|---|---|
| 1 | **224** | 99 + 99 + 28 |
| 2 | **112** | 100 + 14 |
| 3 | 112 | 100 + 12 |
| 31 | **1** | **1** |

224 → 112（第一層 stride=2 砍半）→ … → 1×1（分類層）。**網路形狀直接寫在暫存器裡。**

高度被切開是因為每核內部緩衝只有 384KB，整張圖塞不下，得切橫條分批算。
`99+99+28 = 226`，比 224 多 2 列 —— **推測**是捲積接縫重疊，用實驗 5.2 證明，不直接斷言。

**結論：核心章寫得下去，而且比原本預期的深。**

### A.4 實驗程式碼的擺放

沿用既有慣例：`notes/rknpu/experiments/`，內含 `.c`、`.sh`，
每章開頭提供一段「一鍵重現全部實驗」的可貼上 shell 區塊。

沒板子期間：實驗的指令與步驟照寫，輸出處標 `⏳ 待實機`。

---

## 開工前的檢查清單

- [x] 板子（Radxa Rock 5B）到手 —— 2026-09-07
- [x] 板子能開機、能跑 RKNN SDK 範例 —— mobilenet_v1 2.94ms / 339 FPS
- [x] 工作目錄含 `/home/awe/disk/rknn-toolkit2`
- [x] 優先驗證實驗 5.1（regcmd dump）可不可行 —— **通過**，見 A.3
- [x] 產出大綱 —— `OUTLINE.md`，10 章
- [x] 使用者確認大綱 —— 2026-09-07
- [x] 寫完 ch00 ~ ch09 —— **2026-09-08 全書完成**

---

## 完成紀錄（2026-09-08）

**十章全部寫完。** 索引見 [`README.md`](./README.md)。

| 項目 | 結果 |
|---|---|
| 章數 | 10（ch00 ~ ch09），符合「8～10 章」 |
| 實驗 | **34 個**全部在實機跑過，另有 3 個標 ⏳（需重編 kernel 或未測） |
| 工具程式 | 7 個（`tools/rkspy.c` + `experiments/` 6 個） |
| TRM／程式碼落差 | **19 筆**，其中 6 筆結論是「待查」 |
| 本簡報自己被實機推翻的說法 | 3 處（`mm` 節點、`0444` 唯讀、`reset` 語意） |

### ★ 規矩有沒有守住

> 「每一個『這段程式碼對應 TRM 哪裡』的說法，都必須真的在 `chapter_36.txt` 裡找到。
> 找不到就明講『TRM 沒寫』。絕對不准掰一個聽起來合理的暫存器名字出來。」

守住了。而且過程中**自己抓到並修正了兩次違規**：

1. ch01 初稿把 `CNA` 展開成 "Convolution Neural network Accelerator" ——
   查證後發現 **TRM 從未展開這個縮寫**，改成明講不知道
2. ch05 初稿推測「分條多出的 2 列 = 卷積接縫重疊」——
   換 YOLOv5s 一驗就不成立，改標待查。
   **後續把卷積參數暫存器也解出來，證實方向對但公式不完整**：
   正解是 `加總 = 特徵圖高 + (片數−1)×(k_h − s_y)`，兩模型 37 層全中

**兩次都是「聽起來很合理」的說法。** 這正是當初設這條規矩要防的東西。
