# 02　散熱：風扇永遠全速、降頻的其實不是 thermal 框架

> 實驗日期：2026-09-26　工具：`tools/heat.c`、`tools/thermlog.sh`、`tools/fan_off_run.sh`、`tools/load_run.sh`
> 原始資料：`data/run_fan255.csv`、`data/run_fan0.csv`、`data/run_stepwise.csv`

## 一句話結論

1. **風扇從開機起就卡在 100%**，溫度 40°C 也一樣。預設的 `power_allocator` 根本不管風扇。
2. 關掉風扇滿載，約 9 分鐘到 85°C，**把頻率壓下來的是 Rockchip 自己的 `system_monitor`**，
   不是 Linux thermal 框架（所有 cpufreq 冷卻裝置 state 一直是 0）。
3. 換成 `step_wise` 風扇就會照設計工作（60°C 以下停、以上開），**但之後換不回 `power_allocator`**
   ——這是 6.1 `power_allocator` 綁定時檢查的副作用。

---

## 0. 背景：這台的散熱設定

```
thermal_zone0 soc-thermal   policy=power_allocator
  trip0 60°C passive  (rock-5b.dts 把 rk3588s.dtsi 的 75°C 改成 60°C)
  trip1 85°C passive
  trip2 115°C critical
cooling devices:
  0 devfreq-dmc  1 devfreq-gpu  2 cpufreq-cpu0  3 cpufreq-cpu4  4 cpufreq-cpu6  5 pwm-fan
```

`rk3588-rock-5b.dts:923-942` 把 trip0 改成 60°C，並把風扇接到 trip0（60°C）與 trip1（85°C），權重 8192；
`cooling-levels = <0 64 128 192 255>` → 風扇 state 0~4。

## 1. 加熱器：先修好一個「看起來會燒，其實只燒一半」的 bug

`heat.c` 每顆核心跑 8 條 NEON FMA 鏈。第一版寫成 `a = vfmaq_f32(c, a, m)`（`c + a*m`），
反組譯發現 FMLA 的累加器是 `c`，編譯器只好每次 `mov v18, c` 再算，**8 條鏈全擠在 v18 上**：

```
d00: mov  v18.16b, v0.16b
d08: fmla v18.4s, v7.4s, v1.4s
d0c: mov  v7.16b, v18.16b      ← 每條鏈都經過 v18，變成串行
```

改成 `a = vfmaq_f32(a, c, m)`（`a + c*m`，累加器就是自己）後每條鏈獨立：

| 版本 | A55 (1.8 GHz) | A76 (2.256 GHz) |
|------|---------------|-----------------|
| 錯誤版 | 2.4 GFLOPS | 9.7 GFLOPS |
| 修正版 | **14.35 GFLOPS = 7.97 FLOP/週期** | **35.8 GFLOPS = 15.9 FLOP/週期** |

修正版剛好打到理論峰值：
- A76：2 條 128-bit FMA 管線 × 4 lanes × 2 FLOP = **16 FLOP/週期** ✅
- A55：每週期 1 條 128-bit FMA = **8 FLOP/週期** ✅

## 2. 實驗 A：預設狀態（風扇 100%）滿載 5 分鐘

`./heat 300`（8 核全開）＋每秒記錄溫度與頻率。

| 時間 | soc 溫度 | 頻率 (小/大/大) | 風扇 |
|------|---------|----------------|------|
| 0 s | 39.8°C | 1800/2256/2256 | 4 |
| 60 s | 52.7°C | 同上 | 4 |
| 140 s | 55.5°C | 同上 | 4 |
| 240 s | 56.4°C（最高） | 同上 | 4 |

**全程沒降頻**，溫度停在 56°C，沒碰到 60°C 的 trip0。

> 注意：這一輪用的是舊版 `thermlog.sh`，每行其實間隔約 1.15 秒（表上時間是行號）。
> 之後的實驗都改成用 `/proc/uptime` 記真實時間。

## 3. 為什麼風扇 40°C 也全速？

```
$ cat /sys/class/thermal/cooling_device5/cur_state   → 4
$ cat /sys/class/hwmon/hwmon*/pwm1                  → 255
```

兩件事疊在一起：

**(a) pwm-fan 驅動開機就設成最大**

`drivers/hwmon/pwm-fan.c:623` `set_pwm(ctx, MAX_PWM)`、`:718` `ctx->pwm_fan_state = ctx->pwm_fan_max_state`。

**(b) power_allocator 只管「功率元件」**

`drivers/thermal/gov_power_allocator.c:565` `allow_maximum_power()`：

```c
if ((instance->trip != params->trip_max_desired_temperature) ||
    (!cdev_is_power_actor(instance->cdev)))
        continue;          /* 風扇不是 power actor → 直接跳過 */
```

`allocate_power()` 也只算 power actor。風扇沒有 `get_requested_power/state2power/power2state`，
所以 **power_allocator 從頭到尾不會動風扇**，它就一直停在 probe 時的 255。

DT 替風扇寫的 trip0/trip1 對應，是給 `step_wise` 這類 governor 用的；在 `power_allocator` 下等於沒寫。

## 4. 實驗 B：關掉風扇滿載 10 分鐘

`fan_off_run.sh 600`：`echo 0 > cooling_device5/cur_state`，滿載 600 秒；
任何溫區 >95°C 立刻停；結束自動把風扇設回 4。

