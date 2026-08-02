// SPDX-License-Identifier: GPL-2.0
/*
 * sched_probe.ko —— 《奔跑吧 Linux 內核》卷1 第 8/9 章 實驗模組
 * 平台：Radxa ROCK 5B (RK3588, 4×A76 + 4×A55)，Linux 6.1.115+ aarch64
 *
 * 涵蓋題目：
 *   Ch8 Q1/Q7   prio / nice / weight，prio_to_weight 與 prio_to_wmult 的關係
 *   Ch8 Q3      calc_delta_fair() 的乘法＋移位實作
 *   Ch8 Q13     ARM64 ASID 機制（ID_AA64MMFR0_EL1.ASIDBits、TTBR1_EL1[63:48]、
 *               mm->context.id 的 generation|asid 佈局）
 *   Ch8 Q19/Q20 LOAD_AVG_MAX、decay_load()、y^32 = 0.5
 *   Ch8 Q28     wake affine 相關欄位：wake_cpu / recent_used_cpu / wakee_flips
 *   Ch8 Q43~45  進程上下文：cpu_context 的成員與偏移、THREAD_SIZE
 *   Ch9 Q7      與優先級相關的所有欄位
 *   Ch9 Q12     中斷上下文呼叫可睡眠函式 → "BUG: sleeping function called ..."
 *   Ch9 Q13     關中斷後呼叫 schedule()，回來時中斷已被 finish_task_switch() 打開
 *
 * 用法：
 *   sudo insmod sched_probe.ko                 # 基本資訊
 *   sudo insmod sched_probe.ko target_pid=1234 # 另外 dump 某個行程
 *   sudo insmod sched_probe.ko test_irq=1      # Ch9 Q13
 *   sudo insmod sched_probe.ko test_atomic=1   # Ch9 Q12（只會印警告，不會當機）
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/cpu.h>
#include <asm/processor.h>
#include <asm/sysreg.h>
#include <asm/mmu.h>
#include <asm/memory.h>
#include <generated/utsrelease.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Scheduler / context-switch prober for RK3588 kernel study notes");

static int target_pid;
module_param(target_pid, int, 0444);
static int test_irq;
module_param(test_irq, int, 0444);
static int test_atomic;
module_param(test_atomic, int, 0444);

#define P(fmt, ...) pr_info("sched_probe: " fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Ch8 Q1/Q7：權重表                                                   */
/* ------------------------------------------------------------------ */

/* kernel/sched/core.c 的 sched_prio_to_weight[]（未匯出，這裡複製一份對帳） */
static const int my_prio_to_weight[40] = {
 /* -20 */ 88761, 71755, 56483, 46273, 36291,
 /* -15 */ 29154, 23254, 18705, 14949, 11916,
 /* -10 */  9548,  7620,  6100,  4904,  3906,
 /*  -5 */  3121,  2501,  1991,  1586,  1277,
 /*   0 */  1024,   820,   655,   526,   423,
 /*   5 */   335,   272,   215,   172,   137,
 /*  10 */   110,    87,    70,    56,    45,
 /*  15 */    36,    29,    23,    18,    15,
};

/* kernel/sched/core.c 的 sched_prio_to_wmult[] = 2^32 / weight */
static const u32 my_prio_to_wmult[40] = {
 /* -20 */     48388,     59856,     76040,     92818,    118348,
 /* -15 */    147320,    184698,    229616,    287308,    360437,
 /* -10 */    449829,    563644,    704093,    875809,   1099582,
 /*  -5 */   1376151,   1717300,   2157191,   2708050,   3363326,
 /*   0 */   4194304,   5237765,   6557202,   8165337,  10153587,
 /*   5 */  12820798,  15790321,  19976592,  24970740,  31350126,
 /*  10 */  39045157,  49367440,  61356676,  76695844,  95443717,
 /*  15 */ 119304647, 148102320, 186737708, 238609294, 286331153,
};

