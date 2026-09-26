# 01　晶片體質：為什麼這顆 RK3588 大核只跑到 2.256 GHz，不是 2.4 GHz

> 實驗日期：2026-09-26　機台：ROCK 5B（`192.168.68.58`，Linux 6.1.115+）
> 對照原始碼：本 repo 的 `drivers/soc/rockchip/rockchip_opp_select.c`、
> `drivers/cpufreq/rockchip-cpufreq.c`、`arch/arm64/boot/dts/rockchip/rk3588s.dtsi`

## 一句話結論

RK3588 規格寫大核 2.4 GHz，但這片板子的大核最高只給 **2256 MHz**。
原因不是設定錯，而是核心**每次開機都會量一次晶片的「體質分數」（PVTM）**，
再用分數去查 DT 裡的表，決定 (1) 每個頻率要給多少電壓、(2) 最高能開到多少。
這顆晶片大核分數 1676，**剛好比分級邊界高 1 分**。

---

## 1. 現象

```
$ cat /sys/devices/system/cpu/cpufreq/policy4/scaling_available_frequencies
408000 600000 816000 1008000 1200000 1416000 1608000 1800000 2016000 2208000 2256000
```

DT（`rk3588s.dtsi` 的 `cluster1_opp_table`）其實列了 2304 / 2352 / 2400 MHz，但都沒出現。

## 2. 開機時核心印了什麼

> 若 dmesg 已被序列埠訊息洗掉（見 05 §1），改用 `sudo journalctl -k -b | grep ...`。

```
$ sudo dmesg | grep -E "cpu cpu[046]: (bin|leakage|pvtm|pvtm-volt-sel|l=)"
cpu cpu0: bin=0
cpu cpu0: leakage=11
cpu cpu0: pvtm=1460
cpu cpu0: pvtm-volt-sel=3
cpu cpu4: bin=0
cpu cpu4: leakage=9
cpu cpu4: pvtm=1676
cpu cpu4: pvtm-volt-sel=4
cpu cpu6: bin=0
cpu cpu6: leakage=9
cpu cpu6: pvtm=1689
cpu cpu6: pvtm-volt-sel=4
cpu cpu0: l=15000 h=85000 hyst=5000 l_limit=0 h_limit=1608000000 h_table=0
cpu cpu4: l=15000 h=85000 hyst=5000 l_limit=0 h_limit=2208000000 h_table=0
```

三種數字，來源不同：

| 名稱 | 來源 | 意思 |
|------|------|------|
| `bin` | OTP（晶片出廠燒死的 eFuse） | 晶片型號：0=RK3588、1=RK3588M、2=RK3588J、3=RK3588S |
| `leakage` | OTP | 出廠量的漏電流 |
| `pvtm` | **開機當下現場量** | 晶片內建環形振盪器能跑多快（越大=晶片越快） |

## 3. 自己讀 OTP，和 dmesg 對帳

OTP 在 `/sys/bus/nvmem/devices/rockchip-otp0/nvmem`。每個欄位的位址寫在 `rk3588s.dtsi` 的 `otp: otp@fecc0000` 節點。

```
$ sudo od -An -tx1 -v -N 64 /sys/bus/nvmem/devices/rockchip-otp0/nvmem
 52 4b 35 88 12 fe 21 41 32 50 47 57 00 00 00 00
 00 00 00 00 12 15 1a 09 09 0b 1f 0f 08 00 00 00
 00 00 00 00 00 00 00 00 08 0c 00 00 00 0e d8 9b
 4f 25 09 30 09 19 07 1a 07 fa 03 ea 03 00 00 00
```

| DT 欄位 | 位址 | 原始值 | 解讀 | dmesg |
|---------|------|--------|------|-------|
| `cpu_code` | 0x02 | `35 88` | 「3588」 | — |
| `specification_serial_number` | 0x06 bits[4:0] | `0x41` → `0x01` | 不是 0xd/0xa/0x13 → 一般 RK3588 | `bin=0` ✅ |
| `cpub0_leakage` | 0x17 | `0x09` | 9 | cpu4 `leakage=9` ✅ |
| `cpub1_leakage` | 0x18 | `0x09` | 9 | cpu6 `leakage=9` ✅ |
| `cpul_leakage` | 0x19 | `0x0b` | 11 | cpu0 `leakage=11` ✅ |
| `log_leakage` | 0x1a | `0x1f` | 31 | dmc/vop `leakage=31` ✅ |
| `gpu_leakage` | 0x1b | `0x0f` | 15 | — |
| `npu_leakage` | 0x28 | `0x08` | 8 | NPU `leakage=8` ✅ |
| `cpul_opp_info` 等 | 0x3d~0x60 | 全 0 | 沒燒 → 不用 OTP 調表 | — |

