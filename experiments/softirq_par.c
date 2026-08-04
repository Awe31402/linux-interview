// SPDX-License-Identifier: GPL-2.0
/*
 * softirq_par.ko —— 「同一類軟中斷可以多 CPU 並行」vs「同一個 tasklet 只能串行」
 * 平台：Radxa ROCK 5B (RK3588, 8 核)，Linux 6.1.115+ aarch64
 *
 * mode=0  Q6：8 個【不同的 tasklet】同時掛到 8 顆 CPU 的 tasklet_vec，
 *             量測 TASKLET_SOFTIRQ 這一種軟中斷同時有幾顆 CPU 在跑。
 * mode=1  Q9：【同一個 tasklet】從 8 顆 CPU 一起 tasklet_schedule()，
 *             量測它同一時刻最多有幾顆 CPU 在執行回呼（答案應該是 1）。
 * mode=2  Q9 進階：回呼還在 CPU-A 上忙等時，從別的 CPU 再 schedule 一次，
 *             重現書上 §2.6.2 那張 CPU0/CPU1 時序圖（TASKLET_STATE_RUN 搶不到鎖 → 重新掛回）。
 *
 * 用法：
 *   sudo insmod softirq_par.ko mode=0 ; sudo rmmod softirq_par
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/atomic.h>
#include <linux/sched.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("softirq parallelism vs tasklet serialization on RK3588");

static int mode;
static int spin_ms = 30;
module_param(mode, int, 0444);
module_param(spin_ms, int, 0444);

#define P(fmt, ...) pr_info("softirq_par: " fmt, ##__VA_ARGS__)

static atomic_t cur = ATOMIC_INIT(0);
static atomic_t maxcur = ATOMIC_INIT(0);
static atomic_t runs = ATOMIC_INIT(0);
static atomic_t sched_calls = ATOMIC_INIT(0);
static unsigned long cpu_seen;		/* bitmap of CPUs the callback ran on */
static u64 t_first, t_last;

static void bump(void)
{
	int c = atomic_inc_return(&cur);
	int m;

	do {
		m = atomic_read(&maxcur);
		if (c <= m)
			break;
	} while (atomic_cmpxchg(&maxcur, m, c) != m);
}

static void spin_for(int ms)
{
	ktime_t t0 = ktime_get();

	while (ktime_to_ns(ktime_sub(ktime_get(), t0)) < (s64)ms * NSEC_PER_MSEC)
		cpu_relax();
}

static void reset_stats(void)
{
	atomic_set(&cur, 0);
	atomic_set(&maxcur, 0);
	atomic_set(&runs, 0);
	atomic_set(&sched_calls, 0);
	cpu_seen = 0;
	t_first = t_last = 0;
}

/* ============ mode=0：Q6 —— 8 個不同 tasklet，同一類軟中斷並行 ============ */

#define MAXCPU 8
static struct tasklet_struct tl_many[MAXCPU];
static int done_cnt[MAXCPU];

static void many_fn(struct tasklet_struct *t)
{
	int cpu = raw_smp_processor_id();
	u64 now = ktime_get_ns();

	if (!t_first)
		t_first = now;
	set_bit(cpu, &cpu_seen);
	done_cnt[cpu]++;
	bump();
	spin_for(spin_ms);
	atomic_dec(&cur);
	atomic_inc(&runs);
	t_last = ktime_get_ns();
}

static void sched_many(void *unused)
{
	/* 這裡跑在 IPI 的【硬中斷上下文】裡，tasklet 會掛到本 CPU 的 tasklet_vec，
	 * 中斷返回時 irq_exit() → invoke_softirq() → __do_softirq() 就會跑它 */
	atomic_inc(&sched_calls);
	tasklet_schedule(&tl_many[raw_smp_processor_id()]);
}

