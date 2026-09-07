# ch04 — 記憶體：一塊記憶體的三個名字

> **本章目的**：六個 ioctl 有五個在管記憶體（[ch03](./ch03_six_ioctls.md)）。
> 這章解釋為什麼——並且用實機證明 **IOMMU 是怎麼把散落的實體頁面
> 變成硬體眼中一整塊連續記憶體的**。
>
> 這是全書最重要的基礎章。看懂了，[ch05](./ch05_regcmd.md) 和
> [ch06](./ch06_submit.md) 就只是「把位址填進暫存器」而已。
>
> **實驗平台**：Radxa ROCK 5B（RK3588），`Linux rock-5b 6.1.115+`
>
> **對照素材**
> - `drivers/rknpu/rknpu_gem.c`（1757 行）
>   `:139` `alloc_buf`、`:326` `handle_create`、`:371` `gem_init`、
>   `:678` `object_create`、`:876` `create_ioctl`、`:910` `map_ioctl`、
>   `:1132` `mmap_buffer`、`:1398` `prime_import`、`:1594` `cache_sync`、`:1667` `sync_ioctl`
> - `drivers/rknpu/include/rknpu_gem.h:46`（`struct rknpu_gem_object`）
> - `drivers/rknpu/include/rknpu_ioctl.h:52`（`enum e_rknpu_mem_type`）
> - `include/drm/drm_gem.h`、`include/linux/dma-buf.h`
> - TRM §36.3.1 AHB/AXI Interface
>
> **上一章** → [ch03 六個 ioctl](./ch03_six_ioctls.md)

---

## 目錄

