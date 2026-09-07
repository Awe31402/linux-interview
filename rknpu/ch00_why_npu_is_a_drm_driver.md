# ch00 — 你為什麼會在圖形子系統裡找到一顆 AI 加速器

> **本章目的**：把三件「不先講清楚就會一路卡住」的事講掉，然後給你一張全書地圖。
> 這章不碰硬體細節，也不碰 TRM。它是暖身。
>
> **實驗平台**：Radxa ROCK 5B（Rockchip RK3588），`ssh radxa@192.168.68.58`
> **OS / Kernel**：Debian 12 bookworm，`Linux rock-5b 6.1.115+ #1 SMP aarch64`
> **NPU driver**：`RKNPU driver: v0.9.8`
>
> **對照素材**
> - 驅動原始碼：`drivers/rknpu/`（本樹）
> - 對照組（真正的顯示驅動）：`drivers/gpu/drm/rockchip/rockchip_drm_drv.c`
> - DRM 核心：`include/drm/drm_drv.h`、`drivers/gpu/drm/drm_mode_config.c`
> - 本章**不**引用 TRM。TRM 不管作業系統的事。
>
> **相關文件**：📋 [任務簡報](./BRIEF.md)、🗺️ [全書大綱](./OUTLINE.md)

---

## 目錄

