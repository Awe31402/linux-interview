# RK3588 NPU 入門教材 — 大綱

> 對應 `BRIEF.md` 的「★ 第一個檢查點」：先只交大綱，不寫內文。
> 產出日期：2026-09-07（板子到手當天）
> 狀態：**待使用者確認**

---

## 主線

全書follow 一次推論，從頭到尾：

```
rknn_run()
  → librknnrt.so 把預編好的暫存器指令準備好
  → ioctl(RKNPU_SUBMIT)
  → driver 寫 8 個暫存器
  → NPU 的 Register File Fetch Unit 自己去 DRAM 抓指令
  → CNA / DPU / PPU 照著算
  → 中斷
  → 回到 rknn_run() 返回
```

每一章都是這條線上的一段。不繞路。

---

## 開工前已驗證的事實（2026-09-07 實機）

| 項目 | 結果 |
|---|---|
| 板子 driver 版本 | v0.9.8，與本樹 `rknpu_drv.h:33-35` 相同 |
| 走哪道門 | DRM GEM（`/dev/dri/renderD129`，`CONFIG_ROCKCHIP_RKNPU_DRM_GEM=y`） |
| 實驗 5.1（regcmd dump） | **可行**，工具見 `tools/rkspy.c` |
| regcmd 格式 | 每筆 8 bytes：`[15:0]`=offset、`[47:16]`=值、`[63:48]`=區塊標籤 |
| 未開的 config | `RKNPU_SRAM` / `RKNPU_FENCE` / `RKNPU_PROC_FS` 皆 not set |

⚠️ **對 BRIEF 的兩處修正**

1. 附錄 A.1 的 debugfs 表列了 `mm` 節點 —— 實機沒有，因為 `CONFIG_ROCKCHIP_RKNPU_SRAM` 沒開。
   → 實驗 4.3 與第 5 章的 SRAM 段落改標「需重編 kernel」。
2. 附錄 A.1 說 `power`/`freq`/`delayms`/`reset` 可寫 —— **成立**，但 `ls -l` 顯示 `0444`，
   看起來像唯讀。實測 root 寫得進去（`CAP_DAC_OVERRIDE` 蓋過權限位元）。
   → 這個反直覺點本身值得寫進第 8 章當一個小警告。

⚠️ **對 BRIEF 目錄配置的一處增補**

BRIEF 只規劃了 `experiments/`。實際上 `tools/rkspy.c` 被第 4～7 章共用，
不屬於任何單一實驗，因此另開 `tools/`：

```
notes/rknpu/
├── BRIEF.md
├── OUTLINE.md        ← 本檔
├── ch00_*.md … ch09_*.md
├── tools/rkspy.c     ← 跨章共用的 ioctl 攔截器
└── experiments/      ← 各章專屬的 .c / .sh
```

---

## 章節

### ch00 — 你為什麼會在圖形子系統裡找到 AI 加速器

三件事先講清楚，不然讀者從第一頁就卡住：
(1) 上游 mainline kernel **完全沒有**這個 driver，只有 Rockchip BSP 有；
(2) `rknpu` 註冊成 DRM driver 但一張圖都不畫，它只借用 DRM 的記憶體管理；
(3) 全書的主線圖，先給讀者一張地圖再上路。

| 對照原始碼 | `rknpu_drv.c:700`（`.driver_features = DRIVER_GEM \| DRIVER_RENDER`，沒有 MODESET/ATOMIC） |
|---|---|
| 對照 TRM | 無（純軟體架構議題） |
| 實驗 | 0.1 `ls -l /dev/dri/` + 讀 `/sys/class/drm/renderD*/device/driver` 認出哪個是 NPU |

---

### ch01 — 硬體長什麼樣

RK3588 的 NPU 是三顆核心，每顆裡面有 CNA（捲積）、DPU（資料處理）、PPU（平面處理），
共用一個 4KB 一格的暫存器位址空間。這章把 TRM 的方塊圖和位址表講完，
讓讀者之後看到 `0x1020` 就知道「那是 CNA 的」。

| 對照原始碼 | `rk3588s.dtsi:3450`（node）、`:3482`（OPP）、`:3712`（IOMMU）；`rknpu_drv.c:196`（`rk3588_rknpu_config`） |
|---|---|
| 對照 TRM | **36.1** Overview、**36.2** Block Diagram、**36.3.1~36.3.4**（AHB/AXI、CNA、DPU、PPU）、**36.4.1** Internal Address Mapping |
| 實驗 | 1.1 `devmem` 讀暫存器對 TRM offset<br>1.2 `/proc/interrupts` 找三個 `npuN_irq`<br>1.3 `/proc/device-tree/npu@fdab0000/` |

