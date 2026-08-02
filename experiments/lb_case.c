// lb_case.c —— 第 8 章 Q25/Q26/Q27 + 第 9 章 Q5 的負載均衡實驗
//
// 模擬書上 §9.2 的場景：先把 N 個 CPU-bound 行程全部塞到 CPU0，
// 再把它們的 affinity 放寬到 <mask> 指定的 CPU 集合，
// 觀察 SMP 負載均衡（run_rebalance_domains -> load_balance）多久、
// 以什麼方式把它們攤平。
//
// 用法： ./lb_case <nr_task> <cpumask_hex> <seconds>
// 例：   ./lb_case 5 3 5        # 5 個行程，只允許跑在 CPU0/CPU1 上，觀察 5 秒
//        ./lb_case 8 ff 5       # 8 個行程，8 顆 CPU 都可以跑
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>

#define MAXT 64
#define NCPU 8

static int task_cpu(pid_t pid)
{
	char path[64], buf[2048], *p;
	int fd, n, i, cpu = -1;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strrchr(buf, ')');
	if (!p)
		return -1;
	p += 2;
	for (i = 3; i <= 39; i++) {	/* field 39 = processor */
		if (i == 39) { cpu = atoi(p); break; }
		p = strchr(p, ' ');
		if (!p) return -1;
		p++;
	}
	return cpu;
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int n, secs, i, step;
	unsigned long mask;
	pid_t pid[MAXT];
	cpu_set_t s0, sw;
	double t0;

	if (argc != 4) {
		fprintf(stderr, "usage: %s <nr_task> <cpumask_hex> <seconds>\n", argv[0]);
		return 1;
	}
	n    = atoi(argv[1]);
	mask = strtoul(argv[2], NULL, 16);
	secs = atoi(argv[3]);
	if (n > MAXT) n = MAXT;

	CPU_ZERO(&s0);
	CPU_SET(0, &s0);			/* 全部先塞到 CPU0 */
	CPU_ZERO(&sw);
	for (i = 0; i < NCPU; i++)
		if (mask & (1UL << i))
			CPU_SET(i, &sw);

	for (i = 0; i < n; i++) {
		pid[i] = fork();
		if (pid[i] == 0) {
			volatile unsigned long x = 0;
			sched_setaffinity(0, sizeof(s0), &s0);
			for (;;)
				x++;
		}
	}
	usleep(300000);
	printf("t=0.00s  全部釘在 CPU0：");
	for (i = 0; i < n; i++)
		printf(" p%d@c%d", i, task_cpu(pid[i]));
	printf("\n");

	/* 放寬 affinity —— 此刻起負載均衡才有事可做 */
	for (i = 0; i < n; i++)
		sched_setaffinity(pid[i], sizeof(sw), &sw);
	t0 = now_s();

	for (step = 0; step < secs * 10; step++) {
		int hist[NCPU];
		double t;

		usleep(100000);
		t = now_s() - t0;
		memset(hist, 0, sizeof(hist));
		for (i = 0; i < n; i++) {
			int c = task_cpu(pid[i]);
			if (c >= 0 && c < NCPU)
				hist[c]++;
		}
		if (step < 20 || step % 5 == 0) {
			printf("t=%5.2fs  nr_running/CPU:", t);
			for (i = 0; i < NCPU; i++)
				if (mask & (1UL << i))
					printf(" c%d=%d", i, hist[i]);
			printf("\n");
		}
	}
	for (i = 0; i < n; i++)
		kill(pid[i], SIGKILL);
	while (wait(NULL) > 0)
		;
	return 0;
}