static void run_mode0(void)
{
	int cpu, i;

	P("mode=0：Q6 —— 同一類型的軟中斷（TASKLET_SOFTIRQ）能不能多 CPU 並行？");
	P("作法：準備 %d 個【不同的】tasklet，用 on_each_cpu() 讓每顆 CPU 各掛一個，",
	  num_online_cpus());
	P("      每個回呼忙等 %d ms，量最大同時執行數。", spin_ms);

	reset_stats();
	for (i = 0; i < MAXCPU; i++) {
		tasklet_setup(&tl_many[i], many_fn);
		done_cnt[i] = 0;
	}

	on_each_cpu(sched_many, NULL, 0);
	msleep(spin_ms * 3 + 200);

	P("");
	P("  tasklet_schedule() 呼叫次數 = %d", atomic_read(&sched_calls));
	P("  回呼實際執行次數            = %d", atomic_read(&runs));
	P("  最大同時執行數              = %d   ← 這就是同時在跑 TASKLET_SOFTIRQ 的 CPU 數",
	  atomic_read(&maxcur));
	P("  執行過回呼的 CPU 位圖       = 0x%02lx", cpu_seen);
	for_each_online_cpu(cpu)
		P("    cpu%d: %d 次", cpu, done_cnt[cpu]);
	P("  全部完成耗時                = %llu ms（若是串行應該要 %d ms）",
	  (t_last - t_first) / NSEC_PER_MSEC, spin_ms * atomic_read(&runs));
	P("");
	if (atomic_read(&maxcur) > 1)
		P("  ✓ 結論：同一類型的軟中斷【可以】在多顆 CPU 上並行執行（實測 %d 顆同時）。",
		  atomic_read(&maxcur));
	else
		P("  ？只量到 1，可能是 tasklet 沒有同時被觸發，重跑或加大 spin_ms。");
	P("    所以自己新增軟中斷類型時，回呼函式必須自己處理多核同步問題。");
}

/* ============ mode=1/2：Q9 —— 同一個 tasklet ============ */

static struct tasklet_struct tl_one;
static unsigned long state_at_entry;
static int per_cpu_runs[MAXCPU];

static void one_fn(struct tasklet_struct *t)
{
	int cpu = raw_smp_processor_id();
	u64 now = ktime_get_ns();

	state_at_entry = t->state;
	if (!t_first)
		t_first = now;
	set_bit(cpu, &cpu_seen);
	per_cpu_runs[cpu]++;
	bump();
	spin_for(spin_ms);
	atomic_dec(&cur);
	atomic_inc(&runs);
	t_last = ktime_get_ns();
}

static void sched_one(void *unused)
{
	atomic_inc(&sched_calls);
	tasklet_schedule(&tl_one);
}

static void run_mode1(void)
{
	int cpu;

	P("mode=1：Q9 —— 同一個 tasklet 能不能在多個 CPU 上並行？");
	P("作法：一個 tasklet，用 on_each_cpu() 讓 %d 顆 CPU 同時 tasklet_schedule() 它，",
	  num_online_cpus());
	P("      回呼忙等 %d ms，量最大同時執行數。", spin_ms);

	reset_stats();
	memset(per_cpu_runs, 0, sizeof(per_cpu_runs));
	tasklet_setup(&tl_one, one_fn);

	on_each_cpu(sched_one, NULL, 0);
	msleep(spin_ms * 4 + 300);

	P("");
	P("  tasklet_schedule() 呼叫次數 = %d（%d 顆 CPU 各一次）",
	  atomic_read(&sched_calls), num_online_cpus());
	P("  回呼實際執行次數            = %d", atomic_read(&runs));
	P("  最大同時執行數              = %d", atomic_read(&maxcur));
	P("  執行過回呼的 CPU 位圖       = 0x%02lx", cpu_seen);
	for_each_online_cpu(cpu)
		if (per_cpu_runs[cpu])
			P("    cpu%d: %d 次", cpu, per_cpu_runs[cpu]);
	P("  進入回呼時 t->state = 0x%lx  (bit0=TASKLET_STATE_SCHED, bit1=TASKLET_STATE_RUN)",
	  state_at_entry);
	P("");
	P("  → %d 次 tasklet_schedule() 只換到 %d 次執行：",
	  atomic_read(&sched_calls), atomic_read(&runs));
	P("    tasklet_schedule() 先 test_and_set_bit(TASKLET_STATE_SCHED)，");
	P("    已經被掛上的就直接返回，所以重複觸發會被吃掉。");
	if (atomic_read(&maxcur) == 1)
		P("  ✓ 結論：同一個 tasklet【絕不會】在多顆 CPU 上並行，最大同時執行數 = 1。");
	else
		P("  ？量到 %d，與預期不符。", atomic_read(&maxcur));
}