> 本章要點名一處 TRM 與程式碼的落差：TRM 36.4.1 說 GLOBAL 區只有 `0xf000~0xf004`（4 bytes），
> 但 `rknpu_ioctl.h:35` 定義 `RKNPU_OFFSET_ENABLE_MASK 0xf008`。標為待查，不猜。

---

### ch02 — 開機：從 device tree 到一個能用的裝置

`rknpu_probe()` 逐行走一次。時脈、電源域、中斷、IOMMU、devfreq 是怎麼一個一個接起來的，
以及最後那兩行 —— 註冊成 DRM 裝置，順便也註冊了一個 misc 裝置當備用門。

| 對照原始碼 | `rknpu_drv.c`（probe 主體、`:867` `drm_dev_register`、`:1409` `misc_register`）、`drivers/rknpu/Kconfig`（兩道門的 choice） |
|---|---|
| 對照 TRM | **36.5.2** Clock and Reset |
| 實驗 | 2.1 `dmesg \| grep -i rknpu` 對照 probe 的每一則 log<br>2.2 `/sys/class/devfreq/fdab0000.npu/available_frequencies` 對 dtsi OPP 表 |

---

### ch03 — 六個 ioctl：使用者空間與核心的唯一交界

`librknnrt.so` 閉源，但它跟核心講話只有這六句。這章把 `rknpu_ioctl.h` 整份讀完，
並用 `strace` 看真實推論送了哪些 ioctl、各幾次。順便教一個陷阱：
strace 會把 DRM 驅動私有的 ioctl **叫錯名字**。

| 對照原始碼 | `rknpu_ioctl.h`（全檔）、`rknpu_drv.c` 的 `rknpu_ioctls[]` 分派表 |
|---|---|
| 對照 TRM | 無（TRM 不管作業系統介面） |
| 實驗 | 3.1 `strace` 跑一次推論，用 `struct` 大小把號碼對回 `rknpu_ioctl.h`<br>3.2 驗證 `ACTION/GET_DRV_VERSION` 回傳值 == debugfs `version` |

---

### ch04 — 記憶體：一塊記憶體的三個名字

一次推論配了 5 塊記憶體。這章講它們怎麼來的：GEM 給它一個號碼牌，
IOMMU 給硬體一個看得懂的位址，mmap 給使用者一個指標 —— 同一塊實體記憶體，三個名字。
再講為什麼要 `MEM_SYNC`（快取），以及 `PRIME_HANDLE_TO_FD`（dma-buf）在這裡做什麼。

| 對照原始碼 | `rknpu_gem.c`（`:326` handle_create、`:876` create_ioctl、`:910` map_ioctl、`:1398` prime_import、`:1419` prime_import_sg_table）、`rknpu_iommu.c`、`include/drm/drm_gem.h` |
|---|---|
| 對照 TRM | **36.3.1** AHB/AXI Interface（設定走 AHB、資料走 AXI）|
| 實驗 | 4.1 用 `tools/rkspy.c` 印出 5 塊 buffer 的 handle / size / dma_addr / mmap offset<br>4.2 同一塊記憶體，比對 kernel 給的 `dma_addr` 與使用者拿到的指標<br>4.3 ⏳ SRAM 配置器（`rknpu_mm.c`）— **需重編 kernel 開 `CONFIG_ROCKCHIP_RKNPU_SRAM`** |

---

### ch05 — ★ regcmd：模型被編譯成什麼

**全書核心。** SDK 把神經網路編譯成一長串「往哪個暫存器寫什麼值」的指令，放在 DRAM。
這章用 `tools/rkspy.c` 把它整包挖出來、解出格式、逐筆對回 TRM 的暫存器表，
然後從暫存器值裡把 MobileNet 的張量形狀讀出來（224→112→…→1×1）。

| 對照原始碼 | `rknpu_ioctl.h:216`（`struct rknpu_task`）、`tools/rkspy.c` |
|---|---|
| 對照 TRM | **36.3.5** Register File Fetch Unit（Rockchip 自己那句「從外部記憶體經 AXI 抓暫存器設定」）、**36.4.1** 位址表、**36.4.2/36.4.3** 暫存器詳表 |
| 實驗 | 5.1 dump regcmd，對照 TRM offset 表（**已驗證可行**）<br>5.2 換不同輸入尺寸重跑，看哪些欄位變、哪些不變<br>5.3 從 `cna_data_size0` 的 width/height 還原整個網路的層形狀 |

