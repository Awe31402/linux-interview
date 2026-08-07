// SPDX-License-Identifier: GPL-2.0
/*
 * flush_reload.c —— Q1「高速緩存側信道攻擊原理」的實機演示。
 *
 * 對應書上 §6.1（行 26-104）：熔斷/幽靈漏洞都利用「cache 命中 vs 未命中」
 * 的存取延遲差當側信道。本程式做兩件事：
 *
 *   (A) 直方圖：對同一條 cache line，分別在「已 flush」與「已載入」兩種狀態
 *       下量存取延遲，畫出兩個分開的峰，並自動找出判別門檻。
 *       —— 這就是書上圖 6.1 說的「存取時間短=命中、長=未命中」。
 *
 *   (B) 隱蔽通道 (covert channel)：用 Flush+Reload 把一個祕密位元組
 *       的 8 個位元逐一「透過 cache 狀態」傳出來，證明側信道能傳資料。
 *       user_probe[] 就是書上偽代碼裡「攻擊者可安全存取的陣列」(行 53)，
 *       每個候選值錯開一個 page(0x1000) 以躲開硬體預取器。
 *
 * 需要先 `sudo insmod pmu_user.ko`（拿週期級時鐘）。
 *   gcc -O2 -o flush_reload flush_reload.c
 *   taskset -c 4 ./flush_reload      # 綁 A76 大核
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sidechannel.h"

/* 步長 4160=0x1040：既錯開 page 躲預取器，又讓 (slot*4160)>>6 對 256 取模
 * 成為排列，使每個 slot 落在不同的 L1 cache set（A76 L1d 只有 4-way，
 * 若用 4096 步長全部撞進 set 0，掃描時互相驅逐 -> 側信道失真）。 */
#define STRIDE   4160
#define NPROBE   256           /* 一個位元組有 256 種可能 */
static uint8_t user_probe[NPROBE * STRIDE] __attribute__((aligned(4096)));

#define HIST_MAX 400
static unsigned hit_hist[HIST_MAX], miss_hist[HIST_MAX];

static void hist_bump(unsigned *h, uint64_t c)
{
	if (c >= HIST_MAX) c = HIST_MAX - 1;
	h[c]++;
}

/* (A) 量 cache 命中/未命中的延遲分佈，回傳自動門檻 */
static uint64_t calibrate(void)
{
	volatile uint8_t *slot = &user_probe[64 * STRIDE];
	uint64_t hit_sum = 0, miss_sum = 0;
	int i, N = 200000;

	for (i = 0; i < N; i++) {
		/* 未命中：先 flush 再量 */
		flush((void *)slot); mfence();
		miss_sum += probe_time((void *)slot);

		/* 命中：先載入再量 */
		maccess((void *)slot); mfence();
		hit_sum += probe_time((void *)slot);
	}
	/* 再跑一輪填直方圖 */
	for (i = 0; i < N; i++) {
		flush((void *)slot); mfence();
		hist_bump(miss_hist, probe_time((void *)slot));
		maccess((void *)slot); mfence();
		hist_bump(hit_hist, probe_time((void *)slot));
	}

	double hit_avg = (double)hit_sum / N, miss_avg = (double)miss_sum / N;
	printf("[cache 延遲量測 @ %d 次取樣]\n", N);
	printf("  命中(cached)   平均 = %.1f 週期\n", hit_avg);
	printf("  未命中(flushed)平均 = %.1f 週期\n", miss_avg);
	printf("  差異 = %.1f 週期  <-- 這就是側信道能利用的時間差\n",
	       miss_avg - hit_avg);
	return (uint64_t)((hit_avg + miss_avg) / 2);   /* 取中點當門檻 */
}

