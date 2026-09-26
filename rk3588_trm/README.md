# RK3588 TRM 逐模組筆記與實機驗證

依《RK3588 TRM Part1/Part2》（`books/rk3588_trm/`，共 67 章）逐章整理，
能在 ROCK 5B（`192.168.68.58`，Linux 6.1.115+）上驗證的就驗證，對不上的就記成「落差」。

## 進度

| 組 | 筆記 | TRM 章節 | 狀態 |
|----|------|---------|------|
| A 系統基礎 | [A_system.md](./A_system.md) | P1 ch1/2/6/7/8/9/10/11/15/16/17/32/34/35、P2 ch1 | ✅ |
| B 運算核心 | [B_compute.md](./B_compute.md) | P1 ch3/4/36 | ✅ |
| C 低速周邊 | [C_lowspeed_io.md](./C_lowspeed_io.md) | P1 ch12/13/14/18/19/20/21/26/29/30/31 | ✅ |
| D 音訊 | [D_audio.md](./D_audio.md) | P1 ch22/23/24/27/28/39 | ✅ |
| E 安全 | [E_security.md](./E_security.md) | P1 ch33、P2 ch10 | ✅ |
| F 儲存與高速介面 | [F_storage_highspeed.md](./F_storage_highspeed.md) | P2 ch2/3/4/11/12/13/14/15/16、P1 ch25 | ✅ |
| G 多媒體 | [G_multimedia.md](./G_multimedia.md) | P1 ch5/37/38、P2 ch5/6/8 | ✅ |
| H 顯示、相機介面、PVTM/PVTPLL | [H_display_camera_pvt.md](./H_display_camera_pvt.md) | P2 ch7/9/17/18/19/20/21/22/23/24/25/26/27/28 | ✅ |

TRM 全部 67 章（Part1 39 章 + Part2 28 章）都已歸入以上 8 組。沒接螢幕、相機、SATA、CAN 等外設的模組，筆記會寫明「無法實驗」與原因。
P2 ch2 DMC、ch17/18 的體質分數另見 [`rock5b_explore/`](../rock5b_explore/README.md)。

## 最重要的發現

- **PVTPLL**：TRM 漏列的 STATUS 暫存器找到了；量測窗剛好 1 µs 所以讀值 = MHz；頻率掃描量出晶片的電壓—頻率曲線（低頻縮短振盪環、高頻加電壓），並用它內插出開機體質分數，誤差 < 1%（H §5）
- **VPU**：H.264 硬體和 H.265 一樣快（每幀 3.76 ms），GStreamer 外掛卻隨機卡住數秒；解碼是雙核輪流（G §1）

- **TSADC**：Linux 驅動用的溫度對照表和 TRM 不同；Linux 說 85°C 時，照 TRM 是 90°C（C §2）
- **GPU**：真實 FP32 約 325 GFLOPS；連畫互相覆蓋的畫面會被 Forward Pixel Kill 跳過，算力「灌水」剛好 N 倍（B §2）
- **看門狗**：DW WDT 開了就停不了；magic close 後核心每 42 秒代餵，行程直接死掉則依設計讓板子重開——兩種都實測過（A §10）
- **GIC600**：設定全對上，但版本欄位是 r1p7，TRM 寫 r1p6（A §8）
- **CRU**：用 TRM 公式從暫存器算出的 PLL 頻率和 Linux 完全一致（A §2）
- **SCMI**：從共享記憶體讀到核心和 BL31 之間最後一則訊息，回覆的頻率就是剛設定的值（A §12）

## ⚠ 安全規則（踩過雷才訂的）

讀錯 MMIO 會讓整台板子卡死（A 章開頭有紀錄，發生過一次）。一律用 `tools/safe_mmio.py`：
位址必須屬於已綁驅動、runtime active、所有 consumer 時脈都開著的裝置，否則拒絕。

## 工具（`tools/`）

| 檔案 | 用途 |
|------|------|
| `trm_addrmap.py` | 把 TRM Table 1-1 解析成 CSV |
| `dt_inventory.py` | 板上列出 DT 節點、status、驅動、runtime PM |
| `join_map.py` | TRM 位址表 × DT 清單 |
| `safe_mmio.py` | 有守門的唯讀 MMIO |
| `mmio.py` | 無守門版（只用於確定常開的區塊） |
| `pll_decode.py` | 依 TRM 公式算 PLL 頻率 |
| `timer_watch.py` | 抓 rk_timer 倒數、量計數頻率 |
| `wdt_probe.py` / `wdt_magic_test.sh` / `wdt_kernel_ping.sh` | 看門狗實驗（⚠ 會讓 WDT 保持啟動到下次重開機） |
| `gpu_burn.py` | ctypes 呼叫 EGL/GLES 的 GPU 燒機 |
| `tsadc_compare.py` | TSADC 原始碼 → TRM 表 vs 驅動表 |

## 資料（`data/`）

`trm_addrmap.csv`、`dt_inventory.csv`、`trm_vs_dt.csv`、`clk_summary.txt`、`pm_genpd_summary.txt`、
`interrupts.txt`、`iomem.txt`、`iommu_groups.txt`、`kernel_config.txt`
