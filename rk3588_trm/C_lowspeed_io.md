# C　低速周邊：DMA、TSADC、PWM、UART、GPIO、I2C、SARADC、CAN、FSPI、SPI

> TRM：Part1 ch12/13/14/18/19/20/21/26/29/30/31
> 工具：`tools/tsadc_compare.py`、`tools/safe_mmio.py`

## 板上啟用狀況（`data/trm_vs_dt.csv`）

| 模組 | TRM 數量 | 板上啟用 | 用途 |
|------|---------|---------|------|
| I2C | 9（I2C0~8） | I2C0、1、4、6、7 | 0：rk8602/rk8603 CPU 電壓晶片；1：rk8602（NPU）；4：fusb302（USB-C PD）；6：hym8563 RTC；7：es8316 音訊 codec |
| UART | 10（UART0~9） | UART6 | 藍牙（`hci0`）。除錯口 UART2 由 fiq_debugger 接管成 `ttyFIQ0`，DT 節點是 disabled |
| SPI | 5（SPI0~4） | SPI2 | rk806 PMIC（`chip id: RK806, ver:0x2`） |
| FSPI | 1 | 1 | 16 MB SPI NOR（`mtd0 "loader"`） |
| PWM | 4 組 × 4 通道 | PMU PWM 的 ch1（`fd8b0010`） | 風扇 |
| DMAC | 3 × PL330 | 3 | I2S、SPDIF、SPI2 |
| TSADC | 1（7 感測器） | 1 | 7 個 thermal zone |
| SARADC | 1（8 通道） | 1 | ch3：耳機孔按鍵偵測 |
| GPIO | 5 bank | 5 | `gpiochip0~4`：GPIO 0-159（5 × 32）✅ |
| CAN | 3 | 0（全部 disabled） | — |
| SDMMC_BUFFER | 1 | DT 無節點 | — |

## 1. DMAC（ch12）— DMA 請求號全部對上 TRM Table 1-4

TRM：DMAC0/1/2 分別支援 24/23/21 個周邊請求，每個 PL330 8 條通道。
`/sys/kernel/debug/dmaengine/summary` 看到的使用者 vs DT 的 `dmas = <&dmacN 請求號>` vs TRM Table 1-4：

| 使用者 | DT | TRM | |
|--------|----|-----|---|
| I2S0_8CH tx/rx（`fe470000`） | dmac0 0 / 1 | DMAC0：0 I2S0_8ch_tx、1 I2S0_8ch_rx | ✅ |
| SPDIF_TX2（`fddb0000`） | dmac1 6 | 6 SPDIF_tx2 | ✅ |
| SPI2 tx/rx（`feb20000`） | dmac1 15 / 16 | 15 SPI2_tx、16 SPI2_rx | ✅ |
| I2S5_8CH tx（`fddf0000`） | dmac2 2 | 2 I2S5_8ch_tx | ✅ |
| I2S6_8CH tx（`fddf4000`） | dmac2 4 | 4 I2S6_8ch_tx | ✅ |
| I2S7_8CH rx（`fddf8000`） | dmac2 21 | 21 I2S7_8ch_rx | ✅ |

Linux 顯示「number of channels: 32」是它把周邊請求數當通道數報，不是 PL330 的 8 條硬體通道。

## 2. TSADC（ch14）— TRM 的對照表和 Linux 用的不一樣

TRM Table 14-1 vs `drivers/thermal/rockchip_thermal.c:786` `rk3588_code_table`：

| 溫度 | TRM 碼 | 驅動碼 |
|------|-------|-------|
| -40°C | 220 | **215** |
| 25°C | 285 | 285 |
| 85°C | 345 | **350** |
| 125°C | 385 | **395** |

TRM 自己也註明「應依矽片實測更新」。`tsadc_compare.py` 直接讀 `TSADC_DATA0~6`（`0xfec0002c` 起），兩種內插並排：

```
ch0 soc        code=303  TRM→ 43.00°C  driver→ 41.62°C  thermal_zone= 40.69°C
ch1 bigcore0   code=303  TRM→ 43.00°C  driver→ 41.62°C  thermal_zone= 41.62°C
ch2 bigcore1   code=303  TRM→ 43.00°C  driver→ 41.62°C  thermal_zone= 41.62°C
ch3 littlecore code=303  TRM→ 43.00°C  driver→ 41.62°C  thermal_zone= 41.62°C
ch4 center     code=302  TRM→ 42.00°C  driver→ 40.69°C  thermal_zone= 40.69°C
ch5 gpu        code=301  TRM→ 41.00°C  driver→ 39.77°C  thermal_zone= 40.69°C
ch6 npu        code=302  TRM→ 42.00°C  driver→ 40.69°C  thermal_zone= 40.69°C
```