static void show_weight_table(void)
{
	int i;

	P("== Ch8 Q1/Q7  nice -> weight -> inv_weight ==\n");
	/* kernel/sched/sched.h 是內部標頭，這裡自己重現它的定義 */
	P("   NICE_0_LOAD = %ld  (= 1024 << SCHED_FIXEDPOINT_SHIFT(%d) = scale_load(1024))\n",
	  (long)(1024L << SCHED_FIXEDPOINT_SHIFT), SCHED_FIXEDPOINT_SHIFT);
	P("   %-5s %-9s %-12s %-12s %-12s\n",
	  "nice", "weight", "wmult(表)", "2^32/weight", "1024/1.25^n");
	for (i = 0; i < 40; i += 5) {
		int nice = i - 20;
		u32 w = my_prio_to_weight[i];
		u32 inv = (u32)(0xffffffffUL / w);
		/* 1024 / 1.25^nice ，用整數做 */
		u64 num = 1024ULL << 20, den = 1UL << 20;
		int k;
		for (k = 0; k < (nice > 0 ? nice : -nice); k++) {
			if (nice > 0) num = num * 4 / 5;
			else          num = num * 5 / 4;
		}
		P("   %-5d %-9u %-12u %-12u %-12llu\n",
		  nice, w, my_prio_to_wmult[i], inv, num / den);
	}
}

/*
 * Ch8 Q3：__calc_delta() 的等價實作
 * vruntime = delta_exec * NICE_0_LOAD * inv_weight >> 32
 */
static u64 my_calc_delta(u64 delta_exec, unsigned long weight, u32 inv_weight)
{
	u64 fact = weight;
	int shift = 32;

	fact = (u64)(u32)fact * inv_weight;
	while (fact >> 32) {
		fact >>= 1;
		shift--;
	}
	return (u64)((delta_exec * fact) >> shift);
}

static void show_calc_delta(void)
{
	static const int nices[] = { -5, 0, 1, 5, 19 };
	int i;

	P("== Ch8 Q3  calc_delta_fair(10ms, nice) —— 乘法＋移位，無浮點 ==\n");
	for (i = 0; i < ARRAY_SIZE(nices); i++) {
		int n = nices[i];
		u32 w = my_prio_to_weight[n + 20];
		u32 inv = my_prio_to_wmult[n + 20];
		u64 v = my_calc_delta(10000000ULL, 1024, inv);	/* 10ms */

		P("   nice=%-3d weight=%-6u inv=%-11u vruntime = %llu ns (= 10ms * 1024/%u)\n",
		  n, w, inv, v, w);
	}
}

/* ------------------------------------------------------------------ */
/* Ch8 Q19/Q20：PELT 衰減表                                            */
/* ------------------------------------------------------------------ */

/* kernel/sched/sched-pelt.h（由 Documentation/scheduler/sched-pelt.c 產生） */
static const u32 my_yN_inv[32] = {
	0xffffffff, 0xfa83b2da, 0xf5257d14, 0xefe4b99a, 0xeac0c6e6, 0xe5b906e6,
	0xe0ccdeeb, 0xdbfbb796, 0xd744fcc9, 0xd2a81d91, 0xce248c14, 0xc9b9bd85,
	0xc5672a10, 0xc12c4cc9, 0xbd08a39e, 0xb8fbaf46, 0xb504f333, 0xb123f581,
	0xad583ee9, 0xa9a15ab4, 0xa5fed6a9, 0xa2704302, 0x9ef5325f, 0x9b8d39b9,
	0x9837f050, 0x94f4efa8, 0x91c3d373, 0x8ea4398a, 0x8b95c1e3, 0x88980e80,
	0x85aac367, 0x82cd8698,
};
#define MY_LOAD_AVG_PERIOD 32
#define MY_LOAD_AVG_MAX    47742

static u64 my_decay_load(u64 val, u64 n)
{
	unsigned int local_n;

	if (unlikely(n > MY_LOAD_AVG_PERIOD * 63))
		return 0;
	local_n = n;
	if (unlikely(local_n >= MY_LOAD_AVG_PERIOD)) {
		val >>= local_n / MY_LOAD_AVG_PERIOD;
		local_n %= MY_LOAD_AVG_PERIOD;
	}
	return mul_u64_u32_shr(val, my_yN_inv[local_n], 32);
}

