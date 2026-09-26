/* membw.c — 單執行緒/多執行緒記憶體頻寬（memset 寫、讀加總、memcpy）＋指標追逐延遲。
 * 用法：./membw [執行緒數=1] [綁定起始 CPU=4] [緩衝區 MB=256] */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int nthr = 1, cpu0 = 4;
static size_t sz;
static pthread_barrier_t bar;
static double res[3][16];

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void *worker(void *arg)
{
	long id = (long)arg;
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu0 + id, &s);
	sched_setaffinity(0, sizeof(s), &s);
	char *a = aligned_alloc(4096, sz), *b = aligned_alloc(4096, sz);
	memset(a, 1, sz);
	memset(b, 2, sz);
	double t, best[3] = {1e9, 1e9, 1e9};
	for (int r = 0; r < 5; r++) {
		pthread_barrier_wait(&bar);
		t = now(); memset(a, r, sz); t = now() - t;
		if (t < best[0]) best[0] = t;
		pthread_barrier_wait(&bar);
		t = now();
		uint64_t acc = 0, *p = (uint64_t *)a;
		for (size_t i = 0; i < sz / 8; i += 4)
			acc += p[i] ^ p[i + 1] ^ p[i + 2] ^ p[i + 3];
		t = now() - t;
		if (acc == 42) puts("");
		if (t < best[1]) best[1] = t;
		pthread_barrier_wait(&bar);
		t = now(); memcpy(b, a, sz); t = now() - t;
		if (t < best[2]) best[2] = t;
	}
	for (int k = 0; k < 3; k++)
		res[k][id] = sz / best[k] / 1e9 * (k == 2 ? 2 : 1); /* memcpy 算讀+寫 */
	free(a); free(b);
	return NULL;
}

static double latency(void)
{
	size_t n = (size_t)256 << 20 >> 6; /* 256MB，每 64B 一個節點 */
	uint64_t *buf = aligned_alloc(4096, n * 64);
	size_t *perm = malloc(n * sizeof(size_t));
	for (size_t i = 0; i < n; i++) perm[i] = i;
	srand(1);
	for (size_t i = n - 1; i > 0; i--) {
		size_t j = ((size_t)rand() * RAND_MAX + rand()) % (i + 1), x = perm[i];
		perm[i] = perm[j]; perm[j] = x;
	}
	for (size_t i = 0; i < n; i++)
		buf[perm[i] * 8] = (uint64_t)&buf[perm[(i + 1) % n] * 8];
	uint64_t *p = &buf[perm[0] * 8];
	long steps = 20000000;
	double t = now();
	for (long i = 0; i < steps; i++) p = (uint64_t *)*p;
	t = now() - t;
	if (!p) puts("");
	free(buf); free(perm);
	return t / steps * 1e9;
}

int main(int argc, char **argv)
{
	if (argc > 1) nthr = atoi(argv[1]);
	if (argc > 2) cpu0 = atoi(argv[2]);
	sz = (size_t)(argc > 3 ? atoi(argv[3]) : 256) << 20;
	pthread_barrier_init(&bar, NULL, nthr);
	pthread_t th[16];
	for (long i = 0; i < nthr; i++) pthread_create(&th[i], NULL, worker, (void *)i);
	for (int i = 0; i < nthr; i++) pthread_join(th[i], NULL);
	double s[3] = {0};
	for (int k = 0; k < 3; k++)
		for (int i = 0; i < nthr; i++) s[k] += res[k][i];
	cpu_set_t c; CPU_ZERO(&c); CPU_SET(cpu0, &c); sched_setaffinity(0, sizeof(c), &c);
	printf("threads=%d write=%.2f read=%.2f copy=%.2f GB/s  lat=%.1f ns\n",
	       nthr, s[0], s[1], s[2], nthr == 1 ? latency() : 0.0);
	return 0;
}
