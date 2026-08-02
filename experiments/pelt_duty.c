// pelt_duty.c —— 第 8 章 Q15/Q16/Q17/Q18/Q19/Q21/Q22/Q23 實驗
//
// 產生一個「週期性」行程，兩種模式：
//   time <busy_ms> <period_ms>   固定「牆鐘忙碌時間」（duty cycle 固定）
//   work <Miter>   <period_ms>   固定「工作量」（迴圈次數固定），
//                                驗證 frequency/CPU invariant load tracking
//
// 每秒印出自己的 se.avg.{load_avg,runnable_avg,util_avg} 與 se.load.weight，
// 以及所在 CPU 的 capacity。
//
// 用法： ./pelt_duty <cpu> <secs> time <busy_ms> <period_ms>
//        ./pelt_duty <cpu> <secs> work <Miter>   <period_ms>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void show(const char *tag)
{
	char line[256];
	long load = 0, run = 0, util = 0, w = 0;
	double vrt = 0, ex = 0;
	FILE *f = fopen("/proc/self/sched", "r");

	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *c = strchr(line, ':');
		if (!c) continue;
		*c = 0;
		double v = atof(c + 1);
		if      (!strncmp(line, "se.avg.load_avg", 15))     load = (long)v;
		else if (!strncmp(line, "se.avg.runnable_avg", 19)) run  = (long)v;
		else if (!strncmp(line, "se.avg.util_avg", 15))     util = (long)v;
		else if (!strncmp(line, "se.load.weight", 14))      w    = (long)v;
		else if (!strncmp(line, "se.vruntime", 11))         vrt  = v;
		else if (!strncmp(line, "se.sum_exec_runtime", 19)) ex   = v;
	}
	fclose(f);
	printf("%-8s cpu=%d weight=%-8ld load_avg=%-6ld runnable_avg=%-6ld util_avg=%-6ld "
	       "vruntime=%.1f exec=%.1f\n",
	       tag, sched_getcpu(), w, load, run, util, vrt, ex);
	fflush(stdout);
}

static unsigned long spin_iters(unsigned long n)
{
	volatile unsigned long x = 0;
	unsigned long i;
	for (i = 0; i < n; i++)
		x += i;
	return x;
}

int main(int argc, char **argv)
{
	int cpu, secs, period, i, peak_sample = 0;
	const char *mode;
	double t_end, t_next, busy_ms = 0;
	unsigned long iters = 0;
	cpu_set_t set;
	char path[128], buf[32];
	FILE *f;

	if (argc == 7 && !strcmp(argv[6], "peak"))
		peak_sample = 1;
	else if (argc != 6) {
		fprintf(stderr,
			"usage: %s <cpu> <secs> time <busy_ms> <period_ms>\n"
			"       %s <cpu> <secs> work <Miter>   <period_ms>\n",
			argv[0], argv[0]);
		return 1;
	}
	cpu    = atoi(argv[1]);
	secs   = atoi(argv[2]);
	mode   = argv[3];
	period = atoi(argv[5]);
	if (!strcmp(mode, "time"))
		busy_ms = atof(argv[4]);
	else
		iters = (unsigned long)(atof(argv[4]) * 1000000.0);

	if (cpu >= 0) {			/* cpu = -1 表示不綁定，讓調度器自己挑 */
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set)) {
			perror("setaffinity");
			return 1;
		}
	}

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity",
		 cpu >= 0 ? cpu : sched_getcpu());
	buf[0] = 0;
	if ((f = fopen(path, "r"))) { if (fgets(buf, sizeof(buf), f)) {} fclose(f); }
	printf("pid=%d cpu=%d cpu_capacity=%s mode=%s arg=%s period=%dms\n",
	       getpid(), cpu, buf[0] ? strtok(buf, "\n") : "?", mode, argv[4], period);

	t_end  = now_ms() + secs * 1000.0;
	t_next = now_ms();
	i = 0;
	while (now_ms() < t_end) {
		double t0 = now_ms(), real_busy;

		if (!strcmp(mode, "time")) {
			while (now_ms() - t0 < busy_ms)
				spin_iters(20000);
		} else {
			spin_iters(iters);
		}
		real_busy = now_ms() - t0;
		if (peak_sample)
			show("PEAK");	/* 剛跑完 busy，PELT 鋸齒的波峰 */

		t_next += period;
		if (t_next > now_ms()) {
			struct timespec ts;
			double s = (t_next - now_ms()) / 1000.0;
			ts.tv_sec  = (time_t)s;
			ts.tv_nsec = (long)((s - ts.tv_sec) * 1e9);
			nanosleep(&ts, NULL);
		} else {
			t_next = now_ms();
		}
		if (peak_sample) {
			show("TROUGH");
		} else if (++i % (1000 / period ? 1000 / period : 1) == 0) {
			char tag[32];
			snprintf(tag, sizeof(tag), "busy=%.1fms", real_busy);
			show(tag);
		}
	}
	show("FINAL");
	return 0;
}
