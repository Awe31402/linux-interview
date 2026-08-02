// wake_cpu.c —— 第 8 章 Q28（唤醒进程应该在哪个 CPU 上运行）實驗
//
// 建立 1 個 waker + N 個 wakee，用 pipe 做 ping-pong 唤醒。
// 統計每一輪唤醒之後 wakee 實際落在哪顆 CPU 上，以驗證：
//   * N=1  ：wake_affine 生效 -> wakee 被拉到 waker 的 CPU（或同 LLC 的閒置 CPU）
//   * N 大 ：wake_wide() 回傳 true -> 放棄 wake affine，wakee 被分散開
//
// 用法： ./wake_cpu <nr_wakee> <rounds> [waker_cpu]
//        waker_cpu 指定時把 waker 釘在該 CPU 上。
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#define MAXW 64
#define NCPU 8

static void pin(int cpu)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	sched_setaffinity(0, sizeof(s), &s);
}

/* 燒掉一點時間，讓 waker 看起來是「正在跑」的行程 */
static void burn_us(long us)
{
	struct timespec a, b;
	volatile unsigned long x = 0;
	clock_gettime(CLOCK_MONOTONIC, &a);
	do {
		int i;
		for (i = 0; i < 2000; i++) x++;
		clock_gettime(CLOCK_MONOTONIC, &b);
	} while ((b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000 < us);
}

int main(int argc, char **argv)
{
	int n, rounds, waker_cpu = -1, i, r;
	int to[MAXW][2], back[MAXW][2];
	pid_t pid[MAXW];
	long hist[MAXW][NCPU];
	long waker_hist[NCPU];
	char c = 'x';

	if (argc < 3) {
		fprintf(stderr, "usage: %s <nr_wakee> <rounds> [waker_cpu]\n", argv[0]);
		return 1;
	}
	n = atoi(argv[1]);
	rounds = atoi(argv[2]);
	if (argc > 3) waker_cpu = atoi(argv[3]);
	if (n > MAXW) n = MAXW;
	memset(hist, 0, sizeof(hist));
	memset(waker_hist, 0, sizeof(waker_hist));

	for (i = 0; i < n; i++) {
		if (pipe(to[i]) || pipe(back[i])) { perror("pipe"); return 1; }
		pid[i] = fork();
		if (pid[i] == 0) {
			char buf[8];
			close(to[i][1]); close(back[i][0]);
			for (;;) {
				if (read(to[i][0], buf, 1) != 1) _exit(0);
				buf[0] = (char)sched_getcpu();	/* 回報自己被唤醒後在哪 */
				write(back[i][1], buf, 1);
			}
		}
		close(to[i][0]); close(back[i][1]);
	}

	if (waker_cpu >= 0)
		pin(waker_cpu);

	for (r = 0; r < rounds; r++) {
		for (i = 0; i < n; i++) {
			char cpu;
			burn_us(200);			/* waker 正在做事 */
			waker_hist[sched_getcpu() % NCPU]++;
			write(to[i][1], &c, 1);		/* 唤醒 wakee i */
			if (read(back[i][0], &cpu, 1) != 1)
				goto done;
			if ((unsigned char)cpu < NCPU)
				hist[i][(unsigned char)cpu]++;
		}
	}
done:
	for (i = 0; i < n; i++) { close(to[i][1]); kill(pid[i], SIGKILL); }
	while (wait(NULL) > 0)
		;

	printf("waker pid=%d  wakee=%d  rounds=%d  waker_cpu=%s\n",
	       getpid(), n, rounds, waker_cpu >= 0 ? argv[3] : "free");
	printf("waker  所在 CPU 分布 :");
	for (i = 0; i < NCPU; i++) printf(" c%d=%-6ld", i, waker_hist[i]);
	printf("\n");
	for (i = 0; i < n && i < 8; i++) {
		int j;
		printf("wakee%-2d 被唤醒後 CPU:", i);
		for (j = 0; j < NCPU; j++) printf(" c%d=%-6ld", j, hist[i][j]);
		printf("\n");
	}
	if (n > 8) {
		long agg[NCPU];
		int j;
		memset(agg, 0, sizeof(agg));
		for (i = 0; i < n; i++)
			for (j = 0; j < NCPU; j++) agg[j] += hist[i][j];
		printf("全部 wakee 合計    :");
		for (j = 0; j < NCPU; j++) printf(" c%d=%-6ld", j, agg[j]);
		printf("\n");
	}
	return 0;
}
