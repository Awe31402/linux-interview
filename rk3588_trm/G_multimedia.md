# G　多媒體：VPU（編解碼）、RGA2/RGA3、IEP、VICAP、FEC、DECOM

> TRM：Part1 ch5（VPU）、ch35（DECOM）、ch37（VICAP）、ch38（FEC）；Part2 ch5（RGA3）、ch6（RGA2）、ch8（IEP）
> VOP（Part2 ch7）歸到 H 組（顯示）。
> 工具：`tools/vpu_bench.sh`、`tools/mpp_timing.sh`

## 板上狀況

| IP | TRM 角色 | 板上 |
|----|---------|------|
| VEPU580 × 2（`fdbd0000`、`fdbe0000`） | H.264/H.265 編碼，最大 16K×8K | okay（`rkvenc-core` ×2，共用一個 ccu） |
| VDPU381 × 2（`fdc38100`、`fdc48100`） | H.264/H.265/VP9/AVS2 解碼，8K@60 | okay（`rkvdec-core` ×2） |
| VDPU981（`fdc70000`） | AV1 解碼，4K@60 | okay（`av1d`） |
| VEPU121 × 4（`fdba0000~fdbac000`） | JPEG 編碼，90 Mpixel/s | okay（`jpege-core` ×4） |
| VDPU121 / VEPU121（`fdb50400`/`fdb50000`） | 舊格式解碼（MPEG-2/4、VP8…）/ H.264 編碼 | okay |
| JPEG decoder（`fdb90000`） | — | okay |
| RGA3 × 2、RGA2 × 1 | 2D 影像加速 | okay |
| IEP（`fdbb0000`） | 去交錯等影像增強 | okay |
| VICAP、ISP0/1、FISHEYE0/1 | 相機擷取、ISP、魚眼校正 | **全部 disabled**（沒接相機） |
| DECOM | 硬體解壓 | disabled（見 A §13） |

使用者態：`librockchip_mpp.so` + GStreamer `rockchipmpp` 外掛（`mpph264enc`、`mpph265enc`、`mppjpegenc`、`mppvideodec`…），
`librga.so.2`（但沒有對應的 GStreamer 元件）。

## 1. VPU（Part1 ch5）

### 1.1 吞吐量（`vpu_bench.sh`，videotestsrc SMPTE 彩條 → 編/解碼 → fakesink，各 300 幀）

同時比對 `/proc/interrupts` 的差值，看是哪顆硬體在做：

```
src only 1920x1080     203.0 fps
h264 enc 1920x1080      10.8 fps   irq: fdbd0000.rkvenc-core+300
h265 enc 1920x1080     181.4 fps   irq: fdbd0000.rkvenc-core+300
h265 dec 1920x1080     912.4 fps   irq: fdc38100.rkvdec-core+150 fdc48100.rkvdec-core+150
h264 dec 1920x1080     937.2 fps   irq: fdc38100.rkvdec-core+150 fdc48100.rkvdec-core+150
jpeg enc 1920x1080       8.7 fps   irq: fdba0000.jpege-core+300
src only 3840x2160      50.1 fps
h264 enc 3840x2160       6.2 fps   irq: fdbd0000.rkvenc-core+300
h265 enc 3840x2160      52.5 fps   irq: fdbd0000.rkvenc-core+300
h265 dec 3840x2160     396.9 fps   irq: fdc38100.rkvdec-core+150 fdc48100.rkvdec-core+150
h264 dec 3840x2160     345.9 fps   irq: fdc38100.rkvdec-core+150 fdc48100.rkvdec-core+150
```

看得出來的事：
- **解碼是雙核輪流**：300 幀，兩顆 rkvdec-core 各拿 150 個中斷（TRM：VDPU381 支援 dual-core decoding）✅
- **編碼只用一顆 VEPU580**：300 個中斷全在 `fdbd0000`，另一顆 `fdbe0000` 沒動
- **JPEG 只用 4 顆裡的第 1 顆**
- H.265 編碼 181/52 fps 幾乎等於「只產生畫面」的 203/50 fps → 瓶頸是 CPU 產生測試畫面，不是編碼器
- 解碼數字遠超 TRM 的「8K@60」（= 1.99 Gpixel/s）：4K 397 fps = 3.3 Gpixel/s。
  因為彩條畫面幾乎沒有變化、壓得極小，**這個測試流太簡單，不能拿來和規格比**

### 1.2 H.264 / JPEG 為什麼那麼慢？→ 不是硬體

