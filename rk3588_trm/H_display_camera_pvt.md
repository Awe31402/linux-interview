# H　顯示、相機介面、PVTM／PVTPLL

> TRM：Part2 ch7（VOP）、ch9（DPTX）、ch17（PVTM）、ch18（PVTPLL）、ch19（MIPI CSI Host）、ch20（MIPI CSI DPHY）、
> ch21（DSI-2）、ch22（D-PHY/C-PHY Combo）、ch23（eDP TX）、ch24（HDMI TX）、ch25（HDMI RX）、ch26（HDCP2.3）、
> ch27（HDMI TX/eDP Combo PHY）、ch28（HDMI RX PHY）
> 工具：`tools/pvtpll_sweep.sh`

⚠ 實驗時**沒有接任何螢幕或相機**：三個輸出接頭全是 `disconnected`，VOP 是 runtime `suspended`（時脈關著）。
所以這組大部分只能查設定；A 章開頭那次「讀 VOP QoS 把板子卡死」就是在這個狀態下發生的。

## 1. 板上狀況

| 模組 | TRM 數量 | 啟用 | 說明 |
|------|---------|------|------|
| VOP2 | 1（4 個 video port） | okay，suspended | |
| HDMI TX | 2 | TX0、TX1 okay | ROCK 5B 兩個 HDMI 輸出 |
| DP（DPTX） | 2 | DP0 okay | USB-C 的 DP Alt Mode |
| eDP | 2 | 0 | |
| DSI-2 | 2 | 0 | |
| HDMI RX | 1 | okay | `/dev/video0`（V4L2） |
| HDCP 2.3 | 2 | 0 | |
| HDPTX Combo PHY | 2 | 2（HDMI 模式） | |
| MIPI D/C-PHY Combo | 2 | 2 okay | |
| MIPI CSI Host | 6 | 6 okay | 但後端 VICAP/ISP 都 disabled（G §4） |
| MIPI CSI DPHY | 2 | 2 okay | |
| PVTM | 6 | 5 個 `pvtm@` probe | 見 §4 |
| PVTPLL | 6 | 由 BL31 使用 | 見 §5 |

DRM connector：`card0-DP-1`、`card0-HDMI-A-1`、`card0-HDMI-A-2`、`card0-Writeback-1`，全部 disconnected / disabled。

## 2. VOP2（Part2 ch7）

開機時驅動印出的 plane 分配（journal）：

```
vp0 assign plane mask: Cluster0 | Esmart0[0x5],   primary plane phy id: Cluster0[0]
vp1 assign plane mask: Cluster1 | Esmart1[0xa],   primary plane phy id: Cluster1[1]
vp2 assign plane mask: Cluster2 | Esmart2[0x140], primary plane phy id: Cluster2[6]
vp3 assign plane mask: Cluster3 | Esmart3[0x280], primary plane phy id: Cluster3[7]
Esmart0-win0 as cursor plane for vp0 ...
```

TRM：VOP2 有 4 個 video port、4 個 Cluster 圖層 + 4 個 Esmart 圖層。驅動把它們一對一分給 4 個 VP，每個 VP 一個 Cluster 當主圖層、一個 Esmart 當游標。
（mask 的位元編號不連續：Cluster2/3 的 phy id 是 6、7，Esmart2/3 是 8、9 — `0x140` = bit 6+8、`0x280` = bit 7+9。）

本 repo 最近兩個 commit 就是在改這裡：`HACK: drm: rockchip: Prefer non-cluster overlay planes`、`Force enable legacy-cursor-update`。

開機時 VOP 也跑了 opp_select：`bin=0`、`leakage=31`，但接著 `no regulator (vop) found` → `failed to init opp info`，VOP 沒有 DVFS。

## 3. HDMI RX（Part2 ch25、ch28）

```
$ v4l2-ctl -d /dev/video0 --info --list-formats
Driver name : rk_hdmirx
[0] 'BGR3'  [1] 'NV24'  [2] 'NV16'  [3] 'NV12'
$ cat /sys/class/hdmirx/hdmirx/status   → disconnected
```

內建的預設 EDID（`--get-edid`）：

```
00 ff ff ff ff ff ff 00 49 70 88 35 01 00 00 00 2d 1f ...
```

- 廠商碼 `0x4970` → 三個 5-bit 字母 18/11/16 → **"RKP"**
- 產品碼（little endian）`0x3588` —— 就是晶片型號
- 年份 byte 17 = `0x1f` = 31 → 1990 + 31 = **2021**

驅動把 HDMI RX 的中斷綁在 CPU5（`cpu_aff:0x500, Bound_cpu:5`）。沒有訊號源，沒做擷取實驗。

## 4. PVTM（Part2 ch17）

TRM：6 個 PVTM（BIGCORE0/1、LITCORE、NPU、GPU、PMU），都是「環形振盪器 + 計數器」，用來量製程/電壓/溫度造成的速度差。
開機時 5 個 probe：`fda40000`、`fda50000`、`fda60000`、`fdaf0000`、`fdb30000`。

但這一代 CPU 的「體質分數」其實不是從 PVTM 讀，而是從 PVTPLL 讀（DT 有 `rockchip,pvtm-pvtpll`，見下節）。
體質分數怎麼決定電壓與最高頻率：[`rock5b_explore/01`](../rock5b_explore/01_silicon_lottery.md)。

## 5. PVTPLL（Part2 ch18）— 把一個「推測」變成「確定」