static void show_pelt(void)
{
	u64 sum = 0;
	int n;

	P("== Ch8 Q19/Q20  PELT 衰減 ==\n");
	P("   y = 0.5^(1/32)；y^32 應為 0.5：decay_load(1024, 32) = %llu （= 1024/2）\n",
	  my_decay_load(1024, 32));
	P("   decay_load(1024, 1)=%llu  (10)=%llu  (32)=%llu  (64)=%llu  (2016)=%llu  (2017)=%llu\n",
	  my_decay_load(1024, 1), my_decay_load(1024, 10), my_decay_load(1024, 32),
	  my_decay_load(1024, 64), my_decay_load(1024, 2016), my_decay_load(1024, 2017));

	/*
	 * LOAD_AVG_MAX：Documentation/scheduler/sched-pelt.c 的算法
	 *   max = 1024; loop: max = ((max * y_inv) >> 32) + 1024;  直到收斂
	 */
	sum = 1024;
	for (n = 0; n < 1000; n++) {
		u64 next = mul_u64_u32_shr(sum, my_yN_inv[1], 32) + 1024;
		if (next == sum)
			break;
		sum = next;
	}
	P("   收斂的 sum 1024*y^n = %llu（%d 次迭代收斂），核心 LOAD_AVG_MAX = %d\n",
	  sum, n, MY_LOAD_AVG_MAX);
	P("   PELT 分母 divider = LOAD_AVG_MAX - 1024 + period_contrib = %d ~ %d\n",
	  MY_LOAD_AVG_MAX - 1024, MY_LOAD_AVG_MAX - 1);

	/* 一個從 0 開始 100%% 執行的行程：util(n) = 1024 * (1 - y^n) */
	{
		int t50 = -1, t80 = -1, t95 = -1;
		for (n = 0; n < 1024; n++) {
			u64 u = 1024 - my_decay_load(1024, n);
			if (t50 < 0 && u * 100 >= 1024 * 50) t50 = n;
			if (t80 < 0 && u * 100 >= 1024 * 80) t80 = n;
			if (t95 < 0 && u * 100 >= 1024 * 95) t95 = n;
		}
		P("   從 0 開始 100%% 執行 util(n)=1024*(1-y^n)："
		  "%d ms 到 50%%，%d ms 到 80%%，%d ms 到 95%%"
		  "（書中 §8.4：74ms / 139ms）\n", t50, t80, t95);
		P("   反過來：完全「忘掉」歷史負載需要 32*63 = 2016ms\n");
	}
}

/* ------------------------------------------------------------------ */
/* Ch8 Q13：ARM64 ASID                                                 */
/* ------------------------------------------------------------------ */
static void show_asid(void)
{
	u64 mmfr0 = read_sysreg(id_aa64mmfr0_el1);
	u64 ttbr0 = read_sysreg(ttbr0_el1);
	u64 ttbr1 = read_sysreg(ttbr1_el1);
	u64 tcr   = read_sysreg(tcr_el1);
	int asid_bits_field = (mmfr0 >> ID_AA64MMFR0_EL1_ASIDBITS_SHIFT) & 0xf;
	int asid_bits = asid_bits_field == 2 ? 16 : 8;
	struct mm_struct *mm = current->mm;

	P("== Ch8 Q13  ARM64 ASID / TLB ==\n");
	P("   ID_AA64MMFR0_EL1        = 0x%016llx，ASIDBits 欄位 = %d -> 硬體 ASID 寬度 = %d bit（最多 %d 個）\n",
	  mmfr0, asid_bits_field, asid_bits, 1 << asid_bits);
	P("   TCR_EL1                 = 0x%016llx，A1(bit22)=%llu -> ASID 由 %s 提供\n",
	  tcr, (tcr >> 22) & 1, ((tcr >> 22) & 1) ? "TTBR1_EL1" : "TTBR0_EL1");
	P("   TTBR0_EL1               = 0x%016llx  (ASID=%llu, BADDR=0x%llx)\n",
	  ttbr0, ttbr0 >> 48, ttbr0 & GENMASK_ULL(47, 0));
	P("   TTBR1_EL1               = 0x%016llx  (ASID=%llu, BADDR=0x%llx)\n",
	  ttbr1, ttbr1 >> 48, ttbr1 & GENMASK_ULL(47, 0));
#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
	P("   kpti (UNMAP_KERNEL_AT_EL0) = %s -> 每個行程%s\n",
	  arm64_kernel_unmapped_at_el0() ? "啟用" : "已編譯但未啟用",
	  arm64_kernel_unmapped_at_el0() ? "配一對(奇/偶)ASID" : "只配一個 ASID");
#else
	P("   kpti (UNMAP_KERNEL_AT_EL0) = 未編譯 -> 每個行程只配一個 ASID\n");
#endif
	if (mm) {
		u64 id = atomic64_read(&mm->context.id);

		P("   current(%s/%d) mm->context.id = 0x%llx -> 硬體 ASID = %llu，軟體 generation = %llu\n",
		  current->comm, current->pid, id,
		  id & ((1UL << asid_bits) - 1), id >> asid_bits);
		P("   mm->pgd = %px, __pa(pgd) = 0x%llx（應等於 TTBR0_EL1 的 BADDR）\n",
		  mm->pgd, (u64)virt_to_phys(mm->pgd));
	}
}

