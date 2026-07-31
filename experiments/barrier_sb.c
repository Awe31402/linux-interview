/* Ch2 Q17/Q18: ARMv8 是 weakly-ordered —— store buffer (SB) litmus test
 *
 *   T0:  x = 1 ;  r0 = y ;          T1:  y = 1 ;  r1 = x ;
 *
 * 若某一輪出現 (r0==0 && r1==0)，代表兩邊的 load 都「越過」了自己的 store。
 * 這在 sequential consistency 下不可能發生，但 ARMv8 的 weak memory model
 * 允許（store buffer）。加上 DMB 或用 SEQ_CST 就會消失。
 *
 * 作法：開一個很大的 slot 陣列，兩個執行緒各自跑完整條，不做每輪握手
 * （握手本身就是同步，會把亂序視窗關掉）；靠兩者速度相近自然重疊。
 *
 *   taskset 由程式內部 pin
 *   ./barrier_sb none 4 5      # 同 cluster 兩顆 A76
 *   ./barrier_sb dmb  4 5
 *   ./barrier_sb seqcst 4 5
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>

#define N     (1 << 20)          /* 100 萬個獨立 slot */
#define REP   40                 /* 重複幾輪 */

struct slot { int x; int pad1[15]; int y; int pad2[15]; int r0, r1; int pad3[14]; };
static struct slot *S;
static int mode, c0, c1;
static volatile int go;
static long n00, n01, n10, n11;

#define DMB_ISH() __asm__ __volatile__("dmb ish" ::: "memory")
#define CBAR()    __asm__ __volatile__("" ::: "memory")

static void pin(int cpu){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu,&s);
                          pthread_setaffinity_np(pthread_self(), sizeof s, &s); }

static void *th0(void *a)
{
	pin(c0);
	__atomic_fetch_add((int *)&go, 1, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&go, __ATOMIC_SEQ_CST) < 2) ;
	for (int i = 0; i < N; i++) {
		if (mode == 2) {
			__atomic_store_n(&S[i].x, 1, __ATOMIC_SEQ_CST);
			S[i].r0 = __atomic_load_n(&S[i].y, __ATOMIC_SEQ_CST);
		} else {
			__atomic_store_n(&S[i].x, 1, __ATOMIC_RELAXED);
			if (mode == 1) DMB_ISH(); else CBAR();
			S[i].r0 = __atomic_load_n(&S[i].y, __ATOMIC_RELAXED);
		}
	}
	return NULL;
}

static void *th1(void *a)
{
	pin(c1);
	__atomic_fetch_add((int *)&go, 1, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&go, __ATOMIC_SEQ_CST) < 2) ;
	for (int i = 0; i < N; i++) {
		if (mode == 2) {
			__atomic_store_n(&S[i].y, 1, __ATOMIC_SEQ_CST);
			S[i].r1 = __atomic_load_n(&S[i].x, __ATOMIC_SEQ_CST);
		} else {
			__atomic_store_n(&S[i].y, 1, __ATOMIC_RELAXED);
			if (mode == 1) DMB_ISH(); else CBAR();
			S[i].r1 = __atomic_load_n(&S[i].x, __ATOMIC_RELAXED);
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	const char *m = argc > 1 ? argv[1] : "none";

	c0 = argc > 2 ? atoi(argv[2]) : 4;
	c1 = argc > 3 ? atoi(argv[3]) : 5;
	if (!strcmp(m, "dmb")) mode = 1;
	else if (!strcmp(m, "seqcst")) mode = 2;
	S = aligned_alloc(4096, sizeof(struct slot) * N);

	printf("SB litmus  T0(cpu%d): x=1; r0=y   |   T1(cpu%d): y=1; r1=x   mode=%-6s  %d 次\n",
	       c0, c1, m, N * REP);
	for (int r = 0; r < REP; r++) {
		pthread_t a, b;
		memset(S, 0, sizeof(struct slot) * N);
		go = 0;
		pthread_create(&a, NULL, th0, NULL);
		pthread_create(&b, NULL, th1, NULL);
		pthread_join(a, NULL); pthread_join(b, NULL);
		for (int i = 0; i < N; i++) {
			int p = S[i].r0, q = S[i].r1;
			if (!p && !q) n00++; else if (!p && q) n01++;
			else if (p && !q) n10++; else n11++;
		}
	}
	printf("  r0=0,r1=0 : %9ld   <<<< 亂序 (SC 下不可能)\n", n00);
	printf("  r0=0,r1=1 : %9ld\n", n01);
	printf("  r0=1,r1=0 : %9ld\n", n10);
	printf("  r0=1,r1=1 : %9ld\n", n11);
	printf("  => 亂序率 %.6f%%  (%ld / %ld)\n",
	       100.0 * n00 / ((long)N * REP), n00, (long)N * REP);
	return 0;
}
