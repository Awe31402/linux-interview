# ch07 — 中斷：硬體怎麼說「我算完了」

> **本章目的**：[ch06](./ch06_submit.md) 結束在驅動寫完 `PC_OP_EN` 之後
> **去睡覺**。這章講誰把它叫醒、怎麼叫醒的。
>
> 用 **ftrace** 把整條路徑抓出來，然後把「一次推論 2 個中斷」這件事
> **用第三種獨立方法**再確認一次。
>
> **實驗平台**：Radxa ROCK 5B（RK3588），`Linux rock-5b 6.1.115+`
>
> **對照素材**
> - `drivers/rknpu/rknpu_job.c`
>   `:615`（`rknpu_fuzz_status`）、`:640`（`rknpu_irq_handler`）、
>   `:684`（三個核心各自的進入點）、`:460`（`rknpu_job_done`）
> - `drivers/rknpu/rknpu_drv.c:65`（`bypass_irq_handler` 模組參數）、
>   `:76`（`rknpu_irqs` 表）、`:1071`（`rknpu_register_irq`）
> - TRM §36.5.3 NPU Interrupt Application、
>   §36.4.3 的 `RKNN_pc_interrupt_status`（`0x0028`）位元定義
>
> **上一章** → [ch06 送出](./ch06_submit.md)

---

## 目錄

