# ch09 — 出事的時候：timeout 與重置

> **最後一章。**
>
> 前面八章講的都是「一切順利」的路徑。這章講**出事之後**：
> job 卡住了怎麼辦、軟重置做了什麼、以及為什麼一個只有 158 行的檔案
> 是整份驅動的安全網。
>
> 我們在 [ch07](./ch07_interrupt.md) 實驗 7.4 已經意外拍到過一次**真實的逾時現場**，
> 這章把它完整解讀。
>
> **實驗平台**：Radxa ROCK 5B（RK3588），`Linux rock-5b 6.1.115+`
>
> **對照素材**
> - `drivers/rknpu/rknpu_reset.c`（158 行，全檔）
> - `drivers/rknpu/rknpu_job.c:176`（`rknpu_job_wait`）、`:586`（逾時處理）、
>   `:699`（`rknpu_job_timeout_clean`）
> - `drivers/rknpu/rknpu_debugger.c:262`（debugfs 的 `reset` 節點）
> - `drivers/rknpu/rknpu_drv.c:70`（`bypass_soft_reset` 模組參數）、
>   `:420`（`RKNPU_ACT_RESET`）
> - TRM §36.5.2 Clock and Reset
>
> **上一章** → [ch08 電源與頻率](./ch08_power_freq.md)

---

## 目錄