/* ------------------------------------------------------------------ */
/* Ch8 Q43~45：進程上下文                                              */
/* ------------------------------------------------------------------ */
static void show_context(void)
{
	P("== Ch8 Q43~Q45  進程上下文（硬體上下文） ==\n");
	P("   sizeof(struct task_struct)  = %zu B\n", sizeof(struct task_struct));
	P("   sizeof(struct thread_struct)= %zu B\n", sizeof(struct thread_struct));
	P("   sizeof(struct cpu_context)  = %zu B  (= 13 個 u64：x19~x28, fp, sp, pc)\n",
	  sizeof(struct cpu_context));
	P("   sizeof(struct pt_regs)      = %zu B  (中斷/異常現場，壓在核心堆疊頂端)\n",
	  sizeof(struct pt_regs));
	P("   THREAD_SIZE                 = %lu B  (核心堆疊)\n", (unsigned long)THREAD_SIZE);
	P("   offsetof(task_struct, thread)               = %zu\n",
	  offsetof(struct task_struct, thread));
	P("   offsetof(task_struct, thread.cpu_context)   = %zu  (組語 THREAD_CPU_CONTEXT)\n",
	  offsetof(struct task_struct, thread.cpu_context));
	P("   cpu_context: x19=%zu x28=%zu fp=%zu sp=%zu pc=%zu\n",
	  offsetof(struct cpu_context, x19), offsetof(struct cpu_context, x28),
	  offsetof(struct cpu_context, fp), offsetof(struct cpu_context, sp),
	  offsetof(struct cpu_context, pc));
	P("   current 的 cpu_context.pc = %pS, sp = 0x%lx\n",
	  (void *)current->thread.cpu_context.pc, current->thread.cpu_context.sp);
	P("   current->stack = %px, task_pt_regs(current) = %px\n",
	  current->stack, task_pt_regs(current));
	P("   read_sysreg(sp_el0) = 0x%llx, current = %px  (ARM64 用 SP_EL0 存 task_struct)\n",
	  read_sysreg(sp_el0), current);
}

/* ------------------------------------------------------------------ */
/* Ch8 Q28 / Ch9 Q7：行程的調度欄位                                    */
/* ------------------------------------------------------------------ */
static void dump_task(struct task_struct *p)
{
	P("== 行程 %s/%d 的調度欄位（Ch9 Q7 / Ch8 Q28）==\n", p->comm, p->pid);
	P("   prio=%d  static_prio=%d  normal_prio=%d  rt_priority=%u  nice=%ld\n",
	  p->prio, p->static_prio, p->normal_prio, p->rt_priority,
	  (long)PRIO_TO_NICE(p->static_prio));
	P("   policy=%u  sched_class=%pS\n", p->policy, p->sched_class);
	P("   se.load.weight=%lu  se.load.inv_weight=%u\n",
	  p->se.load.weight, p->se.load.inv_weight);
	P("   se.vruntime=%llu  se.sum_exec_runtime=%llu  se.exec_start=%llu\n",
	  p->se.vruntime, p->se.sum_exec_runtime, p->se.exec_start);
	P("   se.avg: load_sum=%llu runnable_sum=%llu util_sum=%u period_contrib=%u\n",
	  p->se.avg.load_sum, p->se.avg.runnable_sum,
	  p->se.avg.util_sum, p->se.avg.period_contrib);
	P("   se.avg: load_avg=%lu runnable_avg=%lu util_avg=%lu util_est=(%u,%u)\n",
	  p->se.avg.load_avg, p->se.avg.runnable_avg, p->se.avg.util_avg,
	  p->se.avg.util_est.enqueued, p->se.avg.util_est.ewma);
	P("   on_cpu=%d  on_rq=%d  se.on_rq=%d  cpu(task_cpu)=%d  wake_cpu=%d  recent_used_cpu=%d\n",
	  p->on_cpu, p->on_rq, p->se.on_rq, task_cpu(p), p->wake_cpu, p->recent_used_cpu);
	P("   wakee_flips=%u  wakee_flip_decay_ts=%lu  last_wakee=%s\n",
	  p->wakee_flips, p->wakee_flip_decay_ts,
	  p->last_wakee ? p->last_wakee->comm : "(null)");
	P("   nr_cpus_allowed=%d  cpus_mask=%*pbl  se.nr_migrations=%llu\n",
	  p->nr_cpus_allowed, cpumask_pr_args(&p->cpus_mask), p->se.nr_migrations);
	P("   nvcsw=%lu(主動)  nivcsw=%lu(被搶佔)\n", p->nvcsw, p->nivcsw);
}

