// SPDX-License-Identifier: GPL-2.0
/*
 * dl_lab.ko —— 卷2 第3章 Q10/Q11：「什麼是死鎖？常見的死鎖有哪幾種？」
 *
 * 本機 6.1.115+ 的組態：
 *   CONFIG_PROVE_LOCKING=n、CONFIG_DEBUG_LOCKDEP=n  → **沒有 lockdep**
 *   CONFIG_DEBUG_SPINLOCK=y                          → 有 spinlock 自檢
 *   CONFIG_DETECT_HUNG_TASK=n、CONFIG_SOFTLOCKUP_DETECTOR=n → 死鎖不會有人喊
 * 所以書上那份 lockdep 報告在這台機器上是印不出來的，
 * 這個模組改用「可自我解除」的方式，把死鎖的**現象**量出來。
 *
 * mode：
 *  0 遞歸死鎖 AA（spinlock）—— 安全版：拿到鎖後用 trylock 再拿一次，
 *    永遠失敗；同時把 lock->owner / owner_cpu 印出來，
 *    這正是 kernel/locking/spinlock_debug.c: debug_spin_lock_before() 用來判斷
 *    "recursion on CPU#n" 的那兩個欄位。
 *  1 遞歸死鎖 AA（mutex）—— 真的睡死，靠 SIGKILL 解除。
 *  2 AB-BA 死鎖（兩把 mutex、兩條 kthread）—— 真的睡死，逾時後 SIGKILL 解除，
 *    期間可以用 `cat /proc/<tid>/stack`、`ps -eo pid,stat,wchan,comm` 觀察。
 *  3 AB-BA 安全版：兩邊都用 trylock，記錄「互相搶不到」的次數與時間。
 *  4 書上第二個例子：mutex + cancel_delayed_work_sync() 的循環等待
 *    （worker 端改用 trylock 逾時放手，所以會自己解開）。
 *
 * 用法：
 *   sudo insmod dl_lab.ko mode=0 ; sudo rmmod dl_lab
 *   sudo insmod dl_lab.ko mode=2 escape_ms=8000
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/ktime.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("deadlock taxonomy without lockdep (ch12 Q10/Q11)");

static int mode;
module_param(mode, int, 0444);
static int escape_ms = 6000;
module_param(escape_ms, int, 0444);
MODULE_PARM_DESC(escape_ms, "死鎖多久之後強制解除(ms)");

static DEFINE_SPINLOCK(lock_s);
static DEFINE_MUTEX(mutex_a);
static DEFINE_MUTEX(mutex_b);

static struct task_struct *th_ab, *th_ba, *th_watch;
static struct task_struct *th_aa;
static atomic_t blocked = ATOMIC_INIT(0);

/* ---------- mode 0：spinlock AA（安全版）---------- */
static void aa_spinlock_safe(void)
{
	unsigned long t0;
	int got, tries = 0;

	pr_info("dl_lab: [AA/spinlock] 先拿一次 lock_s\n");
	spin_lock(&lock_s);
#ifdef CONFIG_DEBUG_SPINLOCK
	pr_info("dl_lab: 拿到後 raw_lock.owner=%px(current=%px) owner_cpu=%d(this=%d)\n",
		lock_s.rlock.owner, current, lock_s.rlock.owner_cpu,
		raw_smp_processor_id());
	pr_info("dl_lab: → kernel/locking/spinlock_debug.c debug_spin_lock_before() 的兩個條件"
		" (owner==current, owner_cpu==this_cpu) 現在都成立，\n");
	pr_info("dl_lab:   若這時再 spin_lock() 一次，核心會印 "
		"\"BUG: spinlock recursion on CPU#%d\" 然後**真的卡死在 arch_spin_lock()**\n",
		raw_smp_processor_id());
#endif
	t0 = jiffies;
	while (time_before(jiffies, t0 + msecs_to_jiffies(500))) {
		got = spin_trylock(&lock_s);
		tries++;
		if (got) {
			pr_err("dl_lab: 竟然拿到了？（不該發生）\n");
			spin_unlock(&lock_s);
			break;
		}
		cpu_relax();
	}
	pr_info("dl_lab: [AA/spinlock] 500ms 內 trylock 失敗 %d 次 —— "
		"這就是遞歸死鎖：自己等自己放手\n", tries);
	spin_unlock(&lock_s);
}

