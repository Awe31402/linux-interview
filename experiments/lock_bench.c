// SPDX-License-Identifier: GPL-2.0
/*
 * lock_bench.ko —— 《奔跑吧 Linux 內核》卷2 第 1 章 鎖爭用實驗
 * 平台：Radxa ROCK 5B (RK3588)，Linux 6.1.115+ aarch64
 *
 * 兩個模式：
 *   mode=0（watch，預設）：重現書上圖 1.6~1.12 的排隊自旋鎖狀態機。
 *       CPU0 長時間持有一把 qspinlock，CPU1/2/3 依序加入爭用，
 *       另一顆 CPU 上的監看執行緒每 2ms 取樣一次 lock->val，
 *       把 {tail_cpu, tail_idx, pending, locked} 三元組的變化印出來。
 *       -> Q12、Q15、Q16
 *
 *   mode=1（bench）：多執行緒吞吐量對比。
 *       spinlock / mutex / rwsem(讀) / rwsem(寫) / 純原子 / RCU 讀
 *       -> Q10、Q17、Q20、Q22、Q25、Q27、Q33
 *
 *   mode=2（optspin）：樂觀自旋 vs 睡眠等待的對照。
 *       同一把 mutex，臨界區「短且不睡」 vs 「會睡眠」，
 *       比較 nvcsw/nivcsw（自願/非自願切換次數）。
 *       -> Q19、Q20、Q21
 *
 * 用法：
 *   sudo insmod lock_bench.ko mode=0
 *   sudo insmod lock_bench.ko mode=1 nthread=8 ms=300
 *   sudo insmod lock_bench.ko mode=2
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/cpu.h>
#include <linux/completion.h>
#include <asm/qspinlock.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lock contention benchmark for RK3588 kernel study notes");

static int mode;
module_param(mode, int, 0444);
static int nthread = 4;
module_param(nthread, int, 0444);
static int ms = 100;
module_param(ms, int, 0444);
static int hold_ms = 120;
module_param(hold_ms, int, 0444);

#define P(fmt, ...) pr_info("lock_bench: " fmt, ##__VA_ARGS__)
#define MAXT 16

/*
 * 共用的鎖。
 * ★ 每一個都放在自己的 cache line 上（____cacheline_aligned）。
 * 一開始沒有對齊時，bl_atomic 和 bl_shared 落在同一條 64 位元組的 cache line 上，
 * 於是「純原子操作」那一輪會被前一輪 spinlock/mutex 測試留下的 cache line
 * 顛簸污染，單執行緒吞吐量在 17797 ~ 66716 次/ms 之間亂跳（實測）。
 * 這就是「偽共享（false sharing）」的活教材。
 */
static DEFINE_SPINLOCK(bl_spin);
static struct mutex bl_mutex ____cacheline_aligned;
static struct rw_semaphore bl_rwsem ____cacheline_aligned;
static atomic_t bl_atomic ____cacheline_aligned = ATOMIC_INIT(0);
static int bl_shared ____cacheline_aligned;	/* 被保護的資料 */

static struct task_struct *thr[MAXT];
static unsigned long ops[MAXT];
static unsigned long t_nvcsw[MAXT], t_nivcsw[MAXT];
static atomic_t running = ATOMIC_INIT(0);

/* ================================================================== */
/* mode 0：觀察 qspinlock 的三元組                                      */
/* ================================================================== */
static u32 qval(spinlock_t *l)
{
	return atomic_read(&l->rlock.raw_lock.val);
}

static void show_triple(const char *tag, u32 v)
{
	u32 locked  = v & _Q_LOCKED_MASK;
	u32 pending = (v & _Q_PENDING_MASK) >> _Q_PENDING_OFFSET;
	u32 tidx    = (v & _Q_TAIL_IDX_MASK) >> _Q_TAIL_IDX_OFFSET;
	u32 tcpu    = (v & _Q_TAIL_CPU_MASK) >> _Q_TAIL_CPU_OFFSET;

	if (tcpu)
		P("  %-22s val=0x%08x  {tail=CPU%d, tail_idx=%u, pending=%u, locked=%u}\n",
		  tag, v, (int)tcpu - 1, tidx, pending, locked);
	else
		P("  %-22s val=0x%08x  {tail=--,   tail_idx=%u, pending=%u, locked=%u}\n",
		  tag, v, tidx, pending, locked);
}

struct watch_arg { int idx; int delay_ms; };

