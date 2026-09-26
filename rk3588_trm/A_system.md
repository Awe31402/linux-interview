# A　系統基礎模組（時脈、電源、中斷、計時、看門狗、匯流排）

> TRM：Part1 ch1/2/6/7/8/9/10/11/15/16/17/32/34/35、Part2 ch1
> 機台：ROCK 5B，Linux 6.1.115+，實驗日期 2026-09-26
> 工具：`tools/safe_mmio.py`（有守門的唯讀 MMIO）、`tools/mmio.py`、`tools/pll_decode.py`、`tools/timer_watch.py`、`tools/wdt_*.{py,sh}`

## ⚠ 先讀這段：這一章我把板子弄當了兩次

| 時間 | 做了什麼 | 結果 | 教訓 |
|------|---------|------|------|
| 07:50 | 看門狗實驗中，用 `pkill` 砍掉自己的餵狗程式（樣式還打中自己） | 89 秒後被 WDT 重開 | DW WDT 一旦啟動**硬體上停不了**；行程沒 magic close 就死，核心**依設計不餵** |
| 07:59 | 直接讀 VOP 的 NoC QoS 暫存器 `0xfdf82000` | **匯流排卡死**，整台沒回應；由上一步留著的 WDT 在 ~2.5 分鐘後重開 | pm_genpd 顯示 vop **電源域 on**，但 VOP 本身是 runtime `suspended`（沒接螢幕），時脈關著。TRM Part2 §1.3.2 早就寫了：「設定 QoS 時該 master 的時脈必須開著」 |

之後所有讀取都改走 `tools/safe_mmio.py`：位址要屬於**已綁驅動**、**runtime active**、
**所有 consumer 時脈 enable_count > 0** 的裝置，否則拒絕。GRF/CRU/GIC 這種常開區塊才用 `--force`。

```
0xfdf82000 -> REFUSED: fdf82000.qos: no driver bound
0xfdd90000 -> REFUSED: fdd90000.vop: runtime_status=suspended
0xfe2c0000 -> REFUSED: fe2c0000.mmc: clocks disabled: sdmmc_drv,sdmmc_sample
0xfeaf00f8 -> 3131302a                               （WDT，通過）
```

---

## 1. System Overview（Part1 ch1）— 位址表 × 板上 DT

`tools/trm_addrmap.py` 把 Table 1-1 解析成 342 個區塊（`data/trm_addrmap.csv`），
`tools/dt_inventory.py` 在板上列出 DT 每個節點的位址、status、驅動、runtime PM（`data/dt_inventory.csv`），
`tools/join_map.py` 合併成 `data/trm_vs_dt.csv`：

| 狀態 | 區塊數 |
|------|-------|
| DT 有節點且 okay | 111 |
| DT 有節點但 disabled | 68 |
| 一部分 okay、一部分 disabled | 5 |
| DT 沒有節點 | 158 |

「沒有節點」大多是安全世界的東西（`*_S`、`WDT_S`、`TIMER_S_*`、`OTP_S`、`KEYLAD`）、
DDR PHY/MCU 內部、以及 Linux 不用的第二組 timer／看門狗（`TIMER_PMU`、`WDT_PMU`、`WDT_NPU`、`WDT_DDR`…）。

中斷號對帳（TRM Table 1-3 用的是 GIC INTID = SPI + 32）：DT `rktimer` `<GIC_SPI 289>` → `/proc/interrupts` 顯示 `GICv3 321` ✅。

## 2. CRU（Part1 ch2）— 用 TRM 公式自己算 PLL

TRM §2.3.3.1：`Fvco = 24 MHz × (M + K/65536) / P`，`Fout = Fvco / 2^S`（K 為 16-bit 二補數）。
`tools/pll_decode.py` 讀 CRU（`0xfd7c0000`）的 `*_CON0~2`，對照 Linux `clk_summary`：

```
pll        CON0     CON1     CON2      M    P  S      K         calc Hz        linux Hz
v0pll  000000c6 00002042 00000000    198    2  1      0      1188000000        24000000  (power-down)
aupll  00000106 00000082 ffff24dd    262    2  2   9437       786431992       786431991
cpll   000000fa 00000042 ffff0000    250    2  1      0      1500000000      1500000000
gpll   000000c6 00000042 ffff0000    198    2  1      0      1188000000      1188000000
npll   000001a9 00002083 00000000    425    3  2      0       850000000       850000000  (power-down)
```

- CPLL、GPLL **完全相等** ✅；AUPLL 差 1 Hz（Linux 整數除法捨去）✅
- AUPLL = 786.432 MHz = 48 kHz × 16384 —— 音訊專用的小數 PLL，K=9437 是為了湊出這個數
- `*_CON1` bit13（RESETB）=1 表示 PLL 在 power-down：V0PLL（給 VOP）、NPLL 目前沒用。
  Linux 對 NPLL 仍回報 850 MHz（照暫存器算的，但 PLL 其實關著）