| 時間 | soc | big0 | 頻率上限 (小/大/大) | cpufreq cdev |
|------|-----|------|-------------------|--------------|
| 0 s | 40.7°C | 40.7 | 1800/2256/2256 | 0/0/0 |
| 73 s | 59.2 | 60.1 | 同上 | 0/0/0 |
| 147 s | 67.5 | 67.5 | 同上 | 0/0/0 |
| 295 s | 76.7 | 76.7 | 同上 | 0/0/0 |
| 444 s | 82.2 | 83.2 | 同上 | 0/0/0 |
| 538.5 s | 85.0 | **86.8** | 同上 | 0/0/0 |
| **539.8 s** | 84.1 | 85.0 | **1608/2208/2208** | **0/0/0** |
| 611.5 s | 77.6 | 77.6 | 恢復 1800/2256/2256 | 0/0/0（風扇已恢復） |

### 誰降的頻？

- cpufreq 冷卻裝置 state **全程 0** → 不是 thermal 框架。
- 被改的是 `scaling_max_freq`，而且改成的值正好是開機時印的：

```
cpu cpu0: l=15000 h=85000 hyst=5000 l_limit=0 h_limit=1608000000
cpu cpu4: l=15000 h=85000 hyst=5000 l_limit=0 h_limit=2208000000
```

來源是 DT 的 `rockchip,high-temp = <85000>` 與 `rockchip,high-temp-max-freq`
（小核 1608000、大核 2208000），由 **`drivers/soc/rockchip/rockchip_system_monitor.c`** 執行：

```c
/* rockchip_system_monitor_wide_temp_adjust()，約 1153 行 */
if (temp > info->high_temp) {                        /* > 85000，嚴格大於 */
        if (!info->is_high_temp)
                rockchip_high_temp_adjust(info, true);
} else if (temp < (info->high_temp - info->temp_hysteresis)) {   /* < 80000 */
        if (info->is_high_temp)
                rockchip_high_temp_adjust(info, false);
}
```

它看的溫區是 `rockchip,thermal-zone = "soc-thermal"`（`rk3588s.dtsi:2200`）。
和實測對得上：
- soc 溫度在 85.000°C 停了將近 40 秒都沒觸發（因為要 **嚴格大於** 85000，TSADC 每格約 0.92°C，下一格才算）。
- 降到 77.6°C（< 80°C）時解除。

### power_allocator 那時在幹嘛？

超過 60°C 後它進入控制狀態，但這台 DT 的 `sustainable-power = <5000>`（5 W，原本 dtsi 是 2100），
算出來的預算夠大，**從來沒把任何 cpufreq 冷卻裝置拉起來**。

### 性能影響

600 秒平均 GFLOPS：風扇開 34.0~34.4（大核）/ 13.6~14.1（小核）；
關風扇 33.4~33.9 / 13.35~13.86。差約 1.5%，和「最後 70 秒被限到 2208/1608」算出來的量一致。

## 5. 實驗 C：換成 step_wise，風扇會自己控溫

```
$ echo step_wise | sudo tee /sys/class/thermal/thermal_zone0/policy
```

**閒置（41°C）**：風扇立刻降到 state 0（pwm 0）。切換瞬間曾出現一次 state 3 的閃動，之後 20 秒每 0.5 秒採樣全是 0。

**滿載 8 分鐘**（`load_run.sh 480`）：

| 時間 | soc | 風扇 state |
|------|-----|-----------|
| 0 s | 44.4°C | 0 |
| 42.6 s | 60.1°C | 1 |
| 50.0 s | 60.1°C | **4** |
| 100~490 s | 平均 **59.75°C** | 在 3 ↔ 4 之間來回，**123 次切換** |
| 491 s（負載結束） | 54.5°C | 0 |

溫度被穩穩壓在 60°C 附近，沒降頻，而且閒置時風扇是停的（安靜）。
代價是滿載時風扇會一直 3↔4 跳（每 3 秒左右一次），耳朵可能聽得出來。

這符合 `drivers/thermal/gov_step_wise.c` 的邏輯：溫度 ≥ trip 且上升 → state+1；
低於 trip 且下降 → state−1；到下限就 `THERMAL_NO_TARGET`。

## 6. 意外：換不回 power_allocator

```
$ echo power_allocator | sudo tee /sys/class/thermal/thermal_zone0/policy
tee: ...: Input/output error
$ dmesg
thermal thermal_zone0: power_allocator: pwm-fan is not a power actor
```

`gov_power_allocator.c:606` `check_power_actors()` 在**綁定時**只要看到非功率元件就回 `-EINVAL`。

那開機時為什麼綁得上？**因為 `pwm_fan` 是模組（`CONFIG_SENSORS_PWM_FAN=m`），比 soc-thermal 晚載入**。
thermal zone 先註冊、先綁 power_allocator（當時只有 CPU/GPU 這些功率元件），
風扇後來才「加進」這個 zone，而加入時不會再檢查。

**驗證**（照這個理論操作一次）：

```
$ sudo rmmod pwm_fan                         # zone0 只剩 cdev0~3
$ echo power_allocator | sudo tee .../policy  # 成功
$ sudo modprobe pwm_fan                      # cdev4、cdev5 又出現，policy 仍是 power_allocator
```

完全符合。這也是板子最後還原的方法（還原後：power_allocator、風扇 state 4 / pwm 255，和開機時一樣）。

## 7. 給使用者的建議

| 想要 | 做法 |
|------|------|
| 安靜、溫度穩在 60°C | 開機後 `echo step_wise > /sys/class/thermal/thermal_zone0/policy`（可放 systemd service） |
| 維持現狀 | 什麼都不用做；風扇 100% 下滿載只到 56°C，永遠不降頻 |

兩種都不會降頻。我會選 **step_wise**：效能一樣，閒置時安靜。
