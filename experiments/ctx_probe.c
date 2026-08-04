// SPDX-License-Identifier: GPL-2.0
/*
 * ctx_probe.ko —— 硬中斷 / 軟中斷 / 行程 三種上下文的「指紋」實測
 * 平台：Radxa ROCK 5B (RK3588)，Linux 6.1.115+ aarch64
 *
 * 在六個不同的執行環境裡採同一組樣本（preempt_count、in_*() 巨集、DAIF、
 * SP 落在哪個棧、current 是誰），排成一張表，用來回答：
 *
 *   Q4  為什麼中斷上下文不能睡眠
 *   Q5  軟中斷的回呼執行時是否允許回應本地中斷
 *   Q7  軟中斷上下文包括哪幾種情況
 *   Q8  軟中斷上下文和行程上下文誰優先
 *   Q10 工作佇列跑在哪種上下文、回呼能不能睡
 *
 * mode=0  上下文指紋表（六種上下文）
 * mode=1  Q5：tasklet 回呼裡忙等 20 ms，數這段期間本 CPU 收到幾次硬體中斷
 * mode=2  Q8：軟中斷是否搶在被中斷行程之前執行（時間戳三段式）
 * mode=3  Q10：工作佇列回呼裡真的 msleep()
 *
 * 用法：
 *   sudo insmod ctx_probe.ko mode=0 ; sudo rmmod ctx_probe
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/hrtimer.h>
#include <linux/irq_work.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/completion.h>
#include <linux/ktime.h>
#include <linux/bottom_half.h>
#include <linux/smp.h>
#include <asm/sysreg.h>
#include <asm/memory.h>
#include "ksym.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Interrupt/softirq/process context fingerprints on RK3588");

static int mode;
static int cpu_target = 3;
static int spin_ms = 20;
module_param(mode, int, 0444);
module_param(cpu_target, int, 0444);
module_param(spin_ms, int, 0444);

#define P(fmt, ...) pr_info("ctx_probe: " fmt, ##__VA_ARGS__)

/* 這兩個在 aarch64 都沒有 EXPORT，用 kallsyms 取（見 ksym.h） */
static struct irq_desc *(*p_irq_to_desc)(unsigned int irq);
static bool (*p_irq_work_queue_on)(struct irq_work *work, int cpu);
#define irq_to_desc(i)          p_irq_to_desc(i)
#define irq_work_queue_on(w, c) p_irq_work_queue_on(w, c)

/* ================= 上下文指紋 ================= */

struct fingerprint {
	char		what[40];
	int		cpu;
	char		comm[TASK_COMM_LEN];
	pid_t		pid;
	unsigned int	pc;		/* preempt_count */
	unsigned long	daif;
	int		irqs_off;
	int		hardirq, softirq, serving, interrupt, task;
	unsigned long	sp;
	int		on_task_stack;
	int		valid;
};

static struct fingerprint fp[8];
static int nfp;

static void take(const char *what)
{
	struct fingerprint *f;
	unsigned long tlo;

	if (nfp >= ARRAY_SIZE(fp))
		return;
	f = &fp[nfp++];
	strscpy(f->what, what, sizeof(f->what));
	f->cpu       = raw_smp_processor_id();
	strscpy(f->comm, current->comm, sizeof(f->comm));
	f->pid       = current->pid;
	f->pc        = preempt_count();
	f->daif      = (unsigned long)read_sysreg(daif);
	f->irqs_off  = irqs_disabled();
	f->hardirq   = !!in_hardirq();
	f->softirq   = !!in_softirq();
	f->serving   = !!in_serving_softirq();
	f->interrupt = !!in_interrupt();
	f->task      = !!in_task();
	f->sp        = current_stack_pointer;
	tlo          = (unsigned long)current->stack;
	f->on_task_stack = (f->sp >= tlo && f->sp < tlo + THREAD_SIZE);
	f->valid     = 1;
}

