// SPDX-License-Identifier: GPL-2.0
/*
 * nospec_mask.c —— Q8「CSDB 指令的作用」＋ Q9「array_index_nospec() 如何規避幽靈漏洞」。
 *
 * 對應書上 §6.3.3（行 697-797）。三件事：
 *
 *   (A) 用 C 重算書上 array_index_mask_nospec() 的純算術版本（行 701-705），
 *       證明 index<size 回傳全 1、index>=size 回傳全 0。
 *
 *   (B) 用「和核心 arch/arm64/include/asm/barrier.h 一模一樣」的內嵌組語版
 *       （cmp / sbc / csdb，行 770-786），在真硬體上跑出同樣的 mask，
 *       並反組譯確認 csdb 這條指令的機器碼 = 0xd503229f。
 *
 *   (C) 示範 array_index_nospec(i, s) 把索引夾在 [0, size) —— 即使分支預測器
 *       誤判 `if (i<size)` 成立而推測執行，i&mask 也已被架構化地限制住，
 *       推測執行讀到的位址不會越界（CSDB 保證這條資料相依先於後續使用）。
 *
 *   gcc -O2 -o nospec_mask nospec_mask.c && ./nospec_mask
 *   objdump -d nospec_mask | grep -A2 -B2 csdb   # 看 csdb 機器碼
 */
#include <stdio.h>
#include <stdint.h>

#define BITS_PER_LONG 64

/* (A) 書上行 701-705 的純 C 版 */
static unsigned long mask_c(unsigned long index, unsigned long size)
{
	return ~(long)(index | (size - 1UL - index)) >> (BITS_PER_LONG - 1);
}

/* (B) 書上行 770-786 的組語版：cmp / sbc / csdb（與核心 barrier.h 相同） */
static unsigned long mask_asm(unsigned long idx, unsigned long sz)
{
	unsigned long mask;
	asm volatile(
		"cmp   %1, %2\n"       /* idx - sz，設定 PSTATE.C */
		"sbc   %0, xzr, xzr\n" /* mask = 0 - 0 - !C = idx<sz ? -1 : 0 */
		: "=r"(mask)
		: "r"(idx), "Ir"(sz)
		: "cc");
	asm volatile("csdb" ::: "memory");  /* 消費預測資料屏障（Q8） */
	return mask;
}

/* (C) array_index_nospec：把索引夾進 [0, size) */
#define array_index_nospec(i, s) ((i) & mask_asm((i), (s)))

int main(void)
{
	unsigned long size = 16;
	unsigned long tests[] = { 0, 1, 15, 16, 17, 100, ~0UL };

	printf("size = %lu\n", size);
	printf("%-22s %-20s %-20s\n", "index", "mask_c", "mask_asm");
	for (unsigned i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
		unsigned long x = tests[i];
		printf("%-22lu 0x%016lx  0x%016lx  -> nospec(x)=%lu\n",
		       x, mask_c(x, size), mask_asm(x, size),
		       array_index_nospec(x, size));
	}
	printf("\n判讀：index < size -> mask = 0xffff...ffff（放行原值）；\n");
	printf("      index >= size -> mask = 0（夾成 0），推測執行也讀不到越界位址。\n");
	printf("\n提示：`objdump -d ./nospec_mask | grep csdb` 可看到 csdb = 0xd503229f。\n");
	return 0;
}
