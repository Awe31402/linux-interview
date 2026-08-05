// SPDX-License-Identifier: GPL-2.0
/*
 * oops_lab.ko —— 卷2 第3章 Q14：如何分析一個 oops 錯誤日誌
 *
 * 對照書上 §3.5.3 的 oops_test.c，但把錯誤型態拆成幾種，
 * 方便對照 ESR（Exception Syndrome Register）的差異：
 *
 *   mode=1  寫空指標  *(int *)0 = 0        → ESR 0x96000045/0x96000044（DABT, WnR=1）
 *   mode=2  讀空指標  x = *(int *)0        → ESR 0x96000004/0x96000005（DABT, WnR=0）
 *   mode=3  寫野指標（核心空間未映射位址）
 *   mode=4  執行空指標 → 指令異常 IABT
 *   mode=5  BUG_ON(1)                       → BUG: ... brk #0x800
 *   mode=6  WARN_ON(1)                      → WARNING: 只印堆疊，系統續跑
 *   mode=7  在 kthread 裡觸發（oops 發生在別的行程上下文）
 *
 * 本機 CONFIG_PANIC_ON_OOPS=n、/proc/sys/kernel/panic_on_oops=0，
 * 所以 oops 只會殺掉當前行程（insmod），系統照常跑，可以安全重複實驗。
 *
 * 用法：
 *   sudo insmod oops_lab.ko mode=1 ; sudo dmesg | tail -60
 *   # 事後解碼：
 *   gdb -batch -ex "list *create_oops+0x14" oops_lab.o
 *   objdump -dS --disassemble=create_oops oops_lab.o
 *   ~/disk/kernel-source/scripts/decodecode < code.txt
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("oops generator for post-mortem analysis (ch12 Q14)");

static int mode = 1;
module_param(mode, int, 0444);

static noinline void create_oops(void)
{
	/*
	 * 前面這幾行只是把出錯指令往後推幾個位元組：
	 * arm64 的 dump_kernel_instr() 會印出 PC 前後各 4 條指令（"Code:" 那一行），
	 * 如果出錯點離模組 .text 起點不到 16 B，它讀不到就只會印 "Code: bad PC value"。
	 */
	volatile int a = 1, b = 2, c = 3, d = 4;

	*(int *)0 = a + b + c + d;	/* 人為製造一個空指標寫入 */
}

static noinline int read_oops(void)
{
	return *(volatile int *)0;
}

static noinline void wild_oops(void)
{
	*(volatile unsigned long *)0xffff888800000000UL = 0x1234;
}

static noinline void exec_oops(void)
{
	void (*fn)(void) = (void (*)(void))0;

	fn();
}

static noinline void bug_oops(void)
{
	BUG_ON(mode == 5);
}

static noinline void warn_oops(void)
{
	WARN_ON(mode == 6);
}

static int oops_thread(void *unused)
{
	pr_info("oops_lab: 在 kthread [%s pid=%d] 裡觸發\n",
		current->comm, current->pid);
	create_oops();
	return 0;
}

static int __init oops_lab_init(void)
{
	pr_info("oops_lab: init mode=%d，create_oops=%px\n", mode, create_oops);
	pr_info("oops_lab: 模組 .text 基底 = %px（算偏移量時要減掉它）\n",
		THIS_MODULE->core_layout.base);

	switch (mode) {
	case 1: create_oops();				break;
	case 2: pr_info("read = %d\n", read_oops());	break;
	case 3: wild_oops();				break;
	case 4: exec_oops();				break;
	case 5: bug_oops();				break;
	case 6: warn_oops();				break;
	case 7: kthread_run(oops_thread, NULL, "oops_lab");
		msleep(200);
		break;
	default:
		return -EINVAL;
	}
	return -EAGAIN;
}

static void __exit oops_lab_exit(void) { }

module_init(oops_lab_init);
module_exit(oops_lab_exit);
