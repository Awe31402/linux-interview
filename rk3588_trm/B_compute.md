# B　運算核心：CPU、GPU、NPU

> TRM：Part1 ch3（CPU）、ch4（GPU）、ch36（RKNN）
> 工具：`tools/gpu_burn.py`；CPU 的 NEON 實驗沿用 `rock5b_explore/tools/heat.c`

## 1. CPU（Part1 ch3）

TRM Table 3-1 vs 板上：

| 項目 | TRM | 實測 | |
|------|-----|------|---|
| 小核 | 4 × A55 | cpu0-3，`MIDR_EL1 = 0x412fd050` → Arm / part 0xD05（A55）/ **r2p0** | ✅ |
| 大核 | 4 × A76 | cpu4-7，`MIDR_EL1 = 0x414fd0b0` → Arm / part 0xD0B（A76）/ **r4p0** | ✅ |
| A55 L1 I/D、L2 | 32K / 32K / 128K | `cache/index*`：L1D 32K、L1I 32K、L2 128K | ✅ |
| A76 L1 I/D、L2 | 64K / 64K / 512K | L1D 64K、L1I 64K、L2 512K | ✅ |
| L3 | 3072K | L3 3072K（1 instance，DSU 共用） | ✅ |
| Crypto 擴充 | Yes | `/proc/cpuinfo`：`aes pmull sha1 sha2` | ✅ |
| NEON/FP | Yes | `asimd asimdhp asimddp fphp` | ✅ |

實測 NEON FMA 峰值（`rock5b_explore/02 §1`）：A76 **16 FLOP/週期**、A55 **8 FLOP/週期**。
Cache 延遲階梯與 TRM cache 大小的對帳在 `notes/ch03_memory_management_prerequisites.md`。

## 2. GPU（Part1 ch4）

TRM 只寫「Mali-G610」，細節全部指向 Arm 的 Odin r0p0 TRM。

### 板上身分

這台用的是**開源驅動 panthor**（不是 Arm 的 mali_kbase），使用者態是 Mesa 24.2 的 Panfrost：

```
panthor fb000000.gpu: [drm] mali-g610 id 0xa867 major 0x0 minor 0x0 status 0x5
panthor fb000000.gpu: [drm] Features: L2:0x7120306 Tiler:0x809 Mem:0x301 MMU:0x2830 AS:0xff
panthor fb000000.gpu: [drm] shader_present=0x50005 l2_present=0x1 tiler_present=0x1
panthor fb000000.gpu: [drm] CSF FW using interface v1.1.0
```

- `major 0 minor 0` → **r0p0**，和 TRM 引用的 Odin r0p0 一致 ✅
- `shader_present = 0x50005` → bit 0、2、16、18 → **4 個 shader core（MP4）**，編號不連續
- `L2_FEATURES = 0x07120306`：[23:16]=0x12 → L2 每片 2^18 = 256 KB；[31:24]=7 → 匯流排 2^7 = 128 bit
- 這台的 Mesa **沒有** Vulkan（panvk）與 OpenCL：`vulkaninfo` 只看到 llvmpipe、`clinfo` 沒平台。只有 GLES 3.1

### 實驗：GPU 真的在算嗎？算多快？

沒有 GLES 標頭檔，所以 `gpu_burn.py` 用 Python ctypes 直接呼叫 `libEGL`/`libGLESv2`/`libgbm`：
GBM 開 `/dev/dri/renderD130`（panthor）→ 無 config 的 surfaceless context → 1024×1024 FBO →
全螢幕三角形，fragment shader 每像素跑 512 次 × 4 個 vec4 的 `a*k+0.5`。

```
GL_RENDERER = Mali-G610 (Panfrost) | GL_VERSION = OpenGL ES 3.1 Mesa 24.2
```

同時每秒記錄 GPU devfreq：

```
t=1  f=300000000  load=0
t=2  f=1000000000 load=99     ← 一有負載直接從最低跳最高（simple_ondemand）
...
t=11 f=1000000000 load=61
t=12 f=300000000  load=0
```

### 意外發現：Forward Pixel Kill 讓「算力」灌水 8 倍

第一次算出 1291 GFLOPS，遠超合理值。改變「每次 `glFinish` 前畫幾張」與「是否開混色」：

| 設定 | 結果 |
|------|------|
| 每次畫 1 張 | **323.8** GFLOPS |
| 連畫 4 張（互相完全覆蓋） | 1256.9（≈ ×4） |
| 連畫 8 張 | 2588.4（≈ ×8） |
| 連畫 4 張 + `GL_BLEND`（ONE, ONE） | **327.6** |

Mali 是 tile-based GPU：同一個 tile 裡，後畫的不透明像素會把先畫、還沒跑完的像素**直接殺掉**
（Arm 稱 Forward Pixel Kill）。連畫 N 張互相蓋住，實際只算了最後一張。
開了混色，後畫的必須和先畫的結果相加，就殺不掉了，數字回到 ~325。

→ **真實值約 325 GFLOPS @ 1 GHz**（FP32，靠 shader 迴圈估算）。
常見公開資料說 G610 每 core 每週期 128 FP32 FLOP，MP4 @ 1 GHz 峰值 512 GFLOPS，實測約 63%。
（峰值數字不在 RK3588 TRM 裡，是外部資料，**未查證**。）

### GPU 電源域

`pm_genpd` 的 `gpu` 平常是 off；開機 137 秒只開過 62 ms（probe 時）。跑 `gpu_burn.py` 時開、結束後自動關。

## 3. RKNN / NPU（Part1 ch36）

這章已經有整套教材：[`notes/rknpu/`](../rknpu/README.md)（十章，33 個實機實驗）。
重點回顧：3 個 NPU core（`0xfdab0000`/`fdac0000`/`fdad0000`，TRM 稱 RKNN C0~C2）、
走 DRM GEM（`/dev/dri/renderD129`）、regcmd 格式已解出並對上 TRM 暫存器表。

本輪補充的只有 NPU 的體質分數：兩次開機 `pvtm = 856 / 859`、`pvtm-volt-sel = 2`、`leakage = 8`（見 `rock5b_explore/01`）。