- 小落差：TRM 說 `CON2[31:16]` 是 RO 0x0000 保留，實際讀到 `0xffff`（CPLL/GPLL/AUPLL）

**CPU 的 PLL（B0PLL/B1PLL/LPLL）在 Linux 看來是 24 MHz、enable=0**，因為 CPU 時脈是 SCMI 時脈，
由 BL31 管（見 `notes/rock5b_explore/03_dvfs_latency.md`）；Linux 的 CRU 視角對它們是過期的。

## 3. GRF（Part1 ch6）

TRM Table 6-1 列了 40 個 GRF/IOC 區塊（`PMU0_GRF 0xFD588000`、`SYS_GRF 0xFD58C000`、`BIGCORE0_GRF 0xFD590000`…）。

| 暫存器 | TRM | 實讀 | |
|--------|-----|------|---|
| `SYS_GRF_CHIP_ID`（`0xFD58C600`） | Reads as 0x3588 | `00003588` | ✅ |
| `SYS_GRF_OTP_KEY08`（+0x500） | 「OTP 位址 0x8 的值」 | `00000000` | ⚠ OTP[0x8] 是 0x32，對不上 |
| `SYS_GRF_OTP_KEY0D`（+0x504） | 「OTP 位址 0xd 的值」 | `47503241` | ⚠ 其實是 OTP byte 0x07~0x0a（`41 32 50 47` = "A2PG"） |
| `SYS_GRF_OTP_KEY0E`（+0x508） | 「OTP 位址 0xe 的值」 | `00000057` | ⚠ 其實是 OTP byte 0x0b（"W"） |

OTP 原始內容（`/sys/bus/nvmem/devices/rockchip-otp0/nvmem`）：`52 4b 35 88 12 fe 21 41 32 50 47 57 00 …`。
TRM 的「OTP 位址」單位看來不是 byte，**確切對應關係待查**。

CPU 叢集 GRF（BIGCORE0/1、LITCORE）的 PVTPLL 暫存器見 `rock5b_explore/01`：執行中讀值 ≈ 目前頻率（MHz）。

## 4. PMU（Part1 ch7）— 電源域

TRM 列出的電源域（VD_LOGIC 內 29 個 + CPU/NPU/VCODEC 等）vs Linux：

- `drivers/soc/rockchip/pm_domains.c` 的 `rk3588_pm_domains[]` 有 29 個，執行中 genpd 顯示 26 個
  （PHP、NVM、NVM0 在驅動表裡但 DT 沒用到）
- TRM 有、Linux 完全沒管的：`PD_CPU_0~7`（PSCI/BL31 管）、`PD_CENTER`、`PD_SECURE`、`PD_CRYPTO`、
  `PD_VOP_CLUSTER0~3`、`PD_VOP_DSC8K/4K`、`PD_VOP_ESMART`、`PD_PMU1`

各電源域開機 137 秒內的累計開/關時間（`/sys/kernel/debug/pm_genpd/*/active_time`）：

| 常開 | 開機時亮一下就關 | 從沒開過 |
|------|----------------|---------|
| pcie、sdmmc、usb、vop、vo0、vo1、audio | av1/vdpu/rkvdec0/1/venc0/1 各 ~2 s、vcodec 2.6 s、npu 0.85 s、gmac 0.87 s、sdio 0.97 s、gpu 62 ms、npu1/2/nputop 24 ms | fec、isp1、vi、rga30、rga31 |

「開機亮一下」是驅動 probe 時開電源讀 ID、然後 runtime suspend 關掉。

## 5. MMU600（Part1 ch8）

兩個 SMMUv3（`MMU600_PCIE 0xfc900000`、`MMU600_PHP 0xfcb00000`）在 DT 都是 **disabled**，
雖然 `CONFIG_ARM_SMMU_V3=y`。PCIe（NVMe、Wi-Fi、2.5GbE）的 DMA 因此**不經 IOMMU**，直接打實體位址。

實際在用的是 Rockchip 自家的小 IOMMU（`CONFIG_ROCKCHIP_IOMMU=y`），每個多媒體 IP 一個，共 16 個 group：

```
0: fdab0000.npu          1: fdb50000.vepu fdb50400.vdpu   2: fdb60000.rga   3: fdb70000.rga
4: fdb90000.jpegd        5~8: fdba{0,4,8,c}000.jpege-core  9: fdbb0000.iep
10/11: rkvenc-core ×2    12/13: rkvdec-core ×2            14: fdc70000.av1d  15: fdd90000.vop
```