- [一鍵重現本章全部實驗](#一鍵重現本章全部實驗)
- [1. 什麼時候會重置](#1-什麼時候會重置)
- [2. TRM 的重置規則 vs 驅動做的事](#2-trm-的重置規則-vs-驅動做的事)
- [3. `rknpu_soft_reset()` 逐行](#3-rknpu_soft_reset-逐行)
- [4. 實驗 9.1：手動觸發，用暫存器證明它真的重置了](#4-實驗-91手動觸發用暫存器證明它真的重置了)
- [5. 實驗 9.2：`reset` 這個節點其實有三種用法](#5-實驗-92reset-這個節點其實有三種用法)
- [6. 真實的逾時現場](#6-真實的逾時現場)
- [TRM 與實機對不上的地方（完整版）](#trm-與實機對不上的地方完整版)
- [本章結論一句話](#本章結論一句話)
- [全書回顧](#全書回顧)

---

## 一鍵重現本章全部實驗

```bash
D=/sys/kernel/debug/rknpu
M=~/disk/rknn/rknn-toolkit2/rknpu2/examples/rknn_api_demo/model/RK3588/mobilenet_v1.rknn
# 需要 ch01 的 exp01_regs 和 ch05 的 exp05_run

# ---- 9.1：手動觸發重置，看暫存器變化 ----
sudo sh -c "echo on > $D/power"
sudo ./exp05_run $M 1 5 >/dev/null 2>&1              # 先跑幾次留痕跡
sudo ./exp01_regs | grep -E '0x0010|0x0030|0x003c'   # 重置前
sudo sh -c "echo 1 > $D/reset"
sudo ./exp01_regs | grep -E '0x0010|0x0030|0x003c'   # 重置後
sudo dmesg | grep "soft reset" | tail -1
sudo sh -c "echo off > $D/power"

# ---- 9.2：reset 節點的三種用法 ----
sudo cat $D/reset                                    # on = 軟重置啟用
sudo sh -c "echo off > $D/reset"; sudo cat $D/reset  # 關掉
sudo sh -c "echo on > $D/power; echo 1 > $D/reset"
sudo dmesg | tail -1                                 # 應該是 "bypass soft reset"
sudo sh -c "echo on > $D/reset; echo off > $D/power" # 還原

# ---- 9.3：重置後還能用嗎 ----
sudo ./exp05_run $M 1 50
```

---

## 1. 什麼時候會重置

### 結論

grep 整份驅動，`rknpu_soft_reset()` 只有**四個**呼叫點：

| 呼叫點 | 什麼情況 |
|---|---|
| `rknpu_job.c:604` | **job 逾時**，印完所有核心的暫存器狀態之後 |
| `rknpu_job.c:714` | `rknpu_job_timeout_clean()` —— 送新 job 前發現舊 job 卡住 |
| `rknpu_drv.c:421` | 使用者空間呼叫 `RKNPU_ACTION` 的 `RKNPU_ACT_RESET` |
| `rknpu_debugger.c:295` | 有人 `echo 1 > debugfs/rknpu/reset` |

**前兩個是自動的，後兩個是人為的。**

第二個特別值得注意 —— `rknpu_job_timeout_clean()`（`rknpu_job.c:699`）：

```c
	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (core_mask & rknpu_core_mask(i)) {
			subcore_data = &rknpu_dev->subcore_datas[i];
			job = subcore_data->job;
			if (job && ktime_us_delta(ktime_get(), job->timestamp)
				   >= job->args->timeout) {
				rknpu_soft_reset(rknpu_dev);
				subcore_data->job = NULL;
				...
```

**送新工作之前，先看看有沒有上一個卡在那裡。** 有的話先重置再說。

> 這是很實際的防禦：如果某個程式送了 job 之後就被 kill 掉，
> 沒人去等結果，那個 job 會一直掛在 `subcore_data->job` 上。
> **下一個使用者不該被它拖累。**

---

## 2. TRM 的重置規則 vs 驅動做的事

### TRM 怎麼說

TRM §36.5.2「NPU Reset」（`chapter_36.txt:8953`）：

> Correspond to the clock domain, there are **two reset signals**.
> **Aresetn**, the reset signal for AXI interface and every Calculate Core and Control
> Core. **Hresetn** is the AHB interface reset pin and which is synchronized to the AHB
> clock domain.
>
> All the two signals must be asserted for a **minimum of 32 core clock cycles**,
> using the slowest of the two clocks.
> **Then two signals must be release at the same time.**

兩條硬規則：

1. 兩個訊號都要**拉住至少 32 個核心時脈週期**
2. 然後**同時放開**

### 驅動怎麼做

device tree 給了 **6 個** reset（[ch02](./ch02_probe.md) §5）：

```dts
		reset-names = "srst_a0", "srst_a1", "srst_a2",     /* Aresetn × 3 核 */
			      "srst_h0", "srst_h1", "srst_h2";    /* Hresetn × 3 核 */
```

`rknpu_soft_reset()` 的核心三行（`rknpu_reset.c:124`~`:132`）：

```c
	for (i = 0; i < rknpu_dev->num_srsts; ++i)
		ret |= rknpu_reset_assert(rknpu_dev->srsts[i]);      /* 全部拉住 */

	udelay(10);                                                 /* 等 10 微秒 */

	for (i = 0; i < rknpu_dev->num_srsts; ++i)
		ret |= rknpu_reset_deassert(rknpu_dev->srsts[i]);   /* 全部放開 */

	udelay(10);
```

### 規則一：✅ 過關

`udelay(10)` = 10 微秒。

最慢的時脈是多少？OPP 表最低是 300 MHz，AHB 的 `hclk` 更低但也在百 MHz 等級。
**32 個週期在 100 MHz 下只要 0.32 µs。**

10 µs 遠超過。要違反這條規則，時脈得低於 3.2 MHz —— 不可能。

### 規則二：⚠️ 沒做到

TRM 說 *"release **at the same time**"（同時放開）。

但程式碼是一個 **`for` 迴圈**，一次放開一個。
每次 `reset_control_deassert()` 都是一次獨立的暫存器寫入。

順序是 `srst_a0 → srst_a1 → srst_a2 → srst_h0 → srst_h1 → srst_h2`。

**core0 的 Aresetn 和 Hresetn，中間隔了 3 次寫入。**

> **這是驅動與 TRM 明確不一致的地方。**
>
> 實務上會不會出問題？我們的實驗（9.1、9.3）重置後一切正常。
> 但「測不出問題」不等於「符合規格」。
>
> **記進落差表，不替它辯護，也不宣稱它有 bug。**

---

## 3. `rknpu_soft_reset()` 逐行

整個函式（`rknpu_reset.c:98`）：

```c
int rknpu_soft_reset(struct rknpu_device *rknpu_dev)
{
	/* ① 被關掉了就直接回去 */
	if (rknpu_dev->bypass_soft_reset) {
		LOG_WARN("bypass soft reset\n");
		return 0;
	}

	/* ② 已經有人在重置了就不重複做 */
	if (!mutex_trylock(&rknpu_dev->reset_lock))
		return 0;

	rknpu_dev->soft_reseting = true;

	/* ③ 等 100 毫秒 */
	msleep(100);

	/* ④ 叫醒所有在等的人 */
	for (i = 0; i < rknpu_dev->config->num_irqs; ++i)
		wake_up(&rknpu_dev->subcore_datas[i].job_done_wq);

	LOG_INFO("soft reset, num: %d\n", rknpu_dev->num_srsts);

	/* ⑤ 拉住 → 等 → 放開 */
	for (i = 0; i < rknpu_dev->num_srsts; ++i) rknpu_reset_assert(...);
	udelay(10);
	for (i = 0; i < rknpu_dev->num_srsts; ++i) rknpu_reset_deassert(...);
	udelay(10);

	/* ⑥ IOMMU 重新掛一次 */
	if (rknpu_dev->iommu_en)
		domain = iommu_get_domain_for_dev(rknpu_dev->dev);
	if (domain) {
		iommu_detach_device(domain, rknpu_dev->dev);
		iommu_attach_device(domain, rknpu_dev->dev);
	}

	rknpu_dev->soft_reseting = false;
	mutex_unlock(&rknpu_dev->reset_lock);
	return 0;
}
```

### 四個值得停下來的地方

**② `mutex_trylock` 而不是 `mutex_lock`。**

拿不到鎖就**直接回傳 0（成功）**，不等。
因為「已經有人在重置了」就等同於「重置會發生」，沒必要排隊再做一次。

**③ `msleep(100)` —— 為什麼要先等 100 毫秒？**

**程式碼沒有註解，TRM 也沒說。不猜。**

（合理的方向是「讓還在飛的 AXI 交易做完」，但這是推測。）

**④ 叫醒所有等待者。**

回頭看 [ch06](./ch06_submit.md) §3 的等待條件：

```c
	wait_event_timeout(subcore_data->job_done_wq,
			   job->flags & RKNPU_JOB_DONE || rknpu_dev->soft_reseting,
			   ...);
```

**`rknpu_dev->soft_reseting` 是第二個醒來的理由。**

第 ③ 步把它設成 `true`，第 ④ 步叫醒大家 ——
所以等待中的行程會醒來、發現「不是完成，是在重置」，然後放棄。

**如果沒有這一步，那些行程會一路等到自己的逾時。**

**⑥ IOMMU 要 detach 再 attach。**

硬體重置會把 NPU 內部的 IOMMU 相關狀態清掉（TLB、page table base 之類）。
`detach` + `attach` 讓核心重新把頁表位址寫回去。

**沒有這一步，重置後的第一個 job 會踩到翻譯失敗。**

---

## 4. 實驗 9.1：手動觸發，用暫存器證明它真的重置了

### 做法

用 [ch01](./ch01_hardware.md) 的 [`exp01_regs.c`](./experiments/exp01_regs.c) 讀暫存器，
在重置前後各讀一次。

```bash
sudo sh -c "echo on > /sys/kernel/debug/rknpu/power"
sudo ./exp05_run $M 1 5 >/dev/null      # 先跑 5 次，讓暫存器留下工作痕跡
sudo ./exp01_regs                       # 重置前
sudo sh -c "echo 1 > /sys/kernel/debug/rknpu/reset"
sudo ./exp01_regs                       # 重置後
```

### 結果

```
--- 重置前 ---
0x0010  0xffc3fd00        ← 上一個 task 的 regcmd 位址（IOVA！）
0x0030  0x0000000a
0x003c  0x0000f000

--- 重置後 ---
0x0010  0x00000001
0x0030  0x00000000
0x003c  0x00005000

dmesg: RKNPU: soft reset, num: 6
```

**暫存器真的變了。**

| offset | TRM 的名字 | 重置前 | 重置後 | TRM 的 reset value |
|---|---|---|---|---|
| `0x0010` | `RKNN_pc_base_address` | `0xffc3fd00` | `0x00000001` | `0x00000000` |
| `0x0030` | `RKNN_pc_task_con` | `0x0000000a` | `0x00000000` | `0x00000000` ✅ |
| `0x003c` | `RKNN_pc_task_status` | `0x0000f000` | `0x00005000` | — |

`0x0010` 重置前的 `0xffc3fd00` **是一個 IOVA** ——
就是 [ch05](./ch05_regcmd.md) 挖到的那種 `regcmd_addr`。
硬體上一次去抓指令的位址，還留在暫存器裡。

`0x0030` 回到 TRM 說的 reset value `0x0`。✅

`dmesg` 的 `num: 6` 對上 device tree 的 6 個 reset。✅

> ⚠️ `0x0010` 重置後是 `0x1` 而不是 TRM 說的 `0x0`。
> 巧的是，[ch01](./ch01_hardware.md) 實驗 1.1 在閒置的板子上讀到的也是 `0x1`。
> **兩次觀察一致，但跟 TRM 的 reset value 不符。原因不明，待查。**

### 9.3：重置後還能用嗎

```bash
$ sudo ./exp05_run $M 1 50
rknn_run x50 -> 0
```

**50 次推論全部正常。** 安全網有效。

---

## 5. 實驗 9.2：`reset` 這個節點其實有三種用法

### 結論

`rknpu_debugger.c:277` 的寫入處理（判斷式在 `:293`）：

```c
	if (strcmp(buf, "1") == 0 &&
	    atomic_read(&rknpu_dev->power_refcount) > 0)
		rknpu_soft_reset(rknpu_dev);            /* 立刻重置一次 */
	else if (strcmp(buf, "on") == 0)
		rknpu_dev->bypass_soft_reset = 0;       /* 啟用軟重置 */
	else if (strcmp(buf, "off") == 0)
		rknpu_dev->bypass_soft_reset = 1;       /* 停用軟重置 */
```

**同一個節點，三種語意：**

| 寫入 | 效果 |
|---|---|
| `1` | **觸發一次軟重置**（而且**必須先開電**，`power_refcount > 0`） |
| `on` | 開啟軟重置功能 |
| `off` | 關閉軟重置功能（`bypass_soft_reset = 1`） |

而**讀**這個節點（`rknpu_debugger.c:262`）：

```c
	if (!rknpu_dev->bypass_soft_reset)
		seq_puts(m, "on\n");
	else
		seq_puts(m, "off\n");
```

**讀到的是「軟重置功能開著嗎」，不是「有沒有重置過」。**

> 📌 **這修正了 [BRIEF.md](./BRIEF.md) 附錄 A.1 的描述。**
> 當時（還沒有板子時）根據節點名字把 `reset` 記成「手動觸發重置」。
> **對了一半** —— 寫 `1` 確實會觸發，但它同時也是一個開關，
> 而且讀出來的值跟「有沒有重置」無關。

### 實測

```bash
$ sudo cat $D/reset
on                                    ← 軟重置功能是開的

$ sudo sh -c "echo off > $D/reset"
$ sudo cat $D/reset
off                                   ← 關掉了

$ sudo sh -c "echo on > $D/power; echo 1 > $D/reset"
$ sudo dmesg | tail -1
RKNPU: bypass soft reset              ← 不是 "soft reset, num: 6"！
```

**跟 `rknpu_reset.c:105` 那個提早返回完全對上。**

### ★ 跟 `bypass_irq_handler` 的關鍵差別

[ch07](./ch07_interrupt.md) §6 發現 `bypass_irq_handler` **執行時改不動**，
因為它只在 `rknpu_probe()` 被讀。

`bypass_soft_reset` **不一樣**：

| | 模組參數 `0644` | probe 時讀 | **執行時可改** |
|---|---|---|---|
| `bypass_irq_handler` | ✅ | ✅ | ❌ 只有 probe 讀 |
| `bypass_soft_reset` | ✅ | ✅ | ✅ **debugfs `reset` 也會寫** |

兩個看起來一模一樣的模組參數，**一個能在執行時改，一個不能。**
差別不在參數本身，在**有沒有第二個地方去寫那個欄位**。

> **再次驗證 [ch07](./ch07_interrupt.md) 的那條通則**：
> 看到模組參數，**grep 它被寫在哪、被讀在哪**。權限位元什麼都不保證。

---

## 6. 真實的逾時現場

### 結論

上面兩個實驗都是我們主動觸發的。**真實的逾時長什麼樣？**

[ch07](./ch07_interrupt.md) 實驗 7.4 意外拍到了完整的一次。當時把中斷關掉，
硬體算完了但沒人知道，於是：

```
RKNPU: job: 0000000050e3ec26, mask: 0x1, job iommu domain id: 0,
       dev iommu domain id: 0, wait_count: 1, continue wait: 0,
       commit elapse time: 6144740us, wait time: 6144745us, timeout: 6000000us
RKNPU: failed to wait job, task counter: 0, flags: 0x5, ret = 0,
       elapsed time: 6144800us
RKNPU: job timeout, flags: 0x0:
RKNPU: 	core 0 irq status: 0x200, raw status: 0xc0000200,
        require mask: 0x300, task counter: 0x0, elapsed time: 6251420us
RKNPU: soft reset, num: 6
```

### 一行一行讀

| 訊息 | 出處 | 說什麼 |
|---|---|---|
| `wait_count: 1 ... timeout: 6000000us` | `rknpu_job_wait()`（`rknpu_job.c:204`） | 第 1 輪等待逾時。SDK 設的逾時是 **6 秒** |
| `continue wait: 0` | 同上 | 已經超過逾時，**不再等下一輪**（最多會等 3 輪） |
| `failed to wait job` | `rknpu_job_wait()`（`:265`） | 準備回 `-ETIMEDOUT` |
| `job timeout, flags: 0x0:` | 逾時處理（`rknpu_job.c:586`） | 開始印每個核心的現場 |
| `core 0 irq status: 0x200 ... require mask: 0x300` | `rknpu_job.c:592` | **現場快照** |
| `soft reset, num: 6` | `rknpu_soft_reset()`（`rknpu_reset.c:122`） | **自動救援** |

### 這段 log 的價值

**`irq status: 0x200`** —— 用 [ch07](./ch07_interrupt.md) §1 的位元表查：bit 9 = **DPU group 1**。

**硬體其實算完了。** 它拉了中斷線、寫了狀態。只是沒人在聽。

**`task counter: 0x0`** —— 讀自 `pc_task_status`（`0x003c`），
表示還沒送出的 task 數是 0，**也就是全部送完了**。

> **驅動在逾時時把所有能問的都問了一遍，才放棄。**
> 這幾行 log 幾乎足夠判斷「是硬體沒動、還是軟體沒收到」——
> 這次的答案很清楚是後者。
>
> **卡住的時候，先去 `dmesg` 找這幾行。**

---

## TRM 與實機對不上的地方（完整版）

全書累積，18 筆。

| # | 章 | 說法來源 | 實機／程式碼 | 判斷 |
|---|---|---|---|---|
| 1 | ch01 | TRM 暫存器表沒有 `0x0000` / `0x0004` | 有，`0x0000` = `"FIRE"` | TRM 漏寫 |
| 2 | ch01 | TRM 位址表說 GLOBAL 是 `0xf000~0xf004` | 同章摘要表卻有 `0xF008` | TRM 自己矛盾 |
| 3 | ch01 | TRM 說 `0x0020` bit31 = RO / reset 0 | 讀到 `1` | 不明，待查 |
| 4 | ch01/02/04 | TRM 完全沒提 `rockchip,iommu-v2` | 實測確認它在做 IOVA 翻譯 | TRM 沒收錄 |
| 5 | ch02 | TRM §36.5.2 只提 AHB / AXI 兩個時脈域 | 另有 `clk_npu`、`pclk` | 合理，非錯誤 |
| 6 | ch04 | TRM 沒提 IOMMU domain | `MAX_IOMMU_DOMAIN_NUM = 16` | 待查 |
| 7 | ch05 | TRM 沒有 regcmd 格式 | 實測解出 8 bytes 三欄位 | TRM 沒收錄 |
| 8 | ch05 | TRM 說 `datain_channel_real` 是實際通道數 | 實測一致少 1 | TRM 描述不精確 |
| 9 | ch05 | — | regcmd `tag` 低位元組固定 `0x01` | 待查 |
| 10 | ch06 | SDK `rknn_api.h:238` 說 AUTO 是 *"randomly"* | 實際固定選 core0 | SDK 註解與行為不符 |
| 11 | ch06 | TRM §36.5.4 步驟 4 只說 write `pc_op_enable` | 驅動寫 `1` 後又寫 `0` | TRM 沒寫，待查 |
| 12 | ch07 | TRM §36.5.3 說有 17 種事件 | §36.4.3 只列到 bit 13 | TRM 少列 bit 14~16 |
| 13 | ch07 | 模組參數 `0644` 看似執行時可調 | `bypass_irq_handler` 只在 probe 讀取 | 權限位元誤導 |
| 14 | ch07 | — | irq handler 三個 `return` 全是 `IRQ_HANDLED` | 只做觀察，反向情境未驗證 |
| 15 | ch08 | governor 名叫 `rknpu_ondemand` | 從不讀取負載 | 名字誤導 |
| 16 | ch08 | dtsi 基準說 1 GHz 要 850 mV | 實測 825 mV = `opp-microvolt-L2` | 不是落差，是體質分級 |
| 17 | **ch09** | TRM §36.5.2 說兩個 reset 訊號 **"must be release at the same time"** | 驅動用 `for` 迴圈**一個一個放開**，core0 的 A/H 中間隔 3 次寫入 | **驅動與 TRM 不一致**。實測沒出問題，但不等於符合規格 |
| 18 | **ch09** | TRM 說 `RKNN_pc_base_address` reset value 是 `0x0` | 軟重置後讀到 `0x1`（與 ch01 閒置時觀察一致） | 待查 |

### 另外，本書自己修正過的地方

| 檔案 | 原本寫的 | 實機發現 |
|---|---|---|
| `BRIEF.md` 附錄 A.1 | debugfs 有 `mm` 節點 | 沒有，`CONFIG_ROCKCHIP_RKNPU_SRAM` 未開 |
| `BRIEF.md` 附錄 A.1 | 節點 `0444` 看似唯讀 | root 寫得進去（`CAP_DAC_OVERRIDE`） |
| `BRIEF.md` 附錄 A.1 | `reset` = 手動觸發重置 | **對一半**：也是 bypass 的開關，讀值與重置無關（本章 §5） |
| `OUTLINE.md` ch02 | misc 裝置是「順便註冊的備用門」 | 互斥的 `#ifdef` 分支，只會走一條 |
| [ch05](./ch05_regcmd.md) 初稿推測 | 分條多出的 2 列 = 卷積接縫重疊 | YOLOv5s 的加總剛好等於輸入高，**推測被推翻**，改標待查 |

> **一份誠實的教材，要留下自己改過哪裡。**

---

## 本章結論一句話

> **`rknpu_reset.c` 只有 158 行，但它是整份驅動唯一的「出事了怎麼辦」。**
>
> 拉住 6 個 reset 訊號、等 10 微秒、放開、把 IOMMU 重新掛一次。
> 在那之前先 `msleep(100)`、把所有等待中的行程叫醒，
> 讓它們知道「不是完成，是在重置」。
>
> 而 TRM 說兩個訊號要**同時放開** —— 驅動是用迴圈一個一個放的。
> **實測沒出問題，但那不是「符合規格」的證明。**

---

## 全書回顧

十章走完了一次 `rknn_run()`：

| 章 | 一句話 |
|---|---|
| [ch00](./ch00_why_npu_is_a_drm_driver.md) | NPU 掛在圖形子系統上，但只借記憶體管理，不畫圖 |
| [ch01](./ch01_hardware.md) | 三顆核心，CNA → DPU → PPU，暫存器按第四位數分區 |
| [ch02](./ch02_probe.md) | probe 照 device tree 把時脈、電源、中斷、IOMMU 一樣樣接起來 |
| [ch03](./ch03_six_ioctls.md) | 使用者空間與核心之間只有六句話，五句在管記憶體 |
| [ch04](./ch04_memory.md) | 一塊記憶體三個名字；`dma_addr` 是 IOMMU 造的假象 |
| [**ch05**](./ch05_regcmd.md) | **模型被編譯成一串暫存器寫入指令，躺在 DRAM 裡** |
| [ch06](./ch06_submit.md) | 驅動只寫 8 個暫存器就發動整個神經網路，然後去睡 |
| [ch07](./ch07_interrupt.md) | 中斷是硬體通知軟體的唯一管道 |
| [ch08](./ch08_power_freq.md) | 全有或全無：要用就滿速，閒置三秒就斷電 |
| [ch09](./ch09_reset.md) | 出事就重置，158 行的安全網 |

### 全書用過的工具

| 工具 | 用在哪 |
|---|---|
| [`tools/rkspy.c`](./tools/rkspy.c) | ch04、ch05、ch06 —— `LD_PRELOAD` 攔 ioctl，全書的主力 |
| [`experiments/exp00_whoami.c`](./experiments/exp00_whoami.c) | ch00 —— 問每個 DRM 節點「你是誰」 |
| [`experiments/exp01_regs.c`](./experiments/exp01_regs.c) | ch01、ch09 —— `/dev/mem` 直接讀暫存器 |
| [`experiments/exp03_action.c`](./experiments/exp03_action.c) | ch03 —— 算 ioctl 號碼、問遍 ACTION |
| [`experiments/exp03_count.sh`](./experiments/exp03_count.sh) | ch03 —— `strace -e raw=ioctl` 統計 |
| [`experiments/exp05_run.c`](./experiments/exp05_run.c) | ch05~ch09 —— 通用 `.rknn` 執行器 |

### 幾條反覆用到的通則

1. **看不到程式碼，就看它說了什麼。** `librknnrt.so` 閉源，但 ioctl 攔得到。
2. **同一件事至少換兩種方法量。** 「一次推論 2 個中斷」用了三種。
3. **跑完實驗去 `dmesg` 撿屍體。** 驅動留下的線索比想像中多。
4. **看到模組參數，grep 它被讀在哪、寫在哪。** 權限位元不保證任何事。
5. **看不懂的 log 通常藏著最有意思的事。** 那三行 `can't request region` 就是。
6. **找不到就說找不到。** 全書 18 筆落差，其中 6 筆的結論是「待查」。

---

**回到** → [BRIEF.md 任務簡報](./BRIEF.md) ｜ [OUTLINE.md 大綱](./OUTLINE.md)
