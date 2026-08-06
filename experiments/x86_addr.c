// SPDX-License-Identifier: GPL-2.0
/*
 * x86_addr.c —— 卷2 第4章 Q4 / Q5 / Q6：MOV vs LEA、五種定址方式
 *
 * 書上 §4.2.4 只給了指令長相，這支程式直接把「同一條 -8(%rbp) 用 mov 和 lea
 * 各跑一次，兩個 RAX 差在哪」印出實際數值，並且把 x86 完整的
 *   位移(%基址, %索引, 比例)
 * 定址式各種組合都跑一遍。
 *
 * 全部用 AT&T 語法 inline asm，好和書上的反組譯輸出對照。
 * 編譯：gcc -O0 -g -o x86_addr x86_addr.c
 */
#include <stdio.h>
#include <stdint.h>

static long array[8] = { 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7 };
static long global_var = 0x12345678;

int main(void)
{
	long slot = 0x1122334455667788L;	/* 放在堆疊上，充當書上的 -8(%rbp) */
	long v_mov, v_lea, v_direct, v_indirect, v_base, v_index, v_rip;
	long *p = &slot;

	/* ---- Q6：mov -8(%rbp),%rax  vs  lea -8(%rbp),%rax ------------------
	 * 這裡不寫死 -8（-O0 下 slot 未必剛好在 rbp-8），改用 %1 讓編譯器
	 * 給出 slot 的實際位址，語意完全等價：
	 *   mov (%addr), %rax  ->  rax = *addr   （間接定址，讀出「內容」）
	 *   lea (%addr), %rax  ->  rax =  addr   （只算位址，不讀記憶體）
	 */
	asm volatile("mov (%1), %0" : "=r"(v_mov) : "r"(p));
	asm volatile("lea (%1), %0" : "=r"(v_lea) : "r"(p));

	printf("slot 位於 %p，內容 = 0x%lx\n", (void *)&slot, slot);
	printf("Q6  mov (%%rbx),%%rax  -> 0x%-18lx  <- 讀出「內容」\n", v_mov);
	printf("Q6  lea (%%rbx),%%rax  -> 0x%-18lx  <- 只算「位址」(= %p)\n",
	       v_lea, (void *)&slot);

	/* ---- Q5-1 直接定址（絕對位址／x86_64 實作上是 RIP 相對）---------- */
	asm volatile("mov global_var(%%rip), %0" : "=r"(v_direct));
	printf("Q5  直接定址 mov global_var(%%rip),%%rax -> 0x%lx (global_var=0x%lx)\n",
	       v_direct, global_var);

	/* ---- Q5-2 間接定址：暫存器裡放位址 ------------------------------- */
	asm volatile("mov (%1), %0" : "=r"(v_indirect) : "r"(array));
	printf("Q5  間接定址 mov (%%rbx),%%rax          -> 0x%lx (array[0])\n", v_indirect);

	/* ---- Q5-3 基址定址：位址 + 常數偏移量 ---------------------------- */
	asm volatile("mov 16(%1), %0" : "=r"(v_base) : "r"(array));
	printf("Q5  基址定址 mov 16(%%rbx),%%rax        -> 0x%lx (array[2])\n", v_base);

	/* ---- Q5-4 變址/比例定址：base + index*scale + disp ---------------- */
	{
		long idx = 3;
		asm volatile("mov (%1,%2,8), %0" : "=r"(v_index) : "r"(array), "r"(idx));
		printf("Q5  變址定址 mov (%%rbx,%%rcx,8),%%rax  -> 0x%lx (array[3])\n", v_index);
	}

	/* ---- Q4：LEA 當算術指令用（編譯器最愛的用法）---------------------- */
	{
		long a = 100, b = 7, r1, r2;

		asm volatile("lea (%1,%2,4), %0" : "=r"(r1) : "r"(a), "r"(b));   /* a + b*4 */
		asm volatile("lea 5(%1,%1,2), %0" : "=r"(r2) : "r"(a));          /* a*3 + 5 */
		printf("Q4  lea (%%rax,%%rbx,4),%%rcx  -> %ld  (= 100 + 7*4，一條指令做完乘加，且不動 flags)\n", r1);
		printf("Q4  lea 5(%%rax,%%rax,2),%%rcx -> %ld  (= 100*3 + 5)\n", r2);
	}

	/* ---- RIP 相對定址：位置無關碼的關鍵 ------------------------------ */
	asm volatile("lea global_var(%%rip), %0" : "=r"(v_rip));
	printf("附  lea global_var(%%rip),%%rax -> %p (== &global_var = %p)\n",
	       (void *)v_rip, (void *)&global_var);

	return 0;
}
