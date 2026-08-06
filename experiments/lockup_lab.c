// SPDX-License-Identifier: GPL-2.0
/*
 * lockup_lab.c —— 自己動手做 Softlockup / Hardlockup / hung_task 三個偵測器
 *                （卷2 第4章 Q7、Q8、Q9）
 *
 * 為什麼要自己做？本機（RK3588 + Rockchip 6.1.115）三個機制全部沒編進去：
 *     # CONFIG_SOFTLOCKUP_DETECTOR is not set
 *     # CONFIG_HARDLOCKUP_DETECTOR is not set
 *     # CONFIG_DETECT_HUNG_TASK is not set
 * 所以 /proc/sys/kernel/{watchdog_thresh,softlockup_panic,hung_task_timeout_secs}
 * 通通不存在，也沒有 khungtaskd。這支模組把三個機制的核心演算法照著
 * kernel/watchdog.c 與 kernel/hung_task.c 各實作一份迷你版，再故意製造三種
 * lockup 讓它們抓，藉此把「原理」跑成看得見的 dmesg。
 *
 * 對照本機核心原始碼（6.1）：
 *   softlockup：kernel/watchdog.c:220 sample_period、:269 get_softlockup_thresh、
 *               :358 is_softlockup、:456 __this_cpu_inc(hrtimer_interrupts)、
 *               :470 softlockup_fn、:479 watchdog_timer_fn
 *   hardlockup（本核心是「鄰居 CPU 互相檢查」版本，不是 NMI 版）：
 *               kernel/watchdog.c:382 watchdog_next_cpu、:398 is_hardlockup_other_cpu、
 *               :411 watchdog_check_hardlockup_other_cpu
 *               （NMI 版在 kernel/watchdog_hld.c:103 PERF_COUNT_HW_CPU_CYCLES）
 *   hung_task： kernel/hung_task.c:90 check_hung_task、:178
 *               check_hung_uninterruptible_tasks、:359 watchdog（khungtaskd 本體）
 *
 * 用法（全部輸出到 dmesg）：
 *   sudo insmod lockup_lab.ko
 *   echo 'thresh 5'    | sudo tee /proc/lockup_lab   # 相當於 watchdog_thresh=5
 *   echo 'wd on'       | sudo tee /proc/lockup_lab   # 開迷你 softlockup+hardlockup 偵測器
 *   echo 'khung on 10' | sudo tee /proc/lockup_lab   # 開迷你 khungtaskd（timeout 10 秒）
 *   echo 'soft 3 25'   | sudo tee /proc/lockup_lab   # 在 CPU3 關搶佔死迴圈 25 秒
 *   echo 'hard 3 12'   | sudo tee /proc/lockup_lab   # 在 CPU3 關中斷死迴圈 12 秒
 *   echo 'hung 40'     | sudo tee /proc/lockup_lab   # 產生一條 D 狀態行程 40 秒
 *   echo 'stat'        | sudo tee /proc/lockup_lab   # 印每顆 CPU 的心跳計數
 */
#define pr_fmt(fmt) "lockup_lab: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/hrtimer.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/clock.h>
#include <linux/sched/task_stack.h>
#include <linux/delay.h>
#include <linux/kallsyms.h>
#include <linux/timekeeping.h>
#include <linux/cpumask.h>

/* ============ 參數（對應 watchdog_thresh / hung_task_timeout_secs）============ */
static int thresh = 5;			/* 秒；真核心預設 10 */
static int hung_timeout = 10;		/* 秒；真核心預設 120 */

/* ============ 迷你 softlockup + hardlockup 偵測器 ============================= */
static DEFINE_PER_CPU(struct hrtimer, wd_hrtimer);
static DEFINE_PER_CPU(unsigned long, hrtimer_ticks);		/* = hrtimer_interrupts */
static DEFINE_PER_CPU(unsigned long, hrtimer_ticks_saved);	/* = hrtimer_interrupts_saved */
static DEFINE_PER_CPU(u64, touch_ts);				/* = watchdog_touch_ts */
static DEFINE_PER_CPU(int, kick_pending);
static DEFINE_PER_CPU(struct task_struct *, wd_thread);		/* = watchdog/N */
static DEFINE_PER_CPU(bool, soft_reported);
static DEFINE_PER_CPU(bool, hard_reported);
static bool wd_running;

