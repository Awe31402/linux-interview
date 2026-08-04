// SPDX-License-Identifier: GPL-2.0
/*
 * wq_probe.ko —— CMWQ（並發託管工作佇列）的動態行為實測
 * 平台：Radxa ROCK 5B (RK3588, 8 核)，Linux 6.1.115+ aarch64
 *
 * 回答：
 *   Q10  工作佇列跑在中斷上下文還是行程上下文？回呼能睡嗎？
 *   Q11  舊版（2.6.25）工作佇列的三個問題
 *   Q12  CMWQ 如何動態管理工作線程池裡的線程
 *   Q13  多個 work 掛在同一個工作線程上，其中一個阻塞了，其他怎麼辦？
 *
 * mode=0  把 N 個【會睡】的 work 全部丟到同一顆 CPU 的 worker_pool，
 *         觀察 kworker 執行緒數量變化 + 每個 work 由哪個 worker 跑 + 總耗時。
 *         → 這就是 Q13 的答案，也是 Q12 的直接證據。
 * mode=1  同樣 N 個 work，但改成【純燒 CPU 不睡】。
 *         → keep_working()：nr_running<=1 時同一個 worker 串行做完，不會亂長線程。
 * mode=2  alloc_ordered_workqueue()：max_active=1，就算會睡也強制串行。
 * mode=3  alloc_workqueue(max_active=2)：同時最多 2 個 work 活躍。
 *
 * 用法：
 *   sudo insmod wq_probe.ko mode=0 nwork=6 sleep_ms=200 ; sudo rmmod wq_probe
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/hardirq.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CMWQ dynamic worker-pool behaviour prober for RK3588");

static int mode;
static int nwork = 6;
static int sleep_ms = 200;
static int burn_ms = 60;
static int cpu_target = 3;
module_param(mode, int, 0444);
module_param(nwork, int, 0444);
module_param(sleep_ms, int, 0444);
module_param(burn_ms, int, 0444);
module_param(cpu_target, int, 0444);

#define P(fmt, ...) pr_info("wq_probe: " fmt, ##__VA_ARGS__)
#define MAXW 32

struct probe_work {
	struct work_struct w;
	int		idx;
	int		cpu;
	char		comm[TASK_COMM_LEN];
	pid_t		pid;
	unsigned int	pc;
	int		in_task, in_interrupt;
	u64		t_start, t_end;
	int		slept_ok;
};

static struct probe_work pw[MAXW];
static atomic_t remaining;
static DECLARE_COMPLETION(all_done);
static atomic_t concur = ATOMIC_INIT(0);
static atomic_t maxconcur = ATOMIC_INIT(0);
static u64 t0_global;
static struct workqueue_struct *my_wq;

static void bump(void)
{
	int c = atomic_inc_return(&concur), m;

	do {
		m = atomic_read(&maxconcur);
		if (c <= m)
			break;
	} while (atomic_cmpxchg(&maxconcur, m, c) != m);
}

/* 列出目前系統上綁在某顆 CPU 的 kworker 執行緒 */
static int list_kworkers(int cpu, char *out, size_t n)
{
	struct task_struct *p;
	char pfx[16];
	int cnt = 0;
	size_t used = 0;

	scnprintf(pfx, sizeof(pfx), "kworker/%d:", cpu);

	rcu_read_lock();
	for_each_process(p) {
		if (strncmp(p->comm, pfx, strlen(pfx)))
			continue;
		cnt++;
		if (used < n - 1)
			used += scnprintf(out + used, n - used, "%s(%d) ",
					  p->comm, p->pid);
	}
	rcu_read_unlock();
	out[used] = '\0';
	return cnt;
}

static void work_sleep_fn(struct work_struct *w)
{
	struct probe_work *p = container_of(w, struct probe_work, w);

	p->cpu = raw_smp_processor_id();
	strscpy(p->comm, current->comm, sizeof(p->comm));
	p->pid = current->pid;
	p->pc = preempt_count();
	p->in_task = !!in_task();
	p->in_interrupt = !!in_interrupt();
	p->t_start = ktime_get_ns();
	bump();

	msleep(sleep_ms);		/* ← 阻塞操作 */
	p->slept_ok = 1;

	atomic_dec(&concur);
	p->t_end = ktime_get_ns();
	if (atomic_dec_and_test(&remaining))
		complete(&all_done);
}

