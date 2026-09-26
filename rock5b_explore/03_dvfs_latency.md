# 03　換一次 CPU 頻率要多久？7.5 ms，其中 93% 在等 I2C

> 實驗日期：2026-09-26　工具：`tools/dvfs_trace.sh`、`tools/reg_trace.sh`、`tools/i2c_ev.sh`
> 原始資料：`data/dvfs_trace.txt`、`data/reg_trace.txt`、`data/i2c_ev.txt`

## 一句話結論

大核從 408 MHz 升到 2256 MHz 一次要 **7.5 ms**。
真正改時脈（SMC 陷入 ATF 韌體）約 **40 µs**，電壓爬升的物理等待約 **0.3 ms**，
剩下約 **7 ms** 是 **14 筆 I2C 交易**，其中 **只有 1 筆是有用的寫入**。

---

## 1. 路徑長什麼樣

```
cpufreq (userspace/ondemand)
 └ cpufreq-dt → rockchip_cpufreq_opp_set_rate()      drivers/cpufreq/rockchip-cpufreq.c:530
    └ dev_pm_opp_set_rate()
       ├ _set_opp() → cpu_opp_config_regulators()
       │   └ rockchip_opp_config_regulators()        drivers/soc/rockchip/rockchip_opp_select.c:2263
       │       ├ regulator_set_voltage(cpu-supply)   ← 升頻：先升壓
       │       ├ regulator_set_voltage(mem-supply)
       │       └ rk3588_cpu_set_read_margin()
       └ clk_set_rate() → scmi_clk → smc_send_message()
                            └ arm_smccc_1_1_invoke(0x82000010)   drivers/firmware/arm_scmi/smc.c:207
                               → 陷入 EL3 BL31，由韌體改 PLL
```

CPU、DDR、GPU、NPU 的時脈都是 **SCMI 時脈**（`clk_summary` 裡的 `scmi_clk_cpub01`、`scmi_clk_ddr`…），
Linux 自己不碰 PLL，而是用 SMC 請 BL31 代勞（`rk3588s.dtsi:1865` `compatible = "arm,scmi-smc"`、`arm,smc-id = <0x82000010>`）。

## 2. function_graph：時間花在哪

`tools/dvfs_trace.sh`：大核 policy4 切到 `userspace`，
在 408/2256/1800/1608 MHz 之間切 7 次，`set_graph_function = dev_pm_opp_set_rate`。

一次 408 → 2256 MHz 的摘錄：

```
 5)               |  dev_pm_opp_set_rate() {
 ...
 5)               |          rockchip_opp_set_volt() {
 5) # 5283.833 us |            regulator_set_voltage();     ← cpu-supply
 5)               |          rockchip_opp_set_volt() {
 5) # 2823.041 us |            regulator_set_voltage();     ← mem-supply
 5) + 30.042 us   |            rk3588_cpu_set_read_margin();
 5) # 8183.000 us |        }
 5)               |        clk_set_rate() {
 5) + 37.333 us   |            clk_change_rate();           ← SCMI/SMC 真正改頻
 5) + 69.708 us   |        }
 5) # 8269.333 us |    }
```

7 次切換中，每次兩個 `regulator_set_voltage` 分別 2.3~2.9 ms 和 4.3~5.3 ms。
（function_graph 本身會拉長時間，下面用 tracepoint 量不失真的版本。）

## 3. 往下追：每讀一次電壓都走 I2C

`tools/reg_trace.sh`（`set_graph_function = regulator_set_voltage`，深度 12）：

```
 5)               |                rk860x_get_voltage() {
 5) ! 623.291 us  |                        _regmap_bus_read();
 ...
 5) # 1411.958 us |                } /* rk860x_get_voltage */
 5)               |                rk860x_is_enabled() {
 5) ! 597.333 us  |                        _regmap_raw_read();
 ...
 5)               |                      _regmap_write() {
 5) ! 485.916 us  |                        _regmap_bus_raw_write();
 ...
 5)               |              regulator_set_voltage_time_sel() {
 5)               |                usleep_range_state() {
 5) ! 356.125 us  |                }
```

**每一筆 I2C 讀取約 0.6 ms。**

## 4. 用 tracepoint 數 I2C 交易（不失真）

`tools/i2c_ev.sh`：開 `events/i2c/*`（濾 `adapter_nr==0`）與 `power/cpu_frequency`，只做一次 408 → 2256。