static void print_table(void)
{
	int i;

	P("");
	P("======================= 上下文指紋表 =======================");
	P("%-34s %-4s %-18s %-10s %-4s %s",
	  "執行環境", "cpu", "current", "preempt_cnt", "IRQ", "in_hardirq/in_softirq/in_serving/in_interrupt/in_task");
	for (i = 0; i < nfp; i++) {
		struct fingerprint *f = &fp[i];

		if (!f->valid)
			continue;
		P("%-34s %-4d %-18s 0x%08x %-4s %d / %d / %d / %d / %d",
		  f->what, f->cpu, f->comm, f->pc,
		  f->irqs_off ? "關" : "開",
		  f->hardirq, f->softirq, f->serving, f->interrupt, f->task);
	}
	P("");
	P("%-34s %-18s %-8s %s", "執行環境", "SP", "在行程棧?", "DAIF");
	for (i = 0; i < nfp; i++) {
		struct fingerprint *f = &fp[i];

		if (!f->valid)
			continue;
		P("%-34s 0x%016lx %-8s 0x%08lx",
		  f->what, f->sp, f->on_task_stack ? "是" : "否(中斷棧)", f->daif);
	}
	P("");
	P("解讀：");
	P("  preempt_count 的 bit[19:16] = HARDIRQ、bit[15:8] = SOFTIRQ");
	P("  0x00010000 = HARDIRQ_OFFSET  → 硬中斷上下文");
	P("  0x00000100 = SOFTIRQ_OFFSET  → 正在跑軟中斷回呼（in_serving_softirq）");
	P("  0x00000200 = SOFTIRQ_DISABLE_OFFSET → local_bh_disable() 的關 BH 臨界區");
	P("");
}

/* ================= 各上下文的取樣點 ================= */

static struct irq_work iw;
static struct hrtimer ht;
/* 只清理這一輪 mode 真的初始化過的東西 —— 對沒 init 過的 hrtimer 呼叫
 * hrtimer_cancel() 會踩到 NULL base（第一版就是這樣讓 rmmod segfault 的） */
static bool ht_inited, iw_inited, tl_inited;
static DECLARE_COMPLETION(iw_done);
static DECLARE_COMPLETION(ht_done);
static DECLARE_COMPLETION(tl_done);
static DECLARE_COMPLETION(wk_done);
static DECLARE_COMPLETION(bh_done);

static void iw_fn(struct irq_work *w)
{
	take("硬中斷: irq_work (SGI/IPI)");
	complete(&iw_done);
}

/* hrtimer 在非 PREEMPT_RT 上預設跑在 hrtimer_interrupt() 的硬中斷上下文裡 */
static struct tasklet_struct tl;
static enum hrtimer_restart ht_fn(struct hrtimer *t)
{
	take("硬中斷: hrtimer 回呼");
	/* 就在硬中斷上下文裡觸發 tasklet：中斷返回時 irq_exit() 會看到 pending，
	 * 走 invoke_softirq() → __do_softirq()，這才是「軟中斷上下文①」 */
	tasklet_schedule(&tl);
	complete(&ht_done);
	return HRTIMER_NORESTART;
}

static void tl_fn(struct tasklet_struct *t);
static int tl_from_hardirq = 1;

static void tl_fn(struct tasklet_struct *t)
{
	take(tl_from_hardirq ? "軟中斷①: irq_exit→__do_softirq"
			     : "軟中斷③: local_bh_enable→do_softirq");
	complete(&tl_done);
}

static void wk_fn(struct work_struct *w)
{
	take("行程: workqueue kworker");
	complete(&wk_done);
}
static DECLARE_WORK(wk, wk_fn);

/* 專門用來示範「在關 BH 臨界區裡」的指紋 */
static void bh_probe(void)
{
	local_bh_disable();
	take("行程: local_bh_disable 臨界區內");
	local_bh_enable();
}

/* 第二個 tasklet，專門讓它在 ksoftirqd 裡跑（軟中斷上下文②） */
static void tl2_fn(struct tasklet_struct *t)
{
	take("軟中斷②: ksoftirqd? (視實際)");
	complete(&bh_done);
}
static DECLARE_TASKLET(tl2, tl2_fn);

