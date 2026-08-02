// rt_latency.c —— 第 8 章 Q36~Q40（硬/軟實時、實時延時）實驗
//
// 迷你版 cyclictest：用 clock_nanosleep(TIMER_ABSTIME) 週期性喚醒自己，
// 量「應該被喚醒的時刻」到「真的跑起來的時刻」之間的延時。
// 這段延時 = 中斷延時 + 中斷處理延時 + 調度延時 + 上下文切換延時。
//
// 用法： ./rt_latency <period_us> <loops> [fifo_prio] [cpu]
// 例：   ./rt_latency 1000 20000            # SCHED_OTHER，1ms 週期
//        ./rt_latency 1000 20000 80         # SCHED_FIFO prio 80
//        ./rt_latency 1000 20000 80 3       # 再釘在 CPU3
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#define NBUCKET 24

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

static long long ts_diff_ns(struct timespec *a, struct timespec *b)
{
	return (long long)(b->tv_sec - a->tv_sec) * 1000000000LL +
	       (b->tv_nsec - a->tv_nsec);
}

int main(int argc, char **argv)
{
	long period_us, loops, i;
	int prio = 0, cpu = -1;
	struct timespec next, now;
	long long min = 1LL << 60, max = 0, sum = 0;
	long hist[NBUCKET];
	long long *samples;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <period_us> <loops> [fifo_prio] [cpu]\n", argv[0]);
		return 1;
	}
	period_us = atol(argv[1]);
	loops     = atol(argv[2]);
	if (argc > 3) prio = atoi(argv[3]);
	if (argc > 4) cpu  = atoi(argv[4]);

	memset(hist, 0, sizeof(hist));
	samples = calloc(loops, sizeof(*samples));

	if (cpu >= 0) {
		cpu_set_t s;
		CPU_ZERO(&s);
		CPU_SET(cpu, &s);
		sched_setaffinity(0, sizeof(s), &s);
	}
	/* 預設 timer slack 是 50us，會直接加在 nanosleep 的喚醒時間上。
	 * 實時行程（task_is_realtime()）核心會自動忽略 slack，這裡對
	 * SCHED_OTHER 也手動清掉，兩者才有可比性。 */
	prctl(PR_SET_TIMERSLACK, 1UL);

	if (prio > 0) {
		struct sched_param sp = { .sched_priority = prio };
		if (sched_setscheduler(0, SCHED_FIFO, &sp))
			perror("sched_setscheduler(SCHED_FIFO)");
		mlockall(MCL_CURRENT | MCL_FUTURE);
	}

	clock_gettime(CLOCK_MONOTONIC, &next);
	for (i = 0; i < loops; i++) {
		long long lat;

		next.tv_nsec += period_us * 1000;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
		clock_gettime(CLOCK_MONOTONIC, &now);
		lat = ts_diff_ns(&next, &now);
		if (lat < 0)
			lat = 0;
		samples[i] = lat;
		if (lat < min) min = lat;
		if (lat > max) max = lat;
		sum += lat;
		{
			int b = 0;
			long long v = lat / 1000;	/* us */
			while (v && b < NBUCKET - 1) { v >>= 1; b++; }
			hist[b]++;
		}
	}

	/* 取百分位 */
	{
		long long *s = samples;
		long j, k;
		qsort(s, loops, sizeof(*s), cmp_ll);
		printf("policy=%s prio=%d cpu=%s period=%ldus loops=%ld\n",
		       prio > 0 ? "SCHED_FIFO" : "SCHED_OTHER", prio,
		       cpu >= 0 ? argv[4] : "any", period_us, loops);
		printf("  min=%.1fus  avg=%.1fus  p50=%.1fus  p99=%.1fus  p99.9=%.1fus  max=%.1fus\n",
		       min / 1000.0, (double)sum / loops / 1000.0,
		       s[loops / 2] / 1000.0, s[(long)(loops * 0.99)] / 1000.0,
		       s[(long)(loops * 0.999)] / 1000.0, max / 1000.0);
		printf("  直方圖(us): ");
		for (j = 0, k = 1; j < NBUCKET; j++, k <<= 1)
			if (hist[j])
				printf("[%ld-%ld)=%ld ", j ? k / 2 : 0, k, hist[j]);
		printf("\n");
	}
	return 0;
}