`bin` 的判斷在 `rockchip_opp_select.c:1320-1358`（`rockchip_get_soc_info()`）。

## 4. pvtm 怎麼量（開機時）

`rockchip_opp_select.c:1049` `rockchip_get_pvtm_pvtpll()`：

1. 把 CPU 時脈設成 `rockchip,pvtm-freq`（**1608 MHz**）
2. 把電壓設成 `rockchip,pvtm-volt`（**750 mV**）
3. 等 `rockchip,pvtm-sample-time`（1100 µs）
4. 讀 GRF 暫存器（大核叢集 1 = `bigcore0_grf` `0xfd590000` + `0x18`）
5. 溫度補償：`pvtm += (T − 25°C) × 270 / 1000`（`rockchip,pvtm-temp-prop = <270 270>`）

## 5. 分數 → 電壓等級（volt-sel）

`cluster1_opp_table` 的 `rockchip,pvtm-voltage-sel`（`rk3588s.dtsi:940`）：

| pvtm 區間 | sel |
|-----------|-----|
| 0 – 1595 | 0 |
| 1596 – 1615 | 1 |
| 1616 – 1640 | 2 |
| 1641 – **1675** | 3 |
| **1676** – 1710 | **4** ← cpu4 = 1676、cpu6 = 1689 |
| 1711 – 1743 | 5 |
| 1744 – 1776 | 6 |
| 1777 – 9999 | 7 |

小核（`cluster0_opp_table`）：1459–1482 → 3，cpu0 = 1460 → **sel 3** ✅

## 6. 等級 → 電壓：逐點對帳全部命中

sel 決定用 `opp-microvolt-L<sel>`。從 debugfs 讀實際電壓：

```
$ sudo cat /sys/kernel/debug/opp/opp_summary
```

**大核（L4）**

| 頻率 | DT 預設 | DT `-L4` | 實際 | |
|------|---------|----------|------|---|
| ≤1200 MHz | 675 mV | （無） | 675 mV | ✅ |
| 1416 | 725 | 700 | 700 | ✅ |
| 1608 | 762.5 | 725 | 725 | ✅ |
| 1800 | 850 | 800 | 800 | ✅ |
| 2016 | 925 | 875 | 875 | ✅ |
| 2208 | 987.5 | 962.5 | 962.5 | ✅ |
| 2256 | 1000 | （無） | 1000 | ✅ |

**小核（L3）**：1200→687.5、1416→725、1608→812.5、1800→912.5 mV，**全部命中** ✅

> 小怪事：大核 2208 MHz 的 `-L4`（962.5 mV）比 `-L3`（950 mV）還高，不是單調遞減。
> 其他頻率都是等級越高電壓越低。原因不明，列為待查。

## 7. 等級 → 最高頻率（真正的答案）

`rk3588_set_supported_hw()`（`rockchip-cpufreq.c:289`）：

```c
opp_info->supported_hw[0] = BIT(bin);            /* 型號 */
opp_info->supported_hw[1] = BIT(opp_info->volt_sel);  /* 速度等級 */
```

OPP 核心只留下 `opp-supported-hw` 和這兩個 mask 有交集的頻率。DT 的最後四個頻率：

| 頻率 | `opp-supported-hw` 第 2 格 | 允許的 sel |
|------|---------------------------|-----------|
| 2256 MHz | `0x13` = bit 0,1,4 | 0, 1, **4** |
| 2304 MHz | `0x24` = bit 2,5 | 2, 5 |
| 2352 MHz | `0x48` = bit 3,6 | 3, 6 |
| 2400 MHz | `0x80` = bit 7 | 7 |

這顆大核 sel = 4 → `BIT(4) = 0x10` → **只有 2256 過關**。