/* kernel/watchdog.c:293  sample_period = thresh*2 * (NSEC_PER_SEC/5) */
static u64 sample_period_ns(void)
{
	return (u64)thresh * 2 * (NSEC_PER_SEC / 5);
}

static u64 now_sec(void)
{
	return sched_clock() / NSEC_PER_SEC;	/* 真核心用 running_clock()>>30 */
}

/* 鄰居檢查：kernel/watchdog.c:382 watchdog_next_cpu() */
static unsigned int next_cpu_of(unsigned int cpu)
{
	unsigned int next = cpumask_next(cpu, cpu_online_mask);

	if (next >= nr_cpu_ids)
		next = cpumask_first(cpu_online_mask);
	return next == cpu ? nr_cpu_ids : next;
}

static enum hrtimer_restart wd_timer_fn(struct hrtimer *timer)
{
	int cpu = smp_processor_id();
	u64 now = now_sec(), ts;
	unsigned long ticks;

	/* (1) 心跳計數：真核心 kernel/watchdog.c:456 */
	ticks = __this_cpu_inc_return(hrtimer_ticks);

	/* (2) hardlockup：每 3 個週期檢查一次「鄰居 CPU 的心跳有沒有前進」
	 *     真核心 kernel/watchdog.c:411-448（HARDLOCKUP_DETECTOR_OTHER_CPU）
	 */
	if (ticks % 3 == 0) {
		unsigned int nb = next_cpu_of(cpu);

		if (nb < nr_cpu_ids) {
			unsigned long nb_ticks = per_cpu(hrtimer_ticks, nb);

			if (per_cpu(hrtimer_ticks_saved, nb) == nb_ticks) {
				if (!per_cpu(hard_reported, nb)) {
					per_cpu(hard_reported, nb) = true;
					pr_emerg("BUG: hard lockup - CPU#%u 的時鐘中斷停在 %lu 不再前進！(由 CPU#%d 發現)\n",
						 nb, nb_ticks, cpu);
					pr_emerg("      → 那顆 CPU 連中斷都不回應了，只有「別人」或 NMI 抓得到\n");
				}
			} else {
				if (per_cpu(hard_reported, nb)) {
					pr_emerg("CPU#%u 心跳恢復（%lu → %lu），hard lockup 結束\n",
						 nb, per_cpu(hrtimer_ticks_saved, nb), nb_ticks);
					per_cpu(hard_reported, nb) = false;
				}
				per_cpu(hrtimer_ticks_saved, nb) = nb_ticks;
			}
		}
	}

	/* (3) 踢一下本 CPU 的 watchdog 執行緒：真核心 kernel/watchdog.c:497-503
	 *     （3.10 是 RT 執行緒 watchdog/N；5.0 之後改用 stop 排程類，
	 *      模組拿不到 stop 類，所以這裡用 SCHED_FIFO 執行緒，等同 3.10 的做法）
	 */
	if (!__this_cpu_read(kick_pending)) {
		struct task_struct *p = __this_cpu_read(wd_thread);

		if (p) {
			__this_cpu_write(kick_pending, 1);
			wake_up_process(p);
		}
	}

	/* (4) softlockup 判定：真核心 kernel/watchdog.c:358 is_softlockup() */
	ts = __this_cpu_read(touch_ts);
	if (ts && now > ts + 2 * thresh) {
		if (!__this_cpu_read(soft_reported)) {
			__this_cpu_write(soft_reported, true);
			pr_emerg("BUG: soft lockup - CPU#%d stuck for %llus! [%s:%d]\n",
				 cpu, now - ts, current->comm, current->pid);
			pr_emerg("      → 時鐘中斷還在（心跳 %lu），但 watchdog 執行緒排不上來\n",
				 ticks);
		}
	} else if (__this_cpu_read(soft_reported) && ts && now <= ts + 2 * thresh) {
		__this_cpu_write(soft_reported, false);
		pr_emerg("CPU#%d 的 soft lockup 結束（watchdog 執行緒又跑起來了）\n", cpu);
	}