> 本章有一個「教材級」的發現要寫進去：第一層的高度被切成 `99 + 99 + 28 = 226`，
> 比 224 多 2 列。推測是捲積接縫重疊，**用實驗 5.2 證明，不直接斷言**。

---

### ch06 — 送出：driver 只寫 8 個暫存器

`RKNPU_SUBMIT` 進來之後，driver 做的事少得驚人：組一個 job、排隊、
然後往硬體寫 8 個暫存器就結束。這章逐一解釋那 8 個是什麼，
以及 `core_mask` / `subcore_task[5]` 怎麼決定三核分工。

| 對照原始碼 | `rknpu_job.c:300-375`（`rknpu_job_subcore_commit_pc`，那 8 個 `REG_WRITE`）、`rknpu_drv.c:166-215`（`pc_data_amount_scale` 等 per-SoC 參數） |
|---|---|
| 對照 TRM | **36.5.4** NPU operate flow、**36.5.1** Ping-pong registers（實測 `flags=0x5` 就帶著 PINGPONG） |
| 實驗 | 6.1 跑推論同時 `watch` debugfs `load`（實測：預設只有 Core0 動）<br>6.2 SDK 指定單核 vs 三核，比較 `load` 與 `/proc/interrupts` 增量<br>6.3 用 `rkspy.c` 印 `subcore_task[0..4]`，對照 `rknpu_job.c:320` 的 `core_index + 2` 索引邏輯 |

---

### ch07 — 中斷：硬體怎麼說「我算完了」

寫下 `PC_OP_EN = 1` 之後 driver 就去睡了。叫醒它的是中斷。
這章走完中斷處理的完整路徑，並用「把中斷關掉」來證明它真的是唯一的通知管道。

| 對照原始碼 | `rknpu_drv.c`（irq handler、`:66` `bypass_irq_handler` 模組參數）、`rknpu_job.c`（等待與喚醒） |
|---|---|
| 對照 TRM | **36.5.3** NPU Interrupt Application、`INT_MASK`/`INT_CLEAR`/`INT_STATUS`（`rknpu_ioctl.h:28-31`） |
| 實驗 | 7.1 ftrace 抓完整中斷呼叫鏈<br>7.2 `/proc/interrupts` 前後差值 vs 送出的 job 數<br>7.3 **`bypass_irq_handler=1` 跑推論 → 應該 timeout** |

---

### ch08 — 電源與頻率：閒置三秒就關電

實測閒置時 `power=off`，一送 job 就 `on`，停 3 秒又 `off`。
這章講 runtime PM、三個電源域、devfreq 與 OPP 表怎麼把頻率和電壓綁在一起。

| 對照原始碼 | `rknpu_devfreq.c`、`rknpu_drv.c`（runtime PM、`power_put_delay`）、`rk3588s.dtsi:3482`（OPP 表） |
|---|---|
| 對照 TRM | **36.5.2** Clock and Reset |
| 實驗 | 8.1 閒置/忙碌/停止三個時間點讀 `power`（**已實測**）<br>8.2 改 `freq` 看 `volt` 跟著動<br>8.3 改 `delayms` 量自動關電時間（**已實測可寫**，須 root） |

---

### ch09 — 出事的時候：timeout 與重置

最後一章講錯誤路徑。job 卡住怎麼辦、軟重置做了什麼、
以及為什麼 `rknpu_reset.c` 只有 158 行卻是整個驅動的安全網。

| 對照原始碼 | `rknpu_reset.c`、`rknpu_job.c`（timeout 處理、`:961` 的 `pc_data_addr` 存回復）、`rknpu_drv.c:66`（`bypass_soft_reset`） |
|---|---|
| 對照 TRM | **36.5.2** Clock and Reset |
| 實驗 | 9.1 `bypass_soft_reset=1` 觸發 timeout，比較有無軟重置的復原行為<br>9.2 手動 `echo 1 > .../rknpu/reset` 後確認仍可推論 |

---

## 沒排進去的東西（刻意的）

| 項目 | 為什麼不排 |
|---|---|
| dma-fence（`rknpu_fence.c`） | `CONFIG_ROCKCHIP_RKNPU_FENCE` 沒開，實測 `fence_fd=-1`。改在 ch04 用一段旁白帶過，不獨立成章。 |
| `rknpu_mem.c`（DMA Heap 那道門） | 板子走的是 DRM GEM。在 ch02 講「有兩道門」時提一次，不走進去。 |
| RK356x 分支 | BRIEF 界線已排除。 |
| 效能調校 | BRIEF 界線已排除。 |

---

## 規模

10 章，符合 BRIEF 的「8～10 章」。
節奏：大綱通過後一次寫一章，寫完給看，確認再下一章。
