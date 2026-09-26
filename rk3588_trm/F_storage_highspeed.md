# F　儲存與高速介面：DMC、SD/SDIO/eMMC、PCIe、USB、SATA、PHY、GMAC

> TRM：Part2 ch2（DMC）、ch3（Mobile Storage Host）、ch4（eMMC）、ch11（PCIe）、ch12（USB2 Host & PHY）、
> ch13（USB3）、ch14（USBDP Combo PHY）、ch15（SATA）、ch16（Multi-Protocol PHY）；Part1 ch25（GMAC）

## 板上啟用狀況

| 模組 | TRM 數量 | 啟用 | 接到哪 |
|------|---------|------|-------|
| PCIe | 3.0 x4、3.0 x2、3 × 2.0 x1 | 3.0 x4（`fe150000`）、2.0 x1 L0（`fe170000`）、2.0 x1 L2（`fe190000`） | NVMe、MT7921 Wi-Fi、RTL8125 2.5GbE |
| Combo PIPE PHY | 3 | 3 | 給 PCIe 2.0 x1 用（也可切成 SATA/USB3） |
| PCIe3 PHY | 1 | 1 | 給 PCIe 3.0 用 |
| SATA | 3 | **0** | Combo PHY 被 PCIe 佔走 |
| USB3（DWC3） | 3 | USB3_0（OTG，Type-C）、USB3_1（Host） | 兩個 5 Gbps root hub |
| USB2 Host（EHCI/OHCI） | 2 | 2 | 板上 USB Hub → 藍牙 |
| USBDP PHY | 2 | 2 | USB3 + DP 共用的 PHY |
| SDMMC | 1 | 1 | microSD（**系統碟**） |
| SDIO | 1 | okay 但沒卡 | — |
| eMMC | 1 | okay 但**沒插模組** | — |
| GMAC | 2 | **0** | ROCK 5B 的網路走 PCIe 的 RTL8125 |
| DMC | 4 通道 | — | 由 BL31 管，Linux 只有 devfreq + DFI 監控 |

## 1. DMC（Part2 ch2）

四個 DDR 控制器（`DDRCTL_0~3 f7000000~fa000000`）、PHY、DDR CRU 在 DT **都沒有節點**——
全部由開機的 DDR init 程式（`ddr-v1.16`）和 BL31 管，Linux 只看得到：
- `dfi@fe060000`（`DDR_MON0`，TRM 稱 DDR monitor）：給 devfreq 算頻寬使用率
- `dmc` devfreq：528 / 1068 / 1560 / 2112 MHz，透過 SCMI 請 BL31 換頻（`scmi_clk_ddr`）

頻率 vs 頻寬/延遲的實驗、`dmc_ondemand` 演算法見 [`rock5b_explore/04`](../rock5b_explore/04_ddr_devfreq.md)。

## 2. SD / SDIO / eMMC（Part2 ch3、ch4）

`/sys/kernel/debug/mmc*/ios`：

| host | 控制器 | 狀態 |
|------|--------|------|
| mmc1 | SDMMC `fe2c0000`（dw-mshc） | **SDR104、4-bit、1.8 V**、要求 200 MHz、實際 **198 MHz** |
| mmc0 | eMMC `fe2e0000`（dwcmshc） | power off，沒插 eMMC 模組 |
| mmc2 | SDIO `fe2d0000`（dw-mshc） | 開機時以 400→300→200→187.5 kHz 一路降速探測，最後沒找到裝置 |

SD 實際時脈 198 MHz 而不是 200：來源是 GPLL 1188 MHz ÷ 6 = 198 MHz（GPLL 見 A §2），整數分頻湊不出 200。
SDR104 4-bit 理論上限 = 198 MHz × 4 bit ÷ 8 = **99 MB/s**，實測 `dd` 讀 69.6 MB/s（`rock5b_explore/05 §4`）≈ 70%。

## 3. PCIe（Part2 ch11）

開機訊息（journal）：

```
rk-pcie fe150000.pcie: PCIe Link up, LTSSM is 0x230011
rk-pcie fe150000.pcie: PCIe Gen.3 x4 link up
rk-pcie fe170000.pcie: PCIe Link up, LTSSM is 0x130011
rk-pcie fe170000.pcie: PCIe Gen.2 x1 link up
rk-pcie fe190000.pcie: PCIe Link up, LTSSM is 0x130011
rk-pcie fe190000.pcie: PCIe Gen.2 x1 link up
```

- LTSSM 低 6 bit = **0x11 = L0**（DesignWare 編碼，正常傳輸狀態）✅
- `lspci -vv`：三條連結都跑在能力上限（8 GT/s x4、5 GT/s x1 ×2），**ASPM 全關**
- MSI 走 GIC ITS（LPI），不經 SMMU（兩個 MMU600 在 DT 都 disabled，見 A §5）
- NVMe（Intel 660p）讀 1.5~1.6 GB/s，是硬碟本身上限；Gen3 x4 理論約 3.9 GB/s

TRM 的 PCIe DBI 空間（`PCIe3_*_DBI f5000000~`）在 DT 沒有對應節點：Linux 驅動用的是 DT 裡另一組 64-bit 高位址的 DBI 映射。

## 4. USB（Part2 ch12、ch13、ch14）

```
$ lsusb -t
Bus 05 / Bus 02: xhci-hcd  5000M   ← USB3_1 / USB3_0（SuperSpeed root hub）
Bus 03 / Bus 01: xhci-hcd   480M   ← 同上的 USB2 部分
Bus 07: ehci-platform 480M
    └─ Hub (4p) → Port 3: Class=Wireless（藍牙，IMC Networks 13d3:3583）
Bus 04: ehci-platform 480M ；Bus 06/08: ohci-platform 12M
```

DWC3 身分暫存器（controller runtime active，`safe_mmio.py` 放行）：

| 暫存器 | TRM 重置值 | USB3_0 `fc00c120` | USB3_1 `fc40c120` |
|--------|-----------|-------------------|-------------------|
| `USB3OTG_GSNPSID` | `0x5533300A` | `5533300a` ✅ | `5533300a` ✅ |

`0x5533` = ASCII "U3"，`300a` = DWC_usb3 **3.00a**。
（為了讀到這兩個位址，把 `safe_mmio.py` 改成會往下找一層「位址直通」的父節點：USB 控制器掛在 `usbdrd3_0/`、`usbdrd3_1/` 底下。）

板上沒插任何 USB3 裝置，所以沒做 5 Gbps 傳輸實驗。

## 5. SATA（Part2 ch15）與 Multi-Protocol PHY（ch16）

三個 SATA 控制器都 disabled。TRM：Combo PIPE PHY 可以是 PCIe 2.0、SATA 或 USB3 三選一。
ROCK 5B 把 PHY0 給 PCIe 2.0 x1 L2（2.5GbE）、PHY2 給 L0（Wi-Fi 的 M.2 E-key），所以沒有 SATA。
（M.2 E-key 理論上可以改成 SATA，需要換 DT overlay；沒做。）

## 6. GMAC（Part1 ch25）

兩個 GMAC 都 disabled。ROCK 5B 的有線網路是 PCIe 上的 Realtek RTL8125（2.5 Gbps），不用 SoC 內建 MAC。
（板子當下走 Wi-Fi，`wlP2p33s0`，有線沒插。）
