# E　安全：CRYPTO、TRNG、KEYLAD、OTP

> TRM：Part1 ch33（KEYLAD）、Part2 ch10（CRYPTO，含 TRNG）；OTP 見 Part1 ch1 位址表
> 工具：`tools/rng_quality.py`

## 板上狀況

| 區塊 | 位址 | DT | 說明 |
|------|------|----|------|
| CRYPTO_NS | `fe370000` | **disabled** | 非安全世界的加解密加速器 |
| TRNG_NS | `fe378000` | okay（`trngv1`） | 硬體真亂數，`/dev/hwrng`（`rng_current = rockchip`） |
| OTP_NS | `fecc0000` | okay | eFuse，`/sys/bus/nvmem/devices/rockchip-otp0`（內容解讀見 `rock5b_explore/01 §3`） |
| KEYLADDER_S、CRYPTO_S、TRNG_S、OTP_S、SEC_TRNG_CHK | `fe380000`… | 無節點 | 安全世界專用，Linux 碰不到 |
| HDCP0/1_TRNG | `fde48000`/`fde78000` | 無節點 | HDCP 專用亂數 |

TRM 說 CRYPTO 支援 AES/SM4/DES/TDES 各種模式、SHA-1/256/512、MD5、SM3、HMAC、4096-bit PKA，分安全與非安全兩份。
**這台兩份都沒給 Linux 用**：NS 版在 DT 被關掉，S 版屬於安全世界。
KEYLAD（金鑰階梯，把 OTP/TRNG 裡的金鑰直接送進 CRYPTO、不經 CPU）同樣屬於安全世界，無法從 Linux 驗證。

## 1. 沒有硬體加速，那加密是誰在做？

`/proc/crypto` 裡 AES 全是 `*-aes-ce`（ARMv8 Crypto Extension，CPU 指令），沒有任何 Rockchip 硬體驅動。
`openssl speed`（16 KB 區塊）：

| 演算法 | A76（cpu4） | A55（cpu0） |
|--------|------------|------------|
| AES-128-GCM | **2.09 GB/s** | 0.82 GB/s |
| SHA-256 | **1.39 GB/s** | — |

單顆 A76 就有 2 GB/s 的 AES-GCM。這可能就是 DT 預設關掉 CRYPTO_NS 的原因：對大量資料，CPU 指令已經夠快，
而且不用搬資料、不用中斷。（這是推測，TRM 沒說。）

## 2. TRNG（Part2 §10.4.5）

TRM：從環形振盪器收集亂數，一次最多 256 bit；安全版 TRNG 另外提供遮罩給 CIPHER 防旁路攻擊。

```
$ sudo dd if=/dev/hwrng of=/tmp/hwrng.bin bs=4096 count=256
1048576 bytes (1.0 MB, 1.0 MiB) copied, 0.38242 s, 2.7 MB/s
```

**速度 2.7 MB/s**。品質快篩（`rng_quality.py`，1 MB），和 `/dev/urandom` 並排：

| 指標 | 理想值 | `/dev/hwrng` | `/dev/urandom` |
|------|-------|-------------|----------------|
| 卡方（255 自由度） | 255 ± 23 | 242.4 | 290.0 |
| Shannon 熵 | 8 bit/byte | 7.99983 | 7.99980 |
| 位元 1 的比例 | 0.5 | 0.49999 | 0.49994 |
| zlib 壓縮率 | ≥ 1 | 1.0003 | 1.0003 |
| 相鄰位元組相同 | 4096 | 4207（+1.7σ） | 4150 |

基本統計上和 `/dev/urandom` 分不出來。這只是快篩，不是完整的 NIST SP 800-22／dieharder 測試（板上沒裝 `rngtest`）。

## 3. OTP

OTP 的欄位解讀、和 dmesg 漏電值逐項對帳在 `rock5b_explore/01 §3`；
SYS_GRF 裡的 OTP 鏡像暫存器和 TRM 描述對不上的落差在 A §3。