- thermal_zone 和**驅動表**的換算一致（差一格是兩次讀取之間溫度變了）✅
- 驅動表 25~85°C 段斜率 = 60/65 = **0.923°C/碼**，這正是 `rock5b_explore/02` 看到溫度「每格跳 0.92°C」的原因
- 影響：Linux 說 **85°C**（碼 350）時，照 TRM 表是 **90°C**。system_monitor 的 85°C 降頻門檻，用 TRM 的尺來量其實是 90°C

## 3. PWM（ch18）— 風扇

風扇用 PMU PWM ch1（`fd8b0010`）。`safe_mmio.py` 拒絕讀它的暫存器：`clocks disabled: pclk_pmu1pwm`
（PWM 輸出時只需要功能時脈 `clk_pmu1pwm` 24 MHz，APB 時脈平常關著），所以改看 debugfs：

```
pwm-0 (pwm-fan): requested enabled period: 60000 ns duty: 59999 ns polarity: normal
```

duty 是 59999 不是 60000：`drivers/hwmon/pwm-fan.c:218` `duty = DIV_ROUND_UP(pwm × (period − 1), 255)`，pwm=255 時 = period − 1 ✅。
24 MHz 下一個週期 = 1440 個時脈 → 風扇 PWM 頻率 16.7 kHz。

## 4. UART（ch19）

- UART6（`feb90000`）：16550A，`base_baud = 1500000`，給藍牙用
- 除錯口：`console=ttyFIQ0,1500000n8`，UART2 被 fiq_debugger 包成 `ttyFIQ0`
  （這條線上的「每 17 秒登入失敗」問題見 `rock5b_explore/05 §1`）

## 5. GPIO（ch20）

5 個 bank（`fd8a0000`、`fec20000~fec50000`）× 32 = GPIO 0~159 ✅。另有 `gpiochip5`（509-511）是 rk806 PMIC 的 GPIO，不在 SoC 裡。
GPIO 節點掛在 `pinctrl` 下，所以 `trm_vs_dt.csv` 顯示「no-node」（我的腳本只掃 root 下的節點）。

## 6. I2C（ch21）

5 條啟用的匯流排**全部沒寫 `clock-frequency`** → 全跑預設 100 kHz（TRM 支援到 1 Mbps）。
這就是 `rock5b_explore/03` 裡「換一次 CPU 頻率 7.5 ms」的 I2C 慢速根源之一。

裝置清單（`/sys/bus/i2c/devices`，只讀 sysfs、沒有掃描匯流排）：

```
0-0042 rk8602   0-0043 rk8603   1-0042 rk8602   4-0022 fusb302   6-0051 hym8563   7-0011 es8316
```

## 7. SARADC（ch26）

TRM：8 通道、12 bit、輸入 0~1.8 V。實測 `in_voltage_scale = 0.439453125` mV = 1800 / 4096 ✅（`vref-supply = avcc_1v8_s0`）。

```
ch0=4072 ch1=4092 ch2=3203 ch3=3193 ch4=4093 ch5=3641 ch6=3509 ch7=2941
```

只有 ch3 在 DT 有用途：`io-channels = <&saradc 3>`，耳機孔的線控按鍵（play/pause 門檻 2 mV，放開 1.8 V）。
ch3 = 3193 → 1.403 V。其他通道沒有 DT 用途（多半是上拉或浮接）。

## 8. CAN（ch29）

三個 CAN 都 disabled，ROCK 5B 沒拉出 CAN 收發器。無法實驗。

## 9. FSPI（ch30）— SPI NOR

`fe2b0000.spi`（驅動 `sfc`），`sclk_sfc = 50 MHz`。整顆 16 MB 讀一次：

```
$ sudo dd if=/dev/mtd0 of=/dev/null bs=64k
16777216 bytes (17 MB, 16 MiB) copied, 0.678183 s, 24.7 MB/s
```

50 MHz 下：單線上限 6.25 MB/s、四線（Quad）上限 25 MB/s。實測 24.7 MB/s → **一定是 Quad IO**，效率 99% ✅（TRM：支援 Single/Dual/Quad）。

## 10. SPI（ch31）

SPI2 接 rk806 PMIC（`rk806 spi2.0: chip id: RK806,ver:0x2`），用 DMA（dmac1 15/16）。其他 SPI 全部 disabled。

## 11. SDMMC_BUFFER（ch13）

DT 無節點，TRM 章節只有 75 行。沒有可驗證的東西。
