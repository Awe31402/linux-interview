// SPDX-License-Identifier: GPL-2.0
/*
 * printk_lab.ko —— 卷2 第3章 Q12：printk 的輸出等級
 *
 * 一次把 8 個等級都印一遍，再配合
 *   /proc/sys/kernel/printk 的四個數字（console / default / minimum / boot-default）
 * 觀察「哪些會出現在 console，哪些只進 ring buffer」。
 *
 * 順便示範書上提到的其他輸出工具：
 *   print_hex_dump()、dump_stack()、%pS/%px/%p 的指標雜湊、pr_*_ratelimited()
 *
 * 用法：
 *   sudo insmod printk_lab.ko ; sudo dmesg -x | tail -20
 *   cat /proc/sys/kernel/printk
 *   sudo dmesg -r | tail -20      # 看 <N> 原始等級前綴
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("printk levels & helpers (ch12 Q12)");

static int ratelimit_test;
module_param(ratelimit_test, int, 0444);

static char buf[32];

static int __init printk_lab_init(void)
{
	int i;

	printk(KERN_EMERG   "printk_lab: <0> KERN_EMERG   系統不可用\n");
	printk(KERN_ALERT   "printk_lab: <1> KERN_ALERT   必須立刻處理\n");
	printk(KERN_CRIT    "printk_lab: <2> KERN_CRIT    臨界狀況\n");
	printk(KERN_ERR     "printk_lab: <3> KERN_ERR     錯誤\n");
	printk(KERN_WARNING "printk_lab: <4> KERN_WARNING 警告\n");
	printk(KERN_NOTICE  "printk_lab: <5> KERN_NOTICE  正常但重要\n");
	printk(KERN_INFO    "printk_lab: <6> KERN_INFO    提示訊息\n");
	printk(KERN_DEBUG   "printk_lab: <7> KERN_DEBUG   除錯訊息\n");

	/* 6.1 建議用 pr_xxx()，等價於上面那一組 */
	pr_err("printk_lab: pr_err()  == printk(KERN_ERR ...)，pr_fmt 可加前綴\n");
	pr_info("printk_lab: console_loglevel 目前 = %d（/proc/sys/kernel/printk 第 1 欄）\n",
		console_loglevel);
	pr_info("printk_lab: default_message_loglevel = %d, minimum_console_loglevel = %d\n",
		default_message_loglevel, minimum_console_loglevel);

	pr_info("printk_lab: 除錯常用格式 —— %s:%d %s()\n",
		__FILE__, __LINE__, __func__);

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = i;
	pr_info("printk_lab: print_hex_dump() ↓\n");
	print_hex_dump(KERN_INFO, "printk_lab: ", DUMP_PREFIX_OFFSET,
		       16, 1, buf, sizeof(buf), true);

	pr_info("printk_lab: 指標三種印法：%%p=%p（雜湊過）%%px=%px（真位址）"
		" %%pS=%pS\n", buf, buf, printk_lab_init);

	if (ratelimit_test) {
		for (i = 0; i < 20; i++)
			printk_ratelimited(KERN_INFO
				"printk_lab: ratelimited #%d\n", i);
	}

	pr_info("printk_lab: dump_stack() ↓\n");
	dump_stack();
	return -EAGAIN;
}

static void __exit printk_lab_exit(void) { }

module_init(printk_lab_init);
module_exit(printk_lab_exit);