static int watch_holder(void *arg)
{
	unsigned long end;

	P("  [holder] 在 CPU%d 上取得鎖，持有 %d ms\n", smp_processor_id(), hold_ms);
	spin_lock(&bl_spin);
	show_triple("holder 剛拿到鎖", qval(&bl_spin));
	end = jiffies + msecs_to_jiffies(hold_ms);
	while (time_before(jiffies, end))
		cpu_relax();		/* 忙等，模擬長臨界區 */
	show_triple("holder 即將放鎖", qval(&bl_spin));
	spin_unlock(&bl_spin);
	P("  [holder] 放鎖，CPU%d\n", smp_processor_id());
	atomic_dec(&running);
	return 0;
}

static int watch_contender(void *arg)
{
	struct watch_arg *wa = arg;
	u64 t0, t1;

	msleep(wa->delay_ms);
	P("  [contender%d] CPU%d 開始搶鎖\n", wa->idx, smp_processor_id());
	t0 = local_clock();
	spin_lock(&bl_spin);
	t1 = local_clock();
	P("  [contender%d] CPU%d 拿到鎖，等了 %llu us\n",
	  wa->idx, smp_processor_id(), (t1 - t0) / 1000);
	udelay(200);
	spin_unlock(&bl_spin);
	kfree(wa);
	atomic_dec(&running);
	return 0;
}

static int watch_monitor(void *arg)
{
	int i;

	for (i = 0; i < hold_ms / 10 + 6; i++) {
		char tag[32];

		scnprintf(tag, sizeof(tag), "t=%3dms", i * 10);
		show_triple(tag, qval(&bl_spin));
		msleep(10);
	}
	atomic_dec(&running);
	return 0;
}

static void run_watch(void)
{
	int i;
	struct task_struct *t;

	P("=========================================================\n");
	P("== Q12/Q15/Q16：排隊自旋鎖的狀態機（書上圖 1.6~1.12）==\n");
	P("=========================================================\n");
	P("  三元組表示法 {tail, pending, locked}；tail_cpu 存的是 cpu+1\n");
	show_triple("初始（無人持有）", qval(&bl_spin));

	atomic_set(&running, 0);

	/* holder 釘在 CPU0 */
	atomic_inc(&running);
	t = kthread_create(watch_holder, NULL, "lb_holder");
	kthread_bind(t, 0);
	wake_up_process(t);

	/* 監看執行緒釘在 CPU7（不參與搶鎖） */
	atomic_inc(&running);
	t = kthread_create(watch_monitor, NULL, "lb_monitor");
	kthread_bind(t, 7);
	wake_up_process(t);

	/* 三個競爭者依序在 CPU1/2/3 上加入 */
	for (i = 1; i <= 3; i++) {
		struct watch_arg *wa = kmalloc(sizeof(*wa), GFP_KERNEL);

		if (!wa)
			break;
		wa->idx = i;
		wa->delay_ms = i * 20;
		atomic_inc(&running);
		t = kthread_create(watch_contender, wa, "lb_cont%d", i);
		kthread_bind(t, i);
		wake_up_process(t);
	}

	while (atomic_read(&running) > 0)
		msleep(20);
	show_triple("全部結束", qval(&bl_spin));
	P("\n  怎麼讀這份輸出：\n");
	P("    {0,0,1}          -> 只有持有者，快速通道（atomic_try_cmpxchg_acquire 成功）\n");
	P("    {0,1,1}          -> 第一個競爭者變成 pending（第一順位繼承者），不進 MCS 佇列\n");
	P("    {CPUn,0,1,1}     -> 第二個競爭者開始排 MCS 佇列，tail 指向它\n");
	P("    tail 一直換人    -> 後來的競爭者接到佇列尾巴，只在自己的 mcs_node 上自旋\n");
}

/* ================================================================== */
/* mode 1：吞吐量對比                                                   */
/* ================================================================== */
enum { L_SPIN, L_MUTEX, L_RWSEM_R, L_RWSEM_W, L_ATOMIC, L_RCU, L_NR };
static const char * const lname[L_NR] = {
	"spinlock", "mutex", "rwsem(讀)", "rwsem(寫)", "atomic_inc", "rcu_read_lock"
};
static int cur_lock;

struct rcu_obj { int v; struct rcu_head rh; };
static struct rcu_obj __rcu *g_obj;

