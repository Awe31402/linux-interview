// vruntime_place.c —— 第 8 章 Q2/Q5/Q6（min_vruntime、新建/喚醒行程的 vruntime）
//
// 觀察 place_entity() 的兩條路徑：
//   * 新建行程（initial=1，START_DEBIT）： vruntime = min_vruntime + sched_vslice()
//   * 喚醒行程（initial=0，GENTLE_FAIR_SLEEPERS）：
//         vruntime = max(自己的 vruntime, min_vruntime - sysctl_sched_latency/2)
//     -> 睡再久也最多只能「欠」半個調度週期，不會餓死別人
//
// 需要 root（要讀 /sys/kernel/debug/sched/debug 拿 cfs_rq 的 min_vruntime）。
// 用法： sudo ./vruntime_place <cpu>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

static int g_cpu;

static double my_vruntime(void)
{
	char line[256];
	double v = -1;
	FILE *f = fopen("/proc/self/sched", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "se.vruntime", 11)) {
			char *c = strchr(line, ':');
			if (c) v = atof(c + 1);
			break;
		}
	}
	fclose(f);
	return v;
}

/* 從 /sys/kernel/debug/sched/debug 取 cpu 的 root cfs_rq min_vruntime（ms） */
static double rq_min_vruntime(int cpu)
{
	char cmd[256], buf[128];
	FILE *p;
	double v = -1;

	snprintf(cmd, sizeof(cmd),
		 "awk '/^cfs_rq\\[%d\\]:\\/$/{f=1} f&&/\\.min_vruntime/{print $3; exit}' "
		 "/sys/kernel/debug/sched/debug", cpu);
	p = popen(cmd, "r");
	if (!p)
		return -1;
	if (fgets(buf, sizeof(buf), p))
		v = atof(buf);
	pclose(p);
	return v;
}

static void pin(int cpu)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	sched_setaffinity(0, sizeof(s), &s);
}

static void burn_ms(int ms)
{
	struct timespec a, b;
	volatile unsigned long x = 0;
	clock_gettime(CLOCK_MONOTONIC, &a);
	do {
		int i;
		for (i = 0; i < 5000; i++) x++;
		clock_gettime(CLOCK_MONOTONIC, &b);
	} while ((b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6 < ms);
}

int main(int argc, char **argv)
{
	pid_t bg[3], child;
	int i;
	double vmin, vme, vchild;

	setvbuf(stdout, NULL, _IONBF, 0);	/* fork 前後都要立刻輸出 */
	g_cpu = argc > 1 ? atoi(argv[1]) : 3;
	pin(g_cpu);

	/* 先在同一顆 CPU 上放 3 個 busy 行程，把 min_vruntime 推起來 */
	for (i = 0; i < 3; i++) {
		bg[i] = fork();
		if (bg[i] == 0) {
			volatile unsigned long x = 0;
			pin(g_cpu);
			for (;;) x++;
		}
	}
	sleep(3);

	printf("== CPU%d 上已經有 3 個 busy 行程 ==\n", g_cpu);
	burn_ms(50);
	vmin = rq_min_vruntime(g_cpu);
	vme  = my_vruntime();
	printf("父行程(pid=%d) 自己的 se.vruntime = %.6f ms\n", getpid(), vme);
	printf("cfs_rq[%d]:/ 的 min_vruntime      = %.6f ms\n", g_cpu, vmin);

	/* ---- (1) 新建行程：fork() 前後不做任何耗時動作，量測才準 ---- */
	vme = my_vruntime();
	child = fork();
	if (child == 0) {
		pin(g_cpu);
		printf("[新建子行程 pid=%d] 一出生的 se.vruntime = %.6f ms\n",
		       getpid(), my_vruntime());
		_exit(0);
	}
	waitpid(child, NULL, 0);
	vchild = my_vruntime();
	printf("   （fork 瞬間父行程 vruntime = %.6f，fork 回來後 = %.6f）\n", vme, vchild);
	printf("   -> place_entity(initial=1)：vruntime = min_vruntime + sched_vslice()\n");
	printf("      （START_DEBIT 特性：新行程先「欠」一個時間片，避免 fork 炸彈）\n");

	/* ---- (2) 睡很久之後被喚醒 ---- */
	{
		double before, after, min_before, min_after;

		burn_ms(50);
		before     = my_vruntime();
		min_before = rq_min_vruntime(g_cpu);
		printf("\n== 睡 2 秒前 ==\n");
		printf("   自己 vruntime = %.6f ms, cfs_rq min_vruntime = %.6f ms\n",
		       before, min_before);
		sleep(2);
		burn_ms(1);			/* 醒來、被排到，才更新 vruntime */
		after     = my_vruntime();
		min_after = rq_min_vruntime(g_cpu);
		printf("== 睡 2 秒後被喚醒 ==\n");
		printf("   自己 vruntime = %.6f ms, cfs_rq min_vruntime = %.6f ms\n",
		       after, min_after);
		printf("   睡眠期間 min_vruntime 前進了 %.3f ms，我的 vruntime 前進了 %.3f ms\n",
		       min_after - min_before, after - before);
		printf("   -> 我睡了 2 秒幾乎沒跑，vruntime 卻自己往前跳了 %.3f ms，\n"
		       "      這就是 place_entity(initial=0) 做的補償。\n",
		       after - before);
		printf("   -> 殘留落後量 = Δmin - Δ我 = %.3f ms"
		       "（GENTLE_FAIR_SLEEPERS 上限 sysctl_sched_latency/2 = 12ms）\n",
		       (min_after - min_before) - (after - before));
	}

	for (i = 0; i < 3; i++)
		kill(bg[i], SIGKILL);
	while (wait(NULL) > 0)
		;
	(void)vchild;
	return 0;
}