```
3498.304751: i2c_write: i2c-0 a=042 l=1 [08]       ┐
3498.305360: i2c_reply: i2c-0 a=042 l=1 [a0]       ┘ 讀 0x08 (MAX_SET)
3498.305377: i2c_write: i2c-0 a=042 l=1 [06]       ┐
3498.305877: i2c_reply: i2c-0 a=042 l=1 [1c]       ┘ 讀 0x06 (VSEL0_B) = 0x1c
3498.305892: i2c_write: i2c-0 a=042 l=1 [00]       ┐
3498.306390: i2c_reply: i2c-0 a=042 l=1 [97]       ┘ 讀 0x00 (VSEL0_A，看 BUCK_EN)
   … 08, 06, 08, 06, 06 又讀了一輪 …
3498.308942: i2c_write: i2c-0 a=042 l=2 [06-50]    ← 唯一的寫入：VSEL0_B = 0x50
3498.309318: i2c_result: n=1 ret=1
             （usleep 等電壓爬升 313 µs）
3498.309631: i2c_write: i2c-0 a=042 l=1 [08]       ┐
   … 08, 06, 00, 08, 06 …                           ┘ mem-supply 那次呼叫：電壓沒變，但又讀 5 次
3498.312261: cpu_frequency: state=2256000 cpu_id=4
```

| 項目 | 數值 |
|------|------|
| 從第一筆 I2C 到 `cpu_frequency` 事件 | **7.51 ms** |
| I2C 交易總數 | **14**（13 讀 + 1 寫） |
| 每筆讀取 | ≈ 0.50 ms |
| 電壓爬升等待 | 0.31 ms |

### 暫存器值對得上

rk8602（地址 0x42）電壓 = 500 mV + 6.25 mV × 值
（`drivers/regulator/rk860x-regulator.c:338` `vsel_min = 500000`）：

- `0x1c` = 28 → 500 + 175 = **675 mV**（408 MHz 的 OPP）✅
- `0x50` = 80 → 500 + 500 = **1000 mV**（2256 MHz 的 OPP）✅

爬升等待：325 mV ÷ `regulator-ramp-delay = <2300>`（2.3 mV/µs）= 141 µs，加上 `usleep_range` 的寬限，量到 313 µs，合理。

## 5. 為什麼這麼多讀、這麼慢

**(a) regmap 沒開快取**

```c
/* drivers/regulator/rk860x-regulator.c:404 */
static const struct regmap_config rk860x_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};                          /* 沒有 .cache_type → REGCACHE_NONE */
```

每次 `get_voltage` 都真的去讀晶片，而 `rk860x_get_voltage()` 一次就讀兩個暫存器（0x08 + 0x06）。

**(b) cpu-supply 和 mem-supply 是同一顆晶片**

```dts
/* rk3588-rock-5b.dts:280 */
vdd_cpu_big0_s0: vdd_cpu_big0_mem_s0: rk8602@42 {
```

一個節點兩個標籤。OPP 核心把它當兩個 regulator，第二次呼叫明明電壓已經對了，還是再讀 5 次。

**(c) I2C0 跑 100 kHz**

`rk3588-rock-5b.dts` 的 `&i2c0` 沒寫 `clock-frequency`，
`i2c-rk3x.c:1580` `i2c_parse_fw_timings(..., true)` 用預設 100 kHz。
一筆「寫位址 + 重啟 + 讀 1 byte」約 40 bit，100 kHz 下 0.4 ms，加上間隔 ≈ 0.5 ms，和實測一致。

## 6. 有多重要？

- ondemand 預設取樣週期是毫秒級，一次升頻卡 7.5 ms，突發負載的前幾毫秒都跑在低頻。
- 如果用 schedutil（第 8 章做過），頻繁小幅調頻時這 7.5 ms 會一直被付出。

## 7. 可以怎麼改（**沒有實際改**，只是推估）

| 改法 | 估計效果 |
|------|---------|
| DT `&i2c0 { clock-frequency = <400000>; }`（rk8602 支援 400 kHz 與否需查規格書） | 每筆 0.5 → ~0.13 ms，總計 ~2 ms |
| rk860x regmap 加 `.cache_type = REGCACHE_RBTREE` 並把 MONITOR 之類的暫存器標成 volatile | 13 筆讀取大多消失 |
| 兩者都做 | 1 筆寫入 + 爬升 ≈ 0.5 ms |

這些要改 DT／驅動並重開機才能驗證，這次沒動。

## 8. 對照：SCMI 那一段其實很快

`clk_change_rate` 在 function_graph 下 37 µs（含追蹤開銷），包含一次 SMC 陷入 BL31、
韌體改 PLL、回來。和 7.5 ms 比起來可以忽略。