- [一鍵重現本章全部實驗](#一鍵重現本章全部實驗)
- [第 1 件事：這個 driver 不在上游 kernel 裡](#第-1-件事這個-driver-不在上游-kernel-裡)
- [第 2 件事：它註冊成 DRM 驅動，但一張圖都不畫](#第-2-件事它註冊成-drm-驅動但一張圖都不畫)
- [第 3 件事：那它到底跟 DRM 借了什麼](#第-3-件事那它到底跟-drm-借了什麼)
- [第 4 件事：SDK 是怎麼找到它的](#第-4-件事sdk-是怎麼找到它的)
- [全書地圖](#全書地圖)
- [本章結論一句話](#本章結論一句話)

---

## 一鍵重現本章全部實驗

整段貼進板子的終端機就好。

```bash
# ---- 實驗 0.1：/dev/dri 底下誰是誰，誰真的會畫圖 ----
mkdir -p ~/rknpu-lab && cd ~/rknpu-lab
# 把 notes/rknpu/experiments/exp00_whoami.c 複製過來，然後：
gcc -O1 -o exp00_whoami exp00_whoami.c
sudo ./exp00_whoami

# ---- 實驗 0.2：確認驅動版本，跟原始碼樹對得上 ----
sudo cat /sys/kernel/debug/rknpu/version

# ---- 實驗 0.3：SDK 實際開的是哪個節點 ----
D=~/disk/rknn/rknn-toolkit2/rknpu2/examples/rknn_api_demo/install/rknn_api_demo_Linux
cd $D
strace -f -e trace=openat,ioctl -o /tmp/tr.txt \
  ./rknn_create_mem_demo model/RK3588/mobilenet_v1.rknn model/dog_224x224.jpg >/dev/null
grep -n "dri\|DRM_IOCTL_VERSION" /tmp/tr.txt | head
```

---

## 第 1 件事：這個 driver 不在上游 kernel 裡

### 結論

**你在 kernel.org 的原始碼裡找不到 `drivers/rknpu/`。**

它只存在於 Rockchip 自己維護的 BSP kernel（本樹分支 `linux-6.1-stan-rkr4.1-buildroot`）。

這件事的實際後果：

- Google「linux rknpu driver documentation」找到的東西，多半是別人的部落格，不是官方文件
- `Documentation/` 底下沒有它的說明
- LWN、lore.kernel.org 上沒有它的 patch 討論
- **唯一權威的資料就是原始碼本身，加上 RK3588 TRM**

這也是這份教材存在的理由。

### 本樹能查到的旁證

**旁證一：它住在 `drivers/` 的頂層。**

```
drivers/Kconfig:248:  source "drivers/rknpu/Kconfig"
drivers/Makefile:195: obj-$(CONFIG_ROCKCHIP_RKNPU) += rknpu/
```

一個「正經的」DRM 驅動應該住在 `drivers/gpu/drm/` 底下——像同一顆晶片上的
`drivers/gpu/drm/rockchip/`（顯示）和 `drivers/gpu/drm/panthor/`（Mali GPU）都是。

`drivers/rknpu/` 卻跟 `drivers/net/`、`drivers/usb/` 平起平坐，直接掛在 `drivers/` 底下。
**上游不會接受這種擺法。** 這是一個「我是外掛上來的」的明顯訊號。

**旁證二：`MAINTAINERS` 完全沒有它。**

```bash
$ grep -in rknpu MAINTAINERS
（沒有輸出）
```

上游 kernel 的每個子系統都必須在 `MAINTAINERS` 有一筆，寫明誰負責、誰收 patch。
沒有這筆，代表這段程式碼從來沒有走過上游的流程。

> 想自己確認的話：到 https://git.kernel.org 搜 `drivers/rknpu`，會找不到。
> 本節只列出**本樹裡查得到的**旁證，不假裝在上游做過搜尋。

### 給新手的話

如果你以前讀 kernel 都是讀上游的程式碼，這裡要換個習慣：

| 上游 driver | 這個 driver |
|---|---|
| 有 `Documentation/` | 沒有 |
| 有 mailing list 討論可以考古 | 沒有 |
| 有 `MAINTAINERS` | 沒有 |
| commit message 寫得很細 | 常常只有一行 |
| **問「為什麼這樣寫」有地方查** | **只能從程式碼本身推** |

所以全書的規矩是：**看得到的就講，看不到的就說看不到。** 不猜。

---

## 第 2 件事：它註冊成 DRM 驅動，但一張圖都不畫

### 結論

`DRM` 是 **Direct Rendering Manager**（直接算圖管理員），Linux 的圖形子系統。

`rknpu` 確實把自己註冊成一個 DRM 驅動，開出 `/dev/dri/card1` 和 `/dev/dri/renderD129`
兩個節點。但它**完全不做顯示**——沒有螢幕輸出、沒有解析度設定、沒有畫面更新。

證據就在 `drivers/rknpu/rknpu_drv.c:700`（旗標本身在 `:702`）：

```c
static struct drm_driver rknpu_drm_driver = {
#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
#else
	.driver_features = DRIVER_GEM | DRIVER_PRIME | DRIVER_RENDER,
#endif
	...
};
```

只有兩面旗子。跟同一棵樹裡真正的顯示驅動比一比
（`drivers/gpu/drm/rockchip/rockchip_drm_drv.c:2109`）：

```c
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC | DRIVER_RENDER,
```

四面旗子。差在哪？`include/drm/drm_drv.h` 自己寫得很清楚：

| 旗標 | `drm_drv.h` 的原文說明 | 顯示驅動 | **rknpu** |
|---|---|---|---|
| `DRIVER_GEM` (bit 0) | *"Driver use the GEM memory manager."* | ✅ | ✅ |
| `DRIVER_MODESET` (bit 1) | *"Driver supports mode setting interfaces (KMS)."* | ✅ | ❌ |
| `DRIVER_RENDER` (bit 3) | *"Driver supports dedicated render nodes."* | ✅ | ✅ |
| `DRIVER_ATOMIC` (bit 4) | *"Driver supports the full atomic modesetting userspace API."* | ✅ | ❌ |

**`MODESET` 和 `ATOMIC` 是「畫面」那一半，`GEM` 和 `RENDER` 是「記憶體」那一半。**

`rknpu` 只要記憶體那一半。

### 實機驗證（0.1）：問它「你會畫圖嗎」

不用相信我。直接問它。

實驗程式：[`experiments/exp00_whoami.c`](./experiments/exp00_whoami.c)

它對每個 `/dev/dri/cardN` 問兩句話：

1. `DRM_IOCTL_VERSION` —— 「你叫什麼名字？」
2. `DRM_IOCTL_MODE_GETRESOURCES` —— 「你有幾個螢幕輸出？」

第二句是 **KMS**（Kernel Mode Setting，核心端的顯示模式設定）專用的 ioctl。
沒設 `DRIVER_MODESET` 的驅動，DRM 核心會直接把它擋掉。

```bash
$ gcc -O1 -o exp00_whoami exp00_whoami.c
$ sudo ./exp00_whoami
node               driver         version   connectors does KMS?
--------------------------------------------------------------------
/dev/dri/card0     rockchip       4.0.0     3          yes
                     desc: RockChip Soc DRM
/dev/dri/card1     rknpu          0.9.8     -          NO  <- ioctl rejected: Operation not supported
                     desc: RKNPU driver
/dev/dri/card2     panthor        1.3.0     -          NO  <- ioctl rejected: Operation not supported
                     desc: Panthor DRM driver
```

三件事一次看到：

1. **`card1` 就是 NPU**，`rknpu` 版本 `0.9.8`
2. **問它螢幕輸出，直接被拒**：`EOPNOTSUPP`（Operation not supported）
3. **`card2`（Mali GPU）也一樣被拒** —— 連 GPU 都不做顯示

第 3 點值得多想一秒：在現代 Linux 裡，**「畫圖的」跟「顯示的」是兩個不同的裝置**。
GPU 負責算出畫面，顯示控制器負責把畫面送到螢幕。它們都掛在 DRM 底下，但職責分開。

**NPU 只是又多插了一個「算東西的」進來，連畫面都不算，只算張量。**

### 順帶一提：`0.9.8` 這個版本號

```bash
$ sudo cat /sys/kernel/debug/rknpu/version
RKNPU driver: v0.9.8
```

跟原始碼樹對一下 `drivers/rknpu/include/rknpu_drv.h:33-35`：

```c
#define DRIVER_MAJOR 0
#define DRIVER_MINOR 9
#define DRIVER_PATCHLEVEL 8
```

**一模一樣。** 這代表板子上跑的驅動，就是我們手上這份原始碼。
全書後面所有「程式碼寫這樣、實機是那樣」的對照才站得住。

> 開始讀一份 driver 之前先做這件事，是很划算的習慣。
> 版本對不上的話，後面每一個實驗結果都可能在騙你。

### 小注：3 個 connector 還是 4 個？

`sysfs` 底下看得到 **4** 個：

```
card0-DP-1  card0-HDMI-A-1  card0-HDMI-A-2  card0-Writeback-1
```

但 ioctl 只回報 **3** 個。差的是 `Writeback-1`。

原因在 `drivers/gpu/drm/drm_mode_config.c:155`：writeback connector 只有在使用者程式
明確要求（`DRM_CLIENT_CAP_WRITEBACK_CONNECTORS`，見 `drm_ioctl.c:358`）之後才會被列出來。
我們的小程式沒要求，所以看不到。

跟 NPU 無關，但這種「兩個地方數字對不起來」的情況很常見，
**先找出原因再往下走**，不要放著。

---

## 第 3 件事：那它到底跟 DRM 借了什麼

### 結論

借三樣東西。**都跟記憶體有關，沒有一樣跟畫面有關。**

#### 借第一樣：GEM —— 一套管理「給硬體用的記憶體」的現成機制

**GEM = Graphics Execution Manager**（繪圖執行管理員）。

名字裡有「繪圖」，但它做的事其實跟繪圖無關，是：

- 配置一塊實體記憶體
- 給使用者程式一個**號碼牌**（handle，一個小整數），而不是真的位址
- 幫忙算出硬體看得懂的位址
- 管引用計數，沒人用了就釋放

任何「使用者程式準備資料 → 交給硬體算 → 拿結果回來」的裝置都需要這一整套。
GPU 需要，NPU 也需要。**Rockchip 選擇不重寫一份，直接用現成的。**

細節留到 [ch04](./ch04_memory.md)。

#### 借第二樣：render node —— 一個不用 root 就能開的裝置節點

`DRIVER_RENDER` 這面旗子會讓 DRM 額外開出 `/dev/dri/renderD129`。

差別在權限：

| 節點 | 群組 | 誰能開 |
|---|---|---|
| `/dev/dri/card1` | `video` | 需要顯示相關權限 |
| `/dev/dri/renderD129` | `render` | 一般使用者加進 `render` 群組就行 |

而且 render node 上**只能**呼叫標了 `DRM_RENDER_ALLOW` 的 ioctl。
`rknpu` 的六個 ioctl 全都標了（`rknpu_drv.c:669`）：

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

**這張表就是使用者空間跟核心之間的全部交界。** 六句話，沒有第七句。
整個 [ch03](./ch03_six_ioctls.md) 都在講它。

#### 借第三樣：PRIME / dma-buf —— 跟別的裝置共用記憶體，中間不複製

`.prime_handle_to_fd` / `.prime_fd_to_handle`（`rknpu_drv.c:721-722`）。

用途：相機拍完一張圖，直接讓 NPU 去算，中間不搬。
一張 1080p RGB 圖 6 MB，30 fps 下省掉的搬運量是每秒好幾百 MB。

細節同樣留到 [ch04](./ch04_memory.md)。

### 那它「沒借」什麼

一樣重要：

| DRM 提供的東西 | rknpu 有用嗎 |
|---|---|
| KMS / atomic modesetting（顯示） | ❌ 完全沒有 |
| framebuffer、plane、CRTC、connector | ❌ 完全沒有 |
| GEM 記憶體管理 | ✅ |
| render node | ✅ |
| PRIME / dma-buf | ✅ |
| dma-fence（同步） | ⚠️ 程式碼有（`rknpu_fence.c`），但本機 `CONFIG_ROCKCHIP_RKNPU_FENCE` 沒開 |

> ⚠️ 最後一列是本機的 config 決定的，不是驅動本身的限制。
> 實測送出的 job 裡 `fence_fd = -1`，證實沒在用。詳見 [ch04](./ch04_memory.md)。

---

## 第 4 件事：SDK 是怎麼找到它的

### 結論

`/dev/dri/` 底下的編號**不固定**。`card1` 今天是 NPU，換一版 kernel 或改一下開機順序，
就可能變成別的東西。

所以 `librknnrt.so` 不能寫死節點名字。它的做法是：
**一個一個開，每開一個就問 `DRM_IOCTL_VERSION`，名字是 `rknpu` 才留下。**

跟我們的 `exp00_whoami.c` 做的事一模一樣。

### 實機驗證（0.3）：用 strace 看它怎麼找

```bash
$ strace -f -e trace=openat,ioctl -o /tmp/tr.txt \
    ./rknn_create_mem_demo model/RK3588/mobilenet_v1.rknn model/dog_224x224.jpg
$ grep -n "dri\|DRM_IOCTL_VERSION" /tmp/tr.txt | head
```

```
64:  openat(AT_FDCWD, "/dev/dri/card0", O_RDWR) = 3
65:  ioctl(3, DRM_IOCTL_VERSION, 0xaaaaf0687810) = 0
66:  ioctl(3, DRM_IOCTL_VERSION, 0xaaaaf0687810) = 0
67:  openat(AT_FDCWD, "/dev/dri/card0", O_RDWR) = 3
68:  ioctl(3, DRM_IOCTL_VERSION, 0xaaaaf0687940) = 0
69:  ioctl(3, DRM_IOCTL_VERSION, 0xaaaaf0687940) = 0
70:  openat(AT_FDCWD, "/dev/dri/card1", O_RDWR) = 3
71:  ioctl(3, DRM_IOCTL_VERSION, 0xaaaaf0687a30) = 0
72:  ioctl(3, DRM_IOCTL_VERSION, 0xaaaaf0687a30) = 0
73:  ioctl(3, DRM_IOCTL_GET_UNIQUE, 0xffffe5473a80) = 0
```

看得很清楚：

1. 開 `card0`，問名字 → 是 `rockchip`，不對，關掉
2. 再開一次 `card0`（`fd` 又是 3，代表前一個確實關了），再問一次
3. 開 `card1`，問名字 → 是 `rknpu`，**留下**，接著繼續用

> **為什麼 `DRM_IOCTL_VERSION` 每次都呼叫兩次？**
> 因為 `struct drm_version` 裡的字串是「你給我緩衝區，我填給你」。
> 第一次傳長度 0，核心回填「名字有幾個字」；程式配好記憶體，第二次才真的拿到字串。
> 這是 DRM 的常見寫法，不是 bug。

**這一段就是本書第一次直接看到閉源的 `librknnrt.so` 在做什麼。**
我們看不到它的原始碼，但看得到它對核心說的每一句話。
全書都用這個方法對付它。

### 順便學一個坑

同一份 `strace` 輸出裡，之後那些 `rknpu` 專屬的 ioctl，`strace` 會**叫錯名字**：

```
ioctl(3, DRM_IOCTL_QXL_ALLOC, ...)      ← 其實是 RKNPU_ACTION
ioctl(3, DRM_IOCTL_EXYNOS_GEM_GET, ...) ← 其實是 RKNPU_MEM_DESTROY
```

QXL 是虛擬機的顯示卡，Exynos 是三星的晶片，兩個都跟 RK3588 無關。

原因：DRM 讓每個驅動自己定義編號 `0x40` 開始的私有 ioctl，
**這些編號在不同驅動之間是重複的**。`strace` 只看得到編號，看不到你開的是哪個裝置，
就照它內建的表隨便挑一個名字印出來。

怎麼對回正確的名字？看 **結構大小**。[ch03](./ch03_six_ioctls.md) 會示範。

---

## 全書地圖

一次 `rknn_run()` 會走過這條路。每一站對應一章。

```
 使用者程式
     │  rknn_run()
     ▼
┌─────────────────────────────────────────────────┐
│  librknnrt.so  （閉源，只能從 ioctl 反推）        │
│  把模型預編好的「暫存器寫入指令」準備到記憶體裡     │
└─────────────────────────────────────────────────┘
     │  ioctl(/dev/dri/card1, ...)
     │  ┌─ MEM_CREATE / MEM_MAP / MEM_SYNC ──→ ch04 記憶體
     │  └─ SUBMIT ─────────────────────────→ ch06 送出
     ▼
┌─────────────────────────────────────────────────┐
│  drivers/rknpu/   ← 我們要讀懂的東西              │
│  排隊、開電、寫 8 個暫存器、然後去睡              │
└─────────────────────────────────────────────────┘
     │  writel() × 8
     ▼
┌─────────────────────────────────────────────────┐
│  NPU 硬體                                        │
│  自己去 DRAM 抓指令 ──────────────────→ ch05 ★  │
│  CNA / DPU / PPU 照著算 ───────────────→ ch01    │
└─────────────────────────────────────────────────┘
     │  中斷 ─────────────────────────────→ ch07
     ▼
 回到 rknn_run()，返回
```

| 章 | 講什麼 |
|---|---|
| **ch00**（本章） | 為什麼在 DRM 裡、全書地圖 |
| [ch01](./ch01_hardware.md) | 硬體長什麼樣（三核、CNA/DPU/PPU、暫存器位址表） |
| [ch02](./ch02_probe.md) | 開機：device tree → 一個能用的裝置 |
| [ch03](./ch03_six_ioctls.md) | 六個 ioctl：使用者空間與核心的唯一交界 |
| [ch04](./ch04_memory.md) | 記憶體：一塊記憶體的三個名字 |
| [ch05](./ch05_regcmd.md) ★ | **regcmd：模型被編譯成什麼** |
| [ch06](./ch06_submit.md) | 送出：driver 只寫 8 個暫存器 |
| [ch07](./ch07_interrupt.md) | 中斷：硬體怎麼說「我算完了」 |
| [ch08](./ch08_power_freq.md) | 電源與頻率：閒置三秒就關電 |
| [ch09](./ch09_reset.md) | 出事的時候：timeout 與重置 |

---

## 本章結論一句話

> **`rknpu` 是一個掛在圖形子系統上的算術裝置。**
> 它借 DRM 的記憶體管理（GEM）、裝置節點（render node）和跨裝置共享（dma-buf），
> 但完全不碰顯示。
>
> 你之後在程式碼裡看到 `drm_` 開頭的東西，先假設它是在處理**記憶體**，
> 十之八九是對的。

---

## 本章做過的實驗

| # | 實驗 | 檔案 | 結果 |
|---|---|---|---|
| 0.1 | 問每個 DRM 節點「你是誰、你會畫圖嗎」 | [`exp00_whoami.c`](./experiments/exp00_whoami.c) | `card1`=rknpu，KMS ioctl 被 `EOPNOTSUPP` 擋掉 |
| 0.2 | 驅動版本 vs 原始碼版本 | — | 都是 `0.9.8`，對上 |
| 0.3 | `strace` 看 SDK 怎麼找到 NPU 節點 | — | 逐個開 card，用 `DRM_IOCTL_VERSION` 比名字 |

---

**下一章** → [ch01 硬體長什麼樣](./ch01_hardware.md)
