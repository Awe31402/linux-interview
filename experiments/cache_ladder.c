/* Ch3 Q2: 量測 CPU 各級儲存的存取延遲差異（pointer chasing / 指標追逐）
 *
 * 用「隨機打亂的環狀指標鏈」走訪，每次載入都相依於前一次載入的結果，
 * 因此硬體預取器完全失效，測到的就是該層 cache 的 load-to-use 延遲。
 * 工作集由小到大掃過去，就會看到 L1 -> L2 -> L3 -> DRAM 的階梯。
 *
 *   taskset -c 4 ./cache_ladder     # A76 大核 (L1d 64K, L2 512K, L3 3M)
 *   taskset -c 0 ./cache_ladder     # A55 小核 (L1d 32K, L2 128K, L3 3M)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LINE   64        /* 本機 CTR_EL0.DminLine = 4 -> 64 bytes */
#define STRIDE LINE

/* 全域 volatile sink：確保編譯器無法把整條相依鏈最佳化掉 */
volatile void *g_sink;

static double now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

static double measure(size_t sz, unsigned long long reps)
{
	size_t n = sz / STRIDE, i;
	char *buf = aligned_alloc(4096, sz);
	size_t *idx = malloc(n * sizeof(size_t));
	void *cur;
	double t0, t1;

	memset(buf, 0, sz);
	for (i = 0; i < n; i++) idx[i] = i;
	for (i = n - 1; i > 0; i--) {           /* Fisher-Yates */
		size_t j = (size_t)rand() % (i + 1), t = idx[i];
		idx[i] = idx[j]; idx[j] = t;
	}
	for (i = 0; i < n; i++)                 /* 串成一個環 */
		*(void **)(buf + idx[i] * STRIDE) = buf + idx[(i + 1) % n] * STRIDE;

	cur = buf;
	for (i = 0; i < n * 4; i++) cur = *(void **)cur;   /* 暖身 + 填 TLB */

	t0 = now_ns();
	for (unsigned long long r = 0; r < reps; r++)
		cur = *(void **)cur;
	t1 = now_ns();

	g_sink = cur;                            /* 讓相依鏈有副作用 */
	free(buf); free(idx);
	return (t1 - t0) / (double)reps;
}

int main(void)
{
	static const size_t sizes[] = {
		4UL<<10, 8UL<<10, 16UL<<10, 24UL<<10, 32UL<<10, 48UL<<10, 64UL<<10,
		96UL<<10, 128UL<<10, 192UL<<10, 256UL<<10, 384UL<<10, 512UL<<10,
		768UL<<10, 1UL<<20, 2UL<<20, 3UL<<20, 4UL<<20, 6UL<<20, 8UL<<20,
		16UL<<20, 32UL<<20, 64UL<<20, 128UL<<20 };
	int k, nk = sizeof(sizes)/sizeof(sizes[0]);
	char label[16];

	printf("每次存取都相依於上一次（pointer chase），stride = %d bytes\n", STRIDE);
	printf("%-10s %12s   %s\n", "工作集", "ns / access", "延遲階梯");
	for (k = 0; k < nk; k++) {
		unsigned long long reps = sizes[k] <= (4UL<<20) ? 30000000ULL : 8000000ULL;
		double ns = measure(sizes[k], reps);
		int bar = (int)(ns * 2.0); if (bar > 60) bar = 60;

		if (sizes[k] >= (1UL<<20)) snprintf(label, sizeof label, "%zu MB", sizes[k]>>20);
		else                       snprintf(label, sizeof label, "%zu KB", sizes[k]>>10);
		printf("%-10s %12.2f   ", label, ns);
		while (bar--) putchar('#');
		putchar('\n');
		fflush(stdout);
	}
	return 0;
}
