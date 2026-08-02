// sched_weight.c —— 第 8 章 Q1/Q3/Q4/Q7/Q8 實驗
//
// 在「同一顆 CPU」上放 N 個純 CPU-bound 的子行程，每個給不同的 nice 值，
// 觀察：
//   1. se.load.weight 是否等於 sched_prio_to_weight[nice+20] * 1024
//   2. 實際拿到的 CPU 時間比例是否等於 weight 的比例
//   3. Δse.vruntime / Δse.sum_exec_runtime 是否等於 1024/weight
//
// 用法： ./sched_weight <cpu> <seconds> <nice1> [nice2 ...]
// 例：   taskset -c 3 ./sched_weight 3 10 0 0 5
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>

#define MAXK 16

struct snap {
	double vruntime;	/* se.vruntime            (ms) */
	double sum_exec;	/* se.sum_exec_runtime    (ms) */
	long   weight;		/* se.load.weight              */
	long   load_avg;	/* se.avg.load_avg             */
	long   util_avg;	/* se.avg.util_avg             */
	long   runnable_avg;	/* se.avg.runnable_avg         */
	long   nr_sw;		/* nr_switches                 */
	long   nr_invol;	/* nr_involuntary_switches     */
};

static int read_sched(pid_t pid, struct snap *s)
{
	char path[64], line[256];
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%d/sched", pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	memset(s, 0, sizeof(*s));
	while (fgets(line, sizeof(line), f)) {
		char *c = strchr(line, ':');
		if (!c)
			continue;
		*c = 0;
		double v = atof(c + 1);
		if (!strncmp(line, "se.vruntime", 11))            s->vruntime = v;
		else if (!strncmp(line, "se.sum_exec_runtime", 19)) s->sum_exec = v;
		else if (!strncmp(line, "se.load.weight", 14))      s->weight = (long)v;
		else if (!strncmp(line, "se.avg.load_avg", 15))     s->load_avg = (long)v;
		else if (!strncmp(line, "se.avg.util_avg", 15))     s->util_avg = (long)v;
		else if (!strncmp(line, "se.avg.runnable_avg", 19)) s->runnable_avg = (long)v;
		else if (!strncmp(line, "nr_switches", 11))         s->nr_sw = (long)v;
		else if (!strncmp(line, "nr_involuntary_switches", 23)) s->nr_invol = (long)v;
	}
	fclose(f);
	return 0;
}

/* 從 /proc/pid/stat 取 utime+stime（單位：tick） */
static long read_cputime(pid_t pid)
{
	char path[64], buf[1024], *p;
	int fd, n, i;
	long ut = 0, st = 0;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = 0;
	p = strrchr(buf, ')');		/* comm 可能含空白，從 ')' 之後數 */
	if (!p)
		return -1;
	p += 2;
	for (i = 3; i <= 15; i++) {	/* field 14 = utime, 15 = stime */
		char *q = strchr(p, ' ');
		if (i == 14) ut = atol(p);
		if (i == 15) { st = atol(p); break; }
		if (!q) break;
		p = q + 1;
	}
	return ut + st;
}

static void burn(void)
{
	volatile unsigned long x = 0;
	for (;;)
		x++;
}

int main(int argc, char **argv)
{
	int cpu, secs, k, i;
	int nice_v[MAXK];
	pid_t pid[MAXK];
	struct snap a[MAXK], b[MAXK];
	long t0[MAXK], t1[MAXK], total = 0;
	cpu_set_t set;

	if (argc < 4) {
		fprintf(stderr, "usage: %s <cpu> <seconds> <nice1> [nice2 ...]\n", argv[0]);
		return 1;
	}
	cpu  = atoi(argv[1]);
	secs = atoi(argv[2]);
	k    = argc - 3;
	if (k > MAXK) k = MAXK;
	for (i = 0; i < k; i++)
		nice_v[i] = atoi(argv[i + 3]);

	for (i = 0; i < k; i++) {
		pid[i] = fork();
		if (pid[i] == 0) {
			CPU_ZERO(&set);
			CPU_SET(cpu, &set);
			sched_setaffinity(0, sizeof(set), &set);
			if (nice(nice_v[i]) == -1 && nice_v[i])
				perror("nice");
			burn();
			_exit(0);
		}
	}

	sleep(2);			/* 讓 PELT 先暖機 */
	for (i = 0; i < k; i++) {
		read_sched(pid[i], &a[i]);
		t0[i] = read_cputime(pid[i]);
	}
	sleep(secs);
	for (i = 0; i < k; i++) {
		read_sched(pid[i], &b[i]);
		t1[i] = read_cputime(pid[i]);
		total += t1[i] - t0[i];
	}
	for (i = 0; i < k; i++)
		kill(pid[i], SIGKILL);
	while (wait(NULL) > 0)
		;

	printf("CPU%d 上 %d 個 CPU-bound 行程，取樣 %d 秒\n", cpu, k, secs);
	printf("%-6s %-6s %-9s %-9s %-9s %-8s %-8s %-8s\n",
	       "pid", "nice", "weight", "CPU%", "Δvrt/Δex", "load_avg", "util_avg", "invol_sw");
	for (i = 0; i < k; i++) {
		double dex  = b[i].sum_exec - a[i].sum_exec;
		double dvrt = b[i].vruntime - a[i].vruntime;
		double share = total ? 100.0 * (t1[i] - t0[i]) / total : 0;
		printf("%-6d %-6d %-9ld %-9.2f %-9.4f %-8ld %-8ld %-8ld\n",
		       pid[i], nice_v[i], b[i].weight, share,
		       dex > 0 ? dvrt / dex : 0,
		       b[i].load_avg, b[i].util_avg,
		       b[i].nr_invol - a[i].nr_invol);
	}
	return 0;
}
