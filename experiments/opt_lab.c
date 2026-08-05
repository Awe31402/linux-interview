// SPDX-License-Identifier: GPL-2.0
/*
 * opt_lab.ko —— 卷2 第3章 Q1：「用 GCC 的 O0 編譯內核有什麼優勢？」
 *
 * 同一份原始碼，用 -O0 / -O1 / -O2 / -Os 各編一次，比較：
 *   1. 產出的機器碼大小、指令數、堆疊框大小
 *   2. static inline 有沒有被內聯（DW_TAG_inlined_subroutine）
 *   3. 區域變數在 DWARF 裡還有沒有位置資訊（GDB 是否顯示 <optimized out>）
 *   4. 行號表是否單調遞增（游標會不會亂跳）
 *
 * 另外用 -DO0_BREAK 打開「-O0 為什麼編不動內核」的兩個經典案例。
 *
 * 用法：見 opt_build.sh
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("gcc -O0 vs -O2 codegen lab (ch12 Q1)");

/* ---- (a) 會被內聯的小函式 ---------------------------------------- */
static inline int scale(int v, int k)
{
	return v * k + (v >> 1);
}

/* ---- (b) 一堆區域變數 + 迴圈：-O2 會把它們塞進暫存器並展開 -------- */
noinline int opt_lab_work(int n)
{
	int acc = 0;			/* -O2 下會活在暫存器，GDB 看到 <optimized out> */
	int i;
	int tmp_a = n + 1;
	int tmp_b = n * 3;
	int tmp_c;

	for (i = 0; i < 8; i++) {
		tmp_c = scale(i, n);
		acc += tmp_c + tmp_a - tmp_b;
	}
	return acc;
}
EXPORT_SYMBOL(opt_lab_work);

/* ---- (c) 一段有函式呼叫的程式碼，方便看堆疊框大小 ---------------- */
noinline void *opt_lab_alloc(size_t sz)
{
	void *p = kmalloc(sz, GFP_KERNEL);
	char stackbuf[64];

	memset(stackbuf, 0x5a, sizeof(stackbuf));
	if (p)
		memcpy(p, stackbuf, min(sz, sizeof(stackbuf)));
	return p;
}

#ifdef O0_BREAK
/*
 * -O0 為什麼不能直接拿來編內核？兩個經典理由：
 *
 * (1) BUILD_BUG_ON 這一類編譯期斷言，靠的是「編譯器把條件常數摺疊掉」，
 *     -O0 不做內聯 → scale(2,2) 不是常數 → __compiletime_assert 直接報錯。
 * (2) cmpxchg()/this_cpu_*() 這種依 sizeof 展開的巨集，
 *     用 BUILD_BUG() 擋住不合法的 size；-O0 不做死碼消除，
 *     那些不可能走到的分支也會被真的產生出來 → 連結失敗。
 */
static int o0_break_var;
noinline void opt_lab_break(void)
{
	BUILD_BUG_ON(scale(2, 2) != 5);		/* (1) */
	cmpxchg(&o0_break_var, 0, 1);		/* (2) */
}
#endif

static int __init opt_lab_init(void)
{
	void *p;

	pr_info("opt_lab: work(3) = %d\n", opt_lab_work(3));
	p = opt_lab_alloc(32);
	kfree(p);
	return -EAGAIN;
}

static void __exit opt_lab_exit(void) { }

module_init(opt_lab_init);
module_exit(opt_lab_exit);