static int bench_thread(void *arg)
{
	long idx = (long)arg;
	unsigned long n = 0;
	unsigned long end;

	t_nvcsw[idx] = current->nvcsw;
	t_nivcsw[idx] = current->nivcsw;
	/*
	 * 這裡刻意「不」用忙等的起跑柵欄。
	 * 教訓：本機是 CONFIG_PREEMPT_VOLUNTARY（核心態不可搶佔），
	 * 如果每個 CPU 上都有一個 kthread 在
	 *     while (!atomic_read(&go)) cpu_relax();
	 * 裡忙等，而 go 只能由 insmod 行程設定，
	 * insmod 就永遠排不到 CPU -> 整台機器死鎖（實測會失去回應）。
	 * 這正是 Q9/Q11「自旋等待時不能讓出 CPU」的真實後果。
	 */
	end = jiffies + msecs_to_jiffies(ms);

	while (time_before(jiffies, end)) {
		switch (cur_lock) {
		case L_SPIN:
			spin_lock(&bl_spin);   bl_shared++;  spin_unlock(&bl_spin);   break;
		case L_MUTEX:
			mutex_lock(&bl_mutex); bl_shared++;  mutex_unlock(&bl_mutex); break;
		case L_RWSEM_R:
			down_read(&bl_rwsem);  (void)bl_shared; up_read(&bl_rwsem);   break;
		case L_RWSEM_W:
			down_write(&bl_rwsem); bl_shared++;  up_write(&bl_rwsem);     break;
		case L_ATOMIC:
			atomic_inc(&bl_atomic); break;
		case L_RCU: {
			struct rcu_obj *o;
			rcu_read_lock();
			o = rcu_dereference(g_obj);
			if (o) (void)READ_ONCE(o->v);
			rcu_read_unlock();
			break;
		}
		}
		n++;
		if ((n & 0xff) == 0)
			cond_resched();	/* PREEMPT_VOLUNTARY：這是唯一的搶佔點，不能太稀疏 */
	}
	ops[idx] = n;
	t_nvcsw[idx]  = current->nvcsw  - t_nvcsw[idx];
	t_nivcsw[idx] = current->nivcsw - t_nivcsw[idx];
	atomic_dec(&running);
	return 0;
}

static void run_one_bench(int which, int nthr)
{
	long i;
	unsigned long total = 0, vcsw = 0, ivcsw = 0;

	cur_lock = which;
	atomic_set(&running, nthr);
	memset(ops, 0, sizeof(ops));

	for (i = 0; i < nthr; i++) {
		thr[i] = kthread_create(bench_thread, (void *)i, "lb%ld", i);
		if (IS_ERR(thr[i])) { thr[i] = NULL; atomic_dec(&running); continue; }
		/* 保留最後一顆 CPU 給系統（網路 softirq / sshd），否則整台機器會失去回應 */
		kthread_bind(thr[i], i % max(1, (int)num_online_cpus() - 1));
		wake_up_process(thr[i]);
	}
	while (atomic_read(&running) > 0)
		msleep(10);

	for (i = 0; i < nthr; i++) {
		total += ops[i];
		vcsw += t_nvcsw[i];
		ivcsw += t_nivcsw[i];
	}
	P("  %-14s %2d 執行緒 %4d ms : 總計 %9lu 次 = %7lu 次/ms/執行緒"
	  "   自願切換=%lu 被搶佔=%lu\n",
	  lname[which], nthr, ms, total, total / ms / nthr, vcsw, ivcsw);
}

static void run_bench(void)
{
	int nthr = clamp(nthread, 1, min(MAXT, (int)num_online_cpus() - 1));
	struct rcu_obj *o;
	int i;

	o = kzalloc(sizeof(*o), GFP_KERNEL);
	if (o) { o->v = 1; rcu_assign_pointer(g_obj, o); }

	P("=========================================================\n");
	P("== Q10/Q20/Q22/Q25/Q27/Q33：各種鎖的吞吐量對比 ==\n");
	P("=========================================================\n");
	P("  每個執行緒在迴圈裡「拿鎖 -> 改一個 int -> 放鎖」，量固定時間內做了幾次。\n");
	P("  每把鎖都 ____cacheline_aligned，避免偽共享污染量測。\n");
	P("  臨界區極短，所以測出來的幾乎純粹是「鎖本身的成本 + 爭用成本」。\n\n");

	P("  --- 單執行緒（無爭用，量的是快速通道成本）---\n");
	for (i = 0; i < L_NR; i++)
		run_one_bench(i, 1);

	P("\n  --- %d 執行緒（有爭用）---\n", nthr);
	for (i = 0; i < L_NR; i++)
		run_one_bench(i, nthr);

	if (o) {
		rcu_assign_pointer(g_obj, NULL);
		synchronize_rcu();
		kfree(o);
	}
}