	hrtimer_forward_now(timer, ns_to_ktime(sample_period_ns()));
	return HRTIMER_RESTART;
}

/* watchdog/N：真核心 3.10 是 RT 執行緒（書上 §4.8），5.0+ 改成 stop 類 */
static int wd_thread_fn(void *arg)
{
	int cpu = (int)(long)arg;

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!per_cpu(kick_pending, cpu) && !kthread_should_stop())
			schedule();
		set_current_state(TASK_RUNNING);
		per_cpu(kick_pending, cpu) = 0;
		per_cpu(touch_ts, cpu) = now_sec();	/* = update_touch_ts() */
	}
	return 0;
}

static void start_hrtimer_local(void *unused)
{
	struct hrtimer *t = this_cpu_ptr(&wd_hrtimer);

	hrtimer_init(t, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	t->function = wd_timer_fn;
	__this_cpu_write(touch_ts, now_sec());
	hrtimer_start(t, ns_to_ktime(sample_period_ns()), HRTIMER_MODE_REL_PINNED);
}

static void wd_start(void)
{
	int cpu;

	if (wd_running)
		return;
	for_each_online_cpu(cpu) {
		struct task_struct *p;

		p = kthread_create(wd_thread_fn, (void *)(long)cpu, "mywd/%d", cpu);
		if (IS_ERR(p))
			continue;
		kthread_bind(p, cpu);
		sched_set_fifo(p);		/* 和 3.10 的 watchdog/N 一樣是 RT */
		per_cpu(wd_thread, cpu) = p;
		per_cpu(soft_reported, cpu) = false;
		per_cpu(hard_reported, cpu) = false;
		wake_up_process(p);
	}
	on_each_cpu(start_hrtimer_local, NULL, 1);
	wd_running = true;
	pr_info("迷你 watchdog 啟動：thresh=%d 秒，sample_period=%llu ms，"
		"softlockup 門檻=%d 秒\n",
		thresh, sample_period_ns() / NSEC_PER_MSEC, 2 * thresh);
}

static void wd_stop(void)
{
	int cpu;

	if (!wd_running)
		return;
	for_each_online_cpu(cpu)
		hrtimer_cancel(per_cpu_ptr(&wd_hrtimer, cpu));
	for_each_online_cpu(cpu) {
		struct task_struct *p = per_cpu(wd_thread, cpu);

		if (p) {
			per_cpu(wd_thread, cpu) = NULL;
			kthread_stop(p);
		}
	}
	wd_running = false;
	pr_info("迷你 watchdog 停止\n");
}

/* ============ 迷你 khungtaskd：kernel/hung_task.c ============================ */
static struct task_struct *khung_thread;

static void mini_bt(struct task_struct *t)
{
	unsigned long fp = t->thread.cpu_context.fp;
	unsigned long lo = (unsigned long)task_stack_page(t);
	unsigned long hi = lo + THREAD_SIZE;
	char sym[KSYM_SYMBOL_LEN];
	int i = 0;

	while (fp >= lo && fp < hi - 16 && i < 12) {
		unsigned long next = *(unsigned long *)fp;
		unsigned long lr = *(unsigned long *)(fp + 8);

		sprint_symbol(sym, lr);
		pr_emerg("  #%-2d %s\n", i, sym);
		if (next <= fp)
			break;
		fp = next;
		i++;
	}
}

/*
 * 真核心把「上一輪的切換次數」記在 task_struct.last_switch_count 裡，
 * 但那個欄位被 #ifdef CONFIG_DETECT_HUNG_TASK 包起來（include/linux/sched.h），
 * 本機沒開這個選項 → 欄位根本不存在（實測編譯錯誤：
 * 'struct task_struct' has no member named 'last_switch_count'）。
 * 所以自己在模組裡準備一張表來記。
 */
#define HT_MAX 512
static struct { pid_t pid; unsigned long switch_count; } ht_tab[HT_MAX];

static unsigned long *ht_slot(pid_t pid)
{
	int i, free_slot = -1;

	for (i = 0; i < HT_MAX; i++) {
		if (ht_tab[i].pid == pid)
			return &ht_tab[i].switch_count;
		if (free_slot < 0 && ht_tab[i].pid == 0)
			free_slot = i;
	}
	if (free_slot < 0)
		return NULL;
	ht_tab[free_slot].pid = pid;
	ht_tab[free_slot].switch_count = ULONG_MAX;
	return &ht_tab[free_slot].switch_count;
}

/* = kernel/hung_task.c:90 check_hung_task() */
static void check_one(struct task_struct *t, unsigned long timeout)
{
	unsigned long switch_count = t->nvcsw + t->nivcsw;
	unsigned long *last = ht_slot(t->pid);

	if (!last)
		return;
	if (switch_count != *last) {
		*last = switch_count;			/* 有切換過 → 不算 hung */
		return;
	}

	pr_emerg("INFO: task %s:%d blocked for more than %ld seconds.\n",
		 t->comm, t->pid, timeout);
	pr_emerg("      \"echo 0 > /proc/sys/kernel/hung_task_timeout_secs\" disables this message.\n");
	pr_emerg("      switch_count(nvcsw+nivcsw)=%lu 一直沒變 → 從頭到尾沒被排程過\n",
		 switch_count);
	mini_bt(t);
}

/* = kernel/hung_task.c:178 check_hung_uninterruptible_tasks() */
static int khung_fn(void *arg)
{
	while (!kthread_should_stop()) {
		struct task_struct *g, *t;
		unsigned long timeout = hung_timeout;

		/* 真核心是「睡 timeout 秒 → 掃一遍」，所以最久會晚 timeout 秒才報 */
		msleep_interruptible(timeout * 1000);
		if (kthread_should_stop())
			break;

		rcu_read_lock();
		for_each_process_thread(g, t) {
			unsigned int st = READ_ONCE(t->__state);

			/* 判斷式照抄 kernel/hung_task.c:206-210 */
			if ((st & TASK_UNINTERRUPTIBLE) &&
			    !(st & TASK_WAKEKILL) &&
			    !(st & TASK_NOLOAD))
				check_one(t, timeout);
		}
		rcu_read_unlock();
	}
	return 0;
}

/* ============ 三種 lockup 的製造機 =========================================== */
struct trig {
	struct task_struct *task;
	int cpu, secs, kind;	/* kind: 0=soft 1=hard 2=hung */
};

static int trig_fn(void *arg)
{
	struct trig *tr = arg;
	u64 end;

	if (tr->kind == 2) {			/* D 狀態行程 */
		pr_info("[hung   ] pid=%d 進入 TASK_UNINTERRUPTIBLE %d 秒\n",
			current->pid, tr->secs);
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(tr->secs * HZ);
		pr_info("[hung   ] pid=%d 醒了\n", current->pid);
		kfree(tr);
		return 0;
	}

	end = ktime_get_mono_fast_ns() + (u64)tr->secs * NSEC_PER_SEC;
	if (tr->kind == 0) {
		pr_info("[soft   ] pid=%d 在 CPU%d 上 preempt_disable() 死迴圈 %d 秒\n",
			current->pid, smp_processor_id(), tr->secs);
		preempt_disable();
		while (ktime_get_mono_fast_ns() < end)
			cpu_relax();
		preempt_enable();
		pr_info("[soft   ] pid=%d 死迴圈結束\n", current->pid);
	} else {
		unsigned long flags;

		pr_info("[hard   ] pid=%d 在 CPU%d 上 local_irq_disable() 死迴圈 %d 秒\n",
			current->pid, smp_processor_id(), tr->secs);
		local_irq_save(flags);
		while (ktime_get_mono_fast_ns() < end)	/* NMI-safe 的讀時鐘方式 */
			cpu_relax();
		local_irq_restore(flags);
		pr_info("[hard   ] pid=%d 死迴圈結束（中斷已恢復）\n", current->pid);
	}
	kfree(tr);
	return 0;
}

static void trigger(int kind, int cpu, int secs)
{
	struct trig *tr = kzalloc(sizeof(*tr), GFP_KERNEL);
	struct task_struct *p;
	static const char * const nm[] = { "lockup_soft", "lockup_hard", "lockup_hung" };

	if (!tr)
		return;
	tr->cpu = cpu;
	tr->secs = secs;
	tr->kind = kind;
	p = kthread_create(trig_fn, tr, "%s", nm[kind]);
	if (IS_ERR(p)) {
		kfree(tr);
		return;
	}
	if (kind != 2)
		kthread_bind(p, cpu);
	wake_up_process(p);
	pr_info("已啟動 %s pid=%d (cpu=%d secs=%d)\n", nm[kind], p->pid, cpu, secs);
}

static void cmd_stat(void)
{
	int cpu;

	pr_info("=== 每顆 CPU 的心跳（hrtimer_interrupts）與 touch_ts ===\n");
	pr_info("now=%llu 秒，thresh=%d，softlockup 門檻=%d 秒\n",
		now_sec(), thresh, 2 * thresh);
	for_each_online_cpu(cpu)
		pr_info("CPU%-2d ticks=%-8lu saved=%-8lu touch_ts=%-10llu 落後 %lld 秒 %s\n",
			cpu, per_cpu(hrtimer_ticks, cpu),
			per_cpu(hrtimer_ticks_saved, cpu),
			per_cpu(touch_ts, cpu),
			(long long)(now_sec() - per_cpu(touch_ts, cpu)),
			per_cpu(wd_thread, cpu) ? "" : "(無 watchdog 執行緒)");
}

static ssize_t ll_write(struct file *f, const char __user *ubuf,
			size_t len, loff_t *off)
{
	char buf[64];
	int a, b;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (!strncmp(buf, "wd on", 5))
		wd_start();
	else if (!strncmp(buf, "wd off", 6))
		wd_stop();
	else if (sscanf(buf, "thresh %d", &a) == 1) {
		thresh = a;
		pr_info("thresh = %d 秒（softlockup 門檻 %d 秒，sample_period %llu ms）\n",
			thresh, 2 * thresh, sample_period_ns() / NSEC_PER_MSEC);
	} else if (sscanf(buf, "khung on %d", &a) == 1) {
		hung_timeout = a;
		if (!khung_thread)
			khung_thread = kthread_run(khung_fn, NULL, "my_khungtaskd");
		pr_info("迷你 khungtaskd 啟動，timeout=%d 秒\n", hung_timeout);
	} else if (!strncmp(buf, "khung off", 9)) {
		if (khung_thread) {
			kthread_stop(khung_thread);
			khung_thread = NULL;
			pr_info("迷你 khungtaskd 停止\n");
		}
	} else if (sscanf(buf, "soft %d %d", &a, &b) == 2)
		trigger(0, a, b);
	else if (sscanf(buf, "hard %d %d", &a, &b) == 2)
		trigger(1, a, b);
	else if (sscanf(buf, "hung %d", &a) == 1)
		trigger(2, 0, a);
	else if (!strncmp(buf, "stat", 4))
		cmd_stat();
	else
		pr_err("指令：wd on|off / thresh <s> / khung on <s> | khung off / soft <cpu> <s> / hard <cpu> <s> / hung <s> / stat\n");

	return len;
}

static const struct proc_ops ll_ops = {
	.proc_write = ll_write,
};

static int __init ll_init(void)
{
	if (!proc_create("lockup_lab", 0222, NULL, &ll_ops))
		return -ENOMEM;
	pr_info("已載入。本機核心沒有 SOFTLOCKUP/HARDLOCKUP/DETECT_HUNG_TASK，這裡自己做一份。\n");
	return 0;
}

static void __exit ll_exit(void)
{
	remove_proc_entry("lockup_lab", NULL);
	wd_stop();
	if (khung_thread)
		kthread_stop(khung_thread);
	pr_info("卸載\n");
}

module_init(ll_init);
module_exit(ll_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("迷你 softlockup/hardlockup/hung_task 偵測器 + 三種 lockup 製造機（卷2 第4章 Q7~Q9）");