static void work_burn_fn(struct work_struct *w)
{
	struct probe_work *p = container_of(w, struct probe_work, w);
	ktime_t s = ktime_get();

	p->cpu = raw_smp_processor_id();
	strscpy(p->comm, current->comm, sizeof(p->comm));
	p->pid = current->pid;
	p->pc = preempt_count();
	p->in_task = !!in_task();
	p->in_interrupt = !!in_interrupt();
	p->t_start = ktime_get_ns();
	bump();

	while (ktime_to_ns(ktime_sub(ktime_get(), s)) < (s64)burn_ms * NSEC_PER_MSEC)
		cpu_relax();

	atomic_dec(&concur);
	p->t_end = ktime_get_ns();
	if (atomic_dec_and_test(&remaining))
		complete(&all_done);
}

static void report(const char *title, int expect_serial)
{
	int i;
	u64 last = 0;
	int distinct = 0;
	pid_t seen[MAXW];

	P("");
	P("---------------- %s ----------------", title);
	P("%-4s %-4s %-18s %-7s %-11s %-10s %-10s %s",
	  "work", "cpu", "worker(comm)", "pid", "preempt_cnt", "起(ms)", "訖(ms)", "in_task/in_intr");
	for (i = 0; i < nwork; i++) {
		struct probe_work *p = &pw[i];
		int j, dup = 0;

		for (j = 0; j < distinct; j++)
			if (seen[j] == p->pid)
				dup = 1;
		if (!dup && p->pid)
			seen[distinct++] = p->pid;
		if (p->t_end > last)
			last = p->t_end;

		P("%-4d %-4d %-18s %-7d 0x%08x %-10llu %-10llu %d / %d",
		  p->idx, p->cpu, p->comm, p->pid, p->pc,
		  (p->t_start - t0_global) / NSEC_PER_MSEC,
		  (p->t_end - t0_global) / NSEC_PER_MSEC,
		  p->in_task, p->in_interrupt);
	}
	P("");
	P("  不同的 worker 執行緒數 = %d", distinct);
	P("  最大同時活躍 work 數   = %d", atomic_read(&maxconcur));
	P("  總耗時                 = %llu ms", (last - t0_global) / NSEC_PER_MSEC);
	P("  若完全串行應為         = %d ms",
	  nwork * (expect_serial ? burn_ms : sleep_ms));
}

/* ================= mode=0：Q12/Q13 會睡的 work ================= */

static void run_mode0(void)
{
	char buf[512];
	int before, after, i;

	P("mode=0：Q13 —— %d 個【會睡 %d ms】的 work 全部丟到 cpu%d 的 worker_pool",
	  nwork, sleep_ms, cpu_target);

	before = list_kworkers(cpu_target, buf, sizeof(buf));
	P("  排入前 cpu%d 上的 kworker（%d 個）：%s", cpu_target, before, buf);

	atomic_set(&remaining, nwork);
	atomic_set(&concur, 0);
	atomic_set(&maxconcur, 0);
	reinit_completion(&all_done);
	t0_global = ktime_get_ns();

	for (i = 0; i < nwork; i++) {
		memset(&pw[i], 0, sizeof(pw[i]));
		pw[i].idx = i;
		INIT_WORK(&pw[i].w, work_sleep_fn);
		queue_work_on(cpu_target, system_wq, &pw[i].w);
	}
	/* 讓 worker 們都睡進去之後再數一次 */
	msleep(sleep_ms / 2);
	after = list_kworkers(cpu_target, buf, sizeof(buf));
	P("  執行中 cpu%d 上的 kworker（%d 個）：%s", cpu_target, after, buf);

	wait_for_completion_timeout(&all_done, 30 * HZ);
	report("Q13：某個 work 阻塞了，其他 work 怎麼辦？", 0);
	P("");
	P("  → cpu%d 的 kworker 數量：%d → %d（多出 %d 個）",
	  cpu_target, before, after, after - before);
	P("  ✓ 答案：**不用等**。CMWQ 在 worker 睡下去時（schedule() → sched_submit_work()");
	P("    → wq_worker_sleeping()）把 pool->nr_running 減 1，發現變成 0 而 worklist 還有東西，");
	P("    就 wake_up_worker() 叫醒／manage_workers() 新建一個 worker 接手剩下的 work。");
	P("    所以總耗時 ≈ 單一個 work 的時間，而不是 N 倍。");
	P("    （kernel/workqueue.c: wq_worker_sleeping()、need_more_worker()、maybe_create_worker()）");
}