static void run_mode0(void)
{
	P("mode=0：採集六種執行環境的上下文指紋");

	/* ① 行程上下文 */
	take("行程: insmod 系統呼叫路徑");

	/* ② 關 BH 臨界區 */
	bh_probe();

	/* ③ 硬中斷：irq_work → 送一個 SGI 給指定 CPU（/proc/interrupts 的 IPI5） */
	init_irq_work(&iw, iw_fn);
	iw_inited = true;
	irq_work_queue_on(&iw, cpu_target);
	wait_for_completion_timeout(&iw_done, HZ);

	/* ④ 硬中斷：hrtimer 回呼；⑤ 軟中斷①：由 ④ 在硬中斷裡 tasklet_schedule()，
	 *    中斷返回時 irq_exit() → invoke_softirq() → __do_softirq() 直接跑掉 */
	tasklet_setup(&tl, tl_fn);
	tl_inited = true;
	tl_from_hardirq = 1;
	reinit_completion(&tl_done);
	hrtimer_init(&ht, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ht_inited = true;
	ht.function = ht_fn;
	hrtimer_start(&ht, ms_to_ktime(5), HRTIMER_MODE_REL);
	wait_for_completion_timeout(&ht_done, HZ);
	wait_for_completion_timeout(&tl_done, HZ);

	/* ⑥ 軟中斷③：在 local_bh_disable()/enable() 之間 raise，
	 *    → local_bh_enable() 內部直接同步跑掉，comm 會是 insmod 自己 */
	tl_from_hardirq = 0;
	reinit_completion(&tl_done);
	local_bh_disable();
	tasklet_schedule(&tl);
	local_bh_enable();		/* 這行裡面就會把 tasklet 跑完 */
	wait_for_completion_timeout(&tl_done, HZ);

	/* ⑦ 軟中斷②：純行程上下文 raise → raise_softirq_irqoff() 會 wakeup_softirqd() */
	tasklet_schedule(&tl2);
	wait_for_completion_timeout(&bh_done, HZ);

	/* ⑧ 行程：workqueue */
	schedule_work(&wk);
	wait_for_completion_timeout(&wk_done, HZ);

	print_table();
}

/* ================= mode=1：Q5 軟中斷回呼期間本地中斷是開的 ================= */

static int find_irq_by_name(const char *name)
{
	unsigned int irq;

	for (irq = 0; irq < nr_irqs; irq++) {
		struct irq_desc *d = irq_to_desc(irq);

		if (d && d->action && d->action->name &&
		    !strcmp(d->action->name, name))
			return irq;
	}
	return -1;
}

static unsigned long irq_count_on(int virq, int cpu)
{
	struct irq_desc *d = irq_to_desc(virq);

	if (!d || !d->kstat_irqs)
		return 0;
	return *per_cpu_ptr(d->kstat_irqs, cpu);
}

static int timer_virq;
static unsigned long c_before, c_after;
static unsigned long daif_in_tasklet;
static int irqs_off_in_tasklet;
static u64 ns_spun;

static void tl_spin_fn(struct tasklet_struct *t)
{
	ktime_t t0 = ktime_get();
	int cpu = raw_smp_processor_id();

	daif_in_tasklet = (unsigned long)read_sysreg(daif);
	irqs_off_in_tasklet = irqs_disabled();
	c_before = irq_count_on(timer_virq, cpu);

	/* 忙等 spin_ms 毫秒，不睡；期間如果本地中斷是開的，時鐘中斷會照樣進來 */
	while (ktime_to_ns(ktime_sub(ktime_get(), t0)) < (s64)spin_ms * NSEC_PER_MSEC)
		cpu_relax();

	c_after = irq_count_on(timer_virq, cpu);
	ns_spun = ktime_to_ns(ktime_sub(ktime_get(), t0));
	take("軟中斷: tasklet 忙等中");
	complete(&tl_done);
}
static DECLARE_TASKLET(tl_spin, tl_spin_fn);

static void run_mode1(void)
{
	int cpu;

	timer_virq = find_irq_by_name("arch_timer");
	P("mode=1：Q5 —— 軟中斷回呼執行期間本地中斷是否開著？");
	P("找到 arch_timer 的 virq = %d（PPI，每 CPU 私有）", timer_virq);
	if (timer_virq < 0) {
		P("找不到 arch_timer，改用 rk_timer");
		timer_virq = find_irq_by_name("rk_timer");
	}
	P("在 tasklet（TASKLET_SOFTIRQ）回呼裡忙等 %d ms，比對本 CPU 的 arch_timer 計數",
	  spin_ms);

	reinit_completion(&tl_done);
	tasklet_schedule(&tl_spin);
	wait_for_completion_timeout(&tl_done, 5 * HZ);

	cpu = fp[nfp - 1].cpu;
	P("");
	P("  tasklet 執行在 cpu%d，實際忙等 %llu ns", cpu, ns_spun);
	P("  進入 tasklet 時 DAIF = 0x%08lx   irqs_disabled() = %d",
	  daif_in_tasklet, irqs_off_in_tasklet);
	P("  arch_timer 在 cpu%d 的計數： %lu → %lu   （期間收到 %lu 次硬體中斷）",
	  cpu, c_before, c_after, c_after - c_before);
	P("");
	if (!irqs_off_in_tasklet && c_after > c_before)
		P("  ✓ 結論：軟中斷回呼是在【開中斷】的環境下執行的，期間硬體中斷照樣被回應。");
	else
		P("  ？結論：與預期不符，請看上面的原始數據。");
	print_table();
}

/* ================= mode=2：Q8 軟中斷 vs 行程優先級 ================= */

static struct task_struct *victim;
static volatile u64 victim_last_ns;
static volatile u64 victim_gap_ns;
static volatile u64 victim_gap_at;
static volatile int victim_stop;
static u64 t_hardirq, t_soft_in, t_soft_out;

static int victim_fn(void *arg)
{
	u64 prev = ktime_get_ns();

	while (!victim_stop && !kthread_should_stop()) {
		u64 now = ktime_get_ns();
		u64 d = now - prev;

		if (d > victim_gap_ns) {
			victim_gap_ns = d;
			victim_gap_at = now;
		}
		prev = now;
		victim_last_ns = now;
		cpu_relax();
	}
	return 0;
}

static void tl_block_fn(struct tasklet_struct *t)
{
	ktime_t t0;

	t_soft_in = ktime_get_ns();
	t0 = ktime_get();
	while (ktime_to_ns(ktime_sub(ktime_get(), t0)) < (s64)spin_ms * NSEC_PER_MSEC)
		cpu_relax();
	t_soft_out = ktime_get_ns();
	take("軟中斷: tasklet（搶了行程的 CPU）");
	complete(&tl_done);
}
static DECLARE_TASKLET(tl_block, tl_block_fn);

static enum hrtimer_restart ht_raise_fn(struct hrtimer *tm)
{
	t_hardirq = ktime_get_ns();
	take("硬中斷: hrtimer（打斷了行程）");
	tasklet_schedule(&tl_block);
	return HRTIMER_NORESTART;
}

static void start_pinned_timer(void *unused)
{
	hrtimer_start(&ht, ms_to_ktime(20), HRTIMER_MODE_REL_PINNED);
}

static void run_mode2(void)
{
	P("mode=2：Q8 —— 軟中斷上下文 vs 行程上下文，誰先跑？");
	P("作法：在 cpu%d 上跑一個死迴圈 kthread（行程上下文，SCHED_OTHER），", cpu_target);
	P("      再讓一個 hrtimer 在同一顆 CPU 上打斷它，硬中斷裡 tasklet_schedule()，");
	P("      tasklet 回呼忙等 %d ms。看那個 kthread 的迴圈被卡住多久。", spin_ms);

	victim_stop = 0;
	victim_gap_ns = 0;
	victim = kthread_create(victim_fn, NULL, "ctxprobe_victim");
	if (IS_ERR(victim)) {
		P("kthread_create 失敗");
		return;
	}
	kthread_bind(victim, cpu_target);
	wake_up_process(victim);
	msleep(50);			/* 讓它先跑穩 */
	victim_gap_ns = 0;

	hrtimer_init(&ht, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	ht_inited = true;
	ht.function = ht_raise_fn;
	reinit_completion(&tl_done);
	/* 一定要在目標 CPU 上呼叫 hrtimer_start()，PINNED 才會綁到那顆 CPU */
	smp_call_function_single(cpu_target, start_pinned_timer, NULL, 1);
	wait_for_completion_timeout(&tl_done, 5 * HZ);
	msleep(20);
	victim_stop = 1;
	kthread_stop(victim);

	P("");
	P("  t_hardirq  (hrtimer 回呼)      = %llu ns", t_hardirq);
	P("  t_softirq_in  (tasklet 開始)   = %llu ns  （+%lld ns）",
	  t_soft_in, (long long)(t_soft_in - t_hardirq));
	P("  t_softirq_out (tasklet 結束)   = %llu ns  （+%lld ns）",
	  t_soft_out, (long long)(t_soft_out - t_hardirq));
	P("  被中斷的 kthread 迴圈最大空窗 = %llu ns (%llu ms)",
	  victim_gap_ns, victim_gap_ns / NSEC_PER_MSEC);
	P("");
	if (victim_gap_ns >= (u64)spin_ms * NSEC_PER_MSEC)
		P("  ✓ 結論：行程整整被卡住 %llu ms ≥ tasklet 的 %d ms。",
		  victim_gap_ns / NSEC_PER_MSEC, spin_ms);
	else
		P("  ？結論：空窗小於預期，看原始數據。");
	P("    中斷返回時 irq_exit() 先跑軟中斷、跑完才輪到被中斷的行程，");
	P("    所以【軟中斷上下文的優先級高於行程上下文】，軟中斷永遠搶行程。");
	print_table();
}

/* ================= mode=3：Q10 工作佇列回呼真的能睡 ================= */

static u64 wk_t0, wk_t1;
static int wk_slept_ok;

static void wk_sleep_fn(struct work_struct *w)
{
	take("行程: workqueue 回呼（睡之前）");
	wk_t0 = ktime_get_ns();
	msleep(120);				/* ← 真的睡：中斷/軟中斷上下文做這個會 BUG */
	wk_t1 = ktime_get_ns();
	wk_slept_ok = 1;
	take("行程: workqueue 回呼（睡醒後）");
	complete(&wk_done);
}
static DECLARE_WORK(wk_sleep, wk_sleep_fn);

static void run_mode3(void)
{
	P("mode=3：Q10 —— 工作佇列回呼跑在行程上下文，而且真的可以睡");
	take("行程: insmod");
	reinit_completion(&wk_done);
	schedule_work(&wk_sleep);
	wait_for_completion_timeout(&wk_done, 5 * HZ);
	P("");
	P("  msleep(120) %s，實際睡了 %llu ns (%llu ms)",
	  wk_slept_ok ? "成功返回" : "沒有返回",
	  wk_t1 - wk_t0, (wk_t1 - wk_t0) / NSEC_PER_MSEC);
	P("  → 回呼跑在 kworker 內核執行緒裡，preempt_count 的 HARDIRQ/SOFTIRQ 欄位都是 0，");
	P("    in_task()=1，所以 schedule() 是合法的。");
	print_table();
}

static int __init ctx_probe_init(void)
{
	int r = exp_ksym_init();

	if (r) {
		pr_err("ctx_probe: 取不到 kallsyms_lookup_name: %d\n", r);
		return r;
	}
	p_irq_to_desc       = EXP_LOOKUP("irq_to_desc");
	p_irq_work_queue_on = EXP_LOOKUP("irq_work_queue_on");
	if (!p_irq_to_desc || !p_irq_work_queue_on) {
		pr_err("ctx_probe: kallsyms 找不到 irq_to_desc / irq_work_queue_on\n");
		return -ENOENT;
	}

	P("================================================================");
	P("ctx_probe.ko  mode=%d  cpu_target=%d  spin_ms=%d", mode, cpu_target, spin_ms);
	P("================================================================");
	switch (mode) {
	case 0: run_mode0(); break;
	case 1: run_mode1(); break;
	case 2: run_mode2(); break;
	case 3: run_mode3(); break;
	default: P("未知 mode"); break;
	}
	return 0;
}

static void __exit ctx_probe_exit(void)
{
	if (tl_inited)
		tasklet_kill(&tl);
	tasklet_kill(&tl2);
	tasklet_kill(&tl_spin);
	tasklet_kill(&tl_block);
	if (ht_inited)
		hrtimer_cancel(&ht);
	if (iw_inited)
		irq_work_sync(&iw);
	flush_scheduled_work();
	P("卸載完畢");
}

module_init(ctx_probe_init);
module_exit(ctx_probe_exit);
