// lse_bench.c —— 卷2 第 1 章 Q1：LL/SC 與 LSE 原子指令的效能對比
//
// 同一份 C 程式碼編兩次：
//   gcc -O2 -march=armv8-a      -> 只能用 ldxr/stxr 的 LL/SC 迴圈
//   gcc -O2 -march=armv8.2-a+lse -> 可以用 ARMv8.1 的 LDADD/CAS/SWP
// 再用 N 條執行緒同時對「同一個」變數做原子加，量吞吐量。
// 爭用越激烈，LL/SC 因為 stxr 失敗要重試，差距越明顯。
//
// 用法：
//   gcc -O2 -pthread -march=armv8-a       -o lse_bench_llsc lse_bench.c
//   gcc -O2 -pthread -march=armv8.2-a+lse -o lse_bench_lse  lse_bench.c
//   ./lse_bench_llsc <nthread> <ms> [base_cpu]
//   ./lse_bench_lse  <nthread> <ms> [base_cpu]
// base_cpu 用來挑大核或小核：RK3588 的 CPU0~3 是 A55，CPU4~7 是 A76。
//
// 用 objdump -d 看 atomic_worker 就能確認實際產生的指令。
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <stdatomic.h>

static volatile int stop_flag;
static _Atomic long shared_counter;          /* 所有執行緒搶同一條 cache line */
static long per_thread_ops[64];

/* 每個執行緒獨立的計數器，用來對照「完全沒有爭用」的情況 */
static struct {
	_Atomic long v;
	char pad[64 - sizeof(_Atomic long)];     /* 撐開到獨立 cache line */
} priv[64] __attribute__((aligned(64)));

static int g_mode;                            /* 0 = 共享變數, 1 = 各自獨立 */
static int g_base_cpu;                        /* 綁定的起始 CPU */

struct arg { int id; int cpu; };

static void *atomic_worker(void *p)
{
	struct arg *a = p;
	long n = 0;
	cpu_set_t s;

	CPU_ZERO(&s);
	CPU_SET(a->cpu, &s);
	pthread_setaffinity_np(pthread_self(), sizeof(s), &s);

	if (g_mode == 0) {
		while (!stop_flag) {
			atomic_fetch_add_explicit(&shared_counter, 1, memory_order_relaxed);
			n++;
		}
	} else {
		_Atomic long *v = &priv[a->id].v;
		while (!stop_flag) {
			atomic_fetch_add_explicit(v, 1, memory_order_relaxed);
			n++;
		}
	}
	per_thread_ops[a->id] = n;
	return NULL;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void run(int nthr, int ms, int mode, const char *tag)
{
	pthread_t th[64];
	struct arg ar[64];
	double t0, t1;
	long total = 0;
	int i, ncpu = sysconf(_SC_NPROCESSORS_ONLN);

	g_mode = mode;
	(void)ncpu;
	stop_flag = 0;
	memset(per_thread_ops, 0, sizeof(per_thread_ops));
	atomic_store(&shared_counter, 0);

	t0 = now_ms();
	for (i = 0; i < nthr; i++) {
		ar[i].id = i;
		ar[i].cpu = g_base_cpu + (i % 4);
		pthread_create(&th[i], NULL, atomic_worker, &ar[i]);
	}
	usleep(ms * 1000);
	stop_flag = 1;
	for (i = 0; i < nthr; i++)
		pthread_join(th[i], NULL);
	t1 = now_ms();

	for (i = 0; i < nthr; i++)
		total += per_thread_ops[i];
	printf("  %-28s %2d 執行緒: %10ld 次 / %.0f ms = %8.2f M ops/s\n",
	       tag, nthr, total, t1 - t0, total / (t1 - t0) / 1000.0);
}

int main(int argc, char **argv)
{
	int nthr = argc > 1 ? atoi(argv[1]) : 4;
	int ms   = argc > 2 ? atoi(argv[2]) : 500;

	g_base_cpu = argc > 3 ? atoi(argv[3]) : 0;

	if (nthr > 64) nthr = 64;
	printf("編譯目標：%s\n",
#ifdef __ARM_FEATURE_ATOMICS
	       "有 LSE（__ARM_FEATURE_ATOMICS 已定義）-> 應該產生 ldadd/cas/swp"
#else
	       "無 LSE -> 應該產生 ldxr/stxr 的 LL/SC 迴圈"
#endif
	       );
	printf("線上 CPU 數 = %ld，綁定 CPU%d~CPU%d（%s）\n\n",
	       sysconf(_SC_NPROCESSORS_ONLN), g_base_cpu, g_base_cpu + 3,
	       g_base_cpu >= 4 ? "Cortex-A76 大核" : "Cortex-A55 小核");

	run(1,    ms, 0, "共享變數（無爭用）");
	run(nthr, ms, 0, "共享變數（激烈爭用）");
	run(nthr, ms, 1, "各自獨立變數（無爭用）");
	printf("\n  最後一列是對照組：每個執行緒改自己的 cache line，\n");
	printf("  它跟第二列的差距就是「cache line 顛簸」的代價。\n");
	return 0;
}
