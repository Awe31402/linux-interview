/* Ch2 Q17/Q18: ARMv8 是 weakly-ordered —— 用 message-passing litmus test
 * 觀察「沒有屏障就會看到亂序」，以及 Store-Release / Load-Acquire 如何修好它。
 *
 *   T1 (writer):  data = 42;   flag = 1;
 *   T2 (reader):  while(!flag);  r = data;
 *
 * 在 x86 (TSO) 上 r 永遠是 42；在 ARM64 上，若不加屏障，T2 可能看到
 * flag==1 但 data==0（store-store 或 load-load 被重排）。
 *
 *   ./barrier_mp relaxed   # 無屏障 -> 會抓到亂序
 *   ./barrier_mp acqrel    # STLR/LDAR -> 0 次
 *   ./barrier_mp dmb       # dmb ish  -> 0 次
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

static volatile int data_v, flag_v;
static int mode;                 /* 0 relaxed, 1 acqrel, 2 dmb */
static long iters = 2000000;
static long violations, observed;

#define DMB_ISH() __asm__ __volatile__("dmb ish" ::: "memory")

static void pin(int cpu)
{
	cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
	pthread_setaffinity_np(pthread_self(), sizeof s, &s);
}

static void *writer(void *arg)
{
	long i;
	pin((int)(long)arg);
	for (i = 0; i < iters; i++) {
		while (__atomic_load_n(&flag_v, __ATOMIC_RELAXED) != 0)
			;                                  /* 等 reader 收完上一輪 */
		__atomic_store_n(&data_v, 42, __ATOMIC_RELAXED);
		if (mode == 2) DMB_ISH();
		if (mode == 1)
			__atomic_store_n(&flag_v, 1, __ATOMIC_RELEASE);   /* STLR */
		else
			__atomic_store_n(&flag_v, 1, __ATOMIC_RELAXED);   /* STR  */
	}
	return NULL;
}

static void *reader(void *arg)
{
	long i;
	pin((int)(long)arg);
	for (i = 0; i < iters; i++) {
		int f, d;
		do {
			f = (mode == 1) ? __atomic_load_n(&flag_v, __ATOMIC_ACQUIRE)
					: __atomic_load_n(&flag_v, __ATOMIC_RELAXED);
		} while (!f);
		if (mode == 2) DMB_ISH();
		d = __atomic_load_n(&data_v, __ATOMIC_RELAXED);
		observed++;
		if (d != 42) violations++;                 /* 看到 flag=1 卻沒看到 data! */
		__atomic_store_n(&data_v, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&flag_v, 0, __ATOMIC_RELEASE);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t t1, t2;
	const char *m = argc > 1 ? argv[1] : "relaxed";
	int cpu_w = argc > 2 ? atoi(argv[2]) : 0;   /* 預設跨 cluster：A55 <-> A76 */
	int cpu_r = argc > 3 ? atoi(argv[3]) : 4;

	if (!strcmp(m, "acqrel")) mode = 1;
	else if (!strcmp(m, "dmb")) mode = 2;

	printf("mode=%-8s writer=cpu%d reader=cpu%d  iters=%ld\n", m, cpu_w, cpu_r, iters);
	pthread_create(&t1, NULL, writer, (void *)(long)cpu_w);
	pthread_create(&t2, NULL, reader, (void *)(long)cpu_r);
	pthread_join(t1, NULL); pthread_join(t2, NULL);
	printf("  觀察 %ld 次，其中 %ld 次看到 flag==1 但 data!=42  (亂序率 %.6f%%)\n",
	       observed, violations, 100.0 * violations / (observed ? observed : 1));
	return 0;
}
