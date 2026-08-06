// SPDX-License-Identifier: GPL-2.0
/*
 * x86_abi.c —— 卷2 第4章 Q2：x86_64 的函式參數怎麼傳？
 *
 * 書上（奔跑吧卷2 §4.2.2，表 4.2）只講了「≤6 個整數參數用 RDI/RSI/RDX/RCX/R8/R9，
 * 超過的走堆疊」。這支程式把 System V AMD64 ABI 的其餘規則也一起跑出來：
 *   1. 8 個整數參數      → 第 7、8 個在堆疊上（相對 RSP）
 *   2. 浮點參數          → XMM0~XMM7（書上完全沒提）
 *   3. 大結構回傳         → 隱藏的第 0 個參數（RDI 是回傳緩衝區，真正的第 1 參數變 RSI）
 *   4. 小結構（≤16 B）    → 用 RAX:RDX 兩個暫存器回傳
 *   5. 可變參數函式       → AL 記錄「用了幾個向量暫存器」
 *
 * 編譯：gcc -O1 -g -c x86_abi.c -o x86_abi.o && objdump -d x86_abi.o
 *       （用 -O1 是為了讓呼叫端的參數安排乾淨可讀；-O0 會多出一堆堆疊搬運）
 *
 * 執行環境：本檔在「開發主機」上跑（x86_64 Ubuntu 22.04），不是在 RK3588 板子上。
 */
#include <stdio.h>
#include <stdarg.h>

struct big  { long a, b, c, d; };	/* 32 B：走記憶體（MEMORY class） */
struct small{ long a, b; };		/* 16 B：走 RAX:RDX（INTEGER class） */

/* 1. 八個整數參數 */
long callee8(long a1, long a2, long a3, long a4,
	     long a5, long a6, long a7, long a8)
{
	return a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8;
}

/* 2. 整數 + 浮點混合：整數走 RDI…，浮點各自走 XMM0… */
double callee_mix(long i1, double d1, long i2, double d2, long i3, double d3)
{
	return i1 + d1 + i2 + d2 + i3 + d3;
}

/* 3. 大結構回傳：呼叫端要先把「回傳緩衝區位址」放進 RDI */
struct big callee_big(long a1, long a2)
{
	struct big b = { a1, a2, a1 + a2, a1 * a2 };
	return b;
}

/* 4. 小結構回傳：RAX + RDX */
struct small callee_small(long a1, long a2)
{
	struct small s = { a1, a2 };
	return s;
}

/* 5. 可變參數：AL = 使用的向量暫存器個數 */
long callee_var(const char *fmt, ...)
{
	va_list ap;
	long sum = 0;

	va_start(ap, fmt);
	sum += va_arg(ap, long);
	sum += (long)va_arg(ap, double);
	va_end(ap);
	return sum;
}

long caller(void)
{
	struct big b;
	struct small s;
	long r = 0;

	r += callee8(1, 2, 3, 4, 5, 6, 7, 8);
	r += (long)callee_mix(10, 1.5, 20, 2.5, 30, 3.5);
	b  = callee_big(100, 200);
	s  = callee_small(300, 400);
	r += b.a + b.d + s.a + s.b;
	r += callee_var("x", 7L, 8.0);
	return r;
}

int main(void)
{
	printf("caller() = %ld\n", caller());
	return 0;
}