/* ---------- mode 1：mutex AA（真的睡死 + SIGKILL 解除）---------- */
static int aa_mutex_fn(void *unused)
{
	ktime_t t0;
	s64 us;
	int ret;

	allow_signal(SIGKILL);
	mutex_lock(&mutex_a);
	pr_info("dl_lab: [AA/mutex] 第一次 mutex_lock(&mutex_a) 成功，owner=%px\n",
		current);
	pr_info("dl_lab: [AA/mutex] 再鎖一次 —— 從此進 TASK_KILLABLE，等自己\n");
	atomic_set(&blocked, 1);
	t0 = ktime_get();
	ret = mutex_lock_interruptible(&mutex_a);	/* 死鎖點 */
	us = ktime_us_delta(ktime_get(), t0);
	pr_info("dl_lab: [AA/mutex] 被喚醒，ret=%d，睡了 %lld us %s\n",
		ret, us, ret ? "(靠 SIGKILL 逃出來)" : "");
	pr_info("dl_lab: [AA/mutex] signal_pending=%d fatal=%d current=%px\n",
		signal_pending(current), fatal_signal_pending(current), current);
	pr_info("dl_lab: [AA/mutex] mutex_a.owner 原始值 = %#lx"
		"  (低 3 bit：WAITERS=1 HANDOFF=2 PICKUP=4)\n",
		(unsigned long)atomic_long_read(&mutex_a.owner));
	pr_info("dl_lab: [AA/mutex] → owner 仍然是自己（flags 已被 "
		"__mutex_remove_waiter() 清掉）：signal 把我叫醒後，"
		"mutex.c:689 __mutex_trylock_or_handoff() 因為 task==curr "
		"而回傳「成功」，遞歸鎖是被『假裝』拿到的，不是真的排到\n");
	if (!ret)
		mutex_unlock(&mutex_a);
	mutex_unlock(&mutex_a);
	atomic_set(&blocked, 0);
	while (!kthread_should_stop())
		msleep(50);
	return 0;
}

/* ---------- mode 2/3：AB-BA ---------- */
struct abba_arg {
	struct mutex	*first;
	struct mutex	*second;
	const char	*name;
	bool		trylock;
};

static int abba_fn(void *data)
{
	struct abba_arg *a = data;
	ktime_t t0;
	int ret = 0, fails = 0;

	allow_signal(SIGKILL);
	mutex_lock(a->first);
	pr_info("dl_lab: [%s] 拿到第一把鎖 %s\n", a->name,
		a->first == &mutex_a ? "mutex_a" : "mutex_b");
	msleep(300);	/* 讓兩邊都先各拿一把，保證交叉 */
	pr_info("dl_lab: [%s] 現在要拿第二把鎖 %s\n", a->name,
		a->second == &mutex_a ? "mutex_a" : "mutex_b");
	atomic_inc(&blocked);
	t0 = ktime_get();

	if (a->trylock) {
		while (!kthread_should_stop() &&
		       ktime_us_delta(ktime_get(), t0) < escape_ms * 1000LL) {
			if (mutex_trylock(a->second)) {
				pr_info("dl_lab: [%s] trylock 成功（沒死鎖）\n", a->name);
				mutex_unlock(a->second);
				break;
			}
			fails++;
			msleep(20);
		}
		pr_info("dl_lab: [%s] trylock 連續失敗 %d 次 / %lld ms —— 循環等待成立\n",
			a->name, fails, ktime_ms_delta(ktime_get(), t0));
	} else {
		ret = mutex_lock_interruptible(a->second);
		pr_info("dl_lab: [%s] 第二把鎖 ret=%d，阻塞了 %lld ms %s\n",
			a->name, ret, ktime_ms_delta(ktime_get(), t0),
			ret ? "(靠 SIGKILL 逃出來)" : "");
		if (!ret)
			mutex_unlock(a->second);
	}
	atomic_dec(&blocked);
	mutex_unlock(a->first);
	while (!kthread_should_stop())
		msleep(50);
	return 0;
}

static struct abba_arg arg_ab, arg_ba;

/* 逾時把死鎖的兩條執行緒 SIGKILL 掉，避免留下無法回收的 D 狀態任務 */
static int watchdog_fn(void *unused)
{
	msleep(escape_ms);
	pr_info("dl_lab: [watchdog] %d ms 到，發 SIGKILL 解除死鎖\n", escape_ms);
	if (th_ab)
		send_sig(SIGKILL, th_ab, 1);
	if (th_ba)
		send_sig(SIGKILL, th_ba, 1);
	if (th_aa)
		send_sig(SIGKILL, th_aa, 1);
	while (!kthread_should_stop())
		msleep(50);
	return 0;
}

