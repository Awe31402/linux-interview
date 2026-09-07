# ch08 — 電源與頻率：閒置三秒就關電

> **本章目的**：解釋為什麼每次量 `power` 都是 `off`、`delayms` 那個 3000 從哪來、
> 以及**電壓怎麼跟著頻率走**。
>
> 順便拆穿一個名字取錯的東西：叫 `rknpu_ondemand` 的調頻策略，
> **完全不看負載**。
>
> **實驗平台**：Radxa ROCK 5B（RK3588），`Linux rock-5b 6.1.115+`
>
> **對照素材**
> - `drivers/rknpu/rknpu_drv.c`
>   `:327`（`rknpu_power_off_delay_work`）、`:345`（`rknpu_power_get`）、
>   `:357`（`rknpu_power_put`）、`:375`（`rknpu_power_put_delay`）、
>   `:904`（`rknpu_power_on`）、`:1469`（workqueue）、`:1475`（`INIT_DEFERRABLE_WORK`）
> - `drivers/rknpu/rknpu_devfreq.c`
>   `:47`（`npu_devfreq_profile`）、`:54`（`devfreq_rknpu_ondemand_func`）、
>   `:281`（`npu_devfreq_target`）
> - `drivers/rknpu/rknpu_debugger.c:197`（debugfs 的 `freq` 寫入）
> - `arch/arm64/boot/dts/rockchip/rk3588s.dtsi:3482`（OPP 表）
> - TRM §36.5.2 Clock and Reset
>
> **上一章** → [ch07 中斷](./ch07_interrupt.md)

---

## 目錄

