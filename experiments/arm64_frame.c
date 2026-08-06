// SPDX-License-Identifier: GPL-2.0
/*
 * arm64_frame.c —— 卷2 第5章 Q1 / Q2：ARM64 的函式棧佈局，以及「FP 到底指向哪裡」
 *
 * 書上（卷1 §1.6，行 3659-3690）說：
 *   「處理器的 FP 和 SP 寄存器相同。在函數執行時 FP 和 SP 寄存器會指向該函數棧空間的 FP 處，即棧底。」
 * 這句話只對「小框架」成立。這支程式用四種函式證明 ARM64 其實有三種序幕（prologue）寫法：
 *
 *   (A) 葉子函式             —— 完全不建立框架記錄，x29 動都不動（calltrace 裡看不到它）
 *   (B) 小框架非葉子函式     —— stp x29,x30,[sp,#-16]!  +  mov x29,sp   → FP == SP（書上講的情形）
 *   (C) 大框架非葉子函式     —— sub sp,sp,#N ; stp x29,x30,[sp,#M] ; add x29,sp,#M → FP = SP+M（框架中間）
 *   (D) 動態框架（alloca/VLA）—— SP 會再往下移，FP 保持不動 → 這時 FP 和 SP 差很多
 *
 * 不變的只有一件事（這就是 Q2 的答案）：
 *   **x29 永遠指向「本函式的框架記錄」**，也就是 [x29] = 父函式的 x29、[x29+8] = 本函式的返回位址(LR)。
 *
 * 編譯：gcc -O1 -g -fno-omit-frame-pointer -o arm64_frame arm64_frame.c
 *       objdump -d arm64_frame        ← 對照四種序幕
 * 必須在 aarch64 上編譯執行（板子上）。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef __aarch64__
#error "這支程式只能在 ARM64 上編譯"
#endif

#define FP()  ((unsigned long)__builtin_frame_address(0))
#define LRV() ((unsigned long)__builtin_return_address(0))

static unsigned long sp_now(void)
{
	unsigned long sp;

	asm volatile("mov %0, sp" : "=r"(sp));
	return sp;
}

static void show(const char *who, unsigned long fp, unsigned long sp)
{
	printf("%-14s x29=0x%-14lx sp=0x%-14lx x29-sp=%-6ld  [x29]=父x29=0x%-14lx  [x29+8]=LR=0x%lx\n",
	       who, fp, sp, (long)(fp - sp),
	       *(unsigned long *)fp, *(unsigned long *)(fp + 8));
}

/* ---- (A) 真正的葉子函式：不呼叫任何人（連 printf 都不能呼叫，否則就不是葉子了）
 *      → GCC 連框架記錄都不建，x29 原封不動還是呼叫者的
 */
static unsigned long leaf_sp;

static __attribute__((noinline)) long leaf(long a, long b)
{
	/* 這裡「絕對不能」呼叫 __builtin_frame_address()，那會逼 GCC 生出框架記錄 */
	leaf_sp = sp_now();
	return a * b;
}

/* ---- (B) 小框架：stp x29,x30,[sp,#-16]! + mov x29,sp → FP == SP ------------- */
static __attribute__((noinline)) long small_frame(long a)
{
	unsigned long fp = FP(), sp = sp_now();

	show("small_frame", fp, sp);
	{
		long r = leaf(a, 2);

		printf("%-14s sp=0x%-14lx  ← 真葉子函式：它的 sp 和呼叫者 small_frame 的 sp(0x%lx) %s，\n"
		       "               代表它連框架記錄都沒建；x29 完全沒動，所以 calltrace 上永遠看不到葉子函式\n",
		       "leaf", leaf_sp, sp, leaf_sp == sp ? "一模一樣" : "不同");
		return r;
	}
}

/* ---- (C) 大框架：局部陣列把框架撐大，GCC 改用 add x29,sp,#M ---------------- */
static __attribute__((noinline)) long big_frame(long a)
{
	volatile char buf[512];		/* 撐大框架 */
	unsigned long fp = FP(), sp = sp_now();

	memset((void *)buf, a, sizeof(buf));
	show("big_frame", fp, sp);
	printf("%-14s &buf[0]=%p → x29%+ld（局部陣列在 x29 的哪一側，由編譯器決定）\n",
	       "big_frame", buf, (long)((unsigned long)buf - fp));
	return small_frame(buf[0]);
}