> ⚠ 反直覺之處：sel 3（分數**較低**）反而會開到 2352 MHz，比 sel 4 高。
> 這個分組（0,1,4 / 2,5 / 3,6 / 7）在原始碼與 TRM 都沒有解釋。
> 我的猜測是：sel 0~7 其實是「電壓組 × 頻率組」的編碼，不是單純的好壞排序。**未證實**。

## 8. 分數「卡在邊界」代表什麼

cpu4 = **1676**，sel 4 的下限正好是 **1676**。少 1 分就會變 sel 3，後果：

| | sel 4（現在） | sel 3（少 1 分） |
|---|---|---|
| 最高頻 | 2256 MHz | **2352 MHz** |
| 1608 MHz 電壓 | 725 mV | 737.5 mV |
| low-length 模式 | 不開（`pvtm-low-len-sel = 3`，sel > 3） | 開（`rk3588_change_length()`） |

而 pvtm 是**每次開機現場量**的，量測雜訊、開機溫度、電源狀況都可能讓它差 1 分。
也就是說：**這片板子每次開機的大核最高頻率，理論上可能不一樣。**

### 跨開機的實測（2026-09-26 補）

> 更正：原稿說「journald 不是持久化的」是**錯的**——我把 `journalctl --list-boots | wc -l` 的 3（含標題）誤讀了。
> 板子有保存歷次開機紀錄，可以用 `journalctl -k -b -1` 查上一次開機。

同一天發生了一次非預期的重開機（看門狗實驗，見 `notes/rk3588_trm/`），多了一組量測：

| 開機 | cpu0（小核） | cpu4（大核 1） | cpu6（大核 2） | NPU |
|------|------------|---------------|---------------|-----|
| boot -1 | 1460 | **1676** | 1689 | 856 |
| boot 0 | 1460 | **1678** | 1696 | 859 |

同一顆晶片、兩次開機，大核分數差 2~7 分，NPU 差 3 分。
cpu4 兩次都落在 sel 4（≥1676），但離邊界只有 0~2 分；
以這樣的變動幅度，**哪天開機量到 ≤1675、變成 sel 3 是完全可能的**。要看到實際翻轉，還需要更多次開機的紀錄。

## 9. 執行中讀 PVTPLL 暫存器：它其實是「現在的頻率」

用 `/dev/mem` 直接讀（`tools/peek.py`，只讀）：

```
$ sudo python3 peek.py 0xfd590000:0x14:2
```

把大核叢集 1 用 `userspace` governor 固定在各頻率，讀 `+0x14/+0x18`：

| 設定頻率 | +0x14 | +0x18 |
|---------|-------|-------|
| 408 MHz | 1435 | 1447 |
| 1008 | 988 | 989 |
| 1608 | 1618 | 1607 |
| 1800 | 1804 | 1815 |
| 2016 | 1989 | 1996 |
| 2208 | 2171 | 2175 |
| 2256 | 2253 | 2242 |

從 1008 MHz 起，讀值 ≈ 目前頻率（MHz）。408 MHz 時對不上，
推測是低頻時 CPU 不用 PVTPLL，改用一般 PLL，暫存器留著舊值（**推測**）。

### 想驗證溫度補償係數 0.27/°C → 失敗，但學到東西

實驗（`tools/pvtm_temp.sh`）：大核叢集 1 固定 1608 MHz／725 mV，關風扇，
用其他 6 顆核心加熱，43°C → 72°C，每秒讀一次。

```
n=416 samples, slope = -0.132 count/°C, 範圍 1588.5 ~ 1610.5
```

幾乎是平的。原因：**執行中的 PVTPLL 是閉迴路**，會自己調整環長把頻率拉回目標值，
所以它不再反映晶片快慢。開機量測時是「開迴路」量法，兩者不能比。
**結論：執行中無法重現開機的 pvtm 量測**，要驗證 0.27 係數得在開機流程內做。

## 10. 其他裝置也有同一套

| 裝置 | 開機訊息 |
|------|---------|
| NPU | `leakage=8`、`pvtm=856`、`pvtm-volt-sel=2` |
| DMC（記憶體控制器） | `leakage=31`、`leakage-volt-sel=0`、`soc version=0, speed=0` |
| VOP（顯示） | `leakage=31`、`leakage-volt-sel=0`；接著 `no regulator (vop) found` 失敗（無害） |

每個模組都用自己的漏電流／pvtm 選電壓，所以同型號的兩片 RK3588，電壓表可能不同。
