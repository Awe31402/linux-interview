#!/bin/sh
# i2s_probe.sh — 用 es8316（card 4 / I2S0_8CH @ fe470000）播放「靜音」，
# 播放期間讀 I2S0 的 TXCR/CKR/CLKDIV 與 clk_summary 的 mclk，分別在 48 kHz 與 44.1 kHz。需 root。
for R in 48000 44100; do
	aplay -q -D hw:4,0 -r $R -f S16_LE -c 2 -d 4 /dev/zero &
	sleep 1.5
	echo "=== rate $R"
	grep -E "mclk_i2s0|i2s0_8ch|clk_i2s0" /sys/kernel/debug/clk/clk_summary | awk '{printf "  %-28s en=%s rate=%s\n",$1,$2,$5}'
	python3 /tmp/safe_mmio.py dump 0xfe470000 16
	wait
done
