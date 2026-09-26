# D　音訊：I2S、SPDIF TX/RX、Audio Codec、VAD、PDM

> TRM：Part1 ch22（I2S）、ch23（SPDIF TX）、ch24（SPDIF RX）、ch27（Digital Audio Codec）、ch28（VAD）、ch39（PDM）
> 工具：`tools/i2s_probe.sh`

## 板上啟用狀況

| 模組 | TRM 數量 | 啟用 | 接到哪 |
|------|---------|------|-------|
| I2S（8ch TDM） | I2S0/1/4~10 | I2S0、I2S5、I2S6、I2S7 | I2S0 → es8316 codec（耳機孔）；I2S5/6 → HDMI TX0/TX1（只 TX）；I2S7 → HDMI RX（只 RX） |
| I2S（2ch） | I2S2、I2S3 | 無 | — |
| SPDIF TX | TX0~5 | SPDIF_TX2 | DP0 音訊 |
| SPDIF RX | RX0~2 | 無 | — |
| PDM | PDM0、PDM1 | 無 | — |
| VAD | 1 | 無 | — |
| Audio Codec（ACDCDIG_DSM） | 1 | 無 | ROCK 5B 用外接 es8316，不用 SoC 內建 codec |

ALSA 看到 5 張卡：`rockchip-hdmi0`、`rockchip-hdmi1`、`rockchip-hdmi2`（其實是 DP，走 spdif-hifi）、`rockchip,hdmiin`、`rockchip-es8316`。

TRM 的「I2S5/I2S6 只用 TX、I2S7/9/10 只用 RX」（§22.4.4）和板上的接法一致 ✅。

## 1. I2S（ch22）— 播放靜音，讀時脈暫存器

`i2s_probe.sh`：對 es8316（`hw:4,0`）播 `/dev/zero`（全 0 = 靜音，不會出聲），播放中讀 I2S0（`fe470000`）的暫存器。
此時驅動在跑、時脈全開，`safe_mmio.py` 放行。

### 48 kHz / 16-bit / 雙聲道

```
mclk_i2s0_8ch_tx = 12287999 Hz      clk_i2s0_8ch_tx_src = 393215996 Hz
fe470000: 7200000f 01c80017 10003f3f 00000017      TXCR RXCR CKR TXFIFOLR
fe470030: 00003eff 00003eff 00000303 20150001      ...  CLKDIV(0x38) VERSION(0x3c)
```

| 暫存器 | 值 | TRM 解讀 | 驗算 |
|--------|----|---------|------|
| `TXCR` | `7200000f` | 重置值就是 0x7200000F；VDW[4:0]=0xf → 16-bit | ✅ 和 `-f S16_LE` 一致 |
| `CKR` | `10003f3f` | LRCK_CTRL=01、MSS=0（master，SoC 出 sclk）、TSD=RSD=0x3f | sclk = ((0x3f>>1)+1)×2×fs = **64 fs** = 3.072 MHz |
| `CLKDIV` | `00000303` | TX/RX MCLK 分頻 = 3+1 = 4 | MCLK/sclk = 12.288/3.072 = **4** ✅ |
| MCLK | 12.288 MHz | — | = **256 fs**，和 DT `rockchip,mclk-fs = <256>` 一致 ✅ |

時脈來源：`clk_i2s0_8ch_tx_src` = 393.216 MHz = **AUPLL 786.432 MHz ÷ 2**（AUPLL 見 A §2），
再經小數分頻器（`_frac`）÷32 → 12.288 MHz。

### 換取樣率時誰在變？

| 取樣率 | MCLK | CKR | CLKDIV |
|-------|------|-----|--------|
| 48 kHz | 12.288 MHz | `10003f3f` | `00000303` |
| 16 kHz | 4.096 MHz（= 256 × 16k） | `10003f3f` | `00000303` |

**I2S 內部分頻完全不變**，取樣率靠 CRU 的小數分頻器改 MCLK。I2S 只維持固定比例（MCLK = 4 × sclk = 256 fs）。

### es8316 不支援 44.1 kHz

```
$ aplay -D hw:4,0 -r 44100 ...
Warning: rate is not accurate (requested = 44100Hz, got = 48000Hz)
$ aplay -D hw:4,0 --dump-hw-params ...
FORMAT: S16_LE S24_LE    CHANNELS: 2    RATE: [16000 48000]
```

直接用硬體裝置時 44.1 kHz 會被改成 48 kHz。44.1 kHz 家族的音樂（例如 CD）必須經 ALSA `plug` 或 PulseAudio/PipeWire 重新取樣。
（原因推測：MCLK 源頭是 48k 家族的 AUPLL，而 codec 驅動只列出 48k 家族的取樣率；**未深究**。）

`VERSION`（+0x3c）= `20150001`。

## 2. SPDIF TX（ch23）

`spdif-tx@fddb0000`（TRM 名稱 SPDIF_TX2）是 DP0 的音訊來源（ALSA 卡 `rockchip-hdmi2`，DAI 是 `spdif-hifi`），
DMA 走 dmac1 請求 6（和 TRM Table 1-4 一致，見 C §1）。沒接 DP 螢幕，沒做播放實驗。

## 3. SPDIF RX（ch24）、Digital Audio Codec（ch27）、VAD（ch28）、PDM（ch39）

DT 全部 disabled：
- ROCK 5B 沒有 SPDIF 輸入接頭
- 音訊 codec 用外接的 es8316（I2C7 `0x11`），SoC 內建的 ACDCDIG 沒用
- 沒有數位麥克風，所以 PDM、VAD（語音喚醒）都沒用

無法做實驗。