/* ================================================================== */
/* mode 2：樂觀自旋 vs 睡眠等待                                          */
/* ================================================================== */
static int opt_sleep_in_cs;	/* 臨界區裡要不要睡 */

static int optspin_thread(void *arg)
{
	long idx = (long)arg;
	unsigned long n = 0, end;

	t_nvcsw[idx] = current->nvcsw;
	t_nivcsw[idx] = current->nivcsw;
	end = jiffies + msecs_to_jiffies(ms);
	while (time_before(jiffies, end)) {
		mutex_lock(&bl_mutex);
		if (opt_sleep_in_cs)
			usleep_range(200, 300);	/* 持有者睡覺 -> 別人只能睡 */
		else
			udelay(5);		/* 持有者一直在跑 -> 別人樂觀自旋 */
		bl_shared++;
		mutex_unlock(&bl_mutex);
		n++;
		cond_resched();
	}
	ops[idx] = n;
	t_nvcsw[idx]  = current->nvcsw  - t_nvcsw[idx];
	t_nivcsw[idx] = current->nivcsw - t_nivcsw[idx];
	atomic_dec(&running);
	return 0;
}

static void run_optspin_case(int sleep_in_cs, int nthr)
{
	long i;
	unsigned long total = 0, vcsw = 0, ivcsw = 0;

	opt_sleep_in_cs = sleep_in_cs;
	atomic_set(&running, nthr);
	memset(ops, 0, sizeof(ops));
	for (i = 0; i < nthr; i++) {
		thr[i] = kthread_create(optspin_thread, (void *)i, "lo%ld", i);
		if (IS_ERR(thr[i])) { atomic_dec(&running); continue; }
		kthread_bind(thr[i], i % max(1, (int)num_online_cpus() - 1));
		wake_up_process(thr[i]);
	}
	while (atomic_read(&running) > 0)
		msleep(10);
	for (i = 0; i < nthr; i++) {
		total += ops[i]; vcsw += t_nvcsw[i]; ivcsw += t_nivcsw[i];
	}
	P("  臨界區%s : 總計 %6lu 次   自願切換(睡眠) = %5lu   被搶佔 = %lu\n",
	  sleep_in_cs ? "會睡眠 (usleep 200us)" : "只跑不睡 (udelay 5us)",
	  total, vcsw, ivcsw);
	P("      -> 平均每拿一次鎖就睡 %lu.%02lu 次\n",
	  total ? vcsw * 100 / total / 100 : 0,
	  total ? vcsw * 100 / total % 100 : 0);
}

static void run_optspin(void)
{
	int nthr = clamp(nthread, 2, min(MAXT, (int)num_online_cpus() - 1));

	P("=========================================================\n");
	P("== Q19/Q20/Q21：樂觀自旋等待 vs 睡眠等待 ==\n");
	P("=========================================================\n");
	P("  CONFIG_MUTEX_SPIN_ON_OWNER = %s\n",
	  IS_ENABLED(CONFIG_MUTEX_SPIN_ON_OWNER) ? "y" : "n");
	P("  判斷條件（mutex_can_spin_on_owner）：鎖持有者的 task->on_cpu == 1，\n");
	P("  也就是「持有者正在某顆 CPU 上跑」-> 它很快會出臨界區 -> 值得自旋等。\n\n");
	P("  %d 個執行緒搶同一把 mutex，跑 %d ms：\n", nthr, ms);
	run_optspin_case(0, nthr);
	run_optspin_case(1, nthr);
	P("\n  「自願切換」次數就是 mutex 走了睡眠等待路徑的次數。\n");
	P("  持有者一直在跑時，競爭者靠樂觀自旋拿到鎖，幾乎不需要睡眠 -> 切換次數低。\n");
	P("  持有者自己睡著時，mutex_spin_on_owner() 看到 on_cpu==0 就放棄自旋，\n");
	P("  競爭者只能排進等待佇列睡覺 -> 切換次數暴增。\n");
}

/* ================================================================== */
static int __init lock_bench_init(void)
{
	P("######## RK3588 鎖爭用實驗 mode=%d ########\n", mode);
	mutex_init(&bl_mutex);
	init_rwsem(&bl_rwsem);
	switch (mode) {
	case 0: run_watch();   break;
	case 1: run_bench();   break;
	case 2: run_optspin(); break;
	default: P("mode 只能是 0/1/2\n"); break;
	}
	P("######## done ########\n");
	return 0;
}

static void __exit lock_bench_exit(void) { P("bye\n"); }

module_init(lock_bench_init);
module_exit(lock_bench_exit);