- [一鍵重現本章全部實驗](#一鍵重現本章全部實驗)
- [1. 為什麼記憶體是最難的部分](#1-為什麼記憶體是最難的部分)
- [2. GEM：先給你一張號碼牌](#2-gem先給你一張號碼牌)
- [3. 一塊記憶體的三個名字](#3-一塊記憶體的三個名字)
- [4. ★ IOMMU：連續是假的](#4--iommu連續是假的)
- [5. MEM_SYNC：快取為什麼要手動刷](#5-mem_sync快取為什麼要手動刷)
- [6. dma-buf：每一塊都導出成檔案描述符](#6-dma-buf每一塊都導出成檔案描述符)
- [7. 指認一塊記憶體的兩種方式](#7-指認一塊記憶體的兩種方式)
- [TRM 與實機對不上的地方（累積）](#trm-與實機對不上的地方累積)
- [本章結論一句話](#本章結論一句話)

---

## 一鍵重現本章全部實驗

```bash
mkdir -p ~/rknpu-lab && cd ~/rknpu-lab
# 從 notes/rknpu/tools/ 複製 rkspy.c 過來
gcc -shared -fPIC -O1 -o rkspy.so rkspy.c -ldl

D=~/disk/rknn/rknn-toolkit2/rknpu2/examples/rknn_api_demo/install/rknn_api_demo_Linux
cd $D

# ---- 實驗 4.1 + 4.2：記憶體總表 + 實體位址（要 root 才看得到實體位址）----
sudo env LD_PRELOAD=~/rknpu-lab/rkspy.so RKSPY_LOG=/tmp/rkspy.log \
    ./rknn_create_mem_demo model/RK3588/mobilenet_v1.rknn model/dog_224x224.jpg

grep -A30 "實體頁面真的連續嗎" /tmp/rkspy.log     # 實驗 4.2 ★
sed -n '/記憶體總表/,$p' /tmp/rkspy.log           # 實驗 4.1

# ---- 實驗 4.3：確認 IOMMU 真的開著 ----
sudo dmesg | grep -i "iommu group"
ls /sys/kernel/iommu_groups/
```

---

## 1. 為什麼記憶體是最難的部分

### 結論

TRM §36.3.1 一句話就講完了硬體的分工：

> *"The **AXI master interface** is used to fetch data from memory that is attached to
> the Soc AXI interconnect. The **AHB slave interface** is used to access the registers
> for configuration, debug and test."*

翻譯：

- **AHB**：CPU 寫設定給 NPU。慢，但只有幾十筆。
- **AXI**：**NPU 自己去記憶體拿資料。** 快，量大。

第二句是關鍵：**NPU 是主動去記憶體讀寫的**（AXI *master*）。

所以驅動要處理的問題是：

> 「使用者程式在自己的記憶體裡準備好了資料。
> 怎麼讓一個**完全不懂虛擬記憶體、不懂行程**的硬體，也能找到那些資料？」

這件事比「叫硬體開始算」難得多。所以六個 ioctl 裡五個在管記憶體。

三個必須解決的子問題：

| 問題 | 解法 | 本章第幾節 |
|---|---|---|
| 使用者程式不能拿到真實位址（不安全） | **GEM**：發號碼牌 | §2 |
| 硬體看不懂虛擬位址，實體頁面又是散的 | **IOMMU**：造出連續的假象 | §4 ★ |
| CPU 的快取裡有資料，硬體看不到 | **MEM_SYNC**：手動刷快取 | §5 |

---

## 2. GEM：先給你一張號碼牌

### 結論

**GEM = Graphics Execution Manager**，DRM 的記憶體管理機制。
名字裡有「繪圖」，但做的事跟繪圖無關（[ch00](./ch00_why_npu_is_a_drm_driver.md) 講過）。

核心想法：**使用者程式永遠拿不到真實位址，只拿得到一個小整數。**

程式碼把這件事寫得很清楚（`rknpu_gem.c:326`，註解是原本就有的）：

```c
static int rknpu_gem_handle_create(struct drm_gem_object *obj,
				   struct drm_file *file_priv,
				   unsigned int *handle)
{
	/*
	 * allocate a id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, obj, handle);
	...
	/* drop reference from allocate - handle holds it now. */
	rknpu_gem_object_put(obj);
}
```

> **`idr`** 是核心的「整數 → 指標」對照表。
> 給它一個指標，它回你一個沒被用過的小整數；之後拿整數就能換回指標。
>
> 這張表是**每個開啟的檔案各一份**（`file_priv`）。
> 所以 A 程式的 handle 1 和 B 程式的 handle 1 是完全不同的記憶體。
> **關掉檔案，這張表整個清掉，記憶體自動回收。**

`rknpu` 自己的物件是包在 DRM 的物件外面（`rknpu_gem.h:46`）：

```c
struct rknpu_gem_object {
	struct drm_gem_object base;    /* ← DRM 通用的部分，放在最前面 */
	unsigned int flags;
	void *cookie;
	void __iomem *kv_addr;         /* 核心看的位址 */
	dma_addr_t dma_addr;           /* 硬體看的位址 */
	struct page **pages;           /* 實體頁面陣列 */
	struct sg_table *sgt;
	unsigned long size;
	...
};
```

`base` 放在最前面，所以拿到 `drm_gem_object *` 就能用 `container_of()`
換回 `rknpu_gem_object *`。**這是核心裡到處都在用的「繼承」寫法。**

---

## 3. 一塊記憶體的三個名字

### 結論

配一塊記憶體要走三步，每一步給它一個新名字：

```
① ioctl(MEM_CREATE)  → 得到 handle（小整數）＋ dma_addr（硬體位址）
② ioctl(MEM_MAP)     → 得到一個「假的檔案位移」
③ mmap(fd, 假位移)   → 得到使用者空間的指標
```

第 ② 步的「假位移」很反直覺。`rknpu_gem.c:910`：

```c
int rknpu_gem_map_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv)
{
	struct rknpu_mem_map *args = data;
	return drm_gem_dumb_map_offset(file_priv, dev, args->handle,
				       &args->offset);
}
```

> **為什麼要假位移？**
> `mmap()` 的介面是「檔案的第幾個位元組」。但 `/dev/dri/card1` 不是真的檔案，
> 沒有「第幾個位元組」這回事。
>
> DRM 的解法：給每個 GEM 物件配一個**假的檔案位移**（都在 `0x100000000` 以上），
> 當成識別碼用。`mmap` 時把它填進 `offset`，驅動就知道你要對映哪一塊。

### 實機驗證（4.1）：把五塊記憶體攤開來看

工具：[`tools/rkspy.c`](./tools/rkspy.c) —— `LD_PRELOAD` 攔在
`librknnrt.so` 和核心中間，記下每個 ioctl 的參數。

```
================ 記憶體總表 ================
h   size       flags  flags 解開                            dma_addr     mmap offset    user VA
1   12288      0x0b   NON_CONTIG|CACHEABLE|KERNEL_MAPPING   0x00fff6c000 0x000100000000 0xffffb68fc000
2   4636672    0x03   NON_CONTIG|CACHEABLE                  0x00ff800000 0x000100003000 0xffffb53da000
3   2158592    0x03   NON_CONTIG|CACHEABLE                  0x00ff400000 0x00010046f000 0xffffb51cb000
4   151552     0x03   NON_CONTIG|CACHEABLE                  0x00ff3c0000 0x00010067e000 0xffffb51a6000
5   4096       0x03   NON_CONTIG|CACHEABLE                  0x00ff3bf000 0x0001006a3000 0xffffb68fb000
```

**同一塊記憶體，三個名字**（拿 `h=2` 那塊來看）：

| 誰在用 | 叫它什麼 | 值 |
|---|---|---|
| 使用者程式（號碼牌） | `handle` | `2` |
| 使用者程式（CPU 讀寫） | mmap 回傳的指標 | `0xffffb53da000` |
| **NPU 硬體** | `dma_addr` | `0xff800000` |

`mmap offset` 全部從 `0x100000000` 起跳 —— 就是上面說的假位移。

#### 一個很漂亮的細節：只有 h=1 多了 `KERNEL_MAPPING`

`flags` 的定義在 `rknpu_ioctl.h:52`：

```c
	RKNPU_MEM_NON_CONTIGUOUS   = 1 << 0,   /* 實體不連續 */
	RKNPU_MEM_CACHEABLE        = 1 << 1,   /* 走 CPU 快取 */
	RKNPU_MEM_KERNEL_MAPPING   = 1 << 3,   /* 核心也要一個位址 */
```

`0x0b` = `0b1011` = 前面三個都有。`0x03` = 只有前兩個。

**為什麼只有 h=1 需要「核心也看得到」？**

因為 h=1 是 **task 陣列**，而驅動自己要讀它（`rknpu_job.c:339`）：

```c
	task_base = task_obj->kv_addr;      /* ← 核心虛擬位址 */
	first_task = &task_base[task_start];
```

其他四塊（權重、輸入、輸出、regcmd）驅動**從頭到尾不看內容**，
只有硬體和使用者程式碰得到。所以不用浪費核心的位址空間。

> **這一個 flag 位元，就說完了整個驅動的哲學**：
> 驅動只讀「工作清單」，不讀「資料」。

---

## 4. ★ IOMMU：連續是假的

### 結論

看上面那張表：`h=2` 是 **4,636,672 位元組（約 4.4 MB）**，
flags 標了 **`NON_CONTIG`（實體不連續）**，
但 `dma_addr` 只有**一個**值：`0xff800000`。

矛盾嗎？如果實體是散的，硬體怎麼可能用一個位址讀完 4.4 MB？

**答案：`dma_addr` 根本不是實體位址。**

> **IOMMU = Input–Output Memory Management Unit**（輸入輸出記憶體管理單元）。
>
> CPU 有 MMU 把虛擬位址翻成實體位址。
> IOMMU 就是**幫周邊裝置做同一件事**的硬體。
>
> 裝置看到的位址叫 **IOVA**（I/O Virtual Address，輸入輸出虛擬位址）。
> NPU 說「我要讀 `0xff801000`」，IOMMU 查表，翻成真正的實體位址，再去 DRAM 拿。

我們在 [ch01](./ch01_hardware.md) 和 [ch02](./ch02_probe.md) 已經見過這顆 IOMMU 兩次：

- `rk3588s.dtsi:3712` 的 `rknpu_mmu`，`compatible = "rockchip,iommu-v2"`
- 它的暫存器**坐落在 NPU 的暫存器窗戶裡**，害 NPU probe 時抱怨 `can't request region`

### 實機驗證（4.2）：把實體位址挖出來 ★

**這是本章的重點實驗。**

`/proc/self/pagemap` 這個檔案，可以把「使用者虛擬位址」翻成「實體頁框號」。
（需要 root，否則核心會把數字抹成 0。）

`rkspy.c` 在 `SUBMIT` 當下（緩衝區還活著時）讀它，
把每一頁的 **IOVA** 和 **實體位址**排在一起印：

```
=== 實體頁面真的連續嗎？（每塊取前 8 頁）===
h=2  IOVA=0xff800000  size=4636672
   +0  IOVA=0x00ff800000 -> 實體 0x01d0949000
   +1  IOVA=0x00ff801000 -> 實體 0x01c9dbf000   ← 實體不連續
   +2  IOVA=0x00ff802000 -> 實體 0x01fb789000   ← 實體不連續
   +3  IOVA=0x00ff803000 -> 實體 0x01a371e000   ← 實體不連續
   +4  IOVA=0x00ff804000 -> 實體 0x01e050f000   ← 實體不連續
   +5  IOVA=0x00ff805000 -> 實體 0x01e0fba000   ← 實體不連續
   +6  IOVA=0x00ff806000 -> 實體 0x01ac7db000   ← 實體不連續
   +7  IOVA=0x00ff807000 -> 實體 0x01f4927000   ← 實體不連續
```

**左邊每次 +0x1000，整整齊齊。右邊完全亂跳。**

```
NPU 眼中（IOVA）                     真正的實體記憶體
┌──────────────┐  0xff800000        ┌───┐ 0x01d0949000
│              │ ───────────────────▶└───┘
├──────────────┤  0xff801000        ┌───┐ 0x01c9dbf000
│   一整塊     │ ───────────────────▶└───┘
├──────────────┤  0xff802000        ┌───┐ 0x01fb789000
│   4.4 MB     │ ───────────────────▶└───┘
├──────────────┤  0xff803000        ┌───┐ 0x01a371e000
│   連續記憶體 │ ───────────────────▶└───┘
└──────────────┘                     散落在 8 GB 各處
```

**1132 個頁面，散落在整片 DRAM 各處。NPU 完全不知道。**

這就是 IOMMU 的價值：

| 沒有 IOMMU | 有 IOMMU |
|---|---|
| 必須配一整塊連續實體記憶體 | 隨便湊 1132 個零散頁面就行 |
| 開機久了記憶體碎片化就配不到 | 永遠配得到 |
| 裝置能讀寫全部實體記憶體 | 只能碰到有對映的那些 |

第三行同樣重要：**IOMMU 也是一道安全牆。** 沒對映的頁面，NPU 碰不到。

> 順帶一提，這也解釋了 `dma_addr` 為什麼長得那麼整齊
> （`0xff400000`、`0xff800000`）—— IOVA 是驅動自己分配的號碼，
> 當然可以挑好看的。實體位址就沒這種待遇。

### 這些程式碼在哪

配置的入口是 `rknpu_gem.c:139` 的 `rknpu_gem_alloc_buf()`：

```c
	if (!(rknpu_obj->flags & RKNPU_MEM_NON_CONTIGUOUS))
		rknpu_obj->dma_attrs |= DMA_ATTR_FORCE_CONTIGUOUS;
	...
	rknpu_obj->cookie = dma_alloc_attrs(drm->dev, rknpu_obj->size,
					    &rknpu_obj->dma_addr, gfp_mask,
					    rknpu_obj->dma_attrs);
```

`dma_alloc_attrs()` 是核心的通用 DMA API。**驅動根本沒直接碰 IOMMU** ——
只要 device tree 寫了 `iommus = <&rknpu_mmu>`，
核心的 DMA 層就會自動走 IOMMU 路徑，回傳 IOVA 而不是實體位址。

而 IOMMU domain 的切換在 `rknpu_iommu.c`：

```c
	if (rknpu_iommu_domain_get_and_switch(rknpu_dev, iommu_domain_id)) { ... }
```

`rknpu_drv.h:56` 定義 `RKNPU_MAX_IOMMU_DOMAIN_NUM 16` ——
**為什麼是 16、使用者為什麼要自己挑 domain id，程式碼沒說，TRM 也沒收錄這個裝置。
待查，不猜。**（實測 `GET_IOMMU_DOMAIN_ID` 一直回 `0`。）

---

## 5. MEM_SYNC：快取為什麼要手動刷

### 結論

[ch03](./ch03_six_ioctls.md) 數過：一次推論送了 **16 次 `MEM_SYNC`**，是最多的一個。

原因藏在 flags 裡：**五塊記憶體全部是 `CACHEABLE`。**

> **快取（cache）** 是 CPU 旁邊的一小塊超快記憶體。
> CPU 寫資料時，常常只寫進快取就回去做別的事，**還沒真的寫到 DRAM**。
>
> CPU 自己讀得到（它會先看快取）。但 **NPU 是透過 AXI 直接讀 DRAM 的**，
> 看不到快取裡那份。**它會讀到舊資料。**

所以每次交接都得手動同步：

| 方向 | 常數 | 做什麼 |
|---|---|---|
| CPU → NPU | `RKNPU_MEM_SYNC_TO_DEVICE` | 把快取裡的髒資料**寫回** DRAM |
| NPU → CPU | `RKNPU_MEM_SYNC_FROM_DEVICE` | 把快取**作廢**，強迫 CPU 重新從 DRAM 讀 |

程式碼（`rknpu_gem.c:1667` 的 `rknpu_gem_sync_ioctl`）：

```c
	if (!(rknpu_obj->flags & RKNPU_MEM_CACHEABLE))
		return -EINVAL;                 /* 不是 cacheable 就沒得同步 */
	...
	if (args->flags & RKNPU_MEM_SYNC_TO_DEVICE) {
		dma_sync_single_range_for_device(dev->dev, rknpu_obj->dma_addr,
						 args->offset, args->size,
						 DMA_TO_DEVICE);
	}
	if (args->flags & RKNPU_MEM_SYNC_FROM_DEVICE) {
		dma_sync_single_range_for_cpu(...);
	}
```

> **那為什麼不乾脆不要快取？**
> 因為使用者程式要**準備輸入資料**（寫 150 KB 的圖）和**讀取結果**。
> 不走快取的話，這些讀寫會慢好幾倍。
>
> **選擇快取 + 手動同步，是拿「多打幾行程式碼」換「快很多」。**
> 16 次同步的成本，遠低於全程不走快取。

---

## 6. dma-buf：每一塊都導出成檔案描述符

### 結論

[ch03](./ch03_six_ioctls.md) 的統計裡有兩個「多出來的」核心 ioctl，各 5 次：

```
5 次  DRM_IOCTL_GEM_FLINK（核心）
5 次  DRM_IOCTL_PRIME_HANDLE_TO_FD（核心）
```

**5 塊記憶體，每一塊都做了一次。**

`PRIME_HANDLE_TO_FD` 就是把 GEM handle 換成一個 **dma-buf 檔案描述符**。

> **dma-buf**（`Documentation/driver-api/dma-buf.rst`，標題是
> *"Buffer Sharing and Synchronization"*）是核心層級的「記憶體共用標準」。
>
> 它把一塊記憶體包裝成 **fd**。fd 可以傳給別的行程、可以用權限管、
> `close()` 就自動釋放 —— 整套機制免費繼承。
>
> **PRIME** 是 DRM 給 dma-buf 取的名字。看到 `prime` 就是在講 dma-buf。

用途：相機拍完一張圖，**直接**讓 NPU 去算，中間不複製。
一張 1080p RGB 圖 6 MB，30 fps 就是每秒省下好幾百 MB 的搬運。

`rknpu` 兩邊都會（`rknpu_drv.c:721`）：

```c
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,   /* 出口 */
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,   /* 入口 */
	.gem_prime_import   = rknpu_gem_prime_import,
	.gem_prime_import_sg_table = rknpu_gem_prime_import_sg_table,
```

進來的路徑最能說明「零拷貝」是怎麼成立的（`rknpu_gem.c:1419`）：

```c
	rknpu_obj = rknpu_gem_init(dev, attach->dmabuf->size);
	rknpu_obj->dma_addr = sg_dma_address(sgt->sgl);      /* ★ */
	...
	rknpu_obj->sgt = sgt;
```

**別人配的記憶體，我照樣拿得到 NPU 看得懂的位址。** 一個位元組都沒複製。

### 一個很誠實的 TODO

同一個函式往下幾行（`rknpu_gem.c:1454`，TODO 在 `:1461`），原文照抄：

```c
	if (sgt->nents == 1) {
		/* always physically continuous memory if sgt->nents is 1. */
		rknpu_obj->flags |= RKNPU_MEM_CONTIGUOUS;
	} else {
		/*
		 * this case could be CONTIG or NONCONTIG type but for now
		 * sets NONCONTIG.
		 * TODO. we have to find a way that exporter can notify
		 * the type of its own buffer to importer.
		 */
		rknpu_obj->flags |= RKNPU_MEM_NON_CONTIGUOUS;
	}
```

**dma-buf 目前的一個真實侷限**：入口方沒辦法問出口方
「你這塊到底是不是連續的」，只能從片段數量猜。寫的人自己也知道不夠好，留了 TODO。

> 看到官方驅動裡的 `TODO`，比任何解釋都更能讓人理解一套機制的邊界在哪。

### ⏳ 本機做不到的部分

`CONFIG_ROCKCHIP_RKNPU_FENCE` **沒開**（[ch02](./ch02_probe.md) §4），
所以 `rknpu_fence.c` 那套 **dma-fence**（「我還在算，你先別碰」的同步旗標）
本機跑不到，實測 `SUBMIT` 的 `fence_fd` 永遠是 `-1`。

要玩得重編 kernel。本章只點到為止。

---

## 7. 指認一塊記憶體的兩種方式

### 結論

`MEM_CREATE` 回傳了**兩個**識別碼：

```c
	args->handle   = <小整數>;
	args->obj_addr = (__u64)(uintptr_t)rknpu_obj;   /* ← 核心指標！ */
	args->dma_addr = rknpu_obj->dma_addr;
```

`obj_addr` 是 `struct rknpu_gem_object` 在**核心位址空間**裡的指標，
原封不動交給使用者空間。

然後六個 ioctl 分成兩派：

| ioctl | 用什麼指認 | 怎麼查 |
|---|---|---|
| `MEM_DESTROY` | `handle` | `rknpu_gem_object_find(file_priv, args->handle)` —— 查 idr 表 |
| `MEM_SYNC` | `obj_addr` | `(struct rknpu_gem_object *)(uintptr_t)args->obj_addr` —— 直接轉型 |
| `SUBMIT` | `task_obj_addr` | 同上（`rknpu_job.c:152`） |

`MEM_SYNC` 的實際寫法（`rknpu_gem.c:1680`）：

```c
	rknpu_obj = (struct rknpu_gem_object *)(uintptr_t)args->obj_addr;
	if (!rknpu_obj)
		return -EINVAL;

	if (!(rknpu_obj->flags & RKNPU_MEM_CACHEABLE))   /* ← 直接解參考 */
		return -EINVAL;
```

**只檢查了 NULL，沒有查表確認這個指標真的是自己發出去的。**

> ⚠️ 這是讀 driver 時要看得出來的東西。
> 兩種寫法的差別：
>
> - **`handle` + idr 查表**：使用者亂填只會查不到，回 `-EINVAL`。安全。
> - **裸核心指標**：使用者亂填，核心就會去解參考那個位址。
>
> `/dev/dri/renderD129` 屬於 `render` 群組，不需要 root
> （[ch00](./ch00_why_npu_is_a_drm_driver.md) §3）。
>
> **本書只做觀察，不做評價，也不示範怎麼利用。**
> 但「同一個驅動裡兩種指認方式並存」這件事本身，
> 就足以說明為什麼讀 driver 要一行一行讀。

---

## TRM 與實機對不上的地方（累積）

| # | 章 | TRM 怎麼說 | 實機／程式碼 | 判斷 |
|---|---|---|---|---|
| 1 | ch01 | 暫存器表沒有 `0x0000` / `0x0004` | 有，`0x0000` = `"FIRE"` | TRM 漏寫 |
| 2 | ch01 | 位址表說 GLOBAL 是 `0xf000~0xf004` | 同章摘要表卻有 `0xF008` | TRM 自己矛盾 |
| 3 | ch01 | `0x0020` bit31 = RO / reset 0 | 讀到 `1` | 不明，待查 |
| 4 | ch01/ch02 | 完全沒提 `rockchip,iommu-v2` | dtsi 有，暫存器在 NPU 窗戶裡，且**實測確認它在做 IOVA 翻譯** | TRM 沒收錄 |
| 5 | ch02 | §36.5.2 只提 AHB / AXI 兩個時脈域 | 另有 `clk_npu`、`pclk` | 合理，非錯誤 |
| 6 | **ch04** | 沒有任何一章提到 IOMMU domain 的概念 | `RKNPU_MAX_IOMMU_DOMAIN_NUM = 16`，使用者空間可指定 `iommu_domain_id` | **為什麼是 16、為什麼要使用者挑，程式碼與 TRM 都沒說。待查** |

---

## 本章結論一句話

> **同一塊記憶體有三個名字**：使用者拿 `handle`（號碼牌）和 mmap 指標，
> 硬體拿 `dma_addr`。
>
> 而 `dma_addr` **不是實體位址**，是 IOMMU 造出來的假象 ——
> 實測 4.4 MB 的緩衝區，實體上是 1132 個散落在 8 GB 各處的頁面，
> NPU 卻看成一整塊。
>
> **理解了這件事，[ch05](./ch05_regcmd.md) 就只是「把 `dma_addr` 填進暫存器」而已。**

---

## 本章做過的實驗

| # | 實驗 | 檔案 | 結果 |
|---|---|---|---|
| 4.1 | 五塊記憶體的完整總表 | [`tools/rkspy.c`](./tools/rkspy.c) | 三個名字並列；只有 task 陣列有 `KERNEL_MAPPING` |
| 4.2 ★ | `/proc/self/pagemap` 挖實體位址 | 同上 | **IOVA 每次 +0x1000，實體位址完全亂跳** |
| 4.3 | 確認 IOMMU 開著 | — | `dmesg` 有 `Adding to iommu group 0`；`GET_IOMMU_EN` 回 `1` |
| ⏳ | dma-fence | — | `CONFIG_ROCKCHIP_RKNPU_FENCE` 沒開，需重編 kernel |
| ⏳ | SRAM 配置器 | — | `CONFIG_ROCKCHIP_RKNPU_SRAM` 沒開，需重編 kernel |

---

**下一章** → [ch05 ★ regcmd：模型被編譯成什麼](./ch05_regcmd.md)
