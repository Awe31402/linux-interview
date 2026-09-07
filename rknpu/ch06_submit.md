# ch06 — 送出：driver 只寫 8 個暫存器

> **本章目的**：[ch05](./ch05_regcmd.md) 證明了「模型 = 一串暫存器指令」。
> 這章講**驅動怎麼把它交出去** —— 答案短得有點誇張：**寫 8 個暫存器，然後去睡覺。**
>
> 順便把兩個一直懸著的問題解掉：
> 為什麼一次推論送 2 個 `SUBMIT`？那個 `subcore_task[5]` 為什麼是 5？
>
> **實驗平台**：Radxa ROCK 5B（RK3588），`Linux rock-5b 6.1.115+`
>
> **對照素材**
> - `drivers/rknpu/rknpu_job.c`
>   `:272`（`rknpu_job_subcore_commit_pc`，那 8 個 `REG_WRITE`）、
>   `:508`（`rknpu_schedule_core_index`）、`:525`（`rknpu_job_schedule`）、
>   `:746`（`rknpu_submit`）、`:176`（`rknpu_job_wait`）
> - `drivers/rknpu/rknpu_drv.c:183`（`rk3588_rknpu_config`）
> - `drivers/rknpu/include/rknpu_job.h:24`（`RKNPU_CORE_*_MASK`）
> - TRM §36.5.4 NPU operate flow、§36.5.1 Ping-pong registers
>
> **上一章** → [ch05 ★ regcmd](./ch05_regcmd.md)

---

## 目錄