## 6. MCU（Part1 ch9）、Mailbox（ch16）

TRM：Cortex-M0 子系統（16 KB cache、TCM `0xf6f00000`）。三個 Mailbox（`MAILBOX0 MCU_PMU`、`MAILBOX1 MCU_DDR`、`MAILBOX2 MCU_NPU`）
在 DT **全部 disabled**，TCM/CACHE 區塊 DT 沒節點。
→ 這台 Linux 不和任何 MCU 溝通（`CONFIG_ROCKCHIP_MBOX=y` 編了但沒用上）。無法做實驗。

## 7. Timer（Part1 ch10）

TRM：6 通道 timer（ch0~4 遞減、ch5 遞增）＋ 2 通道 timer ＋ HPTIMER，計數時脈 24 MHz。
板上只有 `TIMER_NS_0`（`0xfeae0000`）有節點，驅動 `timer-rockchip.c`，當 **broadcast clockevent**（`rk_timer`）用：

```
$ cat /sys/devices/system/clockevents/broadcast/current_device   → rk_timer
/proc/timer_list:  mult: 51539608  shift: 31   → 51539608/2^31 = 0.024 = 24 MHz ✅
                   max_delta_ns: 178956969070  → 2^32 / 24 MHz = 178.96 s ✅
```

暫存器（`timer_watch.py`，唯讀輪詢）：

| 暫存器 | TRM | 實讀 |
|--------|-----|------|
| `TIMER_6CH_REVISION`（+0xF0） | 0x11972006 | `11972006` ✅ |
| `CONTROLREG`（+0x10）啟動時 | bit0 EN、bit1 mode、bit2 int mask | `7` = 啟用 + user-defined + 開中斷，和驅動 `rk_timer_set_next_event()` 寫的旗標相同 ✅ |
| 倒數速度 | 24 MHz | 6 次抽樣 **23.93~24.10 MHz** ✅ |

觀察：輪詢程式**忙等**時 150 萬次取樣一次都沒看到它啟動；每次讀完 `sleep(0.3ms)` 讓出 CPU 後，60% 取樣都在倒數。
推測和「輪詢的 CPU 自己進 idle 才需要 broadcast」有關（**未證實**）。

## 8. GIC600（Part1 ch11）

TRM Table 11-1 設定 vs 暫存器（GICD `0xfe600000`、GICR `0xfe680000`、ITS `0xfe640000`/`0xfe660000`）：

| 項目 | TRM | 實讀 / Linux | |
|------|-----|-------------|---|
| SPI 數 | num_spis = 480 | `GICD_TYPER=007b040f`：ITLinesNumber=15 → (15+1)×32=512 INTID；dmesg「480 SPIs implemented」 | ✅ |
| LPI | lpi_support = true | TYPER.LPIS(bit17)=1；GICR_TYPER.PLPIS=1 | ✅ |
| ID 寬度 | did/vid = 16 | TYPER.IDbits = 15 → 16-bit | ✅ |
| 安全擴充 | disable_security = false | TYPER.SecurityExtn(bit10)=1 | ✅ |
| ITS 數 | its_count = 2 | 兩個 ITS，`GITS_IIDR` 都 = `0201743b`；dmesg 各配 8192 Devices | ✅ |
| 架構 | GICv3 | `GICD_PIDR2=3b` → ArchRev=3 | ✅ |
| 版本 | **r1p6** | `GICD_IIDR=0201743b`：Product 0x02、Variant 1、**Revision 7** | ⚠ 欄位值是 r1p7 |
| PPI | 「12 PPIs」 | dmesg「16 PPIs」 | ⚠ Linux 看架構的 16 個 |

LPI 實際用在 PCIe MSI：`/proc/interrupts` 有 13 條 `ITS-MSI`（`nvme0q0~8`、`PCIe PME`…）。

## 9. Debug（Part1 ch15）

DAP-LITE2（`0xfd100000`）在 DT 有 `debug@fd104000`（驅動 `rockchip_debug`，panic 時讀各 CPU 的 PC）
和 `cspmu@fd10c000`。`CONFIG_CORESIGHT` 沒開，沒有 ETM/trace。沒做實驗。

## 10. WDT（Part1 ch17）

`WDT_NS`（`0xfeaf0000`，DesignWare `snps,dw-wdt`），`/boot/dtbo/enable-wdt.dtbo` 讓它 okay。

**平常沒在跑**：`WDT_CR = 0x00000008`（TRM 重置值，WDT_EN=0）、`CCVR = 0x0000ffff`。沒有任何程式打開 `/dev/watchdog0`。