/* ---- (D) 動態框架：VLA 讓 SP 在函式執行中再往下移，FP 不動 ----------------- */
static __attribute__((noinline)) long vla_frame(long n)
{
	unsigned long fp1 = FP(), sp1 = sp_now();
	char vla[n];			/* 動態配置 → sub sp, sp, xN */
	unsigned long fp2 = FP(), sp2 = sp_now();

	memset(vla, 0, n);
	printf("%-14s 配置 VLA 前 x29=0x%lx sp=0x%lx；配置後 x29=0x%lx sp=0x%lx"
	       " → **x29 不動、sp 掉了 %ld 位元組**\n",
	       "vla_frame", fp1, sp1, fp2, sp2, (long)(sp1 - sp2));
	return big_frame(vla[0] + n);
}

/* ---- main→func1→func2 三層鏈（書上圖 1.31 / 圖 5.6 的實測版）--------------- */
static unsigned long g_fp[3], g_sp[3];

static __attribute__((noinline)) long func2(long a1, long a2, long a3, long a4,
					    long a5, long a6, long a7, long a8)
{
	long local2 = 0x2222222222222222L;
	unsigned long p;

	g_fp[2] = FP();
	g_sp[2] = sp_now();
	show("func2", g_fp[2], g_sp[2]);
	printf("%-14s &local2=%p → x29%+ld；八個參數全部走 x0~x7，堆疊上沒有參數\n",
	       "func2", &local2, (long)((unsigned long)&local2 - g_fp[2]));

	printf("\n==== 原始堆疊內容（= crash 的 bt -f）====\n");
	for (p = g_sp[2]; p <= g_fp[0] + 8; p += 8) {
		const char *tag = "";

		if (p == g_fp[2])          tag = "<= func2 的框架記錄：P_FP（指向 func1 的框架記錄）";
		else if (p == g_fp[2] + 8) tag = "<= func2 的 P_LR（返回 func1 的位址）";
		else if (p == g_fp[1])     tag = "<= func1 的框架記錄：P_FP（指向 main 的框架記錄）";
		else if (p == g_fp[1] + 8) tag = "<= func1 的 P_LR（返回 main 的位址）";
		else if (p == g_fp[0])     tag = "<= main 的框架記錄：P_FP";
		else if (p == g_fp[0] + 8) tag = "<= main 的 P_LR（返回 libc 的位址）";
		printf("0x%lx  0x%016lx  %s\n", p, *(unsigned long *)p, tag);
	}
	printf("\nFP 鏈：func2(0x%lx) → func1(0x%lx) → main(0x%lx)\n",
	       g_fp[2], g_fp[1], g_fp[0]);
	printf("每層框架大小：func2 = %ld、func1 = %ld 位元組\n",
	       (long)(g_fp[1] - g_fp[2]), (long)(g_fp[0] - g_fp[1]));
	return a1 + a8 + local2;
}

static __attribute__((noinline)) long func1(long a)
{
	long local1 = 0x1111111111111111L;

	g_fp[1] = FP();
	g_sp[1] = sp_now();
	show("func1", g_fp[1], g_sp[1]);
	printf("%-14s &local1=%p → x29%+ld\n", "func1", &local1,
	       (long)((unsigned long)&local1 - g_fp[1]));
	return func2(a, 2, 3, 4, 5, 6, 7, 8);
}

int main(void)
{
	g_fp[0] = FP();
	g_sp[0] = sp_now();

	printf("==== Q2：四種序幕，看 x29 到底指向哪裡 ====\n");
	vla_frame(64);

	printf("\n==== Q1：main → func1 → func2 三層框架鏈 ====\n");
	show("main", g_fp[0], g_sp[0]);
	printf("func2() = 0x%lx\n", func1(1));
	return 0;
}