- [一鍵重現本章全部實驗](#一鍵重現本章全部實驗)
- [1. TRM 說中斷有 17 種](#1-trm-說中斷有-17-種)
- [2. `int_mask = 0x300` 是什麼](#2-int_mask--0x300-是什麼)
- [3. 中斷處理程式，40 行看完](#3-中斷處理程式40-行看完)
- [4. 實驗 7.1：用 ftrace 抓完整呼叫鏈](#4-實驗-71用-ftrace-抓完整呼叫鏈)
- [5. 三顆核心怎麼「一起算完」](#5-三顆核心怎麼一起算完)
- [6. 一個看起來能關、其實關不掉的開關](#6-一個看起來能關其實關不掉的開關)
- [7. 共用中斷線的代價](#7-共用中斷線的代價)
- [TRM 與實機對不上的地方（累積）](#trm-與實機對不上的地方累積)
- [本章結論一句話](#本章結論一句話)

---

## 一鍵重現本章全部實驗

```bash
M=~/disk/rknn/rknn-toolkit2/rknpu2/examples/rknn_api_demo/model/RK3588/mobilenet_v1.rknn
T=/sys/kernel/debug/tracing

# ---- 實驗 7.1：ftrace 抓中斷處理的完整呼叫鏈 ----
sudo bash -c "
  echo 0 > $T/tracing_on; echo > $T/trace
  echo function_graph > $T/current_tracer
  echo rknpu_core0_irq_handler > $T/set_graph_function
  echo 1 > $T/tracing_on
  $PWD/exp05_run $M 1 1 >/dev/null 2>&1
  echo 0 > $T/tracing_on
  head -40 $T/trace
  grep -c 'rknpu_core0_irq_handler() {' $T/trace     # 一次推論幾個中斷
  echo nop > $T/current_tracer
"

# ---- 實驗 7.2：中斷計數 vs 推論次數（ch06 已做過，這裡再驗一次）----
grep fdab0000.npu /proc/interrupts | head -1
sudo ./exp05_run $M 1 20 >/dev/null
grep fdab0000.npu /proc/interrupts | head -1        # 應該 +40

# ---- 實驗 7.3：那個「關掉中斷」的模組參數，真的關得掉嗎 ----
cat /sys/module/rknpu/parameters/bypass_irq_handler
echo 1 | sudo tee /sys/module/rknpu/parameters/bypass_irq_handler
sudo ./exp05_run $M 1 20 >/dev/null                # 還是會動？
echo 0 | sudo tee /sys/module/rknpu/parameters/bypass_irq_handler
```

---

## 1. TRM 說中斷有 17 種

### 結論

TRM §36.5.3（`chapter_36.txt:8960`）：

> RKNN has **3 interrupt output signal** and it **remains asserted until the host
> processor clears the interrupt**. Each bit of `PC_INTERRUPT_STATUS` represents one
> of the **17 possible events** that the RKNN can signal to the host processor.
> By setting the bits of the interrupt enable register (`PC_INTERRUPT_MASK`) the
> programmer can control which of those events will generate an interrupt.

三件事：

1. **三條中斷線**（三顆核心各一條）—— 對上 [ch01](./ch01_hardware.md) 的 `/proc/interrupts`
2. **不清就不會停**（level-triggered）—— 對上 device tree 的 `IRQ_TYPE_LEVEL_HIGH`
3. **17 種事件**，用 `PC_INTERRUPT_MASK` 挑要哪幾種

TRM §36.4.3 給了 `RKNN_pc_interrupt_status`（`0x0028`）的位元定義：

| bit | 事件 |
|---|---|
| 0 | CNA feature **group 0** |
| 1 | CNA feature **group 1** |
| 2 | CNA weight group 0 |
| 3 | CNA weight group 1 |
| 4 | CNA csc group 0 |
| 5 | CNA csc group 1 |
| 6 | CORE group 0 |
| 7 | CORE group 1 |
| **8** | **DPU group 0** |
| **9** | **DPU group 1** |
| 10 | PPU group 0 |
| 11 | PPU group 1 |
| 12 | DMA read error |
| 13 | DMA write error |

**每個單元都有 group 0 / group 1 兩份。** 那就是 [ch06](./ch06_submit.md) §8 講的
**ping-pong 雙緩衝**。

> TRM 說「17 種」，但只列到 bit 13（共 14 個）。
> `int_st` 欄位是 `16:0`（17 位元），**bit 14~16 是什麼，TRM 沒列。**
> 待查。

---

## 2. `int_mask = 0x300` 是什麼

### 結論

[ch05](./ch05_regcmd.md) 挖 regcmd 時，每個 task 都印著 `int_mask=0x300`。

`0x300` = `0b11_0000_0000` = **bit 8 + bit 9**。

查上面那張表：**DPU group 0 + DPU group 1。**

意思是：**「DPU 做完就叫我，不管它用的是哪一組 ping-pong 暫存器。」**

```
CNA（卷積）→ DPU（逐點運算）→ PPU（池化）
                 ▲
                 └── 只在這裡設中斷
```

為什麼是 DPU 不是 PPU（生產線最後一站）？
**程式碼和 TRM 都沒說。** 合理的猜測是這個模型的每一段都以 DPU 收尾，
但那是**編譯器決定的**（`int_mask` 來自 `regcmd`，由 SDK 產生），不是驅動決定的。
**不猜。**

驅動只是原封不動地把它搬進暫存器（[ch06](./ch06_submit.md) 的 8 個 `REG_WRITE`）：

```c
	REG_WRITE(last_task->int_mask,  RKNPU_OFFSET_INT_MASK);
	REG_WRITE(first_task->int_mask, RKNPU_OFFSET_INT_CLEAR);
```

---

## 3. 中斷處理程式，40 行看完

### 結論

`rknpu_job.c:640`，整個處理程式就這麼長：

```c
static inline irqreturn_t rknpu_irq_handler(int irq, void *data, int core_index)
{
	struct rknpu_device *rknpu_dev = data;
	void __iomem *rknpu_core_base = rknpu_dev->base[core_index];
	struct rknpu_subcore_data *subcore_data = &rknpu_dev->subcore_datas[core_index];
	struct rknpu_job *job = NULL;
	uint32_t status = 0;

	/* ① 這顆核心現在在做哪個 job？ */
	spin_lock_irqsave(&rknpu_dev->irq_lock, flags);
	job = subcore_data->job;
	if (!job) {
		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);
		REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
		rknpu_job_next(rknpu_dev, core_index);
		return IRQ_HANDLED;
	}
	job->irq_entry[core_index] = true;
	spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

	/* ② 讀狀態 */
	status = REG_READ(RKNPU_OFFSET_INT_STATUS);
	job->int_status[core_index] = status;

	/* ③ 是我要的那個事件嗎？ */
	if (rknpu_fuzz_status(status) != job->int_mask[core_index]) {
		LOG_ERROR("invalid irq status: %#x, raw status: %#x, "
			  "require mask: %#x, task counter: %#x\n", ...);
		REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
		return IRQ_HANDLED;
	}

	/* ④ 清中斷 */
	REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);

	/* ⑤ 收工，可能叫醒等待者 */
	rknpu_job_done(job, 0, core_index);

	return IRQ_HANDLED;
}
```

三個核心各有一個進入點，只差一個編號（`:684`）：

```c
irqreturn_t rknpu_core0_irq_handler(int irq, void *data) { return rknpu_irq_handler(irq, data, 0); }
irqreturn_t rknpu_core1_irq_handler(int irq, void *data) { return rknpu_irq_handler(irq, data, 1); }
irqreturn_t rknpu_core2_irq_handler(int irq, void *data) { return rknpu_irq_handler(irq, data, 2); }
```

### 第 ④ 步：清中斷，一次清光

```c
#define RKNPU_INT_CLEAR 0x1ffff        /* rknpu_ioctl.h:36 */
```

`0x1ffff` = 17 個位元全 1。**不管來的是哪一種事件，全部清掉。**

呼應 TRM 那句 *"remains asserted until the host processor clears the interrupt"* ——
不清的話這條線會一直拉著，中斷會無限重複進來。

### 第 ③ 步：`rknpu_fuzz_status` 是什麼

`rknpu_job.c:615`：

```c
static inline uint32_t rknpu_fuzz_status(uint32_t status)
{
	uint32_t fuzz_status = 0;
	if ((status & 0x3)  != 0) fuzz_status |= 0x3;    /* bit 0,1 → 補成兩位都亮 */
	if ((status & 0xc)  != 0) fuzz_status |= 0xc;    /* bit 2,3 */
	if ((status & 0x30) != 0) fuzz_status |= 0x30;   /* bit 4,5 */
	...
}
```

**每一對「group 0 / group 1」被合併成一組。** 只要其中一個亮，就當成兩個都亮。

因為 ping-pong 的關係，**硬體實際用了哪一組是不確定的**。
驅動不在乎是哪一組，只在乎「那個單元做完了」。

所以 `int_mask = 0x300`（bit 8+9）可以跟「只有 bit 8 亮」的實際狀態對上。

> 這個函式名字取得很好：`fuzz`（模糊化）——
> **刻意把資訊變模糊，好讓比對成立。**

---

## 4. 實驗 7.1：用 ftrace 抓完整呼叫鏈

### 結論

板子上有 **92 個** rknpu 函式可以被 ftrace 追：

```bash
$ sudo grep -ci rknpu /sys/kernel/debug/tracing/available_filter_functions
92
```

用 `function_graph` 追 `rknpu_core0_irq_handler`，跑一次推論：

```
# tracer: function_graph
#
# CPU  TASK/PID       DURATION           FUNCTION CALLS
 0)  <idle>-0  |             |  rknpu_core0_irq_handler() {
 0)  <idle>-0  |   4.375 us  |    _raw_spin_lock_irqsave();
 0)  <idle>-0  |   2.333 us  |    _raw_spin_unlock_irqrestore();
 0)  <idle>-0  |   0.875 us  |    rknpu_get_task_number();
 0)  <idle>-0  |   2.333 us  |    _raw_spin_lock_irqsave();
 0)  <idle>-0  |   0.583 us  |    rknpu_get_task_number();
 0)  <idle>-0  |   2.041 us  |    ktime_get();
 0)  <idle>-0  |   2.041 us  |    _raw_spin_unlock_irqrestore();
 0)  <idle>-0  |   0.583 us  |    rknpu_iommu_domain_put();
 0)  <idle>-0  |             |    __wake_up() {                    ← ★
 0)  <idle>-0  |             |      __wake_up_common_lock() {
 0)  <idle>-0  |             |        __wake_up_common() {
 0)  <idle>-0  |             |          autoremove_wake_function() {
 0)  <idle>-0  |             |            default_wake_function() {
 0)  <idle>-0  |             |              try_to_wake_up() {
 0)  <idle>-0  |             |                select_task_rq_fair() {
 ...
 0)  <idle>-0  | + 94.500 us |  } /* rknpu_core0_irq_handler */
```

### 讀出來的東西

#### ① `<idle>-0` —— 中斷來的時候 CPU 是閒置的

因為呼叫 `rknn_run()` 的那個行程，正卡在
[ch06](./ch06_submit.md) §3 的 `wait_event_timeout()` 裡睡覺。
CPU 沒事做，就進了 idle。

**中斷是在「沒有任何相關行程在跑」的情況下發生的。** 這正是中斷的意義。

#### ② `rknpu_job_done()` 不見了 —— 被 inline 了

trace 裡看不到 `rknpu_irq_handler` 和 `rknpu_job_done`，
因為兩個都宣告成 `static inline`，被編譯器展平進 `rknpu_core0_irq_handler`。

但**它們的內容看得到**：`rknpu_get_task_number()`、`ktime_get()`、
`rknpu_iommu_domain_put()` 都是 `rknpu_job_done()` 裡的呼叫
（對照 `rknpu_job.c:460`）。

> **讀 trace 時看不到某個函式，先想想是不是被 inline 了。**
> 從「它呼叫了什麼」反推，通常認得出來。

#### ③ `ktime_get()` 就是 `load` 百分比的來源

`rknpu_job_done()` 裡（`rknpu_job.c:480`）：

```c
	now = ktime_get();
	job->hw_elapse_time = ktime_sub(now, job->hw_commit_time);
	subcore_data->timer.busy_time += ktime_sub(now, job->hw_recoder_time);
```

**每次中斷都累加一次 `busy_time`。** debugfs 的 `load` 就是拿它除以總時間。

（這也解釋了 [ch06](./ch06_submit.md) 為什麼說 `load` 是取樣值 ——
它是一段時間內的累計比例，不是瞬間值。）

#### ④ `__wake_up()` —— 整條路徑的終點

```
__wake_up → __wake_up_common_lock → __wake_up_common
          → autoremove_wake_function → default_wake_function
          → try_to_wake_up → select_task_rq_fair
```

`select_task_rq_fair()` 是排程器在挑「要把這個行程放到哪顆 CPU 上跑」。

**到這裡，睡在 `wait_event_timeout()` 的那個行程就被放回執行佇列了。**

一整圈閉合：

```
使用者 rknn_run()
   → ioctl(SUBMIT)
   → 驅動寫 8 個暫存器（ch06）
   → wait_event_timeout() 睡著
        ⋮  NPU 自己去 DRAM 讀 regcmd、算（ch05）
   → 硬體拉中斷
   → rknpu_core0_irq_handler()
   → __wake_up()
   → 行程醒來，ioctl 返回
```

#### ⑤ 一次推論，剛好 2 次

```bash
$ grep -c 'rknpu_core0_irq_handler() {' trace
2
```

**第三種獨立方法，同一個答案。**

| 方法 | 章 | 結果 |
|---|---|---|
| `strace` 數 `SUBMIT` ioctl | [ch03](./ch03_six_ioctls.md) | 2 |
| `/proc/interrupts` 差值 ÷ 推論次數 | [ch01](./ch01_hardware.md)、[ch06](./ch06_submit.md) | 2 |
| **ftrace 數處理程式進入次數** | **本章** | **2** |

#### ⑥ 耗時 94.5 µs 和 78.2 µs

> ⚠️ **這個數字包含 ftrace 自己的成本。** `function_graph` 會在每個函式進出
> 插樁，開銷不小。真實的中斷處理時間應該短很多。
> **不要拿這個數字去算效能。**

---

## 5. 三顆核心怎麼「一起算完」

### 結論

[ch06](./ch06_submit.md) 看到三核模式下，一個 `SUBMIT` 會產生 3 個中斷（三顆核心各一）。

那「job 什麼時候算完成」？答案在 `rknpu_job_done()`（`rknpu_job.c:485`）：

```c
	if (atomic_dec_and_test(&job->interrupt_count)) {
		/* 只有「最後一顆」核心的中斷會進到這裡 */
		job->flags |= RKNPU_JOB_DONE;
		job->ret = ret;
		if (job->fence)
			dma_fence_signal(job->fence);
		if (use_core_num > 1)
			wake_up(&(&rknpu_dev->subcore_datas[0])->job_done_wq);
		...
	}
```

`interrupt_count` 在 `rknpu_job_alloc()` 時被設成 `use_core_num`：

```c
	atomic_set(&job->interrupt_count, job->use_core_num);
```

**每來一個中斷就減一，減到 0 才算完成。**

```
三核模式，一個 SUBMIT：
  core0 中斷 → interrupt_count 3→2 → 還沒完，不叫醒
  core2 中斷 → interrupt_count 2→1 → 還沒完，不叫醒
  core1 中斷 → interrupt_count 1→0 → ★ 完成！叫醒等待者
```

順序無所謂，**誰最後到誰負責叫醒**。這是很標準的「屏障（barrier）」寫法。

### 還有一個分批機制

`rknpu_job_done()` 最前面（`:468`）：

```c
	if (atomic_inc_return(&job->submit_count[core_index]) <
	    (rknpu_get_task_number(job, core_index) + max_submit_number - 1) /
		    max_submit_number) {
		rknpu_job_subcore_commit(job, core_index);     /* 送下一批，不叫醒 */
		return;
	}
```

RK3588 的 `max_submit_number` 是 **4095**（`rknpu_drv.c:196`，`(1 << 12) - 1`）。

如果一次要送的 task 超過 4095，硬體吃不下，**驅動就分批**：
**用中斷當「送下一批」的觸發點**。

> 我們的模型只有 120 個 task，遠低於 4095，所以這條路走不到。
> 但 `rkspy` 印出的 `regcfg_amount` 和 task 數量，
> 讓你可以自己算什麼樣的模型會觸發它。

---

## 6. 一個看起來能關、其實關不掉的開關

### 結論

`rknpu_drv.c:65` 有一個模組參數：

```c
static int bypass_irq_handler;
module_param(bypass_irq_handler, int, 0644);
MODULE_PARM_DESC(bypass_irq_handler, ...);
```

權限 `0644` —— **看起來可以在執行時改**。

而且名字直白：「跳過中斷處理」。如果能關掉，就能證明
「中斷是硬體通知的唯一管道」。

### 實機驗證（7.3）：試著關掉它

```bash
$ cat /sys/module/rknpu/parameters/bypass_irq_handler
0
$ echo 1 | sudo tee /sys/module/rknpu/parameters/bypass_irq_handler
1
$ cat /sys/module/rknpu/parameters/bypass_irq_handler
1                                    ← 真的寫進去了

$ grep fdab0000.npu /proc/interrupts | head -1        # 6439
$ sudo ./exp05_run $M 1 20
rknn_run x20 -> 0                    ← 完全正常！
$ grep fdab0000.npu /proc/interrupts | head -1        # 6479  → +40
```

**參數改了，但推論照跑，中斷照來（20 次推論 = 40 個中斷，一個不少）。**

### 為什麼

grep 整份驅動，這個變數只被讀**兩次**，兩次都在 `rknpu_probe()` 裡：

```
rknpu_drv.c:1309:  rknpu_dev->bypass_irq_handler = bypass_irq_handler;
rknpu_drv.c:1392:  if (!rknpu_dev->bypass_irq_handler) {
                       ret = rknpu_register_irq(pdev, rknpu_dev);
```

**它決定的是「probe 時要不要註冊中斷」。**
裝置已經 probe 完了，中斷早就註冊好了，之後改這個變數不會有任何效果。

> ⚠️ **`0644` 給了錯誤的暗示。**
> 一個可寫的模組參數，看起來像執行時開關，實際上只在開機時有意義。
>
> **讀 driver 的習慣：看到模組參數，先 grep 它被讀幾次、在哪裡讀。**
> 權限位元不會告訴你這件事。

### ⏳ 真的要關掉的話

`CONFIG_ROCKCHIP_RKNPU=y`（內建，不是模組），所以只能靠**開機參數**：

```bash
# 1. 備份
sudo cp /boot/extlinux/extlinux.conf /boot/extlinux/extlinux.conf.bak

# 2. 在 append 那行尾端加上參數
sudo sed -i 's|\(^\s*append .*\)|\1 rknpu.bypass_irq_handler=1|' /boot/extlinux/extlinux.conf

# 3. 檢查改對了再重開
grep append /boot/extlinux/extlinux.conf
sudo reboot

# 4. 開機後
cat /proc/cmdline | tr ' ' '\n' | grep rknpu
grep fdab0000.npu /proc/interrupts        # 預期：整行消失（沒註冊）
sudo dmesg | grep -i "bypass irq"         # 預期：RKNPU ...: bypass irq handler!
sudo ./exp05_run $M 1 1                   # 預期：卡住然後 timeout
sudo dmesg | tail                         # 預期：failed to wait job ... -ETIMEDOUT

# 5. 還原
sudo cp /boot/extlinux/extlinux.conf.bak /boot/extlinux/extlinux.conf
sudo reboot
```

**這個實驗需要重開機兩次，本書尚未執行。**
風險不高（系統照常開機，只有 NPU 會逾時），
但改開機設定要自己確認過再做。

預期看到的東西已經從程式碼推得出來：
`rknpu_probe()` 會走 `LOG_DEV_WARN(dev, "bypass irq handler!\n")` 那一支
（`rknpu_drv.c:1397`），`rknpu_job_wait()` 會等到 `args->timeout` 逾時、
重試 3 輪、印出那則很長的 `LOG_ERROR`，最後回 `-ETIMEDOUT`。

---

## 7. 共用中斷線的代價

### 結論

[ch01](./ch01_hardware.md) 發現 NPU 和它的 IOMMU **共用同一條中斷線**：

```
 40:  ...  GICv3 142 Level   fdab9000.iommu, fdab0000.npu
```

[ch02](./ch02_probe.md) 找到原因：驅動註冊時給了 `IRQF_SHARED`（`rknpu_drv.c:1113`）。

**共用中斷的規矩是：中斷來的時候，掛在這條線上的每一個處理程式都會被呼叫。**
每個處理程式要自己判斷「是不是我的」，不是的話回 `IRQ_NONE`，讓核心去問下一個。

但 `rknpu_irq_handler()` 有三個 `return`，**全部是 `IRQ_HANDLED`**
—— 包括「這顆核心沒有 job」和「狀態不對」這兩條路。

而且在「沒有 job」那條路上，它還是會寫：

```c
		REG_WRITE(RKNPU_INT_CLEAR, RKNPU_OFFSET_INT_CLEAR);
```

> **本書只做觀察。** 這段程式碼在什麼情況下會出問題、會不會實際發生，
> 需要另外設計實驗才能回答（例如故意製造 IOMMU 頁面錯誤）。
> **沒驗證過的事不寫成結論。**
>
> 但「讀到 `IRQF_SHARED` 就要去看處理程式怎麼回傳」
> 是讀任何 driver 都該有的反射動作。

---

## TRM 與實機對不上的地方（累積）

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
| 12 | **ch07** | TRM §36.5.3 說有 **17 種**事件，欄位也是 `16:0` | §36.4.3 只列到 **bit 13**（14 種） | **TRM 少列了 bit 14~16**，待查 |
| 13 | **ch07** | 模組參數 `bypass_irq_handler` 權限 `0644`，看似執行時可調 | 只在 `rknpu_probe()` 讀取，執行時改**完全無效** | **權限位元造成誤導**（非 TRM 問題） |

---

## 本章結論一句話

> **驅動寫完 `PC_OP_EN` 就去睡，硬體算完拉中斷，處理程式清掉中斷、
> 累加耗時、然後 `__wake_up()` 把行程叫回來。**
>
> `int_mask = 0x300` 是「DPU 兩組 ping-pong 都算數」，
> `rknpu_fuzz_status()` 刻意把 group 0/1 模糊化，好讓比對成立。
>
> 三核模式靠 `interrupt_count` 當屏障：**誰最後到，誰負責叫醒。**
>
> 而「一次推論 2 個中斷」這件事，
> **`strace`、`/proc/interrupts`、`ftrace` 三種獨立方法給了同一個答案。**

---

## 本章做過的實驗

| # | 實驗 | 結果 |
|---|---|---|
| 7.1 | ftrace `function_graph` 追中斷處理 | 完整呼叫鏈到 `__wake_up() → try_to_wake_up()`；`<idle>-0` 證明行程確實睡著了 |
| 7.2 | 中斷計數 vs 推論次數 | 20 次推論 = 40 個中斷；ftrace 也數到 2 次／推論 |
| 7.3 | 執行時寫 `bypass_irq_handler=1` | **無效**。只在 probe 讀取，`0644` 是誤導 |
| ⏳ 7.4 | 用開機參數真的關掉中斷 | 需重開機兩次，**尚未執行**（步驟已寫在 §6） |

---

**下一章** → [ch08 電源與頻率：閒置三秒就關電](./ch08_power_freq.md)
