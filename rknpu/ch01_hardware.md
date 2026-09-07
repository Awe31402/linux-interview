# ch01 — 硬體長什麼樣

> **本章目的**：把 RK3588 的 NPU 硬體講清楚 —— 有幾顆核心、一顆核心裡面有什麼、
> 暫存器怎麼排。看完之後，你看到 `0x1020` 這種數字就知道它屬於誰。
>
> 這章第一次同時用到三份資料：**TRM**、**device tree**、**實機**。
> 三邊互相對照，是全書之後每一章都會重複的動作。
>
> **實驗平台**：Radxa ROCK 5B（RK3588），`Linux rock-5b 6.1.115+`
>
> **對照素材**
> - TRM：`books/rk3588_trm/part1/chapter_36.txt`
>   §36.1 Overview、§36.2 Block Diagram、§36.3.1~36.3.4 Function Description、
>   §36.4.1 Internal Address Mapping、§36.4.2 Registers Summary
> - Device tree：`arch/arm64/boot/dts/rockchip/rk3588s.dtsi:3450`（NPU）、`:3712`（IOMMU）
>   `arch/arm64/boot/dts/rockchip/rk3588-rock-5b.dts:265`（本板打開它的地方）
> - 驅動：`drivers/rknpu/rknpu_drv.c:183`（`rk3588_rknpu_config`）、
>   `drivers/rknpu/include/rknpu_ioctl.h:21-38`（offset 定義）
>
> **上一章** → [ch00 為什麼在圖形子系統裡](./ch00_why_npu_is_a_drm_driver.md)

---

## 目錄

