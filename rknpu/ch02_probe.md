# ch02 — 開機：從 device tree 到一個能用的裝置

> **本章目的**：把 `rknpu_probe()` 從頭走到尾，每一步都對回開機時的 `dmesg`。
> 看完你會知道：一個 platform driver 從「核心讀到 device tree」到
> 「`/dev/dri/card1` 出現」中間到底發生了什麼。
>
> **實驗平台**：Radxa ROCK 5B（RK3588），`Linux rock-5b 6.1.115+`
>
> **對照素材**
> - 驅動：`drivers/rknpu/rknpu_drv.c`
>   `:289`（`of_match` 表）、`:856`（`rknpu_drm_probe`）、`:1071`（中斷註冊）、
>   `:1243`（`rknpu_probe` 本體）、`:1630`（`platform_driver`）
> - Kconfig：`drivers/rknpu/Kconfig`（兩道門的 `choice`）
> - Device tree：`rk3588s.dtsi:3450`、`rk3588-rock-5b.dts:265`
> - TRM：§36.5.2 Clock and Reset
>
> **上一章** → [ch01 硬體長什麼樣](./ch01_hardware.md)

---

## 目錄

- [一鍵重現本章全部實驗](#一鍵重現本章全部實驗)
- [1. 驅動怎麼知道「這台機器上有我要的硬體」](#1-驅動怎麼知道這台機器上有我要的硬體)
- [2. probe 一步一步走](#2-probe-一步一步走)
- [3. 那三行 `can't request region` —— IOMMU 住在 NPU 的房子裡](#3-那三行-cant-request-region--iommu-住在-npu-的房子裡)
- [4. 兩道門，但只能選一道](#4-兩道門但只能選一道)
- [5. 時脈、重置、調頻](#5-時脈重置調頻)
- [6. probe 的最後一件事：把電關掉](#6-probe-的最後一件事把電關掉)
- [TRM 與實機對不上的地方（累積）](#trm-與實機對不上的地方累積)
- [本章結論一句話](#本章結論一句話)

---

## 一鍵重現本章全部實驗

```bash
# ---- 實驗 2.1：開機 log，對照 probe 的每一步 ----
sudo dmesg | grep -i -E "rknpu|npu" | head -20

# ---- 實驗 2.2：devfreq 的頻率表，對照 dtsi 的 OPP 表 ----
D=/sys/class/devfreq/fdab0000.npu
cat $D/available_frequencies
cat $D/available_governors
cat $D/governor
cat $D/cur_freq

# ---- 實驗 2.3：確認走的是哪一道門 ----
ls -l /dev/dri/card1      # DRM GEM 這道門
ls -l /dev/rknpu          # DMA Heap 那道門（本機沒有）
zcat /proc/config.gz | grep ROCKCHIP_RKNPU
```

---

## 1. 驅動怎麼知道「這台機器上有我要的硬體」

### 結論

**靠 `compatible` 字串配對。**

device tree 那邊寫（`rk3588s.dtsi:3451`）：

```dts
	rknpu: npu@fdab0000 {
		compatible = "rockchip,rk3588-rknpu";
```

驅動這邊寫（`rknpu_drv.c:289`）：

```c
static const struct of_device_id rknpu_of_match[] = {
	{ .compatible = "rockchip,rknpu",        .data = &rk356x_rknpu_config },
	{ .compatible = "rockchip,rk3568-rknpu", .data = &rk356x_rknpu_config },
	{ .compatible = "rockchip,rk3588-rknpu", .data = &rk3588_rknpu_config },  /* ← 我們 */
	{ .compatible = "rockchip,rv1106-rknpu", .data = &rv1106_rknpu_config },
	{ .compatible = "rockchip,rk3562-rknpu", .data = &rk3562_rknpu_config },
	{ .compatible = "rockchip,rk3576-rknpu", .data = &rk3576_rknpu_config },
	{},
};
```

核心開機時掃過 device tree 的每個節點，拿字串去比對每個已註冊驅動的 `of_match` 表。
對上了，就呼叫那個驅動的 `probe()`，並把節點交給它。

**`.data` 那一欄是重點。** 同一份程式碼要伺候六種晶片，差異全部塞在那個 config 結構裡。
配對成功時，`probe()` 順手就拿到「這顆晶片的參數」：

```c
	config = of_device_get_match_data(dev);
```

RK3588 拿到的是 `rk3588_rknpu_config`（`rknpu_drv.c:183`）：

```c
static const struct rknpu_config rk3588_rknpu_config = {
	.dma_mask = DMA_BIT_MASK(40),      /* 位址 40 位元 */
	.pc_data_amount_scale = 2,
	.pc_task_number_bits = 12,
	.num_irqs = ARRAY_SIZE(rk3588_npu_irqs),   /* 3 */
	.max_submit_number = (1 << 12) - 1,        /* 4095 */
	.core_mask = 0x7,                          /* 三顆核心 0b111 */
	...
};
```

`core_mask = 0x7` = `0b111` = 三顆核心都有。
（隔壁 `rk3583_rknpu_config` 是 `0x3` = 只有兩顆 —— 同一顆晶片切掉一核的便宜版。）

> **給新手**：`platform driver` 是「掛在 SoC 內部匯流排上的裝置」用的驅動模型。
> 不像 USB / PCI 可以熱插拔自己回報身分，SoC 內部的裝置**沒辦法自我介紹**，
> 所以要靠 device tree 這份「事先寫好的清單」告訴核心有什麼、在哪裡。

最底下把驅動註冊進系統（`rknpu_drv.c:1630`）：

```c
static struct platform_driver rknpu_driver = {
	.probe = rknpu_probe,
	.remove = rknpu_remove,
	...
};
static int rknpu_init(void) { return platform_driver_register(&rknpu_driver); }
```

---

## 2. probe 一步一步走

### 結論

`rknpu_probe()`（`rknpu_drv.c:1243`）大約 280 行，做的事可以切成六段：

```
① 檢查身分          有 device tree 嗎？compatible 對得上嗎？拿到 config
② 拿資源            IOMMU、時脈、電源、暫存器位址、中斷
③ 開門              註冊成 DRM 裝置 → /dev/dri/card1 出現
④ 接上電源管理      runtime PM、三個電源域、devfreq
⑤ 建立配套          關電用的 workqueue、IOMMU domain、SRAM 配置器
⑥ 收工              把電關掉、開 debugfs、啟動計時器
```

### 實機驗證（2.1）：開機 log 對照 probe

```bash
$ sudo dmesg | grep -i -E "rknpu|npu"
```

```
[14.105954] RKNPU fdab0000.npu: Adding to iommu group 0
[14.106088] RKNPU fdab0000.npu: RKNPU: rknpu iommu is enabled, using iommu mode
[14.106186] RKNPU fdab0000.npu: Looking up rknpu-supply from device tree
[14.106935] RKNPU fdab0000.npu: Looking up mem-supply from device tree
[14.107524] RKNPU fdab0000.npu: can't request region for resource [mem 0xfdab0000-0xfdabffff]
[14.107597] RKNPU fdab0000.npu: can't request region for resource [mem 0xfdac0000-0xfdacffff]
[14.107619] RKNPU fdab0000.npu: can't request region for resource [mem 0xfdad0000-0xfdadffff]
[14.108352] [drm] Initialized rknpu 0.9.8 20240828 for fdab0000.npu on minor 1
```

**每一行都能指回程式碼。** 一行一行對：

| dmesg | 對應程式碼 | 在幹嘛 |
|---|---|---|
| `Adding to iommu group 0` | 核心的 IOMMU 層（不是 rknpu 印的） | device tree 的 `iommus = <&rknpu_mmu>` 生效了 |
| `rknpu iommu is enabled, using iommu mode` | `rknpu_probe()` 裡的 `rknpu_is_iommu_enable(dev)` | 確認走 IOMMU 模式，不用保留記憶體 |
| `Looking up rknpu-supply` | `devm_regulator_get_optional(dev, "rknpu")` | 找 NPU 主電源。這個屬性是**板子的 dts** 給的（[ch01](./ch01_hardware.md) §4） |
| `Looking up mem-supply` | `devm_regulator_get_optional(dev, "mem")` | 找 NPU 記憶體電源 |
| `can't request region ×3` | `devm_ioremap_resource()` 的失敗路徑 | 見下一節，**這個超有意思** |
| `Initialized rknpu 0.9.8 20240828 ... on minor 1` | `rknpu_drm_probe()` → `drm_dev_register()` | `/dev/dri/card1` 誕生 |

最後一行拆開來看：

| 欄位 | 值 | 從哪來 |
|---|---|---|
| 名字 | `rknpu` | `rknpu_drv.h:30` `DRIVER_NAME` |
| 版本 | `0.9.8` | `rknpu_drv.h:33-35` |
| 日期 | `20240828` | `rknpu_drv.h:32` `DRIVER_DATE` |
| minor | `1` | → `/dev/dri/card1` |

**`minor 1` 就是 [ch00](./ch00_why_npu_is_a_drm_driver.md) 實驗 0.1 裡那個 `card1`。**
兩章的證據在這裡接起來了。

### 註冊中斷的地方（`rknpu_drv.c:1071`）

```c
	for (i = 0; i < config->num_irqs; i++) {
		irq = platform_get_irq_byname(pdev, config->irqs[i].name);
		...
		ret = devm_request_irq(dev, irq, config->irqs[i].irq_hdl,
				       IRQF_SHARED, dev_name(dev), rknpu_dev);
	}
```

`config->irqs[i].name` 就是 device tree 的 `"npu0_irq"` / `"npu1_irq"` / `"npu2_irq"`。

**`IRQF_SHARED` 這個旗標，正好解釋了 [ch01](./ch01_hardware.md) §5 看到的現象** ——
`/proc/interrupts` 一行掛了 `fdab9000.iommu, fdab0000.npu` 兩個裝置。
NPU 明講「我這條線可以跟別人共用」，所以核心才允許 IOMMU 也掛上來。

---

## 3. 那三行 `can't request region` —— IOMMU 住在 NPU 的房子裡

### 結論

開機時抱怨了三次「拿不到這塊記憶體區間」，但驅動照樣正常運作。這不是 bug。

程式碼長這樣（`rknpu_probe()` 裡）：

```c
		rknpu_dev->base[i] = devm_ioremap_resource(dev, res);
		if (PTR_ERR(rknpu_dev->base[i]) == -EBUSY) {
			rknpu_dev->base[i] = devm_ioremap(dev, res->start,
							  resource_size(res));
		}
```

翻譯：

1. 先用**正規方式** `devm_ioremap_resource()` —— 它會先「登記佔用」這塊位址，再對映
2. 如果登記失敗（`-EBUSY`，有人先佔了），**退而求其次**用 `devm_ioremap()` ——
   跳過登記，直接對映

**那誰先佔走了？** 把 [ch01](./ch01_hardware.md) 的兩個 device tree 節點位址排在一起看：

| 誰 | 位址範圍 |
|---|---|
| NPU core0 | `0xfdab0000` ~ `0xfdab_ffff` |
| NPU core1 | `0xfdac0000` ~ `0xfdac_ffff` |
| NPU core2 | `0xfdad0000` ~ `0xfdad_ffff` |
| IOMMU reg[0] | `0xfdab9000` ~ `0xfdab90ff` |
| IOMMU reg[1] | `0xfdaba000` ~ `0xfdaba0ff` |
| IOMMU reg[2] | `0xfdaca000` ~ `0xfdaca0ff` |
| IOMMU reg[3] | `0xfdada000` ~ `0xfdada0ff` |

看出來了嗎：

```
core0  0xfdab0000 ─────────────────────────────── 0xfdabffff
                       ▲ 0xfdab9000   ▲ 0xfdaba000
                       └─ IOMMU       └─ IOMMU

core1  0xfdac0000 ─────────────────────────────── 0xfdacffff
                              ▲ 0xfdaca000
                              └─ IOMMU

core2  0xfdad0000 ─────────────────────────────── 0xfdadffff
                              ▲ 0xfdada000
                              └─ IOMMU
```

**IOMMU 的暫存器，全部坐落在 NPU 三顆核心的暫存器窗戶裡面。**

IOMMU 驅動比 NPU 驅動先 probe，先把那四小塊登記走了。
NPU 之後要登記整個 64KB 窗戶，當然被擋 —— 因為裡面已經有人。

所以 NPU 只好用「不登記、直接對映」這條路。**功能完全不受影響**，
只是失去了「別人不能重複佔用」這層保護。

### 順手解掉 ch01 留的一個懸案

[ch01](./ch01_hardware.md) §5 問過：**IOMMU 有 4 段 `reg`，NPU 只有 3 段，多的那段是什麼？**

現在答案浮出來了：**core0 底下有兩塊 IOMMU 暫存器（`0xfdab9000` 和 `0xfdaba000`），
core1 和 core2 各一塊。**

至於 core0 為什麼是兩塊 —— **TRM 完全沒收錄 `rockchip,iommu-v2` 這個裝置**，
所以查不到。**仍然是待查，我不猜。**

> 但至少「4 段」這個數字不再神秘了：**2 + 1 + 1**，而不是「3 + 1 個不明的」。

---

## 4. 兩道門，但只能選一道

### 結論

`rknpu` 有兩套完全不同的記憶體介面，開機時只會開其中一道。

`drivers/rknpu/Kconfig` 用 `choice` 寫死了這件事：

```
choice
	prompt "RKNPU memory manager"
	default ROCKCHIP_RKNPU_DRM_GEM

	config ROCKCHIP_RKNPU_DRM_GEM
		depends on DRM

	config ROCKCHIP_RKNPU_DMA_HEAP
		depends on DMABUF_HEAPS_ROCKCHIP_CMA_HEAP
endchoice
```

> **`choice` 是 Kconfig 的「單選題」**。裡面的選項互斥，只能挑一個。
> 對比 `menu` 底下的一堆 `config` 是「複選題」。

probe 裡對應的兩段是 `#ifdef`，互斥：

```c
#ifdef CONFIG_ROCKCHIP_RKNPU_DRM_GEM
	ret = rknpu_drm_probe(rknpu_dev);          /* → /dev/dri/card1 */
#endif
#ifdef CONFIG_ROCKCHIP_RKNPU_DMA_HEAP
	rknpu_dev->miscdev.name = "rknpu";
	ret = misc_register(&rknpu_dev->miscdev);  /* → /dev/rknpu */
	rknpu_dev->heap = rk_dma_heap_find("rk-dma-heap-cma");
#endif
```

| | DRM GEM 這道門 | DMA Heap 那道門 |
|---|---|---|
| 裝置節點 | `/dev/dri/card1` + `/dev/dri/renderD129` | `/dev/rknpu` |
| 記憶體從哪來 | DRM 的 GEM 配置器 | Rockchip CMA heap |
| 需要 | `CONFIG_DRM` | `CONFIG_DMABUF_HEAPS_ROCKCHIP_CMA_HEAP` |
| 檔案 | `rknpu_gem.c`（1757 行） | `rknpu_mem.c`（353 行） |

### 實機驗證（2.3）：本機走哪一道

```bash
$ ls -l /dev/dri/card1
crw-rw----+ 1 root video 226, 1 Jun 16  2024 /dev/dri/card1

$ ls -l /dev/rknpu
ls: cannot access '/dev/rknpu': No such file or directory

$ zcat /proc/config.gz | grep ROCKCHIP_RKNPU
CONFIG_ROCKCHIP_RKNPU=y
CONFIG_ROCKCHIP_RKNPU_DEBUG_FS=y
# CONFIG_ROCKCHIP_RKNPU_PROC_FS is not set
# CONFIG_ROCKCHIP_RKNPU_FENCE is not set
# CONFIG_ROCKCHIP_RKNPU_SRAM is not set
CONFIG_ROCKCHIP_RKNPU_DRM_GEM=y        ← 這道
```

**走 DRM GEM。** 沒人特別選過，是 Kconfig 的 `default` 生效。

> **全書之後只講 DRM GEM 這道門。** `rknpu_mem.c` 那條路本機跑不到，
> 沒有實機可以驗證的東西，不寫。

---

## 5. 時脈、重置、調頻

### 結論

TRM §36.5.2 講時脈和重置，**每一句都能在 device tree 找到對應的一行。**

TRM 原文（`chapter_36.txt:8942-8949`）：

> *"RKNN has two clock domains, one is AHB clock, the other is AXI clock.
> AHB clock, which is the clock for AHB interface, while AXI clock, which is the clock
> for AXI interface. AXI clock also used for core clock for every Calculate Core..."*

> *"Correspond to the clock domain, there are two reset signals. **Aresetn**, the reset
> signal for AXI interface and every Calculate Core and Control Core.
> **Hresetn** is the AHB interface reset pin..."*

對回 `rk3588s.dtsi:3463`（clocks）與 `:3469`（resets）：

```dts
		clock-names = "clk_npu", "aclk0", "aclk1", "aclk2",
			      "hclk0", "hclk1", "hclk2", "pclk";
		reset-names = "srst_a0", "srst_a1", "srst_a2",
			      "srst_h0", "srst_h1", "srst_h2";
```

| TRM 講的 | device tree 的名字 | 幾個 |
|---|---|---|
| AXI clock（核心時脈） | `aclk0` / `aclk1` / `aclk2` | 3（每核一個） |
| AHB clock（設定介面） | `hclk0` / `hclk1` / `hclk2` | 3 |
| **Aresetn**（AXI 重置） | `srst_a0` / `srst_a1` / `srst_a2` | 3 |
| **Hresetn**（AHB 重置） | `srst_h0` / `srst_h1` / `srst_h2` | 3 |

`a` = AXI，`h` = AHB。**命名直接照抄 TRM。**

還有兩個 TRM 沒提的：`clk_npu`（走 SCMI，讓安全韌體管的總頻率）和 `pclk`。

> TRM 還提到重置有個時序要求：
> *"All the two signals must be asserted for a minimum of 32 core clock cycles...
> Then two signals must be release at the same time."*
> （兩個訊號都要拉住至少 32 個核心時脈週期，然後同時放開。）
>
> 這條規則在 [ch09](./ch09_reset.md) 講軟重置時會再回來看驅動有沒有照做。

### 實機驗證（2.2）：devfreq 的頻率表 vs dtsi 的 OPP 表

probe 裡呼叫 `rknpu_devfreq_init(rknpu_dev)`，把 device tree 的
`operating-points-v2 = <&npu_opp_table>` 變成一個可調頻的裝置。

```bash
$ D=/sys/class/devfreq/fdab0000.npu
$ cat $D/available_frequencies
300000000 400000000 500000000 600000000 700000000 800000000 900000000 1000000000

$ cat $D/available_governors
rknpu_ondemand dmc_ondemand simple_ondemand

$ cat $D/governor
rknpu_ondemand

$ cat $D/cur_freq
1000000000
```

**8 個頻率，300 MHz ~ 1 GHz。**

但 dtsi 的 OPP 表其實有 **15** 筆：

```bash
$ grep -c "opp-hz" <npu_opp_table 區塊>
15
$ grep "opp-supported-hw" <同上> | sort | uniq -c
      7   opp-supported-hw = <0x06 0xffff>;
      8   opp-supported-hw = <0xf9 0xffff>;
```

分成兩組：8 筆 + 7 筆。**devfreq 只吃到 8 筆那組。**

為什麼？dtsi 的 OPP 表開頭有這幾行：

```dts
	nvmem-cells = <&npu_leakage>, <&npu_opp_info>, <&specification_serial_number>;
	nvmem-cell-names = "leakage", "opp-info", "specification_serial_number";
	rockchip,supported-hw;
```

`nvmem` 就是晶片上的 **eFuse**（出廠時燒死的一小塊資料）。
裡面記著這顆晶片的漏電特性、體質分級。
Rockchip 的 OPP 層讀了它，再拿 `opp-supported-hw` 去比對，**篩掉不適用這顆晶片的那組**。

**同一份 dtsi，不同體質的晶片開機後拿到的頻率表不一樣。**

> `governor` 是 `rknpu_ondemand` —— Rockchip 自己寫的調頻策略，
> 不是核心內建的 `simple_ondemand`。細節留到 [ch08](./ch08_power_freq.md)。

---

## 6. probe 的最後一件事：把電關掉

### 結論

probe 快結束時有這麼一段：

```c
	ret = rknpu_power_on(rknpu_dev);        /* 先開電 */
	...
	rknpu_devfreq_init(rknpu_dev);          /* 初始化調頻 */
	rknpu_dev->power_put_delay = 3000;      /* ← 預設閒置 3 秒關電 */
	...
	if (rknpu_dev->iommu_en)
		rknpu_iommu_init_domain(rknpu_dev);

	rknpu_power_off(rknpu_dev);             /* ← 再關掉 */
	atomic_set(&rknpu_dev->power_refcount, 0);

	rknpu_debugger_init(rknpu_dev);
	rknpu_init_timer(rknpu_dev);

	return 0;
```

**開機 → 開電 → 做完初始化 → 關電。**

為什麼要先開再關？因為中間那些初始化（devfreq、IOMMU domain）需要硬體是活的。
做完就沒必要繼續耗電了。

這解釋了兩件我們之前就觀察到的事：

```bash
$ sudo cat /sys/kernel/debug/rknpu/power
off                       ← probe 最後關掉的，一直維持到有人送工作

$ sudo cat /sys/kernel/debug/rknpu/delayms
3000                      ← 就是 power_put_delay = 3000 這行，寫死在程式碼裡
```

`delayms` 不是從 device tree 讀的，是 **hard-code 在 probe 裡的 3000**。
（不過 debugfs 可以改，[ch08](./ch08_power_freq.md) 會玩。）

最後兩行 `rknpu_debugger_init()` 開出我們一路在用的 `/sys/kernel/debug/rknpu/`，
`rknpu_init_timer()` 啟動計算負載百分比用的計時器（就是 `load` 那個節點）。

---

## TRM 與實機對不上的地方（累積）

| # | 章 | TRM 怎麼說 | 實機／程式碼 | 判斷 |
|---|---|---|---|---|
| 1 | ch01 | 暫存器表沒有 `0x0000` / `0x0004` | 有，`0x0000` = `"FIRE"` | TRM 漏寫 |
| 2 | ch01 | 位址表說 GLOBAL 是 `0xf000~0xf004` | 同章摘要表卻有 `0xF008` | TRM 自己矛盾 |
| 3 | ch01 | `0x0020` bit31 = RO / reset 0 | 讀到 `1` | 不明，待查 |
| 4 | ch01 | 完全沒提 `rockchip,iommu-v2` | dtsi 有，且**暫存器坐落在 NPU 的窗戶裡**（本章解開一半） | TRM 沒收錄 |
| 5 | **ch02** | §36.5.2 只提 AHB / AXI 兩個時脈域 | device tree 另有 `clk_npu`（SCMI）和 `pclk` | TRM 只講 IP 內部，SoC 層級的時脈不歸它管。**合理，非錯誤** |

---

## 本章結論一句話

> **probe 做的事就是「照 device tree 這張清單，把硬體需要的東西一樣一樣接起來」** ——
> 時脈、電源、暫存器位址、中斷、IOMMU、調頻，最後開一扇門讓使用者空間進來。
>
> 開機 log 的每一行都能指回一段程式碼。**看不懂的 log 通常藏著最有意思的事**
> —— 例如那三行 `can't request region`，就把 IOMMU 和 NPU 的位址關係整個揭開了。

---

## 本章做過的實驗

| # | 實驗 | 結果 |
|---|---|---|
| 2.1 | `dmesg` 對照 probe 每一步 | 8 行 log 全部對回程式碼；`minor 1` 接上 [ch00](./ch00_why_npu_is_a_drm_driver.md) 的 `card1` |
| 2.2 | devfreq 頻率表 vs dtsi OPP 表 | dtsi 有 15 筆，devfreq 只吃 8 筆；由 eFuse 體質分級篩選 |
| 2.3 | 確認走哪一道門 | `CONFIG_ROCKCHIP_RKNPU_DRM_GEM=y`，`/dev/rknpu` 不存在 |

---

**下一章** → [ch03 六個 ioctl：使用者空間與核心的唯一交界](./ch03_six_ioctls.md)
