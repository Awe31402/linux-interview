// SPDX-License-Identifier: GPL-2.0
/*
 * pic_demo —— 卷2 第3章 Q2/Q3/Q4/Q5：
 *   在真機上把「鏈接地址 / 加載地址 / 運行地址」三者拆開，
 *   證明哪些 ARM64 指令是位置無關的、哪些需要「重定位」才能用。
 *
 * 作法：
 *   1. pic_asm.S 裡的一段程式碼 blob（含 PIC 與非 PIC 兩類函式）
 *      被鏈接器放在鏈接地址 L（= 執行檔載入後的位址）。
 *   2. 本程式 mmap 一塊 RWX 記憶體 R，把整段 blob memcpy 過去
 *      （這一步就是 bootloader 幹的事：把映像從「加載地址」搬到別的位置）。
 *   3. 分別呼叫 L 與 R 兩份程式碼的同名函式，比對回傳值。
 *
 * 期待結果：
 *   PIC  函式（adr / bl / adrp）：回傳值跟著搬家 → 搬到哪都能正確執行
 *   非PIC函式（ldr x0,=sym / blr）：回傳值永遠是鏈接地址 → 沒有重定位就是錯的
 *
 * 編譯（在板子上）：
 *   gcc -O2 -o pic_demo pic_demo.c pic_asm.S          # 預設 PIE
 *   gcc -O2 -no-pie -o pic_demo_nopie pic_demo.c pic_asm.S
 * 執行：
 *   ./pic_demo
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

extern char blob_start[], blob_end[];
extern uint64_t gvar;

typedef unsigned long (*fn_t)(void);

unsigned long pic_where(void);
unsigned long abs_where(void);
unsigned long callee(void);
unsigned long pic_call(void);
unsigned long abs_call(void);
unsigned long pic_data(void);

struct item {
	const char	*name;
	void		*fn;
	const char	*kind;
	const char	*insn;
};

int main(void)
{
	size_t len = (size_t)(blob_end - blob_start);
	size_t pgsz = (size_t)sysconf(_SC_PAGESIZE);
	size_t maplen = (len + pgsz - 1) & ~(pgsz - 1);
	unsigned char *copy;
	long delta;
	int i;

	struct item items[] = {
		{ "pic_where", pic_where, "PIC   ", "adr x0, pic_where" },
		{ "abs_where", abs_where, "non-PIC", "ldr x0, =pic_where" },
		{ "pic_call ", pic_call,  "PIC   ", "bl callee" },
		{ "abs_call ", abs_call,  "non-PIC", "ldr x1,=callee; blr x1" },
		{ "pic_data ", pic_data,  "PIC   ", "adrp/add :lo12:gvar" },
	};

	copy = mmap(NULL, maplen, PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (copy == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memcpy(copy, blob_start, len);
	/* ARM64 是 Harvard 架構：改完程式碼一定要做 I-cache 維護 */
	__builtin___clear_cache((char *)copy, (char *)copy + len);

	delta = (long)(copy - (unsigned char *)blob_start);

	printf("=== pic_demo：鏈接地址 vs 運行地址 ===\n");
	printf("blob 鏈接地址(執行檔載入後) L = %p .. %p (%zu bytes)\n",
	       blob_start, blob_end, len);
	printf("blob 複製後的運行地址       R = %p (delta = %+ld = %#lx)\n",
	       copy, delta, (unsigned long)delta);
	printf("全域變數 gvar               = %p\n", (void *)&gvar);
	printf("\n%-10s %-8s %-24s %-18s %-18s %s\n",
	       "函式", "類型", "指令", "在 L 執行", "在 R 執行", "判定");
	printf("---------------------------------------------------------------"
	       "----------------------------------------\n");

	for (i = 0; i < (int)(sizeof(items) / sizeof(items[0])); i++) {
		unsigned long off = (unsigned long)((char *)items[i].fn - blob_start);
		fn_t at_l = (fn_t)items[i].fn;
		fn_t at_r = (fn_t)(copy + off);
		unsigned long rl = at_l();
		unsigned long rr = at_r();
		const char *verdict;

		if (rr == rl)
			verdict = "回傳值不變 → 位置有關（寫死鏈接地址）";
		else if ((long)(rr - rl) == delta)
			verdict = "回傳值跟著搬家 → 位置無關";
		else
			verdict = "PC 相對但指向沒搬家的資料 → 需連資料一起搬(±4GB)";

		printf("%-10s %-8s %-24s %-18lx %-18lx %s\n",
		       items[i].name, items[i].kind, items[i].insn,
		       rl, rr, verdict);
	}

	printf("\n--- 非 PIC 函式的「文字池」內容（絕對位址就寫死在這裡）---\n");
	{
		unsigned long off = (unsigned long)((char *)abs_where - blob_start);
		uint32_t *p = (uint32_t *)abs_where;
		int n;

		printf("abs_where 的指令 @L:");
		for (n = 0; n < 2; n++)
			printf(" %08x", p[n]);
		printf("   （ldr x0,[pc,#imm] ; ret）\n");
		/* ldr 的 literal 就緊接在 ret 之後 */
		printf("abs_where 文字池 @L = %#lx\n", *(unsigned long *)((char *)abs_where + 8));
		printf("abs_where 文字池 @R = %#lx  （複製過來也還是同一個絕對位址）\n",
		       *(unsigned long *)((char *)copy + off + 8));
		printf("pic_where 的鏈接地址 = %p\n", (void *)pic_where);
	}

	return 0;
}
