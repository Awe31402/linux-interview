# RK3588 NPU 入門教材

> 一份給**沒寫過 Linux driver 的人**的教材，目標是看完能回答：
>
> **「我呼叫 `rknn_run()`，到 RK3588 的 NPU 硬體真的開始算，中間發生了什麼事？」**
>
> 全書 10 章、33 個實驗，**每一個都在真機上跑過**。
> 平台：Radxa ROCK 5B（RK3588），`Linux 6.1.115+`，RKNPU driver `v0.9.8`。

---

## 從哪裡開始

**照順序讀。** 每一章都用得到前一章的結論。

| 章 | 標題 | 一句話 |
|---|---|---|
| [ch00](./ch00_why_npu_is_a_drm_driver.md) | 你為什麼會在圖形子系統裡找到一顆 AI 加速器 | 它掛在 DRM 上，但只借記憶體管理，不畫圖 |
| [ch01](./ch01_hardware.md) | 硬體長什麼樣 | 三顆核心，CNA → DPU → PPU，暫存器按第四位數分區 |
| [ch02](./ch02_probe.md) | 開機：從 device tree 到一個能用的裝置 | probe 照清單把時脈、電源、中斷、IOMMU 一樣樣接起來 |
| [ch03](./ch03_six_ioctls.md) | 六個 ioctl：使用者空間與核心的唯一交界 | 六句話，五句在管記憶體 |
| [ch04](./ch04_memory.md) | 記憶體：一塊記憶體的三個名字 | `dma_addr` 不是實體位址，是 IOMMU 造的假象 |
| [**ch05**](./ch05_regcmd.md) ★ | **regcmd：模型被編譯成什麼** | **模型 = 一串暫存器寫入指令，躺在 DRAM 裡** |
| [ch06](./ch06_submit.md) | 送出：driver 只寫 8 個暫存器 | 8 次 `writel()` 發動整個神經網路，然後去睡 |
| [ch07](./ch07_interrupt.md) | 中斷：硬體怎麼說「我算完了」 | 中斷是硬體通知軟體的唯一管道 |
| [ch08](./ch08_power_freq.md) | 電源與頻率：閒置三秒就關電 | 全有或全無：要用就滿速，閒置就斷電 |
| [ch09](./ch09_reset.md) | 出事的時候：timeout 與重置 | 158 行的安全網 |

**趕時間的話**：讀 [ch00](./ch00_why_npu_is_a_drm_driver.md) 建立方向感，
直接跳 [ch05](./ch05_regcmd.md) 看主線論證，再回頭補。

---

## 主線

全書跟著**一次推論**走，不繞路：

```
 使用者程式  rknn_run()
     ▼
┌─────────────────────────────────────────────┐
│  librknnrt.so（閉源，只能從 ioctl 反推）      │  ch03
│  把預編好的暫存器指令準備到記憶體裡            │
└─────────────────────────────────────────────┘
     │  ioctl(/dev/dri/card1, ...)
     │  ├─ MEM_CREATE / MEM_MAP / MEM_SYNC ──▶ ch04
     │  └─ SUBMIT ─────────────────────────▶ ch06
     ▼
┌─────────────────────────────────────────────┐
│  drivers/rknpu/                              │  ch00 ch02
│  排隊、開電、寫 8 個暫存器、然後去睡           │  ch08
└─────────────────────────────────────────────┘
     │  writel() × 8
     ▼
┌─────────────────────────────────────────────┐
│  NPU 硬體                                    │  ch01
│  自己去 DRAM 抓指令 ──────────────────▶ ch05 ★│
│  CNA / DPU / PPU 照著算                      │
└─────────────────────────────────────────────┘
     │  中斷 ───────────────────────────────▶ ch07
     ▼                        （出事了 ──────▶ ch09）
 回到 rknn_run()，返回
```

---

## 工具

全部不用改 kernel，一般使用者權限就能編。

| 檔案 | 用在 | 做什麼 |
|---|---|---|
| [`tools/rkspy.c`](./tools/rkspy.c) | ch04 ch05 ch06 | **主力工具。** `LD_PRELOAD` 攔 `ioctl`/`mmap`，把 regcmd、記憶體總表、實體位址全挖出來 |
| [`experiments/exp00_whoami.c`](./experiments/exp00_whoami.c) | ch00 | 問每個 DRM 節點「你是誰、你會畫圖嗎」 |
| [`experiments/exp01_regs.c`](./experiments/exp01_regs.c) | ch01 ch09 | `/dev/mem` 直接讀三顆核心的暫存器 |
| [`experiments/exp03_action.c`](./experiments/exp03_action.c) | ch03 | 算出 ioctl 號碼、問遍 `ACTION` 的 26 個子命令 |
| [`experiments/exp03_count.sh`](./experiments/exp03_count.sh) | ch03 | `strace -e raw=ioctl` 統計一次推論送了什麼 |
| [`experiments/exp05_run.c`](./experiments/exp05_run.c) | ch05~ch09 | 通用 `.rknn` 執行器，可指定 `core_mask` 和次數 |

**編譯**（板子上）：

```bash
gcc -shared -fPIC -O1 -o rkspy.so rkspy.c -ldl
gcc -O1 -o exp05_run exp05_run.c -lrknnrt
gcc -O1 -o exp00_whoami exp00_whoami.c
gcc -O1 -o exp01_regs   exp01_regs.c
gcc -O1 -o exp03_action exp03_action.c
```

---

## 這本書的規矩

> **每一個「這段程式碼對應 TRM 哪裡」的說法，都必須真的在 TRM 裡找到。
> 找不到就明講「TRM 沒寫」。絕對不掰一個聽起來合理的暫存器名字。**

結果是全書累積了 **18 筆** TRM／程式碼落差（完整表在
[ch09](./ch09_reset.md#trm-與實機對不上的地方完整版)），其中 **6 筆的結論是「待查」**。

寫作過程中**自己抓到並修正了兩次違規**，兩次都是「聽起來很合理」的說法：

- `CNA` 展開成 "Convolution Neural network Accelerator" —— **TRM 從未展開這個縮寫**
- 「分條多出的 2 列 = 卷積接縫重疊」—— 換一個模型驗證就不成立

---

## 幾條反覆用到的通則

1. **看不到程式碼，就看它說了什麼。** `librknnrt.so` 閉源，但 ioctl 攔得到。
2. **同一件事至少換兩種方法量。** 「一次推論 2 個中斷」用了三種。
3. **跑完實驗去 `dmesg` 撿屍體。** 驅動留下的線索比想像中多。
4. **看到模組參數，grep 它被讀在哪、寫在哪。** 權限位元不保證任何事。
5. **看不懂的 log 通常藏著最有意思的事。**
6. **找不到就說找不到。**

---

## 相關文件

- [BRIEF.md](./BRIEF.md) —— 任務簡報（為什麼寫、界線在哪、完成條件）
- [OUTLINE.md](./OUTLINE.md) —— 大綱（每章要對照哪些檔案、哪幾節 TRM）
