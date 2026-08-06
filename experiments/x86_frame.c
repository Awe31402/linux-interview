// SPDX-License-Identifier: GPL-2.0
/*
 * x86_frame.c —— 卷2 第4章 Q3 / Q10：main() → func1() → func2() 的 x86_64 堆疊佈局
 *
 * 這支程式做三件事：
 *  (1) 自己把三層函式的 RBP 鏈、返回位址、局部變數位址通通印出來
 *      —— 不用 gdb 也能畫出書上圖 4.2 的堆疊結構圖。
 *  (2) 從 func2 的 RSP 一路把原始堆疊內容 dump 到 main 的 RBP，
 *      每個 8 位元組都標註它是誰（saved RBP / 返回位址 / 局部變數 / 第 7、8 個參數）
 *      —— 這就是 crash 工具 `bt -f` 做的事。
 *  (3) func2() 裡放一個和書上 §4.10 一模一樣的 struct mydev_priv 局部變數，
 *      練習 Q10：「只知道栈返回地址，怎麼推導局部變數在堆疊的哪裡」。
 *
 * 重點：DUMP_FRAME 必須是「巨集」不能是函式，否則 __builtin_frame_address(0)
 *       取到的是那個輔助函式自己的框架（第一版就踩到這個坑）。
 *
 * 一定要 -O0 且 -fno-omit-frame-pointer 編，否則 GCC 會省掉 RBP。
 * 編譯：gcc -O0 -g -fno-omit-frame-pointer -o x86_frame x86_frame.c
 *
 * 同一份原始碼也能在 RK3588（aarch64）上編譯執行，用來對照 ARM64 的
 * 「x29 框架記錄」：[x29]=父 x29、[x29+8]=LR，和 x86_64 的 [RBP]/[RBP+8] 完全同構，
 * 差別在 ARM64 前 8 個參數全部走 x0~x7，不會 push 到堆疊上。
 */
#include <stdio.h>
#include <string.h>

struct mydev_priv {			/* 書上 §4.10 的資料結構 */
	char name[64];
	int i;
	void *mm;
	void *sem;
};

static unsigned long g_rbp[3], g_ret[3], g_rsp_func2;
static const char *g_name[3] = { "main", "func1", "func2" };

#if defined(__x86_64__)
#  define FPREG  "RBP"
#  define READ_SP(v) asm volatile("mov %%rsp, %0" : "=r"(v))
#elif defined(__aarch64__)
#  define FPREG  "x29"
#  define READ_SP(v) asm volatile("mov %0, sp" : "=r"(v))
#else
#  error "只支援 x86_64 / aarch64"
#endif

#define DUMP_FRAME(idx)								\
do {										\
	unsigned long _rbp = (unsigned long)__builtin_frame_address(0);		\
	unsigned long _ret = (unsigned long)__builtin_return_address(0);	\
	g_rbp[idx] = _rbp;							\
	g_ret[idx] = _ret;							\
	printf("%-6s %s=0x%lx  [%s]=父框架=0x%lx  [%s+8]=返回位址欄位=0x%lx"	\
	       " → 0x%lx\n", g_name[idx], FPREG, _rbp, FPREG, 			\
	       *(unsigned long *)_rbp, FPREG,					\
	       _rbp + 8, *(unsigned long *)(_rbp + 8));				\
	printf("%-6s __builtin_return_address(0)=0x%lx（應等於上一行的 → 值）\n",	\
	       g_name[idx], _ret);						\
} while (0)

static void dump_stack_raw(void);

/* 把「變數位址 - 框架指標」印成 x86 習慣的 RBP-0x60 / ARM64 習慣的 x29+0x20 */
static const char *off_str(unsigned long fp, const void *p)
{
	static char buf[4][32];
	static int idx;
	long d = (long)(unsigned long)p - (long)fp;
	char *b = buf[idx++ & 3];

	if (d < 0)
		snprintf(b, 32, "%s-0x%lx", FPREG, -d);
	else
		snprintf(b, 32, "%s+0x%lx", FPREG, d);
	return b;
}

