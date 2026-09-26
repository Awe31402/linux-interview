/* heat.c — 每個執行緒綁一顆 CPU，跑 NEON FMA 迴圈把功耗拉滿。
 * 用法：./heat <秒數> [cpu 清單，預設全部]   例：./heat 120 4,5,6,7 */
#define _GNU_SOURCE
#include <arm_neon.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile int stop;
static double secs;
static unsigned long long iters[64];

static void *burn(void *arg)
{
	int cpu = (long)arg;
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
	float32x4_t a0 = vdupq_n_f32(1.0001f), a1 = a0, a2 = a0, a3 = a0,
		    a4 = a0, a5 = a0, a6 = a0, a7 = a0;
	const float32x4_t m = vdupq_n_f32(0.99999f), c = vdupq_n_f32(1e-5f);
	unsigned long long n = 0;
	while (!stop) {
		for (int i = 0; i < 4096; i++) {
			a0 = vfmaq_f32(a0, c, m); a1 = vfmaq_f32(a1, c, m);
			a2 = vfmaq_f32(a2, c, m); a3 = vfmaq_f32(a3, c, m);
			a4 = vfmaq_f32(a4, c, m); a5 = vfmaq_f32(a5, c, m);
			a6 = vfmaq_f32(a6, c, m); a7 = vfmaq_f32(a7, c, m);
		}
		n++;
	}
	float32x4_t s = vaddq_f32(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)),
				  vaddq_f32(vaddq_f32(a4, a5), vaddq_f32(a6, a7)));
	iters[cpu] = n + (vgetq_lane_f32(s, 0) == 12345.f);
	return NULL;
}

int main(int argc, char **argv)
{
	secs = argc > 1 ? atof(argv[1]) : 60;
	int cpus[64], nc = 0;
	if (argc > 2) {
		for (char *t = strtok(argv[2], ","); t; t = strtok(NULL, ","))
			cpus[nc++] = atoi(t);
	} else {
		for (int i = 0; i < sysconf(_SC_NPROCESSORS_ONLN); i++)
			cpus[nc++] = i;
	}
	pthread_t th[64];
	for (int i = 0; i < nc; i++)
		pthread_create(&th[i], NULL, burn, (void *)(long)cpus[i]);
	usleep(secs * 1e6);
	stop = 1;
	for (int i = 0; i < nc; i++)
		pthread_join(th[i], NULL);
	for (int i = 0; i < nc; i++)
		printf("cpu%d: %.2f GFLOPS\n", cpus[i], iters[cpus[i]] * 4096.0 * 8 * 4 * 2 / secs / 1e9);
	return 0;
}
