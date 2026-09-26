# 05　零碎發現

> 實驗日期：2026-09-26

## 1. ⚠ 序列埠每 17 秒一次登入失敗（開機 30 分鐘 496 次）

dmesg 一直冒：

```
ttyFIQ ttyFIQ0: tty_port_close_start: tty->count = 1 port count = 3
ttyFIQ ttyFIQ0: tty_port_close_start: tty->count = 1 port count = 2
```

查 journal：

```
login[28774]: FAILED LOGIN (1) on '/dev/ttyFIQ0' FOR 'UNKNOWN', Authentication failure
login[28774]: FAILED LOGIN (2) on '/dev/ttyFIQ0' FOR 'UNKNOWN', Authentication failure
login[28774]: FAILED LOGIN (3) on '/dev/ttyFIQ0' FOR 'UNKNOWN', Authentication failure
$ sudo journalctl -b | grep -c "FAILED LOGIN"
496
```

`serial-getty@ttyFIQ0` 收到「字」→ 當成帳號密碼 → 連錯 3 次 → `login` 結束 → systemd 重開 getty，循環。
從開機第 1 秒就開始（`00:09:58` 第一筆）。

**副作用：dmesg 被洗掉。** 開機約 90 分鐘後：

```
$ sudo dmesg | wc -l          → 3261
$ sudo dmesg | grep -c ttyFIQ → 3222     （99%）
$ sudo dmesg | head -1
[  340.447893] ttyFIQ ttyFIQ0: tty_port_close_start: ...
```

開機前 340 秒的核心訊息（包括 01 章用到的 `pvtm=`、`leakage=`）已經被擠出 ring buffer，
只能改用 `journalctl -k -b` 看。除錯時很容易以為「開機訊息裡沒有」。

`ttyFIQ0` 是 debug UART（`console=ttyFIQ0,1500000n8`），**只能從實體序列線進來**，不是網路攻擊。
最可能的原因：
- 接了 USB-TTL 線，但電腦那端的程式在亂送字（或迴授）
- 沒接線但 RX 腳浮接，吃到雜訊

**要處理的話**（二選一）：
- 拔掉/檢查序列線那一端
- 不需要序列登入：`sudo systemctl mask serial-getty@ttyFIQ0`（核心 console 輸出不受影響）

## 2. ⚠ 板子上的核心原始碼樹被切到 6.12，模組編不起來了

```
$ ls -l /lib/modules/6.1.115+/build
... -> /home/radxa/disk/kernel-source
$ cd ~/disk/kernel-source && head -4 Makefile
VERSION = 6
PATCHLEVEL = 12
SUBLEVEL = 69
$ git reflog -1
470f9dccbdc4 HEAD@{0}: checkout: moving from linux-6.1-stan-rkr5.1 to develop-6.12
```

Makefile 時間戳 2026-09-12 23:04。`.config` 仍是 9/7 給 6.1 的。
結果：編任何外部模組都失敗（`gcc: error: missing argument to '-falign-functions='`，
因為 6.12 的 Makefile 要 `CONFIG_FUNCTION_ALIGNMENT`，舊 `.config` 沒有）。

**影響**：`notes/experiments/` 裡所有核心模組（ch02~ch15）現在都沒辦法在板子上重編。
這次我沒有動這棵樹（不知道你是不是正在用 6.12），改用 `/dev/mem` 讀暫存器。

**要恢復**：`cd ~/disk/kernel-source && git checkout linux-6.1-stan-rkr5.1`
（切之前先確認 6.12 那邊沒有沒存的東西；之後可能需要 `make modules_prepare`）。
更好的做法是 6.12 用 `git worktree add ../kernel-6.12 develop-6.12` 另開一棵，兩邊不互相干擾。

## 3. NEON 峰值：A76 16 FLOP/週期、A55 8 FLOP/週期

詳見 [02 §1](./02_thermal_and_fan.md#1-加熱器先修好一個看起來會燒其實只燒一半的-bug)。

| 核心 | 頻率 | 實測 | 每週期 |
|------|------|------|--------|
| A76 | 2256 MHz | 35.8 GFLOPS | 15.9 FLOP |
| A55 | 1800 MHz | 14.35 GFLOPS | 7.97 FLOP |

整顆 SoC 8 核同時跑：4 × 14 + 4 × 34 ≈ **192 GFLOPS**（FP32）。

## 4. 儲存裝置與 PCIe

```
$ sudo lspci -vv | grep -E "^[0-9]|LnkSta:"
0000:01:00.0 Intel SSD 660P       LnkSta: Speed 8GT/s, Width x4    (Gen3 x4)
0002:21:00.0 MediaTek MT7921 Wi-Fi LnkSta: Speed 5GT/s, Width x1    (Gen2 x1)
0004:41:00.0 Realtek RTL8125 2.5GbE LnkSta: Speed 5GT/s, Width x1   (Gen2 x1)
```

所有 PCIe 連結都跑滿能力，**ASPM 全關**（`LnkCtl: ASPM Disabled`，L1 substates 也關）。

直接讀原始裝置（`dd iflag=direct bs=4M`，只讀）：

| 裝置 | 速度 | 瓶頸 |
|------|------|------|
| NVMe（Intel 660p 512GB） | **1.5~1.6 GB/s** | 硬碟本身（660p 標稱約 1.5 GB/s）；Gen3 x4 上限約 3.9 GB/s |
| SD 卡 `mmcblk1`（**根目錄 `/` 在這裡**） | **69.6 MB/s** | SD 卡 |

系統碟在 SD 卡上，比 NVMe 慢 20 倍以上。`/home/radxa/disk` 才在 NVMe。
另外 dmesg 有一行 `dwmmc_rockchip fe2c0000.mmc: swiotlb buffer is full (sz: 1048576 bytes)`
（開機 543 秒時，SD 控制器），沒去追。

## 5. 韌體版本

`/proc/cmdline` 帶著 U-Boot 塞進來的版本字串：

```
androidboot.fwver=ddr-v1.16-9fffbe1e78,bl31-v1.45,uboot-17.09-33-f-08/06/2024
```

- DDR 初始化程式 v1.16、BL31（ARM Trusted Firmware）v1.45、U-Boot 2017.09 分支
- `rockchip-dmc dmc: current ATF version 0x100`
- **OP-TEE 沒起來**：DT 有 `firmware/optee` 節點、`CONFIG_OPTEE=y`，但
  ```
  optee: probing for conduit method.
  optee: api uid mismatch
  optee: probe of firmware:optee failed with error -22
  ```
  SMC 問到的 API UID 不是 OP-TEE 的，代表安全世界（BL32）不是 OP-TEE 或沒載入。所以沒有 `/dev/tee0`。
- SCMI 走 SMC（`arm,smc-id = <0x82000010>`），CPU/DDR/GPU/NPU 時脈都由 BL31 管（見 03）

## 6. 開機時間戳是 6 月 16 日

`journalctl` 開頭幾行時間是 `Jun 16 17:44:32`，之後才跳成正確的 `Sep 26`。
開機時系統時間不對，等網路校時後才修正（板上有 hym8563 RTC，但顯然沒保存正確時間，可能沒電池）。
**沒深入查**。