/* 八個參數：第 7、8 個一定在「呼叫者」的堆疊上 */
static __attribute__((noinline)) int func2(long a1, long a2, long a3, long a4,
					   long a5, long a6, long a7, long a8)
{
	struct mydev_priv priv;		/* ← Q10 要推導的局部變數 */
	long local_x = 0x1122334455667788L;
	int  local_y = 0xabcd;

	memcpy(priv.name, "benshushu", sizeof("benshushu"));
	priv.i   = 10;
	priv.mm  = (void *)0xdeadbeef00001000UL;
	priv.sem = (void *)0xdeadbeef00001078UL;	/* mm + 0x78，模仿 mmap_sem */

	READ_SP(g_rsp_func2);

	printf("\n==== func2 ====\n");
	DUMP_FRAME(2);
	printf("func2  SP =0x%lx  框架大小 = %s-SP = 0x%lx\n", g_rsp_func2, FPREG,
	       g_rbp[2] - g_rsp_func2);
	printf("func2  &priv   =%p  → %s\n", (void *)&priv,    off_str(g_rbp[2], &priv));
	printf("func2  &local_x=%p  → %s\n", (void *)&local_x, off_str(g_rbp[2], &local_x));
	printf("func2  &local_y=%p  → %s\n", (void *)&local_y, off_str(g_rbp[2], &local_y));
#if defined(__x86_64__)
	printf("func2  第7個參數 a7=%ld 在 RBP+0x10=0x%lx；第8個 a8=%ld 在 RBP+0x18=0x%lx\n",
	       a7, g_rbp[2] + 0x10, a8, g_rbp[2] + 0x18);
#else
	printf("func2  ARM64：a1~a8 全部走 x0~x7，堆疊上沒有參數（a7=%ld a8=%ld）\n", a7, a8);
#endif

	/* Q10 的推導：只用「栈返回地址」反推 priv 的位置 */
	{
		unsigned long ret_slot = g_rbp[2] + 8;		/* crash bt -f 看得到的那一格 */
		long delta = (long)(unsigned long)&priv - (long)g_rbp[2];
		unsigned long derived = ret_slot - 0x8 + delta;

		printf("Q10 推導：priv = 栈返回地址欄位(0x%lx) - 0x8 %c 0x%lx = 0x%lx  →  %s\n",
		       ret_slot, delta < 0 ? '-' : '+', delta < 0 ? -delta : delta, derived,
		       derived == (unsigned long)&priv ? "與 &priv 相符 ✓" : "不符 ✗");
		printf("Q10 讀出該位址的內容：name=\"%s\" i=%d mm=%p sem=%p\n",
		       ((struct mydev_priv *)derived)->name,
		       ((struct mydev_priv *)derived)->i,
		       ((struct mydev_priv *)derived)->mm,
		       ((struct mydev_priv *)derived)->sem);
	}
	dump_stack_raw();	/* 必須在 func2 還活著的時候 dump，否則堆疊已被覆寫 */
	return (int)(a1 + a8 + local_y);
}

static __attribute__((noinline)) int func1(long a)
{
	long local_1 = 0x1111111111111111L;

	printf("\n==== func1 ====\n");
	DUMP_FRAME(1);
	printf("func1  &local_1=%p  → %s\n", (void *)&local_1, off_str(g_rbp[1], &local_1));
	return func2(a, 2, 3, 4, 5, 6, 7, 8);
}

/* 模仿 crash 的 bt -f：把整段堆疊逐格印出來並標註 */
static void dump_stack_raw(void)
{
	unsigned long p;

	printf("\n==== 原始堆疊內容（模仿 crash 的 bt -f）====\n");
	printf("位址              內容                說明\n");
	for (p = g_rsp_func2; p <= g_rbp[0] + 8; p += 8) {
		const char *tag = "";

		if (p == g_rbp[2])          tag = "<= func2 的框架指標：存放 func1 的框架指標";
		else if (p == g_rbp[2] + 8) tag = "<= func2 的返回位址（回到 func1）";
#if defined(__x86_64__)
		else if (p == g_rbp[2] + 0x10) tag = "<= 第 7 個參數 a7（由 func1 push）";
		else if (p == g_rbp[2] + 0x18) tag = "<= 第 8 個參數 a8（由 func1 push）";
#endif
		else if (p == g_rbp[1])     tag = "<= func1 的框架指標：存放 main 的框架指標";
		else if (p == g_rbp[1] + 8) tag = "<= func1 的返回位址（回到 main）";
		else if (p == g_rbp[0])     tag = "<= main 的框架指標：存放 libc 的框架指標";
		else if (p == g_rbp[0] + 8) tag = "<= main 的返回位址（回到 libc）";
		else if (p >  g_rbp[2] && p < g_rbp[1]) tag = "   func1 的局部變數區";
		else if (p >  g_rbp[1] && p < g_rbp[0]) tag = "   main 的局部變數區";
		else                        tag = "   func2 的局部變數區";
		printf("0x%lx  0x%016lx  %s\n", p, *(unsigned long *)p, tag);
	}
	printf("\n框架大小：func2 = 0x%lx，func1 = 0x%lx，main = 0x%lx（含 saved RBP + 返回位址）\n",
	       g_rbp[2] + 0x10 - g_rsp_func2, g_rbp[1] + 0x10 - (g_rbp[2] + 0x20),
	       g_rbp[0] + 0x10 - (g_rbp[1] + 0x10));
}

int main(void)
{
	long local_m = 0x2222222222222222L;

	printf("==== main ====\n");
	DUMP_FRAME(0);
	printf("main   &local_m=%p  → %s\n", (void *)&local_m, off_str(g_rbp[0], &local_m));
	printf("\nfunc1() = %d\n", func1(1));
	return 0;
}
