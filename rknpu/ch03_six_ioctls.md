# ch03 — 六個 ioctl：使用者空間與核心的唯一交界

> **本章目的**：`librknnrt.so` 是閉源的，我們看不到它的原始碼。
> 但它跟核心講話**只有六句**。這章把那六句全部拆開，
> 並且學會一招：**看不到程式碼，就看它說了什麼。**
>
> **實驗平台**：Radxa ROCK 5B（RK3588），`Linux rock-5b 6.1.115+`
>
> **對照素材**
> - `drivers/rknpu/include/rknpu_ioctl.h`（330 行，**全書最重要的一個檔**）
> - `drivers/rknpu/rknpu_drv.c:392`（`rknpu_action`）、`:650`（`RKNPU_IOCTL` 巨集）、
>   `:669`（`rknpu_ioctls[]` 分派表）
> - `include/uapi/drm/drm.h`（DRM 核心的 ioctl 號碼）
> - 本章**不引用 TRM**。ioctl 是作業系統的事，硬體手冊不管。
>
> **上一章** → [ch02 開機](./ch02_probe.md)

---

## 目錄

- [一鍵重現本章全部實驗](#一鍵重現本章全部實驗)
- [1. ioctl 的號碼是怎麼組出來的](#1-ioctl-的號碼是怎麼組出來的)
- [2. 六個 ioctl 與那張分派表](#2-六個-ioctl-與那張分派表)
- [3. 一次推論到底送了什麼](#3-一次推論到底送了什麼)
- [4. ACTION：一個 ioctl 裡塞了 26 個子命令](#4-action一個-ioctl-裡塞了-26-個子命令)
- [5. 四個「有名無實」的子命令](#5-四個有名無實的子命令)
- [6. 每個 ioctl 都會自動開電](#6-每個-ioctl-都會自動開電)
- [TRM 與實機對不上的地方（累積）](#trm-與實機對不上的地方累積)
- [本章結論一句話](#本章結論一句話)

---

## 一鍵重現本章全部實驗

```bash
mkdir -p ~/rknpu-lab && cd ~/rknpu-lab
# 從 notes/rknpu/experiments/ 複製 exp03_action.c 和 exp03_count.sh 過來

# ---- 實驗 3.1：一次推論送了哪些 ioctl、各幾次 ----
D=~/disk/rknn/rknn-toolkit2/rknpu2/examples/rknn_api_demo/install/rknn_api_demo_Linux
cp exp03_count.sh $D/ && cd $D && ./exp03_count.sh

# ---- 實驗 3.2：算出六個 ioctl 的號碼，並把 ACTION 問一輪 ----
cd ~/rknpu-lab
gcc -O1 -o exp03_action exp03_action.c
sudo ./exp03_action

# ---- 實驗 3.3：ioctl 的答案 vs debugfs 的答案 ----
sudo cat /sys/kernel/debug/rknpu/version
sudo cat /sys/kernel/debug/rknpu/freq
sudo cat /sys/kernel/debug/rknpu/volt
sudo dmesg | tail -6        # 看實驗 3.2 在核心留下的警告
```

---

## 1. ioctl 的號碼是怎麼組出來的

### 結論

**`ioctl` = I/O control**，「跟裝置講一句話」的通用管道。

`read()` / `write()` 只能搬資料。但「幫我配一塊記憶體」「這個工作交給你」
這種話 `read`/`write` 講不出來，就用 `ioctl`。

它長這樣：

```c
ioctl(fd, 要做什麼, 參數放哪);
```

**「要做什麼」是一個 32 位元的數字，而且是拼出來的**，不是隨便編的：

```
 31 30 │ 29 ............ 16 │ 15 ..... 8 │ 7 ..... 0
┌──────┼───────────────────┼────────────┼───────────┐
│ dir  │      size         │    type    │    nr     │
│ 2 bit│     14 bit        │   8 bit    │   8 bit   │
└──────┴───────────────────┴────────────┴───────────┘
  方向      參數結構大小      哪個子系統    第幾個命令
```

| 欄位 | 意思 |
|---|---|
| `dir` | `0b11` = 讀也寫（核心會把結果寫回去） |
| `size` | **參數結構的 `sizeof`** |
| `type` | 子系統代號。DRM 是 `'d'` = `0x64` |
| `nr` | 這個子系統裡的第幾個命令 |

DRM 把 `nr` 切成兩段：

- `0x00 ~ 0x3f` — **DRM 核心自己的**，所有驅動共用（`include/uapi/drm/drm.h`）
- `0x40 ~ 0x9f` — **各驅動私有的**，`DRM_COMMAND_BASE = 0x40` 起跳

**私有那段，各家驅動的編號是重複的。**
`rknpu` 的 `0x40` 和 QXL 顯示卡的 `0x40` 是同一個數字，意思完全不同。

> 這就是 [ch00](./ch00_why_npu_is_a_drm_driver.md) 那個坑的根源 ——
> `strace` 只看得到號碼，不知道你開的是哪個裝置，只好照它內建的表亂猜名字。

### 實機驗證（3.2 上半）：自己把號碼算出來

實驗程式：[`experiments/exp03_action.c`](./experiments/exp03_action.c)

它把 `rknpu_ioctl.h` 的結構抄過來，用 `_IOWR()` 巨集算出六個號碼：

```
=== 六個 ioctl 的號碼（拿去比對 strace）===
name                 cmd          type   nr     size
RKNPU_ACTION         0xc0086440   0x64   0x40   8 (0x08)
RKNPU_SUBMIT         0xc0686441   0x64   0x41   104 (0x68)
RKNPU_MEM_CREATE     0xc0306442   0x64   0x42   48 (0x30)
RKNPU_MEM_MAP        0xc0106443   0x64   0x43   16 (0x10)
RKNPU_MEM_DESTROY    0xc0106444   0x64   0x44   16 (0x10)
RKNPU_MEM_SYNC       0xc0206445   0x64   0x45   32 (0x20)
```

拿 `RKNPU_SUBMIT` 的 `0xc0686441` 拆開看：

```
0xc0686441
  ││ ││ ││└─ nr   = 0x41  → 0x40 + 0x01 = RKNPU_SUBMIT
  ││ ││└──── type = 0x64  → 'd' = DRM
  ││ └────── size = 0x068 = 104 = sizeof(struct rknpu_submit)
  └────────── dir  = 0b11  = 讀也寫
```

**`size` 這一欄是破案的關鍵。** 名字會撞，但結構大小不會 ——
`rknpu_submit` 是 104 bytes，跟別的驅動的第 1 號命令幾乎不可能一樣大。

---

## 2. 六個 ioctl 與那張分派表

### 結論

`rknpu_drv.c:669` 的表，就是使用者空間跟核心之間的**全部**交界：

```c
static const struct drm_ioctl_desc rknpu_ioctls[] = {
	DRM_IOCTL_DEF_DRV(RKNPU_ACTION,      __rknpu_action_ioctl,      DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_SUBMIT,      __rknpu_submit_ioctl,      DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_CREATE,  __rknpu_gem_create_ioctl,  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_MAP,     __rknpu_gem_map_ioctl,     DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_DESTROY, __rknpu_gem_destroy_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(RKNPU_MEM_SYNC,    __rknpu_gem_sync_ioctl,    DRM_RENDER_ALLOW),
};
```

六句話，分成兩類：

| # | 名字 | 參數結構 | 大小 | 幹嘛 | 哪章講 |
|---|---|---|---|---|---|
| `0x40` | `RKNPU_ACTION` | `rknpu_action` | 8 | 問東問西 / 下小指令 | **本章** |
| `0x41` | `RKNPU_SUBMIT` | `rknpu_submit` | 104 | **把工作交給硬體** | [ch06](./ch06_submit.md) |
| `0x42` | `RKNPU_MEM_CREATE` | `rknpu_mem_create` | 48 | 配一塊記憶體 | [ch04](./ch04_memory.md) |
| `0x43` | `RKNPU_MEM_MAP` | `rknpu_mem_map` | 16 | 拿到 mmap 用的假位移 | [ch04](./ch04_memory.md) |
| `0x44` | `RKNPU_MEM_DESTROY` | `rknpu_mem_destroy` | 16 | 還回去 | [ch04](./ch04_memory.md) |
| `0x45` | `RKNPU_MEM_SYNC` | `rknpu_mem_sync` | 32 | 刷快取 | [ch04](./ch04_memory.md) |

**五個管記憶體，一個管工作。** 這個比例本身就在說明：
對驅動來說，難的是記憶體，不是計算。

> `DRM_RENDER_ALLOW` 這個旗標的意思是「這個命令可以在 render node
> （`/dev/dri/renderD129`）上呼叫」。六個全標了 ——
> 呼應 [ch00](./ch00_why_npu_is_a_drm_driver.md) §3 講的「借 render node」。

---

## 3. 一次推論到底送了什麼

### 結論

用 `strace` 攔一次推論就知道。但要**先解決名字亂掉的問題**。

**招式：`-e raw=ioctl`。** 加上這個選項，`strace` 就不猜名字了，直接印原始號碼。

實驗腳本：[`experiments/exp03_count.sh`](./experiments/exp03_count.sh)

它做兩件事：用 `raw=ioctl` 抓，再自己按 `type`/`nr` 解名字。

### 實機驗證（3.1）

```bash
$ ./exp03_count.sh
=== 一次推論送出的 ioctl ===
   16 次  cmd=0xc0206445   nr=0x45 size=32    RKNPU_MEM_SYNC
   12 次  cmd=0xc0086440   nr=0x40 size=8     RKNPU_ACTION
    6 次  cmd=0xc0406400   nr=0x00 size=64    DRM_IOCTL_VERSION（核心）
    5 次  cmd=0xc0306442   nr=0x42 size=48    RKNPU_MEM_CREATE
    5 次  cmd=0xc0106444   nr=0x44 size=16    RKNPU_MEM_DESTROY
    5 次  cmd=0xc0106443   nr=0x43 size=16    RKNPU_MEM_MAP
    5 次  cmd=0xc00c642d   nr=0x2d size=12    DRM_IOCTL_PRIME_HANDLE_TO_FD（核心）
    5 次  cmd=0xc008640a   nr=0x0a size=8     DRM_IOCTL_GEM_FLINK（核心）
    2 次  cmd=0xc0686441   nr=0x41 size=104   RKNPU_SUBMIT
    2 次  cmd=0xc0106401   nr=0x01 size=16    DRM_IOCTL_GET_UNIQUE（核心）
    1 次  cmd=0x5401       nr=0x01 size=0     (不是 rknpu 的)
```

**左邊那些 `cmd=` 的值，跟實驗 3.2 自己算出來的一模一樣。**
一邊從標頭檔算，一邊從真實系統呼叫抓 —— 兩邊對上，號碼的解讀就確定了。

讀出來的故事：

#### `MEM_CREATE` 5 次 = `MEM_MAP` 5 次 = `MEM_DESTROY` 5 次

**一次推論配了 5 塊記憶體**，每塊都走「配置 → 取得 mmap 位移 → 用完歸還」。
數字完美對稱，沒有洩漏。

#### 每塊記憶體還多做了 `GEM_FLINK` + `PRIME_HANDLE_TO_FD`（各 5 次）

這兩個是 **DRM 核心**的 ioctl，不是 rknpu 私有的。
`PRIME_HANDLE_TO_FD` 就是把記憶體變成 dma-buf 的檔案描述符。

`librknnrt.so` 把**每一塊**記憶體都導出成 dma-buf。
為什麼？[ch04](./ch04_memory.md) 講。

#### `MEM_SYNC` 16 次 —— 最多的一個

刷快取。CPU 寫進去的資料，NPU 不一定看得到；NPU 算完的結果，CPU 也不一定讀得到。
每次交接都得同步一次。

#### `SUBMIT` 只有 2 次

整個 MobileNet v1，**只送了兩次工作**。

而 [ch01](./ch01_hardware.md) 實驗 1.2 數過中斷：3 次推論 = 6 個中斷 = **每次推論 2 個**。

> **`SUBMIT` 2 次 ↔ 中斷 2 個。兩章的數字對起來了。**
> 一次 `SUBMIT` 對應一次中斷。為什麼要分兩次送？留到 [ch06](./ch06_submit.md)。

#### `DRM_IOCTL_VERSION` 6 次

就是 [ch00](./ch00_why_npu_is_a_drm_driver.md) 實驗 0.3 看到的：
開 3 個 card 節點找 NPU，每次問名字要問兩輪（先問長度、再拿字串）。3 × 2 = 6。

---

## 4. ACTION：一個 ioctl 裡塞了 26 個子命令

### 結論

`RKNPU_ACTION` 的參數只有 8 bytes：

```c
struct rknpu_action {
	__u32 flags;   /* 要做什麼 */
	__u32 value;   /* 答案回填在這 */
};
```

`flags` 就是子命令編號，`rknpu_ioctl.h:110` 的 `enum e_rknpu_action` 列了 **26** 個
（`0` ~ `25`）。核心那邊是一個大 `switch`（`rknpu_drv.c:392`）。

**這是常見的做法**：與其開 26 個 ioctl 號碼，不如開一個「萬用查詢」ioctl，
再用參數區分。缺點是型別檢查變弱（大家都是 `u32`）。

### 實機驗證（3.2 下半）：全部問一輪

```
=== ACTION 的子命令，一個一個問 ===
id  name                   ret    value
0   GET_HW_VERSION         ok     1179210309 (0x46495245)
1   GET_DRV_VERSION        ok     908 (0x0000038c)
2   GET_FREQ               ok     1000000000 (0x3b9aca00)
3   SET_FREQ               FAIL   -- Invalid argument   ← 預期如此
4   GET_VOLT               ok     825000 (0x000c96a8)
5   SET_VOLT               FAIL   -- Invalid argument   ← 預期如此
7   GET_BW_PRIORITY        FAIL   -- Invalid argument
9   GET_BW_EXPECT          FAIL   -- Invalid argument
11  GET_BW_TW              FAIL   -- Invalid argument
14  GET_DT_WR_AMOUNT       ok     0 (0x00000000)
15  GET_DT_RD_AMOUNT       ok     0 (0x00000000)
16  GET_WT_RD_AMOUNT       ok     0 (0x00000000)
17  GET_TOTAL_RW_AMOUNT    ok     0 (0x00000000)
18  GET_IOMMU_EN           ok     1 (0x00000001)
20  POWER_ON               FAIL   -- Invalid argument   ← 預期如此
21  POWER_OFF              FAIL   -- Invalid argument   ← 預期如此
22  GET_TOTAL_SRAM_SIZE    ok     0 (0x00000000)
23  GET_FREE_SRAM_SIZE     ok     0 (0x00000000)
24  GET_IOMMU_DOMAIN_ID    ok     0 (0x00000000)
```

### 實機驗證（3.3）：跟 debugfs 對答案

同一件事有兩個問法。答案應該一樣：

| 問題 | ACTION ioctl | debugfs | 一致？ |
|---|---|---|---|
| 硬體版本 | `0x46495245` | — | 跟 [ch01](./ch01_hardware.md) 實驗 1.1 直接讀暫存器的值相同 ✅ |
| 驅動版本 | `908` | `RKNPU driver: v0.9.8` | ✅ |
| 頻率 | `1000000000` | `1000000000` | ✅ |
| 電壓 | `825000` | `825000` | ✅ |

`908` 怎麼變成 `v0.9.8`？看 `rknpu_ioctl.h:45`：

```c
#define RKNPU_GET_DRV_VERSION_CODE(MAJOR, MINOR, PATCHLEVEL) \
	(MAJOR * 10000 + MINOR * 100 + PATCHLEVEL)
```

`0 × 10000 + 9 × 100 + 8 = 908`。✅

**三條完全獨立的路徑（直接讀暫存器 / ioctl / debugfs）給出同一個答案。**
到這裡可以確定：我們對這個驅動的理解沒有走偏。

### 那幾個回 `0` 的呢

`GET_*_AMOUNT` 四個都「成功」但回 `0`。看程式碼就懂了（`rknpu_job.c:997`）：

```c
	if (config->amount_top == NULL) {
		LOG_WARN("Get rw_amount is not supported on this device!\n");
		return 0;          /* ← 回 0 代表「成功」，但 value 沒被填 */
	}
```

而 `rk3588_rknpu_config`（`rknpu_drv.c:183`）正是：

```c
	.amount_top = NULL,
	.amount_core = NULL,
```

**RK3588 沒有這個流量統計功能。** 但 ioctl 回報「成功」，只是值是 0。

> ⚠️ **這是個陷阱。** 呼叫端如果只看回傳值，會以為「NPU 讀寫量真的是 0」。
> 實際上是「這顆晶片沒這功能」。

證據還會留在核心 log 裡：

```bash
$ sudo dmesg | tail -6
[3330.115567] RKNPU: Get rw_amount is not supported on this device!
[3330.115593] RKNPU: Get rw_amount is not supported on this device!
[3330.115600] RKNPU: Get rw_amount is not supported on this device!
[3330.115609] RKNPU: Get total_rw_amount is not supported on this device!
```

**跑實驗，然後去 `dmesg` 撿屍體。** 這招之後每一章都會用。

`GET_BW_*` 三個直接 `EINVAL`，原因同類（`rknpu_job.c:903`）：

```c
	void __iomem *base = rknpu_dev->bw_priority_base;
	if (!base)
		return -EINVAL;
```

而 `rk3588_rknpu_config.bw_priority_length = 0x0` → [ch02](./ch02_probe.md) 的 probe
根本沒去對映那塊位址 → `base` 是 `NULL`。

SRAM 兩個回 `0`，是因為本機 `CONFIG_ROCKCHIP_RKNPU_SRAM` 沒開（[ch02](./ch02_probe.md) §4）。

---

## 5. 四個「有名無實」的子命令

### 結論

`enum e_rknpu_action` 列了名字，實際上做不到事。**四個。**

#### `SET_FREQ`（3）和 `SET_VOLT`（5）：空殼

`rknpu_drv.c:392` 的 switch 裡：

```c
	case RKNPU_SET_FREQ:
		break;              /* 什麼都沒做 */
	...
	case RKNPU_SET_VOLT:
		break;              /* 什麼都沒做 */
```

而函式開頭是 `int ret = -EINVAL;`，這兩個 case **沒有改 `ret`**。
所以「什麼都沒做」＋「回報失敗」。

實測正是 `Invalid argument`。

> 改頻率要走 devfreq（`/sys/class/devfreq/fdab0000.npu/`）或 debugfs，
> 不是走這個 ioctl。[ch08](./ch08_power_freq.md) 會做。

#### `POWER_ON`（20）和 `POWER_OFF`（21）：連 case 都沒有

`enum` 裡有：

```c
	RKNPU_SET_PROC_NICE = 19,
	RKNPU_POWER_ON = 20,
	RKNPU_POWER_OFF = 21,
	RKNPU_GET_TOTAL_SRAM_SIZE = 22,
```

但 switch 從 `SET_PROC_NICE`（19）**直接跳到** `GET_TOTAL_SRAM_SIZE`（22）。
**20 和 21 沒有對應的 `case`**，落到 `default: ret = -EINVAL;`。

實測確認：兩個都是 `Invalid argument`。

> **讀 driver 時的一個習慣**：看到 `enum` 不要以為每一項都能用。
> **`enum` 是「宣告」，`switch` 才是「實作」。** 兩邊要自己比對。
>
> 這四個大概是給別的晶片、別的版本用的，或是留著沒實作。
> **程式碼沒說原因，我不猜。**

---

## 6. 每個 ioctl 都會自動開電

### 結論

`rknpu_drv.c:650` 有一個很漂亮的巨集：

```c
#define RKNPU_IOCTL(func)                                                   \
	static int __##func(struct drm_device *dev, void *data,             \
			    struct drm_file *file_priv)                     \
	{                                                                   \
		struct rknpu_device *rknpu_dev = dev_get_drvdata(dev->dev); \
		int ret = -EINVAL;                                          \
		rknpu_power_get(rknpu_dev);       /* ← 進來先開電 */         \
		ret = func(dev, data, file_priv);                           \
		rknpu_power_put_delay(rknpu_dev); /* ← 出去排程關電 */        \
		return ret;                                                 \
	}

RKNPU_IOCTL(rknpu_action_ioctl);
RKNPU_IOCTL(rknpu_submit_ioctl);
RKNPU_IOCTL(rknpu_gem_create_ioctl);
RKNPU_IOCTL(rknpu_gem_map_ioctl);
RKNPU_IOCTL(rknpu_gem_destroy_ioctl);
RKNPU_IOCTL(rknpu_gem_sync_ioctl);
```

分派表裡登記的 `__rknpu_action_ioctl`（**兩條底線開頭**）就是巨集生出來的包裝版。

**六個 ioctl 全部被包起來**：進來自動開電，出去排程 3 秒後關電。

這解釋了 [ch02](./ch02_probe.md) §6 留下的問題 ——
既然 probe 最後把電關了，那第一次推論時誰把它開回來的？

**答案：第一個 ioctl。** 使用者根本不用管電源。

> `power_put_delay` 就是 [ch02](./ch02_probe.md) 看到那個寫死的 `3000` 毫秒。
> 所以連續推論時電源不會一直開開關關 —— 每個 ioctl 都把關電時間往後推 3 秒。

---

## TRM 與實機對不上的地方（累積）

| # | 章 | TRM 怎麼說 | 實機／程式碼 | 判斷 |
|---|---|---|---|---|
| 1 | ch01 | 暫存器表沒有 `0x0000` / `0x0004` | 有，`0x0000` = `"FIRE"` | TRM 漏寫 |
| 2 | ch01 | 位址表說 GLOBAL 是 `0xf000~0xf004` | 同章摘要表卻有 `0xF008` | TRM 自己矛盾 |
| 3 | ch01 | `0x0020` bit31 = RO / reset 0 | 讀到 `1` | 不明，待查 |
| 4 | ch01 | 完全沒提 `rockchip,iommu-v2` | dtsi 有，暫存器坐落在 NPU 的窗戶裡 | TRM 沒收錄 |
| 5 | ch02 | §36.5.2 只提 AHB / AXI 兩個時脈域 | 另有 `clk_npu`（SCMI）和 `pclk` | 合理，非錯誤 |

**本章沒有新增。** ioctl 是作業系統介面，TRM 本來就不管，**沒有可對照的東西**
—— 這跟「TRM 該寫沒寫」是兩回事。

不過本章有另一種落差，值得單獨記一張表：

### 程式碼自己前後不一致的地方

| # | `enum` 宣告了 | 實際做得到嗎 | 證據 |
|---|---|---|---|
| A | `RKNPU_SET_FREQ` (3) | ❌ 空 `case`，回 `-EINVAL` | `rknpu_drv.c:410` |
| B | `RKNPU_SET_VOLT` (5) | ❌ 空 `case`，回 `-EINVAL` | `rknpu_drv.c:418` |
| C | `RKNPU_POWER_ON` (20) | ❌ **連 `case` 都沒有** | switch 從 19 跳到 22 |
| D | `RKNPU_POWER_OFF` (21) | ❌ **連 `case` 都沒有** | 同上 |
| E | `RKNPU_GET_*_AMOUNT` (14~17) | ⚠️ **回報成功但值恆為 0** | `rknpu_job.c:997`，RK3588 的 `amount_top = NULL` |

> E 最危險：**它不報錯。** 呼叫端會以為 0 是真實答案。

---

## 本章結論一句話

> **`librknnrt.so` 閉源，但它對核心說的每一句話都攔得到。**
> 六個 ioctl —— 五個管記憶體，一個送工作。一次推論送出
> 5 塊記憶體、16 次快取同步、**2 次工作提交**。
>
> 攔 ioctl 的關鍵技巧是 **`strace -e raw=ioctl`**：不看名字，看號碼；
> 再用**參數結構的大小**把號碼對回標頭檔。

---

## 本章做過的實驗

| # | 實驗 | 檔案 | 結果 |
|---|---|---|---|
| 3.1 | 一次推論的 ioctl 統計 | [`exp03_count.sh`](./experiments/exp03_count.sh) | 5 塊記憶體、16 次 SYNC、**2 次 SUBMIT**（對上 ch01 的 2 個中斷） |
| 3.2 | 算出六個 ioctl 號碼 + 問遍 ACTION | [`exp03_action.c`](./experiments/exp03_action.c) | 號碼與 strace 抓到的完全相同；找出 4 個有名無實的子命令 |
| 3.3 | ioctl 的答案 vs debugfs vs 直接讀暫存器 | — | 三條獨立路徑答案一致 |

---

**下一章** → [ch04 記憶體：一塊記憶體的三個名字](./ch04_memory.md)