- [一鍵重現本章全部實驗](#一鍵重現本章全部實驗)
- [1. TRM 直接給了操作步驟](#1-trm-直接給了操作步驟)
- [2. 那 8 個 REG_WRITE](#2-那-8-個-reg_write)
- [3. SUBMIT 從進來到回去](#3-submit-從進來到回去)
- [4. core_mask：三顆核心怎麼分工](#4-core_mask三顆核心怎麼分工)
- [5. `subcore_task[5]` 為什麼是 5](#5-subcore_task5-為什麼是-5)
- [6. 中斷數的算術，完全對得起來](#6-中斷數的算術完全對得起來)
- [7. 三核快多少](#7-三核快多少)
- [8. Ping-pong：TRM、flags、regcmd 三邊接起來](#8-ping-pongtrmflagsregcmd-三邊接起來)
- [TRM 與實機對不上的地方（累積）](#trm-與實機對不上的地方累積)
- [本章結論一句話](#本章結論一句話)

---

## 一鍵重現本章全部實驗

```bash
mkdir -p ~/rknpu-lab && cd ~/rknpu-lab
# 需要 tools/rkspy.c 和 experiments/exp05_run.c（ch05 已建好）
gcc -shared -fPIC -O1 -o rkspy.so rkspy.c -ldl
gcc -O1 -o exp05_run exp05_run.c -lrknnrt

M=~/disk/rknn/rknn-toolkit2/rknpu2/examples/rknn_api_demo/model/RK3588/mobilenet_v1.rknn

# ---- 實驗 6.1 / 6.2：不同 core_mask 下的負載與中斷 ----
# exp05_run <模型> <core_mask> <次數>；0=AUTO 1=core0 2=core1 4=core2 3=core0+1 7=三核
for m in 0 1 2 4 7; do
  b=$(grep fdab0000.npu /proc/interrupts | awk '{printf "%s ", $2}')
  sudo ./exp05_run $M $m 200 >/dev/null 2>&1 &
  sleep 1.2; sudo cat /sys/kernel/debug/rknpu/load; wait
  echo "mask=$m  中斷 前:[$b] 後:[$(grep fdab0000.npu /proc/interrupts | awk '{printf "%s ", $2}')]"
done

# ---- 實驗 6.3：SUBMIT 的結構（單核 vs 三核）----
sudo env LD_PRELOAD=./rkspy.so RKSPY_LOG=/tmp/c1.log RKSPY_RAW=0 ./exp05_run $M 0 1
sudo env LD_PRELOAD=./rkspy.so RKSPY_LOG=/tmp/c3.log RKSPY_RAW=0 ./exp05_run $M 7 1
grep -E "SUBMIT #|flags=|subcore\[" /tmp/c1.log
grep -E "SUBMIT #|flags=|subcore\[" /tmp/c3.log

# ---- 實驗 6.4：三核快多少 ----
for m in 0 1 3 7; do
  ./exp05_run $M $m 20 >/dev/null 2>&1          # 暖機
  s=$(date +%s.%N); ./exp05_run $M $m 300 >/dev/null 2>&1; e=$(date +%s.%N)
  echo "mask=$m  $(echo "($e-$s)/300*1000" | bc -l | cut -c1-6) ms/次"
done
```

---

## 1. TRM 直接給了操作步驟

### 結論

TRM §36.5.4「NPU operate flow」不但講了怎麼用，還畫了流程圖（Fig. 36-3）。
原文照抄（`chapter_36.txt:8973`）：

> RKNN has two types of work mode: **slave configured mode** and **pc work mode**.
> Pc work mode need to initial register information to the system memory, then obey
> flowing flows as Figure 1-3 shows.

流程圖的四個步驟（原文照抄）：

```
Start
  ↓
Initial register information in system memory
  ↓
Write pc_interrupt_clear and pc_interrupt_mask
  ↓
Write pc_amount / pc_addr / pc_task_number / pc_task_dma_base
  ↓
Write pc_op_enable to start a group of tasks
  ↓
End
```

**兩種工作模式**，這是理解整章的關鍵：

| 模式 | 意思 |
|---|---|
| **slave configured mode** | CPU 一筆一筆寫暫存器。傳統做法。 |
| **PC work mode** | 設定先放記憶體，NPU 自己去讀。**我們看到的就是這個。** |

（`PC` 這裡是 **Program Counter**，不是個人電腦。）

我們在 [ch05](./ch05_regcmd.md) 挖到的 `regcmd`，就是第一步「initial register
information in system memory」的產物。**剩下三步就是本章的主題。**

---

## 2. 那 8 個 REG_WRITE

### 結論

`rknpu_job.c:272` 的 `rknpu_job_subcore_commit_pc()` —— 整份驅動最關鍵的函式。
中間那段只有 8 行真的碰硬體：

```c
	REG_WRITE(first_task->regcmd_addr, RKNPU_OFFSET_PC_DATA_ADDR);      /* 1 */

	REG_WRITE((first_task->regcfg_amount + RKNPU_PC_DATA_EXTRA_AMOUNT +
		   pc_data_amount_scale - 1) / pc_data_amount_scale - 1,
		  RKNPU_OFFSET_PC_DATA_AMOUNT);                              /* 2 */

	REG_WRITE(last_task->int_mask,  RKNPU_OFFSET_INT_MASK);              /* 3 */
	REG_WRITE(first_task->int_mask, RKNPU_OFFSET_INT_CLEAR);             /* 4 */

	REG_WRITE(((0x6 | task_pp_en) << pc_task_number_bits) | task_number,
		  RKNPU_OFFSET_PC_TASK_CONTROL);                             /* 5 */

	REG_WRITE(args->task_base_addr, RKNPU_OFFSET_PC_DMA_BASE_ADDR);      /* 6 */

	job->first_task = first_task;
	job->last_task = last_task;
	job->int_mask[core_index] = last_task->int_mask;

	REG_WRITE(0x1, RKNPU_OFFSET_PC_OP_EN);                               /* 7 GO! */
	REG_WRITE(0x0, RKNPU_OFFSET_PC_OP_EN);                               /* 8 */
```

**就這樣。** 一整個神經網路，8 次 `writel()`。

### 逐一對回 TRM

| # | 驅動寫的 | offset | TRM §36.4.2 的名字 | TRM §36.5.4 的哪一步 |
|---|---|---|---|---|
| 3 | `last_task->int_mask` | `0x0020` | `RKNN_pc_interrupt_mask` | **步驟 2** |
| 4 | `first_task->int_mask` | `0x0024` | `RKNN_pc_interrupt_clear` | **步驟 2** |
| 2 | 換算過的筆數 | `0x0014` | `RKNN_pc_register_amounts` | **步驟 3**（`pc_amount`） |
| 1 | `regcmd_addr` | `0x0010` | `RKNN_pc_base_address` | **步驟 3**（`pc_addr`） |
| 5 | `task_number` + 旗標 | `0x0030` | `RKNN_pc_task_con` | **步驟 3**（`pc_task_number`） |
| 6 | `task_base_addr` | `0x0034` | `RKNN_pc_task_dma_base_addr` | **步驟 3**（`pc_task_dma_base`） |
| 7/8 | `1` 然後 `0` | `0x0008` | `RKNN_pc_operation_enable` | **步驟 4** |

**TRM 的四個步驟，一步不漏、一步不多。**

> 這是全書最乾淨的一次 TRM ↔ 程式碼對照。
> 手冊怎麼寫，驅動就怎麼做。

### 兩個細節

**① 位址被拆成兩半。**

`REG_WRITE` 是 `writel()`，**只寫 32 位元**（`rknpu_job.c:22`）：

```c
#define _REG_WRITE(base, value, offset) writel(value, base + (offset))
```

但 RK3588 的 DMA 位址是 **40 位元**（`rknpu_drv.c:186` `.dma_mask = DMA_BIT_MASK(40)`）。

所以低位塞進 `PC_DATA_ADDR`，高位的基底走 `PC_DMA_BASE_ADDR`。
兩個湊起來才是完整位址。

**② `PC_OP_EN` 要寫 1 再寫 0。**

那是一個**脈衝**，不是開關。寫 1 觸發，馬上寫 0 放開。
TRM 步驟 4 只說「write pc_op_enable to start」，**沒說要寫回 0**。
驅動多做了這一步，**TRM 沒寫原因，不猜**。

---

## 3. SUBMIT 從進來到回去

### 結論

`rknpu_submit()`（`rknpu_job.c:746`）的骨架：

```
① 檢查參數
     task_number == 0 → 錯
     core_mask > config->core_mask（RK3588 是 0x7）→ 錯
② rknpu_job_alloc()          配一個 job，算出 use_core_num
③ fence 處理                 本機 config 沒開，跳過（ch04 §6）
④ rknpu_job_schedule(job)    決定用哪顆核心、排進佇列、送進硬體
⑤ rknpu_job_wait(job)        睡著，等中斷叫醒
⑥ rknpu_job_cleanup(job)     收工
```

第 ⑤ 步是重點（`rknpu_job.c:194`）：

```c
		ret = wait_event_timeout(subcore_data->job_done_wq,
					 job->flags & RKNPU_JOB_DONE ||
						 rknpu_dev->soft_reseting,
					 msecs_to_jiffies(args->timeout));
```

**寫完那 8 個暫存器之後，驅動就在這裡睡著。**

叫醒它的是中斷 —— 那是 [ch07](./ch07_interrupt.md) 的主題。

> 注意 `while (ret == 0 && continue_wait)` 那個迴圈：
> 逾時之後不會馬上放棄，**最多再等 3 輪**（`wait_count >= 3`）。
> 每一輪都會印一則很長的 `LOG_ERROR`，內容包含 `job` 指標、`core_mask`、
> 已經等了多久。
>
> **卡住的時候去 `dmesg` 找這則訊息。** 它是這個驅動最有用的除錯輸出。

---

## 4. core_mask：三顆核心怎麼分工

### 結論

`rknpu_job.h:24` 定義了核心遮罩：

```c
#define RKNPU_CORE_AUTO_MASK 0x00      /* 你挑 */
#define RKNPU_CORE0_MASK     0x01
#define RKNPU_CORE1_MASK     0x02
#define RKNPU_CORE2_MASK     0x04
```

SDK 那邊有對應的公開 API（`/usr/include/rknn_api.h:237`）：

```c
typedef enum _rknn_core_mask {
    RKNN_NPU_CORE_AUTO = 0,      /* default, run on NPU core randomly. */
    RKNN_NPU_CORE_0 = 1,
    RKNN_NPU_CORE_1 = 2,
    RKNN_NPU_CORE_2 = 4,
    RKNN_NPU_CORE_0_1   = 3,
    RKNN_NPU_CORE_0_1_2 = 7,
    RKNN_NPU_CORE_ALL = 0xffff,
} rknn_core_mask;

int rknn_set_core_mask(rknn_context context, rknn_core_mask core_mask);
```

前面幾章我們一直看到 `core_mask=0x0`，也就是 **AUTO**。
驅動收到 `0` 時自己挑（`rknpu_job.c:532`）：

```c
	if (job->args->core_mask == RKNPU_CORE_AUTO_MASK) {
		core_index = rknpu_schedule_core_index(rknpu_dev);
		job->args->core_mask = rknpu_core_mask(core_index);
		job->use_core_num = 1;              /* ← 只用一顆 */
		...
	}
```

而 `rknpu_schedule_core_index()`（`:508`）是一個**最少待辦優先**的排程器：

```c
	int task_num = rknpu_dev->subcore_datas[0].task_num;
	int core_index = 0;
	for (i = 1; i < core_num; i++) {
		if (task_num > rknpu_dev->subcore_datas[i].task_num) {
			core_index = i;
			task_num = rknpu_dev->subcore_datas[i].task_num;
		}
	}
	return core_index;
```

**三顆都閒著的時候，`task_num` 全是 0，`>` 永遠不成立 → 固定回 `core_index = 0`。**

> 這就是前面幾章「為什麼只有 Core0 在動」的答案。
> `rknn_api.h` 的註解說 AUTO 是 *"run on NPU core randomly"* ——
> **實際上一點都不隨機**，閒置時永遠是 core0。
> （這是 SDK 標頭檔的註解與驅動行為不符，記進落差表。）

### 實機驗證（6.1 / 6.2）：五種模式，各跑 200 次推論

```
AUTO(0)          NPU load:  Core0: 38%, Core1:  0%, Core2:  0%
                 中斷 前:[148 0 0]  後:[548 0 0]        → IRQ0 +400

CORE_0(1)        NPU load:  Core0: 21%, Core1:  0%, Core2:  0%
                 中斷 前:[548 0 0]  後:[948 0 0]        → IRQ0 +400

CORE_1(2)        NPU load:  Core0:  0%, Core1: 42%, Core2:  0%
                 中斷 前:[948 0 0]  後:[948 400 0]      → IRQ1 +400

CORE_2(4)        NPU load:  Core0:  0%, Core1:  0%, Core2: 55%
                 中斷 前:[948 400 0] 後:[948 400 400]   → IRQ2 +400

三核(7)          NPU load:  Core0: 24%, Core1: 22%, Core2: 22%
                 中斷 前:[948 400 400] 後:[1948 800 800] → IRQ0 +1000, IRQ1 +400, IRQ2 +400
```

三件事：

1. **指定哪顆就跑哪顆。** `core_mask` 真的有效。
2. **單核模式：200 次推論 = 400 個中斷。** 正好是 [ch03](./ch03_six_ioctls.md) 數到的
   「一次推論 2 個 `SUBMIT`」× 200。
3. **三核模式的中斷數不對稱** —— IRQ0 多了 1000，IRQ1/IRQ2 各 400。
   為什麼？下一節解。

> ⚠️ `load` 的百分比是**瞬間取樣**（`rknpu_init_timer` 定期抽），
> 同樣的工作量在不同時間點抽會不一樣（AUTO 38% vs CORE_0 21% 就是這樣）。
> **中斷計數才是可靠的度量。**

---

## 5. `subcore_task[5]` 為什麼是 5

### 結論

`struct rknpu_submit` 裡有一個一直沒解釋的欄位：

```c
	struct rknpu_subcore_task subcore_task[5];
```

**三顆核心，為什麼是 5 個？**

答案在 `rknpu_job_subcore_commit_pc()`（`rknpu_job.c:313`）：

```c
		switch (job->use_core_num) {
		case 1:
		case 2:
			task_start  = args->subcore_task[core_index].task_start;
			task_number = args->subcore_task[core_index].task_number;
			break;
		case 3:
			task_start  = args->subcore_task[core_index + 2].task_start;
			task_number = args->subcore_task[core_index + 2].task_number;
			break;
		}
```

- **用 1 或 2 顆核心** → 讀 `subcore_task[core_index]`，也就是索引 **0、1**
- **用 3 顆核心** → 讀 `subcore_task[core_index + 2]`，也就是索引 **2、3、4**

**`5 = 2 + 3`。** 兩套互不重疊的分工表，塞在同一個陣列裡。

### 實機驗證（6.3）：兩種模式的 SUBMIT 結構

**單核（AUTO）—— 2 個 SUBMIT：**

```
================ SUBMIT #1 ================
flags=0x5 task_start=0 task_number=120 core_mask=0x0
  subcore[0]: start=0 number=40
  subcore[1]: start=0 number=40
  subcore[2]: start=0 number=40
  subcore[3]: start=0 number=0        ← 沒用到
  subcore[4]: start=0 number=0        ← 沒用到

================ SUBMIT #2 ================
flags=0x1 task_start=40 task_number=30 core_mask=0x0
  subcore[0]: start=40 number=10
  ...
```

**三核 —— 5 個 SUBMIT：**

```
SUBMIT #1  flags=0x5 task_number=84  core_mask=0x7   ← 三核
  subcore[2]: start=131 number=28     ┐
  subcore[3]: start=174 number=28     ├ 三份不同的工作！
  subcore[4]: start=204 number=28     ┘

SUBMIT #2  flags=0x5 task_number=1   core_mask=0x1   ← 只有 core0
SUBMIT #3  flags=0x5 task_number=6   core_mask=0x7   ← 三核
SUBMIT #4  flags=0x5 task_number=2   core_mask=0x1   ← 只有 core0
SUBMIT #5  flags=0x1 task_number=10  core_mask=0x1   ← 只有 core0
```

看 `SUBMIT #1`：**`subcore[2]/[3]/[4]` 的 `start` 是三個不同的值**
（131 / 174 / 204），各 28 個 task。三顆核心各做一份。

而 `subcore[0]/[1]` 在三核模式下**根本沒被讀**（雖然 SDK 還是填了值）。

> **一個猜不到、但一看程式碼就懂的設計。**
> 如果只看 `struct` 定義，永遠想不通為什麼是 5。

### 順便回答：為什麼三核要送 5 次？

因為**不是每一層都能拆給三顆核心做**。

`SUBMIT #2 / #4 / #5` 的 `core_mask` 都是 `0x1` —— SDK 主動把這幾段
指定成**只跑 core0**。

**為什麼那幾層不能拆，`librknnrt.so` 是閉源的，看不到決策邏輯。**
我們只看得到結果。

---

## 6. 中斷數的算術，完全對得起來

### 結論

三核模式為什麼 IRQ0 多了那麼多？把上一節的 5 個 SUBMIT 排開來算：

| SUBMIT | `core_mask` | 會叫醒哪幾顆核心 | IRQ0 | IRQ1 | IRQ2 |
|---|---|---|---|---|---|
| #1 | `0x7` 三核 | core0, core1, core2 | +1 | +1 | +1 |
| #2 | `0x1` | core0 | +1 | | |
| #3 | `0x7` 三核 | core0, core1, core2 | +1 | +1 | +1 |
| #4 | `0x1` | core0 | +1 | | |
| #5 | `0x1` | core0 | +1 | | |
| **一次推論合計** | | | **5** | **2** | **2** |

乘上 200 次推論：

| | 預測 | 實測 |
|---|---|---|
| IRQ0 | 5 × 200 = **1000** | **+1000** ✅ |
| IRQ1 | 2 × 200 = **400** | **+400** ✅ |
| IRQ2 | 2 × 200 = **400** | **+400** ✅ |

**一個都不差。**

> 這種「先預測、再量、對得上」的驗證，比任何解釋都有說服力。
> 而且它同時確認了三件事：
> `SUBMIT` 的結構讀對了、`core_mask` 的語意讀對了、
> **「一顆核心做完一段工作 = 發一個中斷」這個模型是對的。**

---

## 7. 三核快多少

### 實機驗證（6.4）

每種模式先暖機 20 次，再量 300 次：

| 模式 | 每次耗時 | FPS | 相對單核 |
|---|---|---|---|
| AUTO（實際 = core0） | 2.74 ms | 366 | 1.00× |
| 指定 core0 | 2.72 ms | 367 | 1.00× |
| core0 + core1 | 2.21 ms | 452 | **1.23×** |
| 三核 | **1.53 ms** | **652** | **1.78×** |

**三顆核心只快 1.78 倍，不是 3 倍。**

原因上一節已經看到了：三核模式的 5 個 `SUBMIT` 裡，**有 3 個是單核專用的**
（`core_mask=0x1`）。那幾段的時間完全沒有被平行化。

> 這是很實際的一課：**加核心不會線性變快。**
> 而且我們不是靠猜的 —— `rkspy` 直接看到哪幾段沒被拆開。
>
> 「哪些層可以拆、怎麼拆比較好」是模型編譯器的事，
> 屬於效能調校，不在本書範圍（見 [BRIEF.md](./BRIEF.md) 的界線）。

---

## 8. Ping-pong：TRM、flags、regcmd 三邊接起來

### 結論

一直出現的 `flags=0x5` 是什麼？查 `rknpu_ioctl.h:96`：

```c
enum e_rknpu_job_mode {
	RKNPU_JOB_SLAVE    = 0 << 0,
	RKNPU_JOB_PC       = 1 << 0,      /* 0x1 */
	RKNPU_JOB_BLOCK    = 0 << 1,
	RKNPU_JOB_NONBLOCK = 1 << 1,
	RKNPU_JOB_PINGPONG = 1 << 2,      /* 0x4 */
	...
};
```

`0x5` = `PC | PINGPONG`。`0x1` = 只有 `PC`。

**（順帶確認了 §1 講的兩種工作模式：`PC` 位元就是「PC work mode」。）**

`PINGPONG` 是什麼？TRM §36.5.1 說（`chapter_36.txt:8935`）：

> In order to reduce the time of fetch registers, every Calculate Core and Control Core
> of RKNN has its owner **ping-pong registers**. Configure the group 0 when use the
> group 1, and configure the group 1 when use group 0. As a result of hiding the time
> of fetch registers. **Writing S_POINTER of every block can enable this function.**

翻譯：每個單元有**兩組**暫存器。用第 0 組算的時候，同時把第 1 組設定好；
反過來也一樣。**設定的時間就被算的時間蓋掉了。**

而最後那句 —— *"Writing S_POINTER of every block can enable this function"* ——
把它跟 [ch05](./ch05_regcmd.md) 接起來了。回頭看我們挖到的 regcmd 前幾筆：

```
off=0x4004 val=0x0000000e tag=0x1001      ← RKNN_dpu_s_pointer
off=0x5004 val=0x0000000e tag=0x2001      ← RKNN_dpu_rdma_s_point
```

**那兩筆就是在開 ping-pong。** 名字裡的 `s_pointer` 正是 TRM 說的 `S_POINTER`。

驅動這邊也有對應（`rknpu_job.c:292` 與 `:361`）：

```c
	int task_pp_en = args->flags & RKNPU_JOB_PINGPONG ? 1 : 0;
	...
	REG_WRITE(((0x6 | task_pp_en) << pc_task_number_bits) | task_number,
		  RKNPU_OFFSET_PC_TASK_CONTROL);
```

`task_pp_en` 被塞進 `PC_TASK_CONTROL` 的高位元。

```
TRM §36.5.1  ──┐
               ├──▶ 同一個機制的三個面向
SUBMIT flags   ──┤    （ch06）
0x5 = PC|PP    ──┤
               │
regcmd 裡的    ──┘
S_POINTER 寫入      （ch05）
```

> 最後一個 `SUBMIT` 的 flags 是 `0x1`（沒有 ping-pong）。
> **為什麼最後一段不用，程式碼和 TRM 都沒說。不猜。**

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
| 10 | **ch06** | **SDK 標頭檔** `rknn_api.h:238` 說 AUTO 是 *"run on NPU core randomly"* | 驅動用**最少待辦優先**排程；三顆都閒時**固定選 core0**，一點都不隨機 | **SDK 註解與驅動行為不符** |
| 11 | **ch06** | TRM §36.5.4 步驟 4 只說 write `pc_op_enable` to start | 驅動寫 `1` 之後**又寫 `0`** | TRM 沒寫，待查 |

---

## 本章結論一句話

> **驅動把整個神經網路交給硬體，只用了 8 次 `writel()`** ——
> 而且那 8 次一步不漏地對應 TRM §36.5.4 畫的四個步驟。
>
> 寫完就去 `wait_event_timeout()` 睡覺，等中斷叫醒。
>
> 三顆核心靠 `core_mask` 分工，`subcore_task[5]` 的 `5 = 2 + 3`
> （1~2 核用前兩格，3 核用後三格）。
> 三核實測快 **1.78 倍**，不是 3 倍 —— 因為有幾段工作 SDK 指定只能跑 core0。

---

## 本章做過的實驗

| # | 實驗 | 結果 |
|---|---|---|
| 6.1 | 推論時看 `debugfs/load` | 指定哪顆就跑哪顆；`load` 是瞬間取樣，別過度解讀 |
| 6.2 | 五種 `core_mask` 的中斷增量 | 單核 200 次推論 = 400 中斷；三核為 1000/400/400 |
| 6.3 | 單核 vs 三核的 `SUBMIT` 結構 | 單核 2 個、三核 5 個；解出 `subcore_task[5]` = 2 + 3 |
| 6.4 | 三核加速比 | 1.78×（不是 3×），因為 5 個 SUBMIT 有 3 個只跑 core0 |

**中斷數預測 vs 實測：`5/2/2 × 200 = 1000/400/400`，完全吻合。**

---

**下一章** → [ch07 中斷：硬體怎麼說「我算完了」](./ch07_interrupt.md)