/* ------------------------------------------------------------------ */
/* Ch9 Q13：關中斷 -> schedule() -> 回來時中斷狀態                     */
/* ------------------------------------------------------------------ */
static void test_irq_across_schedule(void)
{
	int before, inside_after;

	P("== Ch9 Q13  raw_local_irq_disable() 之後直接 schedule() ==\n");
	local_irq_disable();
	before = irqs_disabled();
	set_current_state(TASK_RUNNING);	/* 只是讓出 CPU，不睡眠 */
	schedule();
	inside_after = irqs_disabled();
	local_irq_enable();
	P("   schedule() 之前 irqs_disabled() = %d\n", before);
	P("   schedule() 之後 irqs_disabled() = %d  <-- 中斷已經被打開了！\n", inside_after);
	P("   原因：__schedule() -> context_switch() -> finish_task_switch() ->\n");
	P("         finish_lock_switch() -> raw_spin_unlock_irq(&rq->lock) 會無條件開中斷。\n");
	P("         「關中斷」這個狀態屬於 CPU，不屬於行程；切換出去就不再屬於你。\n");
}

/* ------------------------------------------------------------------ */
/* Ch9 Q12：中斷（軟中斷/timer）上下文呼叫可睡眠函式                   */
/* ------------------------------------------------------------------ */
static struct timer_list probe_timer;
static DECLARE_COMPLETION(timer_done);

static void probe_timer_fn(struct timer_list *t)
{
	pr_info("sched_probe: [timer softirq] in_interrupt()=%lu preempt_count=0x%x\n",
		in_interrupt(), preempt_count());
	pr_info("sched_probe: [timer softirq] 接下來呼叫 might_sleep()，"
		"這正是 schedule()/mutex_lock() 內部的檢查：\n");
	pr_info("sched_probe: [timer softirq] in_atomic()=%d -> "
		"若此時呼叫 schedule()，schedule_debug() 會噴 "
		"\"BUG: scheduling while atomic\"\n", in_atomic());
	might_sleep();	/* CONFIG_DEBUG_ATOMIC_SLEEP=n 時是空操作 */
	complete(&timer_done);
}

static void test_atomic_sleep(void)
{
	P("== Ch9 Q12  在原子上下文呼叫 schedule() ==\n");
	P("   (a) 先看軟中斷（timer）上下文的 preempt_count：\n");
	timer_setup(&probe_timer, probe_timer_fn, 0);
	mod_timer(&probe_timer, jiffies + msecs_to_jiffies(20));
	wait_for_completion(&timer_done);
	del_timer_sync(&probe_timer);

	/*
	 * (b) 在「行程上下文」裡故意讓 preempt_count != 0 再呼叫 schedule()，
	 *     觸發 kernel/sched/core.c:__schedule_bug() 的
	 *     "BUG: scheduling while atomic"。
	 *     這是安全的：我們有自己的核心堆疊，不是真的在中斷上下文。
	 *     注意 schedule_debug() 會把 preempt_count 重設成 PREEMPT_DISABLED，
	 *     所以回來之後 preempt_count 已經是 0，不可以再 preempt_enable()。
	 */
	P("   (b) 行程上下文 + preempt_disable() + schedule() -> 應該噴 BUG splat：\n");
	preempt_disable();
	P("       呼叫前 preempt_count = 0x%x, in_atomic() = %d\n",
	  preempt_count(), in_atomic());
	schedule();
	P("       呼叫後 preempt_count = 0x%x  <-- 被 schedule_debug() 重設過了\n",
	  preempt_count());
	if (preempt_count())
		preempt_enable();
}

/* ------------------------------------------------------------------ */
static int __init sched_probe_init(void)
{
	P("---- RK3588 排程器探針，kernel %s ----\n", UTS_RELEASE);
	show_weight_table();
	show_calc_delta();
	show_pelt();
	show_asid();
	show_context();
	dump_task(current);

	if (target_pid) {
		struct pid *pid = find_get_pid(target_pid);
		struct task_struct *p = pid ? get_pid_task(pid, PIDTYPE_PID) : NULL;

		if (p) {
			dump_task(p);
			put_task_struct(p);
		} else {
			P("找不到 pid %d\n", target_pid);
		}
		if (pid)
			put_pid(pid);
	}
	if (test_irq)
		test_irq_across_schedule();
	if (test_atomic)
		test_atomic_sleep();

	P("---- done, 用 rmmod 移除 ----\n");
	return 0;
}

static void __exit sched_probe_exit(void)
{
	P("bye\n");
}

module_init(sched_probe_init);
module_exit(sched_probe_exit);