static void dump_hist(const char *name, unsigned *h)
{
	int i, lo = -1, hi = 0;
	unsigned peak = 0;

	for (i = 0; i < HIST_MAX; i++) {
		if (h[i]) { if (lo < 0) lo = i; hi = i; }
		if (h[i] > peak) peak = h[i];
	}
	printf("  %-14s 範圍 [%d..%d] 週期，峰值 bar：\n", name, lo, hi);
	for (i = lo; i <= hi && i < HIST_MAX; i++) {
		if (!h[i]) continue;
		int bars = peak ? (int)(50.0 * h[i] / peak) : 0;
		if (bars == 0 && h[i]) bars = 1;
		printf("   %3d cyc |", i);
		while (bars--) putchar('#');
		printf(" %u\n", h[i]);
	}
}

/* Flush+Reload 掃 user_probe[]，回傳延遲最短（=剛被載入）的那個索引 */
static int reload_scan(uint64_t threshold)
{
	int best = -1, guess, i;
	uint64_t best_t = ~0ULL;

	for (i = 0; i < NPROBE; i++) {
		/* 打亂掃描順序，避免 stride 預取器沿線預取 */
		guess = ((i * 167) + 13) & 255;
		volatile uint8_t *slot = &user_probe[guess * STRIDE];
		uint64_t t = probe_time((void *)slot);
		if (t < threshold && t < best_t) { best_t = t; best = guess; }
	}
	return best;
}

/* (B) 隱蔽通道：把 secret 這個位元組經由 cache 狀態傳出來
 *     完全按書上偽代碼（行 53-78）：sender 以「祕密值」當索引把
 *     user_probe[secret] 拉進 cache，receiver 掃出哪個 index 變快就還原。 */
static void covert_channel(uint64_t threshold)
{
	const char *msg = "benshushu";   /* 要偷偷傳出去的祕密字串 */
	int ok = 0, tot = 0;

	printf("\n[隱蔽通道] 用 Flush+Reload 傳字串 \"%s\"（門檻 %lu 週期）\n",
	       msg, threshold);
	printf("  祕密位元組 -> 側信道還原：");
	for (const char *s = msg; *s; s++) {
		uint8_t secret = (uint8_t)*s;
		static int hits[NPROBE];
		int i, best = 0;

		memset(hits, 0, sizeof(hits));
		for (i = 0; i < 100; i++) {                 /* 多輪投票抗雜訊 */
			int j;
			for (j = 0; j < NPROBE; j++)
				flush(&user_probe[j * STRIDE]);
			mfence();
			maccess(&user_probe[secret * STRIDE]);  /* sender：索引=祕密值 */
			mfence();
			/* receiver：亂序量每個 slot，命中門檻就記一票 */
			for (j = 0; j < NPROBE; j++) {
				int slot = ((j * 167) + 13) & 255;
				if (probe_time(&user_probe[slot * STRIDE]) < threshold)
					hits[slot]++;
			}
		}
		for (i = 0; i < NPROBE; i++)
			if (hits[i] > hits[best]) best = i;

		tot++; ok += (best == secret);
		putchar((best >= 32 && best < 127) ? best : '?');
	}
	printf("\n  正確還原 %d/%d 個位元組\n", ok, tot);
}

int main(void)
{
	int src = sc_timer_init();
	if (src < 0) {
		fprintf(stderr, "錯誤：拿不到週期計數器。請 `sudo sysctl kernel.perf_user_access=1`"
			"（或 `sudo insmod pmu_user.ko`）\n");
		return 1;
	}
	printf("時鐘來源：%s\n", src == 0 ? "perf mmap (PMCCNTR_EL0)" : "raw mrs (pmu_user.ko)");
	memset(user_probe, 1, sizeof(user_probe));   /* 先 fault-in 全部頁 */

	uint64_t th = calibrate();
	printf("\n[延遲直方圖]\n");
	dump_hist("命中(cached)", hit_hist);
	dump_hist("未命中(flush)", miss_hist);
	printf("  自動判別門檻 = %lu 週期\n", th);

	covert_channel(th);
	return 0;
}