/* ================= mode=1：純燒 CPU，keep_working() ================= */

static void run_mode1(void)
{
	char buf[512];
	int before, after, i;

	P("mode=1：Q12 —— %d 個【純燒 CPU %d ms 不睡】的 work 丟到 cpu%d",
	  nwork, burn_ms, cpu_target);

	before = list_kworkers(cpu_target, buf, sizeof(buf));
	P("  排入前 cpu%d 上的 kworker（%d 個）：%s", cpu_target, before, buf);

	atomic_set(&remaining, nwork);
	atomic_set(&concur, 0);
	atomic_set(&maxconcur, 0);
	reinit_completion(&all_done);
	t0_global = ktime_get_ns();

	for (i = 0; i < nwork; i++) {
		memset(&pw[i], 0, sizeof(pw[i]));
		pw[i].idx = i;
		INIT_WORK(&pw[i].w, work_burn_fn);
		queue_work_on(cpu_target, system_wq, &pw[i].w);
	}
	msleep(burn_ms * nwork / 2 + 5);
	after = list_kworkers(cpu_target, buf, sizeof(buf));
	P("  執行中 cpu%d 上的 kworker（%d 個）：%s", cpu_target, after, buf);

	wait_for_completion_timeout(&all_done, 60 * HZ);
	report("Q12：work 不睡的時候，工作線程池不會亂長線程", 1);
	P("");
	P("  → kworker 數量：%d → %d", before, after);
	P("  ✓ keep_working()：!list_empty(&pool->worklist) && pool->nr_running <= 1");
	P("    只要還有活躍的 worker 在跑，就不叫醒／新建第二個 —— 同一個 worker 串行做完，");
	P("    所以總耗時 ≈ N × %d ms，且所有 work 的 worker pid 相同。", burn_ms);
}

/* ================= mode=2/3：ordered / max_active ================= */

static void run_wq_mode(const char *title, int max_active, int ordered)
{
	int i;

	if (ordered)
		my_wq = alloc_ordered_workqueue("wq_probe_ord", 0);
	else
		my_wq = alloc_workqueue("wq_probe_ma", 0, max_active);
	if (!my_wq) {
		P("alloc_workqueue 失敗");
		return;
	}

	atomic_set(&remaining, nwork);
	atomic_set(&concur, 0);
	atomic_set(&maxconcur, 0);
	reinit_completion(&all_done);
	t0_global = ktime_get_ns();

	for (i = 0; i < nwork; i++) {
		memset(&pw[i], 0, sizeof(pw[i]));
		pw[i].idx = i;
		INIT_WORK(&pw[i].w, work_sleep_fn);
		queue_work_on(cpu_target, my_wq, &pw[i].w);
	}
	wait_for_completion_timeout(&all_done, 60 * HZ);
	report(title, 0);
	P("");
	P("  → max_active = %d（ordered 就是 max_active=1 的 UNBOUND 佇列），",
	  ordered ? 1 : max_active);
	P("    pool_workqueue->nr_active 到頂之後，新的 work 會被放進 pwq->inactive_works，");
	P("    等前面的做完才輪到（6.1 的欄位名，書上 5.0 叫 delayed_works）。");
}

static int __init wq_probe_init(void)
{
	if (nwork > MAXW)
		nwork = MAXW;

	P("================================================================");
	P("wq_probe.ko  mode=%d nwork=%d sleep_ms=%d burn_ms=%d cpu_target=%d",
	  mode, nwork, sleep_ms, burn_ms, cpu_target);
	P("Q10 先答：insmod 這裡 in_task()=%d in_interrupt()=%d preempt_count=0x%08x",
	  !!in_task(), !!in_interrupt(), preempt_count());
	P("================================================================");

	switch (mode) {
	case 0: run_mode0(); break;
	case 1: run_mode1(); break;
	case 2: run_wq_mode("Q11：alloc_ordered_workqueue() 強制串行", 1, 1); break;
	case 3: run_wq_mode("max_active=2：同時最多 2 個 work 活躍", 2, 0); break;
	default: P("未知 mode"); break;
	}
	return 0;
}

static void __exit wq_probe_exit(void)
{
	if (my_wq) {
		flush_workqueue(my_wq);
		destroy_workqueue(my_wq);
	}
	P("卸載完畢");
}

module_init(wq_probe_init);
module_exit(wq_probe_exit);