/* mode=2：回呼還在跑的時候，從別的 CPU 再 schedule 一次 */

static atomic_t late_sched_ok = ATOMIC_INIT(0);
static unsigned long state_during_run;

static void one2_fn(struct tasklet_struct *t)
{
	int cpu = raw_smp_processor_id();

	set_bit(cpu, &cpu_seen);
	per_cpu_runs[cpu]++;
	bump();
	/* 回呼一進來，TASKLET_STATE_SCHED 已經被 tasklet_action_common() 清掉，
	 * TASKLET_STATE_RUN 則已被 tasklet_trylock() 設起來。 */
	state_during_run = t->state;
	spin_for(spin_ms);
	atomic_dec(&cur);
	atomic_inc(&runs);
	t_last = ktime_get_ns();
}

static void late_sched(void *unused)
{
	atomic_inc(&sched_calls);
	tasklet_schedule(&tl_one);
	atomic_inc(&late_sched_ok);
}

static void run_mode2(void)
{
	int cpu, me, other;

	P("mode=2：Q9 進階 —— 重現書上 §2.6.2 的 CPU0/CPU1 時序");
	P("作法：先讓 tasklet 在 cpu0 上跑起來並忙等 %d ms，", spin_ms);
	P("      中途從別的 CPU 再 tasklet_schedule() 一次 —— 這次 TASKLET_STATE_SCHED");
	P("      已經被清掉，所以掛得進去；但 tasklet_action_common() 拿不到");
	P("      tasklet_trylock()（TASKLET_STATE_RUN 還在），只好把它重新掛回本 CPU 的鏈結串列。");

	reset_stats();
	memset(per_cpu_runs, 0, sizeof(per_cpu_runs));
	atomic_set(&late_sched_ok, 0);
	tasklet_setup(&tl_one, one2_fn);

	me = 0;
	other = num_online_cpus() > 4 ? 4 : 1;

	smp_call_function_single(me, sched_one, NULL, 0);
	msleep(spin_ms / 3 + 2);		/* 等它跑起來 */
	P("");
	P("  忙等中：t->state = 0x%lx（RUN=bit1 已設，SCHED=bit0 已清）", state_during_run);
	smp_call_function_single(other, late_sched, NULL, 0);
	msleep(spin_ms * 4 + 300);

	P("");
	P("  tasklet_schedule() 呼叫次數 = %d", atomic_read(&sched_calls));
	P("  回呼實際執行次數            = %d", atomic_read(&runs));
	P("  最大同時執行數              = %d", atomic_read(&maxcur));
	for_each_online_cpu(cpu)
		if (per_cpu_runs[cpu])
			P("    cpu%d: %d 次", cpu, per_cpu_runs[cpu]);
	P("");
	P("  ✓ 第二次 schedule 掛在 cpu%d 上，但它必須等 cpu%d 上的回呼跑完、", other, me);
	P("    清掉 TASKLET_STATE_RUN 之後，下一輪 TASKLET_SOFTIRQ 才會真的執行 ——");
	P("    最大同時執行數依然是 %d。", atomic_read(&maxcur));
}

static int __init softirq_par_init(void)
{
	P("================================================================");
	P("softirq_par.ko  mode=%d  spin_ms=%d  online_cpus=%d",
	  mode, spin_ms, num_online_cpus());
	P("================================================================");
	switch (mode) {
	case 0: run_mode0(); break;
	case 1: run_mode1(); break;
	case 2: run_mode2(); break;
	default: P("未知 mode"); break;
	}
	return 0;
}

static void __exit softirq_par_exit(void)
{
	int i;

	for (i = 0; i < MAXCPU; i++)
		tasklet_kill(&tl_many[i]);
	tasklet_kill(&tl_one);
	P("卸載完畢");
}

module_init(softirq_par_init);
module_exit(softirq_par_exit);
