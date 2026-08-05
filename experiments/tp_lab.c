// SPDX-License-Identifier: GPL-2.0
/*
 * tp_lab.ko —— 卷2 第3章 Q8：在核心程式碼裡加一個跟蹤點，並證明
 *              「跟蹤點沒打開時的代價 = 一條 NOP」（static key / jump label）。
 *
 * 這個模組做四件事：
 *   1. 用 TRACE_EVENT() 定義兩個跟蹤點（見 tp_lab_trace.h），
 *      載入後 /sys/kernel/debug/tracing/events/tp_lab/ 就會長出來。
 *   2. 每 200 ms 觸發一次，讓 `cat trace` 抓得到資料。
 *   3. 用 register_trace_tp_lab_alloc() 掛一個自己的 probe，
 *      示範跟蹤點的另一種消費方式（不經過 ftrace ring buffer）。
 *   4. 把 tp_lab_hit() 的機器碼 dump 出來 —— 打開/關閉跟蹤點前後各一次，
 *      可以親眼看到 static key 把 NOP 改寫成 B（arm64 jump label）。
 *
 * 用法：
 *   sudo insmod tp_lab.ko
 *   sudo cat /sys/kernel/debug/tracing/events/tp_lab/tp_lab_alloc/format
 *   echo 1 | sudo tee /sys/kernel/debug/tracing/events/tp_lab/tp_lab_alloc/enable
 *   echo 1 | sudo tee /sys/module/tp_lab/parameters/dump   # 再 dump 一次機器碼
 *   sudo cat /sys/kernel/debug/tracing/trace
 *   sudo rmmod tp_lab
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/jump_label.h>

#define CREATE_TRACE_POINTS
#include "tp_lab_trace.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("out-of-tree tracepoint + static key evidence (ch12 Q8)");

static int period_ms = 200;
module_param(period_ms, int, 0644);
MODULE_PARM_DESC(period_ms, "觸發跟蹤點的週期(ms)");

static int probe_on = 1;
module_param(probe_on, int, 0444);
MODULE_PARM_DESC(probe_on, "是否註冊自己的 probe 函式");

static struct task_struct *tp_thread;
static u64 counter;

/* 這個函式裡只有「一個跟蹤點」，方便觀察 NOP ↔ B 的改寫 */
static noinline void tp_lab_hit(u64 size, unsigned long addr)
{
	trace_tp_lab_alloc(current, size, addr);
	trace_tp_lab_big_alloc(size);
}

static void dump_hit_insn(const char *when)
{
	u32 *p = (u32 *)tp_lab_hit;
	int i;

	pr_info("tp_lab: --- tp_lab_hit() 機器碼 (%s) @ %px ---\n", when, p);
	for (i = 0; i < 12; i++) {
		u32 insn = p[i];
		const char *note = "";

		if (insn == 0xd503201f)
			note = "  <-- NOP（跟蹤點關閉：static key 的 no-op 分支）";
		else if ((insn & 0xfc000000) == 0x14000000)
			note = "  <-- B（跟蹤點打開：跳去呼叫 probe）";
		pr_info("tp_lab:   +0x%02x: %08x%s\n", i * 4, insn, note);
	}
}

static int dump_set(const char *val, const struct kernel_param *kp)
{
	dump_hit_insn("手動 dump");
	pr_info("tp_lab: tracepoint tp_lab_alloc 目前 %s\n",
		trace_tp_lab_alloc_enabled() ? "已啟用(enabled)" : "未啟用");
	return 0;
}
static const struct kernel_param_ops dump_ops = { .set = dump_set };
module_param_cb(dump, &dump_ops, NULL, 0644);

/* 跟蹤點的第二種用法：自己寫 probe 掛上去（不經過 ftrace）*/
static void my_probe(void *data, struct task_struct *tsk, u64 size,
		     unsigned long addr)
{
	if (size == 4096)
		pr_info("tp_lab: [my_probe] 收到 comm=%s pid=%d size=%llu addr=%#lx\n",
			tsk->comm, tsk->pid, size, addr);
}

static int tp_thread_fn(void *unused)
{
	while (!kthread_should_stop()) {
		void *p = kmalloc(64, GFP_KERNEL);

		tp_lab_hit(64 + (counter % 4) * 512, (unsigned long)p);
		kfree(p);
		counter++;
		msleep(period_ms);
	}
	return 0;
}

static int __init tp_lab_init(void)
{
	int ret;

	pr_info("tp_lab: init，tp_lab_hit=%px\n", tp_lab_hit);
	dump_hit_insn("載入時（跟蹤點還沒 enable）");

	if (probe_on) {
		ret = register_trace_tp_lab_alloc(my_probe, NULL);
		pr_info("tp_lab: register_trace_tp_lab_alloc() = %d\n", ret);
	}

	tp_thread = kthread_run(tp_thread_fn, NULL, "tp_lab");
	if (IS_ERR(tp_thread))
		return PTR_ERR(tp_thread);
	return 0;
}

static void __exit tp_lab_exit(void)
{
	if (tp_thread)
		kthread_stop(tp_thread);
	if (probe_on) {
		unregister_trace_tp_lab_alloc(my_probe, NULL);
		tracepoint_synchronize_unregister();
	}
	pr_info("tp_lab: exit，共觸發 %llu 次\n", counter);
}

module_init(tp_lab_init);
module_exit(tp_lab_exit);