- [一鍵重現本章全部實驗](#一鍵重現本章全部實驗)
- [1. 為什麼要關電](#1-為什麼要關電)
- [2. 引用計數 + 延遲關電](#2-引用計數--延遲關電)
- [3. ★ DVFS：電壓跟著頻率走](#3--dvfs電壓跟著頻率走)
- [4. ★ 一個叫 ondemand 卻不看負載的東西](#4--一個叫-ondemand-卻不看負載的東西)
- [TRM 與實機對不上的地方（累積）](#trm-與實機對不上的地方累積)
- [本章結論一句話](#本章結論一句話)

---

## 一鍵重現本章全部實驗

```bash
D=/sys/kernel/debug/rknpu
M=~/disk/rknn/rknn-toolkit2/rknpu2/examples/rknn_api_demo/model/RK3588/mobilenet_v1.rknn

# ---- 8.1：電源三態 ----
sudo cat $D/power                      # 閒置 → off
sudo ./exp05_run $M 1 400 >/dev/null & sleep 1
sudo cat $D/power                      # 忙碌 → on
wait; sleep 4
sudo cat $D/power                      # 停 4 秒 → off

# ---- 8.2：改頻率，看電壓跟不跟 ----
for f in 300000000 500000000 800000000 1000000000; do
  echo $f | sudo tee $D/freq >/dev/null; sleep 0.3
  echo "freq=$(sudo cat $D/freq)  volt=$(sudo cat $D/volt)"
done

# ---- 8.3：delayms 決定關電時間 ----
for d in 500 1000 2000 3000 4000; do
  echo $d | sudo tee $D/delayms >/dev/null
  sudo ./exp05_run $M 1 1 >/dev/null 2>&1
  s=$(date +%s.%N)
  for i in $(seq 1 200); do [ "$(sudo cat $D/power)" = off ] && break; sleep 0.02; done
  e=$(date +%s.%N)
  echo "delayms=$d -> $(echo "($e-$s)*1000" | bc -l | cut -d. -f1) ms"
done
echo 3000 | sudo tee $D/delayms

# ---- 8.4：跑推論時頻率會自己升嗎 ----
sudo ./exp05_run $M 1 600 >/dev/null 2>&1 &
for i in 1 2 3 4 5 6; do echo "freq=$(sudo cat $D/freq) volt=$(sudo cat $D/volt)"; sleep 0.1; done
wait
```

---

## 1. 為什麼要關電

### 結論

NPU 有**三個獨立的電源域**（`rk3588s.dtsi:3473`）：

```dts
		power-domains = <&power RK3588_PD_NPUTOP>,
				<&power RK3588_PD_NPU1>,
				<&power RK3588_PD_NPU2>;
		power-domain-names = "npu0", "npu1", "npu2";
```

加上兩個電源軌（板子 dts 給的，[ch02](./ch02_probe.md) §2）：

```dts
		rknpu-supply = <&vdd_npu_s0>;       /* 主電源 */
		mem-supply   = <&vdd_npu_mem_s0>;   /* 記憶體電源 */
```

`rknpu_power_on()`（`rknpu_drv.c:904`）把它們一個一個打開：

```
① regulator_enable(vdd)                    開主電源
② regulator_enable(mem)                    開記憶體電源
③ clk_bulk_prepare_enable(所有時脈)         開 8 個時脈（ch02 §5 那張表）
④ pm_runtime_resume_and_get(genpd_dev_npu0)  開電源域 0
⑤ pm_runtime_resume_and_get(genpd_dev_npu1)  開電源域 1
⑥ pm_runtime_resume_and_get(genpd_dev_npu2)  開電源域 2
⑦ pm_runtime_get_sync(dev)                 裝置本身
```

**七個步驟，順序不能亂**（電源要先於時脈）。`rknpu_power_off()` 反過來做一遍。

> 這解釋了 [ch01](./ch01_hardware.md) 實驗 1.1 為什麼一定要先
> `echo on > power` 才能讀暫存器 —— 沒開電，AHB 匯流排根本沒有回應。

### 實機驗證（8.1）：電源三態

```
閒置:  power=off  freq=1000000000  volt=825000
忙碌:  power=on   freq=1000000000  volt=825000  load=Core0: 56%
停 4s: power=off
```

**沒工作就關電。** 一台開著但沒在跑 AI 的機器，NPU 是完全斷電的。

> 有趣的是：**關電了，`freq` 和 `volt` 還讀得到值。**
> 因為那兩個讀的是驅動記在 `rknpu_dev->current_freq` / `current_volt`
> 的**軟體狀態**，不是真的去問硬體（`rknpu_debugger.c` 的 show 函式）。
> **看到數字不代表硬體是活的。**

---

## 2. 引用計數 + 延遲關電

### 結論

[ch03](./ch03_six_ioctls.md) §6 看過：六個 ioctl 全被 `RKNPU_IOCTL` 巨集包起來，
進來 `rknpu_power_get()`、出去 `rknpu_power_put_delay()`。

這兩個函式是一組**引用計數**（`rknpu_drv.c:345`）：

```c
int rknpu_power_get(struct rknpu_device *rknpu_dev)
{
	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_inc_return(&rknpu_dev->power_refcount) == 1)   /* 0 → 1 才真的開 */
		ret = rknpu_power_on(rknpu_dev);
	mutex_unlock(&rknpu_dev->power_lock);
}
```

**只有從 0 變 1 的那一次才真的開電。** 之後的 ioctl 只是把計數加上去。

關電那邊多了一層（`rknpu_drv.c:375`）：

```c
int rknpu_power_put_delay(struct rknpu_device *rknpu_dev)
{
	if (rknpu_dev->power_put_delay == 0)
		return rknpu_power_put(rknpu_dev);        /* 設 0 就立刻關 */

	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_read(&rknpu_dev->power_refcount) == 1)
		queue_delayed_work(rknpu_dev->power_off_wq,
				   &rknpu_dev->power_off_work,
				   msecs_to_jiffies(rknpu_dev->power_put_delay));
	else
		atomic_dec_if_positive(&rknpu_dev->power_refcount);
	mutex_unlock(&rknpu_dev->power_lock);
}
```

**計數剩最後一個時，不馬上關 —— 排一個延遲工作。**

如果 `delayms` 之內又有新的 ioctl 進來，`power_get()` 會把計數推回去，
延遲工作跑到時就發現「還有人在用」，什麼都不做：

```c
static void rknpu_power_off_delay_work(struct work_struct *power_off_work)
{
	mutex_lock(&rknpu_dev->power_lock);
	if (atomic_dec_if_positive(&rknpu_dev->power_refcount) == 0)   /* 還是 1 才關 */
		ret = rknpu_power_off(rknpu_dev);
	mutex_unlock(&rknpu_dev->power_lock);
}
```

> **為什麼要延遲？** 開電那七個步驟不便宜。
> 連續推論時每次都關再開，成本會壓垮效能。
> **延遲關電 = 「等一下說不定還會用」的賭注。**

### 一個容易漏看的細節：`INIT_DEFERRABLE_WORK`

`rknpu_drv.c:1475`：

```c
	INIT_DEFERRABLE_WORK(&rknpu_dev->power_off_work,
			     rknpu_power_off_delay_work);
```

**`DEFERRABLE`（可延後）** 的意思是：這個計時器**不會為了自己去叫醒睡著的 CPU**，
它會等到下一次有別的事情把 CPU 叫醒時，順便一起處理。

> 這很合理 —— 為了「關掉一個已經沒在用的東西」而把整顆 CPU 叫醒，
> 反而更耗電。
>
> **但它也讓實際關電時間會比設定值晚一點，而且不固定。**
> 下面的實測正好看得到。

### 實機驗證（8.3）：`delayms` 真的決定關電時間嗎

```
設定值    實測關電時間    差值
 500 ms     418 ms        -82 ms
1000 ms     889 ms       -111 ms
2000 ms    1944 ms        -56 ms
3000 ms    3478 ms       +478 ms
4000 ms    4098 ms        +98 ms
```

**設定值變大，實測時間跟著變大，比例對得上。**

散布約 ±100 ms（有一次 +478 ms）。三個來源：

1. **我的碼錶起點不對** —— 倒數是從**最後一個 ioctl** 開始，
   但我是在**行程結束後**才開始計時。中間差了行程收尾的時間（負偏差）
2. **輪詢間隔 20 ms**
3. **`INIT_DEFERRABLE_WORK`** —— 計時器不主動叫醒 CPU（正偏差，而且不固定）

> `3000 → 3478` 那次偏差最大，符合第 3 點的特徵。
> **不過只有一次觀測，不足以當成結論，只能說「與 deferrable 的行為相容」。**

`delayms` 預設 3000 是**寫死在 `rknpu_probe()` 裡的**（[ch02](./ch02_probe.md) §6）：

```c
	// set default power put delay to 3s
	rknpu_dev->power_put_delay = 3000;
```

不是從 device tree 讀的。

---

## 3. ★ DVFS：電壓跟著頻率走

### 結論

**DVFS = Dynamic Voltage and Frequency Scaling**（動態電壓頻率調整）。

核心想法：**跑得慢就不用給那麼高的電壓。** 功耗大約跟「電壓平方 × 頻率」成正比，
所以降電壓省得比降頻率多。

哪個頻率配哪個電壓，寫在 device tree 的 **OPP 表**裡
（OPP = Operating Performance Point，工作效能點）。

`npu_devfreq_target()`（`rknpu_devfreq.c:281`）就是換檔的地方：

```c
	opp = devfreq_recommended_opp(dev, freq, flags);   /* 查 OPP 表 */
	opp_volt = dev_pm_opp_get_voltage(opp);            /* 這個頻率要幾伏 */
	dev_pm_opp_put(opp);

	if (*freq == rknpu_dev->current_freq)
		return 0;                                  /* 沒變就不動 */

	ret = dev_pm_opp_set_rate(dev, *freq);             /* 一起換頻率和電壓 */
	if (!ret) {
		rknpu_dev->current_freq = *freq;
		rknpu_dev->current_volt = opp_volt;
	}
```

**`dev_pm_opp_set_rate()` 一次把頻率和電壓都換掉**，順序由核心的 OPP 層負責
（升頻要先升壓，降頻要先降頻 —— 弄反了晶片會不穩）。

### 實機驗證（8.2）：改頻率，量電壓

```
設定頻率        實際 freq       volt
300000000       300000000       675000
500000000       500000000       675000
800000000       800000000       737500
1000000000      1000000000      825000
```

**電壓真的跟著動。** 而且 300 和 500 MHz 共用同一個電壓 ——
OPP 表本來就是分段的，不是連續的。

### ★ 然後對 dtsi，挖出這塊晶片的「體質等級」

拿實測值去對 `rk3588s.dtsi` 的 OPP 表：

| 頻率 | **實測** | dtsi 基準 `opp-microvolt` | 對得上嗎 |
|---|---|---|---|
| 300 MHz | 675000 | 700000 | ❌ 低了 25 mV |
| 500 MHz | 675000 | 700000 | ❌ 低了 25 mV |
| 800 MHz | 737500 | 750000 | ❌ 低了 12.5 mV |
| 1000 MHz | 825000 | 850000 | ❌ 低了 25 mV |

**全部比手冊低。** 那是壞掉嗎？

不是。dtsi 每個 OPP 底下還有 **L1~L5** 六組備選電壓：

```dts
		opp-1000000000 {
			opp-hz = /bits/ 64 <1000000000>;
			opp-microvolt    = <850000 850000 850000>, ... ;
			opp-microvolt-L1 = <837500 837500 850000>, ... ;
			opp-microvolt-L2 = <825000 825000 850000>, ... ;   /* ← */
			opp-microvolt-L3 = <812500 812500 850000>, ... ;
			opp-microvolt-L4 = <800000 800000 850000>, ... ;
			opp-microvolt-L5 = <787500 787500 850000>, ... ;
		};
```

**`825000` 正是 `opp-microvolt-L2`。**

再驗其他三個頻率：

| 頻率 | 實測 | dtsi 的 `opp-microvolt-**L2**` | |
|---|---|---|---|
| 300 MHz | 675000 | **675000** | ✅ |
| 500 MHz | 675000 | **675000** | ✅ |
| 800 MHz | 737500 | **737500** | ✅ |
| 1000 MHz | 825000 | **825000** | ✅ |

**四個頻率全中。這塊板子的 NPU 是 L2 等級。**

> **這是 [ch02](./ch02_probe.md) §5 那個伏筆的完整落地。**
>
> 當時只知道「dtsi 有 15 筆 OPP，devfreq 只吃 8 筆，由 eFuse 體質分級篩選」。
> 現在**連挑了哪一級都量出來了**。
>
> 晶片出廠時測過漏電和 PVTM，把等級燒進 eFuse。
> 體質好的晶片可以用更低的電壓跑同樣的頻率 → 更省電、更涼。
> **同一份 dtsi，不同板子開機後的電壓不一樣。**

想自己確認等級的話，dtsi 的對照規則就在 OPP 表開頭：

```dts
	rockchip,pvtm-voltage-sel = <
		0	815	0
		816	835	1
		...
	>;
	nvmem-cells = <&npu_leakage>, <&npu_opp_info>, ...;
```

---

## 4. ★ 一個叫 ondemand 卻不看負載的東西

### 結論

[ch02](./ch02_probe.md) §5 看到 governor 是 `rknpu_ondemand`，不是核心內建的
`simple_ondemand`。名字聽起來是「按需調頻」。

**它不是。**

`rknpu_devfreq.c:54`，整個 governor 就這麼多：

```c
static int devfreq_rknpu_ondemand_func(struct devfreq *df, unsigned long *freq)
{
	struct rknpu_device *rknpu_dev = df->data;

	if (rknpu_dev && rknpu_dev->ondemand_freq)
		*freq = rknpu_dev->ondemand_freq;      /* 有人指定就用指定的 */
	else
		*freq = df->previous_freq;             /* 否則維持原樣 */

	return 0;
}

static int devfreq_rknpu_ondemand_handler(struct devfreq *devfreq,
					  unsigned int event, void *data)
{
	return 0;                                      /* 什麼都不做 */
}
```

**它從頭到尾沒有看過負載。** 只是「回傳別人叫我用的頻率」。

而且回報負載的那個函式也是空的（`rknpu_devfreq.c:32`）：

```c
static int npu_devfreq_get_dev_status(struct device *dev,
				      struct devfreq_dev_status *stat)
{
	return 0;                    /* stat 一個欄位都沒填 */
}
```

devfreq 框架每 50 ms 會來問一次（`.polling_ms = 50`），**但問不到東西。**

### 誰會設 `ondemand_freq`

grep 整份驅動，只有兩處：

```
rknpu_devfreq.c:383    rknpu_dev->ondemand_freq = rknpu_dev->current_freq;   /* 初始化 */
rknpu_debugger.c:229   rknpu_dev->ondemand_freq = freq;                      /* debugfs 寫入 */
```

**也就是說：頻率只有在「有人手動寫 debugfs」時才會變。**

`rknpu_debugger.c:197` 的寫入處理：

```c
	current_freq = clk_get_rate(rknpu_dev->clks[0].clk);
	if (freq != current_freq) {
		rknpu_dev->ondemand_freq = freq;
		mutex_lock(&rknpu_dev->devfreq->lock);
		update_devfreq(rknpu_dev->devfreq);        /* 逼 devfreq 立刻重算 */
		mutex_unlock(&rknpu_dev->devfreq->lock);
	}
```

先把想要的值塞進 `ondemand_freq`，再叫 devfreq 重新問一次 governor。
**這就是實驗 8.2 能改頻率的原因。**

### 實機驗證（8.4）：跑推論，頻率會自己升嗎

連續跑 600 次推論，每 100 ms 抽一次：

```
  t=100ms  freq=1000000000  volt=825000
  t=200ms  freq=1000000000  volt=825000
  t=300ms  freq=1000000000  volt=825000
  t=400ms  freq=1000000000  volt=825000
  t=500ms  freq=1000000000  volt=825000
  t=600ms  freq=1000000000  volt=825000
```

**一動也不動。**

（開機後就停在 1 GHz，也就是 OPP 表的最高檔。）

> **所以這顆 NPU 的省電策略是「全有或全無」**：
> 要用就開電、跑最高頻；不用就整個關掉。
> **沒有「輕載時降頻」這一段。**
>
> 這也讓 [BRIEF.md](./BRIEF.md) 附錄 A 原本規劃的實驗
> 「6.3 跑推論時觀察 devfreq 升頻」**沒有東西可看** ——
> 因為根本不會升。**這個否定結果本身就是答案。**

### ⚠️ 一個保留

`rknpu_devfreq.c` 裡有溫度相關的掛勾（`rknpu_devfreq.c:21`）：

```c
static struct monitor_dev_profile npu_mdevp = {
	.type = MONITOR_TYPE_DEV,
	.low_temp_adjust  = rockchip_monitor_dev_low_temp_adjust,
	.high_temp_adjust = rockchip_monitor_dev_high_temp_adjust,
	...
};
```

**高溫時可能會有別的東西來降頻。** 我們的實驗只跑幾百毫秒，溫度沒上去。
**長時間高負載會不會降頻，本書沒測過，不下結論。**

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
| 12 | ch07 | TRM §36.5.3 說有 17 種事件 | §36.4.3 只列到 bit 13 | TRM 少列 bit 14~16 |
| 13 | ch07 | 模組參數 `0644` 看似執行時可調 | 只在 probe 讀取 | 權限位元誤導 |
| 14 | ch07 | — | irq handler 三個 `return` 全是 `IRQ_HANDLED` | 只做觀察，反向情境未驗證 |
| 15 | **ch08** | governor 名叫 **`rknpu_ondemand`** | **從不讀取負載**；`get_dev_status()` 是空函式；頻率只在寫 debugfs 時才變 | **名字誤導**。它不是 on-demand，是 on-request |
| 16 | **ch08** | dtsi 基準 `opp-microvolt` 說 1 GHz 要 850 mV | 實測 825 mV = `opp-microvolt-**L2**`，四個頻率全中 | **不是落差**，是 eFuse 體質分級生效（[ch02](./ch02_probe.md) §5 的完整落地） |

---

## 本章結論一句話

> **NPU 的省電策略是「全有或全無」**：
> 第一個 ioctl 開電（引用計數 0→1），最後一個 ioctl 之後排一個
> **可延後的**延遲工作，`delayms`（預設 3000，寫死在 probe 裡）到期就整個斷電。
>
> 頻率固定在 1 GHz，**不會因為負載而變** ——
> 那個叫 `rknpu_ondemand` 的 governor 根本不看負載。
>
> 但電壓確實跟著頻率走。而且實測的電壓
> **精確對上 dtsi 的 `opp-microvolt-L2`**，
> 四個頻率一個不差 —— **這塊板子的 NPU 是 L2 體質。**

---

## 本章做過的實驗

| # | 實驗 | 結果 |
|---|---|---|
| 8.1 | 電源三態 | 閒置 `off` → 忙碌 `on` → 停 4 秒 `off`；關電後 `freq`/`volt` 仍讀得到（軟體狀態） |
| 8.2 ★ | 改頻率量電壓 | 675 / 675 / 737.5 / 825 mV，**四個頻率全部命中 `opp-microvolt-L2`** |
| 8.3 | `delayms` 對關電時間 | 500→418、1000→889、2000→1944、3000→3478、4000→4098 ms；散布與 `INIT_DEFERRABLE_WORK` 相容 |
| 8.4 ★ | 推論時頻率會不會自己升 | **完全不動**。`rknpu_ondemand` 不看負載 |
| ⏳ | 長時間高負載的溫度降頻 | 有 `monitor_dev_profile` 掛勾，**本書沒測** |

---

**下一章** → [ch09 出事的時候：timeout 與重置](./ch09_reset.md)