打開 MPP 驅動內建的計時（`/proc/mpp_service/timing_en` + 各 core 的 `timing_check`；
`drivers/video/rockchip/mpp/mpp_common.c:2462` `mpp_task_dump_timing()`），每個硬體任務記下 create/run/irq/finish：

```
rk_vcodec: task 600 dump timing at 14960 us:
rk_vcodec: timing: run            : 102 us
rk_vcodec: timing: irq            : 14880 us      ← 硬體從開始到發中斷
rk_vcodec: timing: finish         : 14960 us
```

1080p 各 20 幀的平均：

| 編碼器 | 硬體時間（run→irq） | 換算 |
|--------|-------------------|------|
| H.265 | 3762 µs | ~266 fps |
| **H.264** | **3759 µs** | ~266 fps（和 H.265 一樣） |
| JPEG | 14767 µs | ~68 fps（1080p = 2.07 Mpixel → **140 Mpixel/s**，TRM 寫 90 Mpixel/s） |

H.264 硬體和 H.265 一樣快；20 幀時兩個任務間隔也只有 6.5 ms。但總時間很不穩定：

| 編碼器 | 20 幀 | 100 幀 | 300 幀 |
|--------|------|-------|-------|
| `mpph264enc` | 3.7 s | **25.5 s** | 7.6 s |
| `mpph265enc` | 0.17 s | 0.54 s | 1.52 s |
| `mppjpegenc` | 0.87 s | 15.0 s | 40.4 s |

H.265 線性、穩定；H.264 和 JPEG 會**隨機卡住好幾秒**（CPU 幾乎閒著：4K JPEG 跑 17 分鐘只用了 52 秒 CPU）。
→ 問題在 GStreamer 外掛或 MPP 使用者態函式庫，不在硬體。**確切原因沒追**（需要外掛原始碼或 `GST_DEBUG`）。

JPEG 硬體實測 140 Mpixel/s 比 TRM 的 90 Mpixel/s 還快。可能是彩條畫面好壓，也可能 TRM 寫的是保守值；**未確認**。

### 1.3 電源與 IOMMU

各編解碼核心都有自己的 IOMMU group（A §5），電源域平常 off，開機 probe 時亮約 2 秒（A §4）。
兩顆 rkvenc-core 的 devfreq 都失敗：`no regulator (venc) found` → `failed to add venc devfreq`，編碼器頻率固定不調。

## 2. RGA3 / RGA2（Part2 ch5、ch6）

`/sys/kernel/debug/rkrga/hardware`（驅動 `RGA multicore Device Driver: v1.3.7`）vs TRM：

| 項目 | TRM | 驅動回報 | |
|------|-----|---------|---|
| RGA3 最大輸入 | (8192−16)² = 8176 | `input range: 68x2 ~ 8176x8176` | ✅ |
| RGA3 最大輸出 | (8192−64)² = 8128 | `output range: 68x2 ~ 8128x8128` | ✅ |
| RGA3 縮放 | 縮小 1/8、放大 8 | `scale limit: 1/8 ~ 8` | ✅ |
| RGA3 最小尺寸 | **128×128** | **68×2** | ⚠ 驅動比 TRM 寬鬆 |
| RGA2 最大輸入/輸出 | 8192 / 4096 | `2x2 ~ 8192x8192` / `2x2 ~ 4096x4096` | ✅ |
| RGA2 縮放 | 1/16 ~ 16 | `scale limit: 1/16 ~ 16` | ✅ |
| MMU | — | RGA3：`RK_IOMMU`；RGA2：`RGA_MMU`（自帶的舊式 MMU） | |

兩顆 RGA3 版本 `3.0.76831`、RGA2 `3.2.63318`。板上沒有能呼叫 librga 的工具，沒做實際搬圖實驗。

## 3. IEP（Part2 ch8）

`iep: Module initialized.`，`/proc/mpp_service/iep` 存在。TRM：動態影像最大 1920×1080（去交錯用）。沒做實驗。

## 4. VICAP（Part1 ch37）、FEC（Part1 ch38）

VICAP、ISP0/1、FISHEYE0/1 全部 disabled（ROCK 5B 沒接相機）。
怪的是 6 個 `CSI HOST0~5` 與 `MIPI CSI DPHY0/1` 在 DT 是 okay——控制器開著，但後面接收影像的 VICAP 是關的。
TRM 的 FEC：輸入 128×128~8188×8188、輸出最大 4096×4096。無法實驗。