- [一鍵重現本章全部實驗](#一鍵重現本章全部實驗)
- [1. TRM 說這顆 NPU 是什麼](#1-trm-說這顆-npu-是什麼)
- [2. 一顆核心裡面有什麼](#2-一顆核心裡面有什麼)
- [3. 暫存器怎麼排 —— 一張表定江山](#3-暫存器怎麼排--一張表定江山)
- [4. Device tree：把硬體寫成一份文件](#4-device-tree把硬體寫成一份文件)
- [5. 中斷：三條線，而且要跟 IOMMU 共用](#5-中斷三條線而且要跟-iommu-共用)
- [TRM 與實機對不上的地方](#trm-與實機對不上的地方)
- [本章結論一句話](#本章結論一句話)

---

## 一鍵重現本章全部實驗

```bash
mkdir -p ~/rknpu-lab && cd ~/rknpu-lab
# 從 notes/rknpu/experiments/ 複製 exp01_regs.c 過來

# ---- 實驗 1.1：直接讀 NPU 暫存器，對照 TRM ----
gcc -O1 -o exp01_regs exp01_regs.c
echo on | sudo tee /sys/kernel/debug/rknpu/power     # ⚠️ 一定要先開電
sudo ./exp01_regs
echo off | sudo tee /sys/kernel/debug/rknpu/power

# ---- 實驗 1.2：中斷計數，跑推論前後比一比 ----
grep -E "npu" /proc/interrupts
D=~/disk/rknn/rknn-toolkit2/rknpu2/examples/rknn_api_demo/install/rknn_api_demo_Linux
cd $D && for i in 1 2 3; do
  ./rknn_create_mem_demo model/RK3588/mobilenet_v1.rknn model/dog_224x224.jpg >/dev/null
done
grep -E "npu" /proc/interrupts

# ---- 實驗 1.3：實際生效的 device tree ----
cd /proc/device-tree/npu@fdab0000
ls
tr -d '\0' < compatible; echo
tr -d '\0' < status; echo
tr '\0' ' ' < interrupt-names; echo
hexdump -e '8/1 "%02x" " "' reg; echo
```

---

## 1. TRM 說這顆 NPU 是什麼

### 結論

TRM §36.1 用一張規格表講完。原文照抄（`chapter_36.txt:13-27`）：

```
RKNN supports the following features:
⚫ Include triple NPU CORE
⚫ Support triple core co-work, dual core co-work, and work independently
⚫ AHB interface used for configuration only support single
⚫ AXI interface used to fetch data from memory
⚫ Support integer 4, integer 8, integer 16, float 16, Bfloat 16 and tf32 operation
⚫ 1024x3 integer 8 MAC operations per cycle
⚫ 512x3 integer 16 MAC operations per cycle
⚫ 512x3 float 16 MAC operations per cycle
⚫ 512x3 bfloat 16 MAC operations per cycle
⚫ 256x3 tf32 MAC operation per cycle
⚫ 2048x3 integer 4 MAC operation per cycle
⚫ 384KBx3 internal buffer
⚫ Inference Engine: TensorFlow, Caffe, Tflite, Pytorch, Onnx NN, Android NN, etc.
```

翻成人話，四件重點：

**① 三顆核心，可以合作也可以各做各的。**
所有數字後面那個 `x3` 就是「三顆加起來」。單顆是 1024 個 int8 MAC/cycle，三顆 3072。

> **MAC** = Multiply-ACcumulate（乘加）。神經網路九成的運算就是「一堆數字相乘再全部加起來」。
> 「每個 cycle 做 1024 個 MAC」的意思是：時脈跳一下，同時算完 1024 組乘加。
> 本機頻率 1 GHz，所以單核大約 **每秒 1 兆次乘加**。

**② 兩種介面，分工明確。**

| 介面 | 用途 | TRM 原文（§36.3.1） |
|---|---|---|
| **AHB** | 只做設定、除錯 | *"The AHB slave interface is used to access the registers for configuration, debug and test."* |
| **AXI** | 只搬資料 | *"The AXI master interface is used to fetch data from memory..."* |

記住這個分工。**這是理解整顆 NPU 的關鍵。**

CPU 透過 AHB 設定 NPU（慢，但只有幾十筆），
NPU 自己透過 AXI 去記憶體抓資料（快，量大）。

**③ 支援六種數字格式**：int4 / int8 / int16 / fp16 / bf16 / tf32。
位元數越少算越快 —— int4 是 int8 的兩倍速。我們的實驗模型用 int8。

**④ 每顆核心 384 KB 內部緩衝。**

**這個數字之後會咬人。** 一張 224×224×3 的圖是 150 KB，看起來塞得下，
但卷積過程中要同時放輸入、權重、中間結果，就不夠了。
所以硬體會**把圖切成橫條分批算** —— 我們在 [ch05](./ch05_regcmd.md) 會親眼看到切法。

---

## 2. 一顆核心裡面有什麼

### 結論

TRM §36.2 的方塊圖（Fig. 36-2）把一顆核心拆成這些零件：

```
                    ┌──────── AHB（設定） ────────┐
                    ▼                             │
        ┌───────────────────────┐                 │
        │ Register File /       │  ← CPU 寫設定進來
        │ Interrupt Control     │  → 算完了發中斷
        └───────────────────────┘
        ┌───────────────────────┐
        │ Register File Fetch   │  ← ★ 自己去記憶體抓「設定指令」
        └───────────────────────┘
                    ▲
                    │ AXI（搬資料）
        ┌───────────┴───────────────────────────────┐
        │                                           │
   ┌─────────────────────────────┐   ┌───────────┐  ┌──────────┐
   │ CNA                         │──▶│ DPU       │─▶│ PPU      │
   │  Weight Decompress          │   │ (Data     │  │ (Planar  │
   │  Zero-Skipping              │   │  Process  │  │  Process │
   │  Weight/Feature Data Load   │   │  Unit)    │  │  Unit)   │
   │  Sequence Controller        │   └───────────┘  └──────────┘
   │  MAC Array + Accumulator    │
   │  384KB Buffer               │
   └─────────────────────────────┘
```

三個計算單元，串成一條生產線。TRM §36.3.2~36.3.4 各給一段說明：

| 單元 | TRM 給的全名 | 做什麼（TRM 原文摘要） | 白話 |
|---|---|---|---|
| **CNA** | **TRM 沒展開這個縮寫** | §36.3.2 標題是 *"Neural Network Accelerating Engine"*，內容：*"the main process unit for Neural Network arithmetic... convolution pre-process controller, internal buffer, mac array, accumulator"* | **主力**。做卷積，就是那一大堆乘加 |
| **DPU** | Data Process Unit | *"process the single data calculate, such as leaky_relu, relu, relux, sigmoid, tanh… also softmax, transpose, data format conversion"* | 對**每一個數字**單獨做處理 |
| **PPU** | Planar Process Unit | *"planar function followed by output data from Data Processing Unit, such as average pooling, max pooling, min pooling"* | 對**一整片數字**做處理，主要是池化 |

> ⚠️ **`CNA` 這三個字母，TRM 從頭到尾沒有展開過。**
> 方塊圖只標 `CNA`，§36.3.2 的標題又叫 `Neural Network Accelerating Engine`，兩個名字對不起來。
> 網路上會看到有人寫「Convolution Neural network Accelerator」—— **那不是 TRM 講的，別當成事實。**
>
> 可以確定的只有：這個區塊的暫存器全部以 `RKNN_cna_` 開頭，
> 而且欄位叫 `datain_width`、`weight_size`、`conv_con` —— **它就是做卷積的那個。**
> 全名不知道就說不知道。

> 不需要懂這些名詞的數學。只要知道：
> **CNA 做「一大片乘加」，DPU 做「逐個數字加工」，PPU 做「一整片縮小」。**
> 順序是 CNA → DPU → PPU。

還有一個不算計算單元、但最重要的零件：

**Register File Fetch Unit** —— TRM §36.3.5 只有一句話（`chapter_36.txt:119-121`）：

> *"Register File Fetch Unit fetch register configuration from external system memory through AXI interface."*
>
> （暫存器檔擷取單元，透過 AXI 介面從外部系統記憶體抓取暫存器設定。）

**這一句是全書的樞紐。**

意思是：NPU 不需要 CPU 一筆一筆餵設定。你只要告訴它「設定清單放在記憶體的哪裡」，
它會**自己去抓、自己照著設定**。

驅動之所以只寫 8 個暫存器就能發動整個神經網路，靠的就是這個單元。
[ch05](./ch05_regcmd.md) 和 [ch06](./ch06_submit.md) 整整兩章都在講它。

---

## 3. 暫存器怎麼排 —— 一張表定江山

### 結論

TRM §36.4.1 給了一張位址表。**背下這張表，之後看任何暫存器 offset 都能秒認。**

| Base Address[15:12] | 區塊 | offset 範圍 | 是什麼 |
|---|---|---|---|
| `0` | **PC** | `0x0000 ~ 0x0fff` | Program Counter —— 指令抓取的控制台 |
| `1` | **CNA** | `0x1000 ~ 0x1fff` | 卷積 |
| `3` | CORE | `0x3000 ~ 0x3fff` | |
| `4` | **DPU** | `0x4000 ~ 0x4fff` | 逐點運算 |
| `5` | DPU_RDMA | `0x5000 ~ 0x5fff` | DPU 的讀取 DMA |
| `6` | **PPU** | `0x6000 ~ 0x6fff` | 池化 |
| `7` | PPU_RDMA | `0x7000 ~ 0x7fff` | PPU 的讀取 DMA |
| `8` | DDMA | `0x8000 ~ 0x8fff` | |
| `9` | SDMA | `0x9000 ~ 0x9fff` | |
| `f` | GLOBAL | `0xf000 ~ 0xf004` | 全域開關 |

**看第四位數就知道是誰的。** `0x1020` → `1` → CNA。`0x4004` → `4` → DPU。

驅動只在 `rknpu_ioctl.h:21-38` 硬寫了幾個 offset，全部落在 PC 區（外加一個 GLOBAL）：

```c
#define RKNPU_OFFSET_VERSION          0x0
#define RKNPU_OFFSET_VERSION_NUM      0x4
#define RKNPU_OFFSET_PC_OP_EN         0x8
#define RKNPU_OFFSET_PC_DATA_ADDR     0x10
#define RKNPU_OFFSET_PC_DATA_AMOUNT   0x14
#define RKNPU_OFFSET_PC_TASK_CONTROL  0x30
#define RKNPU_OFFSET_PC_DMA_BASE_ADDR 0x34
#define RKNPU_OFFSET_INT_MASK         0x20
#define RKNPU_OFFSET_INT_CLEAR        0x24
#define RKNPU_OFFSET_INT_STATUS       0x28
#define RKNPU_OFFSET_INT_RAW_STATUS   0x2c
#define RKNPU_OFFSET_ENABLE_MASK      0xf008
```

**注意：CNA / DPU / PPU 的暫存器一個都沒有。**

因為驅動根本不設定它們 —— 那是 Register File Fetch Unit 從記憶體抓來自己設的。
驅動只碰「怎麼開始、怎麼結束」。

### 實機驗證（1.1）：那些暫存器真的在那裡嗎

實驗程式：[`experiments/exp01_regs.c`](./experiments/exp01_regs.c)

用 `/dev/mem` 把三顆核心的暫存器區間 mmap 進來直接讀。

> ⚠️ **一定要先開電。**
> NPU 平常是關電的（[ch08](./ch08_power_freq.md) 會講）。
> 對一個沒供電的裝置讀暫存器，匯流排不會回應，**整台板子可能當掉**。
>
> ```bash
> echo on | sudo tee /sys/kernel/debug/rknpu/power
> ```
>
> 這一步不是龜毛，是必要的。

```
offset   TRM 36.4.2 name                core0        core1        core2
-------------------------------------------------------------------------------
0x0000   (TRM 未記載)                   0x46495245   0x46495245   0x46495245
0x0004   (TRM 未記載)                   0x00000000   0x00000000   0x00000000
0x0008   RKNN_pc_operation_enable       0x00000000   0x00000000   0x00000000
0x0010   RKNN_pc_base_address           0x00000001   0x00000001   0x00000001
0x0014   RKNN_pc_register_amounts       0x00000000   0x00000000   0x00000000
0x0020   RKNN_pc_interrupt_mask         0x8001ffff   0x8001ffff   0x8001ffff
0x0024   RKNN_pc_interrupt_clear        0x00000000   0x00000000   0x00000000
0x0028   RKNN_pc_interrupt_status       0x00000000   0x00000000   0x00000000
0x002c   RKNN_pc_interrupt_raw_status   0xc0000000   0x40000000   0x40000000
0x0030   RKNN_pc_task_con               0x00000000   0x00000000   0x00000000
0x0034   RKNN_pc_task_dma_base_addr     0x00000000   0x00000000   0x00000000
0x003c   RKNN_pc_task_status            0x00005000   0x00005000   0x00005000
0xf008   RKNN_global_operation_enable   0x00000000   0x00000000   0x00000000
```

**暫存器真的在 TRM 說的位址上。** 而且三顆核心的佈局一模一樣 —— 印證了「三顆一樣的核心」。

四個值得停下來看的地方：

#### ① `0x0000` 讀到 `0x46495245` —— 那是 ASCII

把這四個位元組當字元讀（由高位到低位）：

```
0x46 0x49 0x52 0x45
 'F'  'I'  'R'  'E'
```

**`FIRE`。** 這是硬體的身分識別魔術數字。

TRM 的暫存器摘要表**從 `0x0008` 才開始**，`0x0000` 和 `0x0004` 一筆都沒列。
但驅動在用（`rknpu_ioctl.h:21-22`）。

`rknpu_job.c:892` 就是拿它當硬體版本：

```c
	*version = REG_READ(RKNPU_OFFSET_VERSION) +
		   (REG_READ(RKNPU_OFFSET_VERSION_NUM) & 0xffff);
```

實驗程式最後那段把迴圈閉起來 —— 用 `RKNPU_ACTION` ioctl 問驅動同一件事：

```
--- 交叉比對：驅動回報的硬體版本 ---
rknpu_job.c:892  version = REG_READ(0x0) + (REG_READ(0x4) & 0xffff)
                         = 0x46495245 + 0x0000 = 0x46495245
ioctl(ACTION/GET_HW_VERSION) -> 0x46495245  ✓ 一致
```

**我們自己讀的、跟驅動回報的，完全一樣。**
這證明我們讀的位址、算的方式都對，可以放心往下走。

#### ② `0x0010` 是 `0x00000001` —— 上一個工作留下的痕跡

TRM 說這是 `RKNN_pc_base_address`（指令清單位址）。`1` 顯然不是一個合法位址。

答案在 `rknpu_job.c:388`：

```c
	// switch to slave mode
	REG_WRITE(0x1, RKNPU_OFFSET_PC_DATA_ADDR);
```

驅動每次送工作前，會先寫 `0x1` 把硬體切回 slave 模式。
我們讀到的就是**上一次推論結束後留在那裡的值**。

> 讀暫存器讀到「奇怪的值」時，先想想「誰最後寫了它」。
> 暫存器不會自己歸零。

#### ③ `0x002c` 三顆核心不一樣

`core0 = 0xc0000000`，`core1 / core2 = 0x40000000`。

差在 **bit 30**。core0 是唯一跑過工作的核心（我們在 [ch06](./ch06_submit.md) 會證明），
所以它的原始中斷狀態多了一位。

#### ④ `0x0020` 讀到 `0x8001ffff` —— 但 TRM 說最高位應該是 0

TRM 對這個暫存器的位元定義（`chapter_36.txt:2031` 之後）：

```
Bit    Attr  Reset Value   Description
31:17  RO    0x0000        reserved
16:0   RW    0x1ffff       int_mask
```

低 17 位 `0x1ffff` **完全對得上** reset value。

但 **bit 31 是 1**，而 TRM 說那是「保留、唯讀、reset 為 0」。

**這是 TRM 跟實機對不上的地方，原因不明。** 我不猜。詳見下面的落差表。

---

## 4. Device tree：把硬體寫成一份文件

### 結論

TRM 講的是「這顆晶片有什麼」。**Device tree 講的是「這塊板子上，它接到哪裡」。**

驅動不會去讀 TRM。它讀的是 device tree。

RK3588 的 NPU 節點在 `arch/arm64/boot/dts/rockchip/rk3588s.dtsi:3450`：

```dts
	rknpu: npu@fdab0000 {
		compatible = "rockchip,rk3588-rknpu";
		reg = <0x0 0xfdab0000 0x0 0x10000>,      /* core0 */
		      <0x0 0xfdac0000 0x0 0x10000>,      /* core1 */
		      <0x0 0xfdad0000 0x0 0x10000>;      /* core2 */
		interrupts = <GIC_SPI 110 IRQ_TYPE_LEVEL_HIGH>,
			     <GIC_SPI 111 IRQ_TYPE_LEVEL_HIGH>,
			     <GIC_SPI 112 IRQ_TYPE_LEVEL_HIGH>;
		interrupt-names = "npu0_irq", "npu1_irq", "npu2_irq";
		clocks = <&scmi_clk SCMI_CLK_NPU>, <&cru ACLK_NPU0>, ... ;
		power-domains = <&power RK3588_PD_NPUTOP>,
				<&power RK3588_PD_NPU1>,
				<&power RK3588_PD_NPU2>;
		operating-points-v2 = <&npu_opp_table>;
		iommus = <&rknpu_mmu>;
		status = "disabled";          /* ← 注意這行 */
	};
```

一行一行對回 TRM：

| device tree 寫的 | 對應 TRM 的哪句話 |
|---|---|
| `reg` 三段，每段 `0x10000` | §36.1「Include triple NPU CORE」+ §36.4.1 位址表（每核 64KB 空間） |
| `interrupts` 三個 | 三顆核心各自發中斷 |
| `power-domains` 三個 | 三顆核心可以獨立開關（呼應「work independently」） |
| `iommus` | §36.3.1 的 AXI master 需要位址翻譯 |
| `operating-points-v2` | 可調頻調壓（TRM 沒講，是 SoC 層級的事） |

**`status = "disabled"` 這行很重要。**

`rk3588s.dtsi` 是**晶片**層級的描述 —— 「這顆晶片有 NPU」。
但「這塊板子要不要用它」是**板子**層級的決定。

我們這塊板子在 `arch/arm64/boot/dts/rockchip/rk3588-rock-5b.dts:265` 打開它：

```dts
&rknpu {
	rknpu-supply = <&vdd_npu_s0>;      /* NPU 主電源，晶片層級不知道接哪 */
	mem-supply = <&vdd_npu_mem_s0>;    /* NPU 記憶體電源 */
	status = "okay";                   /* ← 打開 */
};

&rknpu_mmu {
	status = "okay";
};
```

> **新手常見的坑**：改了 `rk3588s.dtsi` 卻沒效果，
> 常常是因為板子的 `.dts` 又覆蓋回去了。**永遠以最後一層為準。**

### 實機驗證（1.3）：實際生效的是哪一份

`/proc/device-tree/` 是核心開機時真正讀到的那份，所有覆蓋都已經套用完了。

```bash
$ cd /proc/device-tree/npu@fdab0000
$ ls
assigned-clock-rates  clock-names  interrupt-names  mem-supply       power-domains
assigned-clocks       clocks       interrupts       name             reg
compatible            iommus       operating-points-v2  phandle      reset-names
                                   power-domain-names   resets       rknpu-supply
                                                                     status
```

```
compatible : rockchip,rk3588-rknpu
status     : okay
irq names  : npu0_irq npu1_irq npu2_irq
pd names   : npu0 npu1 npu2
clk names  : clk_npu aclk0 aclk1 aclk2 hclk0 hclk1 hclk2 pclk
reg (hex)  : 00000000fdab0000 0000000000010000
             00000000fdac0000 0000000000010000
             00000000fdad0000 0000000000010000
```

三件事一次確認：

1. **`status` 是 `okay`** —— 板子的 `.dts` 確實覆蓋掉了 dtsi 的 `disabled`
2. **多出 `rknpu-supply` 和 `mem-supply`** —— 這兩個只在板子的 `.dts` 有，dtsi 沒有
3. **`reg` 三段位址**，跟 TRM 和 dtsi 完全一致

TRM → dtsi → 板子 dts → 實機，**四層對得起來。**

---

## 5. 中斷：三條線，而且要跟 IOMMU 共用

### 結論

`interrupts = <GIC_SPI 110>, <GIC_SPI 111>, <GIC_SPI 112>` —— 三顆核心，三條中斷線。

但你去 `rk3588s.dtsi:3712` 看 IOMMU 節點：

```dts
	rknpu_mmu: iommu@fdab9000 {
		compatible = "rockchip,iommu-v2";
		reg = <0x0 0xfdab9000 0x0 0x100>, <0x0 0xfdaba000 0x0 0x100>,
		      <0x0 0xfdaca000 0x0 0x100>, <0x0 0xfdada000 0x0 0x100>;
		interrupts = <GIC_SPI 110 IRQ_TYPE_LEVEL_HIGH>,   /* ← 一模一樣 */
			     <GIC_SPI 111 IRQ_TYPE_LEVEL_HIGH>,
			     <GIC_SPI 112 IRQ_TYPE_LEVEL_HIGH>;
		interrupt-names = "npu0_mmu", "npu1_mmu", "npu2_mmu";
	};
```

**同樣的 110 / 111 / 112。** NPU 和它的 IOMMU **共用同一條中斷線**。

意思是中斷來的時候，處理程式得自己判斷：是「算完了」還是「位址翻譯出錯了」。

> 另外注意 IOMMU 有 **4** 段 `reg`，NPU 只有 3 段。多的那一段是什麼，
> TRM 沒有任何一章提到 `rockchip,iommu-v2`。**待查，不猜。**

### 實機驗證（1.2）：中斷真的長這樣嗎

```bash
$ grep npu /proc/interrupts
 40:        120  0  0  0  0  0  0  0   GICv3 142 Level   fdab9000.iommu, fdab0000.npu
 41:          0  0  0  0  0  0  0  0   GICv3 143 Level   fdab9000.iommu, fdab0000.npu
 42:          0  0  0  0  0  0  0  0   GICv3 144 Level   fdab9000.iommu, fdab0000.npu
```

三件事：

#### ① 一行掛兩個裝置 —— 共用中斷確認

`fdab9000.iommu, fdab0000.npu` 出現在同一行。device tree 說的是真的。

#### ② `GICv3 142` vs device tree 的 `GIC_SPI 110` —— 差 32

不是打錯。ARM GIC 的中斷編號是這樣切的：

| 硬體編號 | 類型 | 用途 |
|---|---|---|
| 0 ~ 15 | SGI | CPU 之間互相叫（Software Generated） |
| 16 ~ 31 | PPI | 每個 CPU 私有的（Private Peripheral） |
| **32 起** | **SPI** | **周邊裝置共用的（Shared Peripheral）** |

device tree 寫 `GIC_SPI 110` 是「第 110 個 SPI」，
硬體編號就是 `110 + 32 = 142`。

> 這個 +32 是 ARM 平台的通則，不只 NPU。看到對不上先算一下這個。

#### ③ 只有第一條線在動

跑 3 次推論前後比一比：

```
推論前   40:  120        41:  0        42:  0
推論後   40:  126        41:  0        42:  0
          ↑ +6
```

**IRQ 41 和 42 完全沒動。** 只有 core0 在幹活。

而且 `+6 ÷ 3 次推論 = 每次 2 個中斷`。

我們在 [ch05](./ch05_regcmd.md) 攔下來的資料顯示，**一次推論送出 2 個 `SUBMIT`**。

> **1 個 SUBMIT = 1 個中斷。** 數字對得起來。
>
> 為什麼一次推論要送兩次？為什麼三顆核心只用一顆？
> 這兩題留到 [ch06](./ch06_submit.md)。

---

## TRM 與實機對不上的地方

全書會持續累積這張表。**這些不是 bug，是「文件沒寫全」的正常狀態。**
重要的是**知道自己不知道**，而不是編一個解釋。

| # | TRM 怎麼說 | 實機／程式碼是什麼 | 判斷 |
|---|---|---|---|
| 1 | §36.4.2 暫存器摘要表**從 `0x0008` 開始**，沒有 `0x0000` / `0x0004` | 驅動有定義（`rknpu_ioctl.h:21-22`），`0x0000` 讀到 `0x46495245`（`"FIRE"`），且與 ioctl 回報的硬體版本一致 | **TRM 漏寫**。功能明確（硬體識別），可以放心用 |
| 2 | §36.4.1 位址表說 GLOBAL 區是 `0xf000 ~ 0xf004`，長度 **4 BYTE** | 但同一章的 §36.4.2 摘要表和 §36.4.3 詳細說明都列了 `0xF008`（`RKNN_global_operation_enable`），驅動也在用 | **TRM 自己前後矛盾**。以摘要表為準 |
| 3 | `0x0020` bit 31 是「RO，reset 0，reserved」 | 實機讀到 `0x8001ffff`，**bit 31 = 1** | **不明**。低 17 位完全符合，只有最高位對不上。待查 |
| 4 | 完全沒有任何一章提到 `rockchip,iommu-v2` | dtsi 有這個裝置，而且它比 NPU 多一段 `reg` | **TRM 沒收錄這個裝置**。[ch04](./ch04_memory.md) 會從驅動端補 |

---

## 本章結論一句話

> **三顆一樣的核心，每顆裡面是 CNA → DPU → PPU 一條生產線，
> 加上一個會自己去記憶體抓設定的 Register File Fetch Unit。**
>
> 暫存器按第四位數分區：`0x0xxx`=PC、`0x1xxx`=CNA、`0x4xxx`=DPU、`0x6xxx`=PPU。
>
> 驅動只碰 PC 區。CNA/DPU/PPU 那幾千個暫存器，是硬體自己去記憶體抓來設的 ——
> **那份「設定清單」就是模型本身**，是 [ch05](./ch05_regcmd.md) 的主題。

---

## 本章做過的實驗

| # | 實驗 | 檔案 | 結果 |
|---|---|---|---|
| 1.1 | `/dev/mem` 直接讀三顆核心的暫存器 | [`exp01_regs.c`](./experiments/exp01_regs.c) | 位址與 TRM 一致；`0x0000` = `"FIRE"`，與 ioctl 回報的硬體版本相符 |
| 1.2 | `/proc/interrupts` 推論前後比對 | — | 三條線共用給 IOMMU；只有 core0 動；1 個 SUBMIT = 1 個中斷 |
| 1.3 | `/proc/device-tree/npu@fdab0000/` | — | `status=okay`（板子 dts 覆蓋）、`reg` 三段與 TRM 一致 |

---

**下一章** → [ch02 開機：從 device tree 到一個能用的裝置](./ch02_probe.md)
