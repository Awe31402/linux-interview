# ROCK 5B 自由探索（2026-09-26）

之前的筆記（ch02~ch15、rknpu）都是照書上題目走。這一輪沒有題目，
直接看板子「現在怎麼運作」，再回頭對本 repo 的核心原始碼。

機台：Radxa ROCK 5B（RK3588，8 GB），`ssh radxa@192.168.68.58`，Debian 12，Linux 6.1.115+。

## 五個章節

| 章 | 主題 | 最重要的發現 |
|----|------|-------------|
| [01](./01_silicon_lottery.md) | 晶片體質（OTP / PVTM / OPP） | 大核最高 2256 MHz 不是 2.4 GHz，因為開機現場量的體質分數 **1676 剛好卡在分級邊界**；OTP 漏電值、電壓表 **逐點對帳全中**；執行中的 PVTPLL 暫存器 ≈ 目前頻率 |
| [02](./02_thermal_and_fan.md) | 散熱與風扇 | **風扇開機起就卡在 100%**（power_allocator 不管非功率元件）；關風扇 9 分鐘到 85°C，降頻的是 **Rockchip system_monitor** 不是 thermal 框架；換 step_wise 後風扇會照設計把溫度壓在 60°C，但 **換不回 power_allocator**（綁定檢查 + 模組載入順序，已實際驗證） |
| [03](./03_dvfs_latency.md) | 調頻延遲 | 升頻一次 **7.5 ms**：14 筆 I2C 交易只有 1 筆有用；原因是 rk860x regmap **沒快取** + cpu/mem 兩個 supply **其實是同一顆晶片** + I2C0 **100 kHz**；SCMI/SMC 改 PLL 本身只要 ~40 µs |
| [04](./04_ddr_devfreq.md) | DDR 頻率 | 頻寬隨頻率線性（8 → 28 GB/s）；預設 dmc_ondemand 下 **指標追逐延遲 245 ns，比釘最高頻的 129 ns 慢 1.9 倍** |
| [05](./05_misc_findings.md) | 零碎 | ⚠ 序列埠 **每 17 秒登入失敗一次**（已洗掉整個 dmesg）；⚠ 板上 build tree **被切到 6.12，所有模組都編不起來**；OP-TEE probe 失敗；NEON 峰值；NVMe/SD 速度 |

## ⚠ 需要你決定的事

1. **序列埠登入失敗迴圈**：檢查 debug UART 線；或 `sudo systemctl mask serial-getty@ttyFIQ0`。
2. **板上 `~/disk/kernel-source` 在 `develop-6.12`**：舊實驗的模組都靠它編。要不要切回 `linux-6.1-stan-rkr5.1`（或把 6.12 另開 worktree）？我沒動。
3. **風扇策略**：維持 power_allocator（風扇永遠 100%）或改 step_wise（安靜、溫度 60°C、效能一樣）。我會選 step_wise。

## 板子現在的狀態

全部實驗結束都已還原，和開始時一樣：

| 項目 | 狀態 |
|------|------|
| thermal_zone0 policy | `power_allocator`（02 §6 的方法還原） |
| 風扇 | state 4 / pwm 255 |
| cpufreq governor（3 個 policy） | `ondemand` |
| DDR `min_freq` / `max_freq` | 528000000 / 2112000000 |
| ftrace | `nop`，事件全關 |
| `/tmp` | 留有實驗程式與原始資料（重開機會清掉） |

沒有重開機、沒有載入任何核心模組（`pvtm_probe.c` 因 build tree 問題沒編成）。

## 工具（`tools/`）

| 檔案 | 用途 | 章 |
|------|------|----|
| `heat.c` | 每核綁定的 NEON FMA 加熱器，印出 GFLOPS | 02 |
| `thermlog.sh` | 每秒記 7 個溫區、3 個叢集頻率/上限、冷卻裝置狀態 | 02 |
| `fan_off_run.sh` | 關風扇滿載，>95°C 自動停，結束還原風扇 | 02 |
| `load_run.sh` | 不動設定的滿載 + 記錄 | 02 |
| `peek.py` | `/dev/mem` 唯讀讀暫存器 | 01 |
| `pvtm_temp.sh` | 固定頻率、加熱、每秒讀 PVTPLL 計數 | 01 |
| `pvtm_probe.c` + `Makefile.explore` | 同上的核心模組版（**未編譯**） | 01 |
| `dvfs_trace.sh` | function_graph 追一次調頻 | 03 |
| `reg_trace.sh` | function_graph 追 `regulator_set_voltage` 內部 | 03 |
| `i2c_ev.sh` | tracepoint 數調頻時的 I2C 交易 | 03 |
| `membw.c` | 讀/寫/複製頻寬 + 隨機指標追逐延遲 | 04 |
| `dmc_bw.sh` | DDR 釘在每個 OPP 各跑一次 `membw` | 04 |

所有 shell 腳本需要 root，放到板子 `/tmp/` 下執行（腳本內寫死 `/tmp/heat`、`/tmp/thermlog.sh`、`/tmp/membw`、`/tmp/peek.py`）：

```bash
cd notes/rock5b_explore/tools
scp heat.c membw.c peek.py *.sh radxa@192.168.68.58:/tmp/
ssh radxa@192.168.68.58 'cd /tmp && gcc -O2 -march=armv8.2-a -o heat heat.c -lpthread \
                                  && gcc -O2 -o membw membw.c -lpthread && chmod +x *.sh'
```

## 原始資料（`data/`）

`run_fan255.csv`（風扇 100%）、`run_fan0.csv`（關風扇）、`run_stepwise.csv`（step_wise）、
`pvtm_temp.csv`、`dvfs_trace.txt`、`reg_trace.txt`、`i2c_ev.txt`、各輪的 GFLOPS 輸出。

## 待辦 / 沒做完的

- [ ] **01 的邊界假說**：journald 已是持久化的，
      重開機 N 次記錄 `pvtm=`，看 cpu4 會不會掉到 1675 以下、最高頻變成 2352 MHz。
- [ ] 01 §7 的 supported-hw 分組（0,1,4 / 2,5 / 3,6 / 7）為什麼這樣排。
- [ ] 03 的改善（I2C 400 kHz、regmap cache）要改 DT/驅動並重開機才能驗證。
- [ ] 04 的 DRAM 類型與通道數沒查。
- [ ] GPU（Mali-G610）與 NPU 的 DVFS 也走同一套 opp_select，沒做溫度/延遲實驗。
