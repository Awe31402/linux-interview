// SPDX-License-Identifier: GPL-2.0
/*
 * branch_pred.c —— Q6「分支預測的工作原理」的實機演示。
 *
 * 對應書上 §6.3.1（行 538-606）：分支預測器（BHT/BTB/RSB）猜對時流水線滿載，
 * 猜錯要「丟棄工作、重取指令」，代價很大（行 542-543）。
 *
 * 經典實驗：同一個 `if (data[i] >= 128)` 迴圈，跑「已排序」vs「未排序」的資料。
 *   - 已排序：分支結果一長串 0 再一長串 1，BHT 幾乎全猜對 -> 快
 *   - 未排序：分支結果隨機，BHT 命中率≈50% -> 每次誤判都清流水線 -> 慢
 * 兩者「做的算術完全一樣」，唯一差別就是分支可不可預測，時間差 = 誤判懲罰。
 *
 * 需要先 `sudo insmod pmu_user.ko`（用週期計數器；沒有也能用 clock_gettime）。
 *   gcc -O2 -o branch_pred branch_pred.c && taskset -c 4 ./branch_pred
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "sidechannel.h"

#define N   32768
#define REP 2000

static int data[N];

static int g_pmu;                 /* 1=週期計數器可用 */
static uint64_t cyc(void)
{
	if (g_pmu) return rdcycle();
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;   /* 退回 ns */
}

static int cmp(const void *a, const void *b){ return *(int*)a - *(int*)b; }

static uint64_t run(void)
{
	volatile long sum = 0;
	uint64_t t0 = cyc();
	for (int r = 0; r < REP; r++)
		for (int i = 0; i < N; i++)
			if (data[i] >= 128)          /* <-- 被預測的分支 */
				sum += data[i];
	uint64_t t1 = cyc();
	return t1 - t0;
}

int main(void)
{
	int src = sc_timer_init();
	g_pmu = (src >= 0);
	const char *unit = g_pmu ? "週期" : "ns";

	srand(1);
	for (int i = 0; i < N; i++) data[i] = rand() % 256;

	uint64_t unsorted = run();

	qsort(data, N, sizeof(int), cmp);
	uint64_t sorted = run();

	double per_unsorted = (double)unsorted / ((double)N * REP);
	double per_sorted   = (double)sorted   / ((double)N * REP);

	printf("時鐘來源：%s\n", g_pmu ? "PMCCNTR_EL0（週期級）" : "CLOCK_MONOTONIC（ns）");
	printf("同樣的迴圈、同樣的算術，唯一差別是分支可否預測：\n");
	printf("  未排序（分支隨機，常誤判）：每次迭代 %.3f %s\n", per_unsorted, unit);
	printf("  已排序（分支規律，幾乎全中）：每次迭代 %.3f %s\n", per_sorted, unit);
	printf("  比值 = %.2fx  <-- 這個差距就是「分支誤判懲罰」\n",
	       per_unsorted / per_sorted);
	return 0;
}