`rock5b_explore/01 §9` 發現 CPU 叢集 GRF 的 +0x14/+0x18 讀值 ≈ 目前 CPU 頻率（MHz），當時只能推測原因。TRM ch18 給了答案。

### 5.1 暫存器

TRM Table 18-2（GRF 裡的 PVTPLL 暫存器）：

| offset | 名稱 | 內容 |
|--------|------|------|
| 0x00 | `PVTPLL_CON0_L` | bit0 start、bit1 osc_en、[10:8] osc_ring_sel、[12:11] clk_div_ref、[14:13] clk_div_osc |
| 0x04 | `PVTPLL_CON0_H` | [5:0] ring_length_sel（振盪環長度） |
| 0x08 | `PVTPLL_CON1` | cal_cnt：量測窗長度（單位：ref_clk 週期），重置值 **0x18 = 24** |
| 0x0C | `PVTPLL_CON2` | threshold、ckg_val |
| 0x10 | `PVTPLL_CON3` | ref_cnt |
| 0x14 | `PVTPLL_STATUS0` | **osc_cnt**：量測窗內數到幾個振盪 |
| 0x18 | `PVTPLL_STATUS1` | **osc_cnt_avg**：平均值（DT `rockchip,pvtm-offset = <0x18>` 讀的就是這個） |

⚠ **TRM 落差**：Part1 ch6 的 `BIGCORE_GRF` 暫存器表只列到 0x10（CON0~3），**STATUS0/STATUS1 沒列**；要看 Part2 ch18 才知道。

**為什麼讀值 = MHz**：ref_clk 是 24 MHz，`cal_cnt = 24` → 量測窗 = 24 / 24 MHz = **1 µs**。1 µs 內數到 N 個振盪 = N MHz。

### 5.2 實驗：頻率掃描（`pvtpll_sweep.sh`，大核叢集 2，`BIGCORE1_GRF 0xfd592000`）

```
    freq     mV    CON0_L   CON0_H     CON1     CON2     CON3     osc    avg  ring_sel len
     408    675  00000103 0000000b 00000018 00040000 00000000    1446   1452  1 11
     600    675  00000103 0000000b 00000018 00040000 00000000    1454   1453  1 11
     816    675  00000103 00000021 00000018 00040000 00000000     798    796  1 33
    1008    675  00000103 00000017 00000018 00040000 00000000     998    993  1 23
    1200    675  00000103 00000011 00000018 00040000 00000000    1181   1182  1 17
    1416    700  00000103 0000000d 00000018 00040000 00000000    1419   1424  1 13
    1608    725  00000103 0000000b 00000018 00040000 00000000    1609   1614  1 11
    1800    800  00000103 0000000b 00000018 00040000 00000000    1824   1825  1 11
    2016    875  00000103 0000000b 00000018 00040000 00000000    2005   2010  1 11
    2208    962  00000103 0000000b 00000018 00040000 00000000    2188   2185  1 11
    2256   1000  00000103 0000000b 00000018 00040000 00000000    2258   2251  1 11
```

看得到兩段完全不同的調頻策略：

| 頻段 | 電壓 | 振盪環長度 | 怎麼變快 |
|------|------|-----------|---------|
| 408、600 MHz | 675 mV | 11（沒動） | PVTPLL 沒被當時脈用（osc ≈ 1450 是空轉值），CPU 用一般 PLL |
| 816 → 1200 MHz | **固定 675 mV** | **33 → 23 → 17** | **縮短振盪環**（反相器越少，繞一圈越快） |
| 1416 → 2256 MHz | **700 → 1000 mV** | 13 → **11（最短）** | 環已經最短，改**加電壓** |

- `CON1` 一直是 24（重置值），`osc_ring_sel` 一直是 1
- `osc` 讀值和設定頻率差 < 2%：PVTPLL 本身就是 CPU 的時脈源，BL31 選環長讓它剛好落在目標頻率

### 5.3 回頭驗證開機體質分數

開機量體質的條件（`rockchip_opp_select.c`）：1608 MHz、**750 mV**。1608 MHz 時環長 = 11，
所以「環長 11、750 mV」的振盪頻率，可以用上表環長 11 的點內插：

- 725 mV → 1609 MHz、800 mV → 1824 MHz
- 750 mV ≈ 1609 + (1824 − 1609) × 25/75 ≈ **1681 MHz**

開機實際量到叢集 2（cpu6）的分數：**1689、1696**（兩次開機）。內插值 1681 和實測只差 0.5~0.9% ✅
——開機的「體質分數」就是「這顆晶片在 750 mV 下，最短振盪環能跑多快（MHz）」。

## 6. 其他（無法實驗）

- **DPTX（ch9）/ eDP（ch23）/ DSI-2（ch21）/ D-PHY·C-PHY（ch22）**：DP0 okay 但沒接；eDP、DSI 全 disabled
- **HDMI TX（ch24）/ HDMI-eDP Combo PHY（ch27）**：兩路 okay，沒接螢幕，時脈關著，不能讀暫存器
- **HDCP 2.3（ch26）**：兩個都 disabled
- **MIPI CSI Host（ch19）/ CSI DPHY（ch20）**：控制器 okay，但沒相機、後端 VICAP disabled
- **Writeback connector**：`card0-Writeback-1` 存在，理論上可以不接螢幕讓 VOP 輸出到記憶體來驗證 VOP，需要寫 DRM atomic 程式，**沒做**