`COMP_VERSION`（+0xF8）= `3131302a`（ASCII "110*" → DW v1.10a）、`COMP_TYPE`（+0xFC）= `44570120`（"DW"）。

`wdt_probe.py`：開啟 → 設 60 s → 驅動挑成 **89 s**（TOP=15，2^31/24 MHz = 89.48 s，DW WDT 只有 16 種固定逾時）→ 量計數：
**24.001 MHz** ✅（TRM：24 MHz 或 32 kHz 可選）→ 餵狗 CCVR 回到 `7fff…` ✅ → magic close。

**關不掉**：magic close 後 `WDT_CR` 仍是 9。原因：`dw_wdt_stop()`（`drivers/watchdog/dw_wdt.c:292`）
在 DT 沒給 `resets` 時只設 `WDOG_HW_RUNNING` 就回傳；DW WDT 硬體本身一旦 EN=1 只能靠重置清掉。

接著核心會不會代為餵狗？分兩種情況，**都實測過**：

| 關閉方式 | 核心行為 | 實測 |
|---------|---------|------|
| magic close（寫 'V' 再 close） | `watchdog_need_worker()` 的 `(t && !active && hw_running)` 成立 → 每 timeout/2 餵一次 | `wdt_magic_test.sh`：t=44/85/127 s 各一次「剩 45 s → 88 s」✅ |
| 行程直接死掉（沒 'V'） | 印 `watchdog did not stop!`，`WDOG_ACTIVE` 保持，且 timeout(89 s) ≤ max_hw_heartbeat → **不餵**，讓它重開 | 07:50 板子真的被重開了（journal 有兩行 `did not stop!`）|

→ 也就是說，這顆看門狗**確實能重開板子**（之前 `rknpu` 筆記寫「沒能證明」，這次證明了——雖然是意外）。

## 11. Spinlock（Part1 ch32）

`hwspinlock@fe5a0000` okay，64 個 4-bit 狀態暫存器（TRM：非 0 時不能寫，寫 0 釋放）。
64 個全是 0；DT 沒有任何節點引用 `hwlocks`，Linux 沒人在用。

## 12. Share Memory（Part1 ch34）與 SCMI 共享記憶體

TRM：1 MB（`0xFF000000~0xFF0FFFFF`，4 塊 256 KB）。DT 把 `0xff001000` 起 0xef000 當 `mmio-sram`，
全部分給兩個 rkvdec 當解碼暫存（`rkvdec0_sram` 480 KB、`rkvdec1_sram` 476 KB）。

**SCMI 用的共享記憶體不在這裡**，而在 DDR 的 `0x10f000`（`scmi_shmem`，0x100 bytes，低於 Linux 記憶體起點 0x200000）。
改完大核頻率後讀它（SCMI shared-memory transport 格式）：

```
0010f000: 00000000 00000001 00000000 00000000     +0x04 channel_status = 1（FREE）
0010f010: 00000000 00000010 09f05006 00000000     +0x14 length=16；+0x18 header；+0x1c status=0（SUCCESS）
0010f020: 7829b800 00000000 ...                   +0x20 rate = 0x7829b800 = 2 016 000 000 Hz
```

header `0x09f05006`：message_id = 0x06（CLOCK_RATE_GET）、protocol = 0x14（Clock）、token = 0x27c。
回覆的 **2016000000 正是剛設的頻率** ✅。前一次（設 1416 MHz 後）token 是 0x27a，差 2 → 每次調頻走兩則訊息（SET + GET）。

## 13. DECOM（Part1 ch35）

硬體解壓器（GZIP/LZ4/DEFLATE/ZLIB），`decompress@fea80000` 在 DT 是 **disabled**。沒法實驗。

## 14. Interconnect（Part2 ch1）— QoS

QoS Generator 暫存器（TRM §1.4.3）：`CoreId` 重置 0x67B76E04、`RevisionId` 0x3C9D4E00、`Priority` 0x80000000、`Mode` 3、`Saturation` 0x40。

安全能讀的只有 CPU 端（DSU，永遠有時脈）：

```
0xfe008000 (DSU_M0): a049bb04 3c9d4e00 80000000 00000003 00001638 00000040 00000000
0xfe008800 (DSU_M1): 0abb2904 3c9d4e00 80000000 00000003 00001638 00000040 00000000
```

- RevisionId、Priority、Mode、Saturation 和 TRM 重置值相同 ✅
- CoreId 不是 0x67B76E04 —— 每個 QoS generator 的 CoreId 不同（TRM 欄位說明是「Core ID and checksum」），**TRM 只列了一個**
- Bandwidth = 0x1638（TRM 重置 0），有人（韌體）設過

VOP 的 QoS（`0xfdf82000`）就是讀了會卡死的那個，見本章開頭。