/* ---------- mode 4：書上的 mutex vs cancel_delayed_work_sync ---------- */
static struct delayed_work delay_task;
static struct task_struct *th_book;
static atomic_t worker_waited_ms = ATOMIC_INIT(0);

static void book_worker(struct work_struct *work)
{
	ktime_t t0 = ktime_get();

	pr_info("dl_lab: [worker] 進來了，要拿 mutex_a\n");
	while (!mutex_trylock(&mutex_a)) {
		if (ktime_ms_delta(ktime_get(), t0) > escape_ms) {
			pr_info("dl_lab: [worker] 等 mutex_a 等了 %lld ms 還拿不到 —— 放棄\n",
				ktime_ms_delta(ktime_get(), t0));
			atomic_set(&worker_waited_ms,
				   (int)ktime_ms_delta(ktime_get(), t0));
			return;		/* 放手，讓 flush_work() 能返回 */
		}
		msleep(20);
	}
	pr_info("dl_lab: [worker] 拿到 mutex_a\n");
	mdelay(100);
	mutex_unlock(&mutex_a);
}

static int book_thread(void *unused)
{
	ktime_t t0;

	pr_info("dl_lab: [book_thread] 先拿 mutex_a\n");
	mutex_lock(&mutex_a);
	/* 拿著 mutex_a 的時候才把 work 排進去，並等它真的跑起來卡在 mutex_a */
	schedule_delayed_work(&delay_task, msecs_to_jiffies(50));
	msleep(300);
	pr_info("dl_lab: [book_thread] 再呼叫 cancel_delayed_work_sync()"
		" —— 會等 worker 跑完，而 worker 正在等 mutex_a\n");
	t0 = ktime_get();
	cancel_delayed_work_sync(&delay_task);
	pr_info("dl_lab: [book_thread] cancel_delayed_work_sync() 卡了 %lld ms 才返回\n",
		ktime_ms_delta(ktime_get(), t0));
	mutex_unlock(&mutex_a);
	while (!kthread_should_stop())
		msleep(50);
	return 0;
}

static int __init dl_lab_init(void)
{
	pr_info("======== dl_lab mode=%d escape_ms=%d ========\n", mode, escape_ms);
	pr_info("dl_lab: CONFIG_PROVE_LOCKING=%s CONFIG_DEBUG_SPINLOCK=%s "
		"CONFIG_DETECT_HUNG_TASK=%s\n",
#ifdef CONFIG_PROVE_LOCKING
		"y",
#else
		"n",
#endif
#ifdef CONFIG_DEBUG_SPINLOCK
		"y",
#else
		"n",
#endif
#ifdef CONFIG_DETECT_HUNG_TASK
		"y");
#else
		"n");
#endif

	switch (mode) {
	case 0:
		aa_spinlock_safe();
		break;
	case 1:
		th_aa = kthread_run(aa_mutex_fn, NULL, "dl_aa");
		th_watch = kthread_run(watchdog_fn, NULL, "dl_watch");
		break;
	case 2:
	case 3:
		arg_ab = (struct abba_arg){ &mutex_a, &mutex_b, "A->B", mode == 3 };
		arg_ba = (struct abba_arg){ &mutex_b, &mutex_a, "B->A", mode == 3 };
		th_ab = kthread_run(abba_fn, &arg_ab, "dl_ab");
		th_ba = kthread_run(abba_fn, &arg_ba, "dl_ba");
		if (mode == 2)
			th_watch = kthread_run(watchdog_fn, NULL, "dl_watch");
		break;
	case 4:
		INIT_DELAYED_WORK(&delay_task, book_worker);
		th_book = kthread_run(book_thread, NULL, "dl_book");
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static void __exit dl_lab_exit(void)
{
	if (th_watch)
		kthread_stop(th_watch);
	if (th_ab)
		kthread_stop(th_ab);
	if (th_ba)
		kthread_stop(th_ba);
	if (th_aa)
		kthread_stop(th_aa);
	if (th_book)
		kthread_stop(th_book);
	if (mode == 4)
		cancel_delayed_work_sync(&delay_task);
	pr_info("dl_lab: exit\n");
}

module_init(dl_lab_init);
module_exit(dl_lab_exit);
