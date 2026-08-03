// SPDX-License-Identifier: GPL-2.0
/*
 * rcu_demo.ko —— 《奔跑吧 Linux 內核》卷2 第 1 章 RCU 實驗
 * 平台：Radxa ROCK 5B (RK3588)，Linux 6.1.115+ aarch64
 *
 * 模式：
 *   mode=0（預設）：書上 §1.10.1 的讀者/寫者例子，跑幾輪就自動停。
 *                   -> Q27、Q29、Q36、Q37
 *   mode=1：量 synchronize_rcu() / synchronize_rcu_expedited() / call_rcu()
 *           的實際延遲，以及一個 GP 的生命週期。
 *                   -> Q28、Q29、Q30、Q32
 *   mode=2：SLAB_TYPESAFE_BY_RCU 的行為（書上 §1.11.6 的 anon_vma 場景）。
 *                   -> Q36
 *   mode=3：故意觸發 RCU stall 警告（會先把 stall timeout 調小，結束後還原）。
 *                   -> Q29、§1.11.7
 *           ⚠ 這個模式會佔用一顆 CPU 數秒並在 dmesg 噴警告，屬預期行為。
 *
 * 用法：
 *   sudo insmod rcu_demo.ko mode=0
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 *   sudo rmmod rcu_demo
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include <linux/rculist.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched/clock.h>
#include <linux/atomic.h>
#include <linux/completion.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RCU demo for RK3588 kernel study notes");

static int mode;
module_param(mode, int, 0444);
static int rounds = 5;
module_param(rounds, int, 0444);
static int stall_ms = 8000;
module_param(stall_ms, int, 0444);

#define P(fmt, ...) pr_info("rcu_demo: " fmt, ##__VA_ARGS__)

/* ================================================================== */
/* mode 0：書上 §1.10.1 的例子                                          */
/* ================================================================== */
struct foo {
	int a;
	struct rcu_head rcu;
};

static struct foo __rcu *g_ptr;
static struct task_struct *reader_t, *writer_t;
static atomic_t done = ATOMIC_INIT(0);
static atomic_t nr_freed = ATOMIC_INIT(0);

static int myrcu_reader_thread(void *data)
{
	struct foo *p;
	int i = 0;

	while (!kthread_should_stop() && i++ < rounds * 2) {
		msleep(200);
		/*
		 * 讀者臨界區。在 CONFIG_PREEMPT_RCU=n 的核心上，
		 * rcu_read_lock() 幾乎是空操作（只有 preempt_disable() + barrier）。
		 * 「離開臨界區」是靠 CPU 經歷一次靜止狀態（context switch / idle /
		 * 回到使用者態 / cond_resched）來認定的。
		 */
		rcu_read_lock();
		p = rcu_dereference(g_ptr);
		if (p)
			P("  [reader ] CPU%d 讀到 a = %d\n", smp_processor_id(), p->a);
		rcu_read_unlock();
	}
	atomic_inc(&done);
	return 0;
}

static void myrcu_del(struct rcu_head *rh)
{
	struct foo *p = container_of(rh, struct foo, rcu);

	P("  [callback] 舊資料 a=%d 現在才真正被釋放（GP 結束後）\n", p->a);
	atomic_inc(&nr_freed);
	kfree(p);
}

static int myrcu_writer_thread(void *data)
{
	int value = 5;
	int i = 0;

	while (!kthread_should_stop() && i++ < rounds) {
		struct foo *old, *new_ptr;

		msleep(400);
		new_ptr = kmalloc(sizeof(*new_ptr), GFP_KERNEL);
		if (!new_ptr)
			break;
		old = rcu_dereference_protected(g_ptr, 1);
		*new_ptr = *old;
		new_ptr->a = value;
		P("  [writer ] CPU%d 準備把 a 從 %d 改成 %d（先複製再改）\n",
		  smp_processor_id(), old->a, value);
		rcu_assign_pointer(g_ptr, new_ptr);	/* 發布新資料 */
		call_rcu(&old->rcu, myrcu_del);		/* 註冊回收舊資料的回呼 */
		P("  [writer ] rcu_assign_pointer() 已發布；call_rcu() 已註冊，"
		  "但舊資料還沒被釋放\n");
		value++;
	}
	atomic_inc(&done);
	return 0;
}

static void run_example(void)
{
	struct foo *p;

	P("=========================================================\n");
	P("== Q27/Q29/Q36：RCU 讀者/寫者範例（書上 §1.10.1）==\n");
	P("=========================================================\n");
	P("  CONFIG_PREEMPT_RCU = %s   <-- n 表示「不可搶佔 RCU」，\n",
	  IS_ENABLED(CONFIG_PREEMPT_RCU) ? "y" : "n");
	P("      靜止狀態 = 行程切換 / 進 idle / 回到使用者態 / cond_resched()\n");
	P("      也因此 rcu_read_lock() 幾乎零成本（見 Q27）\n");
	P("  CONFIG_TREE_RCU = %s，GP kthread 叫做 \"%s\"\n",
	  IS_ENABLED(CONFIG_TREE_RCU) ? "y" : "n",
	  IS_ENABLED(CONFIG_PREEMPT_RCU) ? "rcu_preempt" : "rcu_sched");

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return;
	p->a = 0;
	rcu_assign_pointer(g_ptr, p);

	reader_t = kthread_run(myrcu_reader_thread, NULL, "rcu_reader");
	writer_t = kthread_run(myrcu_writer_thread, NULL, "rcu_writer");
	while (atomic_read(&done) < 2)
		msleep(100);
	P("\n  共回收了 %d 份舊資料\n", atomic_read(&nr_freed));
	P("  重點：寫者呼叫 call_rcu() 之後「立刻返回」，不會等待；\n");
	P("        舊資料要等所有 CPU 都經歷一次靜止狀態（= 一個 GP）之後，\n");
	P("        才由 RCU 的軟中斷/kthread 呼叫回呼函式釋放。\n");
}

static void cleanup_example(void)
{
	struct foo *p = rcu_dereference_protected(g_ptr, 1);

	rcu_assign_pointer(g_ptr, NULL);
	synchronize_rcu();
	kfree(p);
	rcu_barrier();		/* 等所有 call_rcu 回呼跑完，否則 rmmod 會 oops */
}

/* ================================================================== */
/* mode 1：量 GP 的延遲                                                 */
/* ================================================================== */
static struct completion cb_done;
static u64 cb_start;
static u64 cb_lat;
static struct rcu_head probe_rh;

static void probe_cb(struct rcu_head *rh)
{
	cb_lat = local_clock() - cb_start;
	complete(&cb_done);
}

static void run_timing(void)
{
	u64 t0, t1;
	int i;
	u64 sum = 0, mn = ~0ULL, mx = 0;

	P("=========================================================\n");
	P("== Q28/Q29/Q30/Q32：宽限期(GP)的實際長度 ==\n");
	P("=========================================================\n");

	P("\n  (a) synchronize_rcu()：同步等待一個 GP 結束\n");
	for (i = 0; i < 10; i++) {
		t0 = local_clock();
		synchronize_rcu();
		t1 = local_clock();
		sum += t1 - t0;
		if (t1 - t0 < mn) mn = t1 - t0;
		if (t1 - t0 > mx) mx = t1 - t0;
		P("      第 %2d 次: %6llu us\n", i + 1, (t1 - t0) / 1000);
	}
	P("      平均 %llu us，最短 %llu us，最長 %llu us\n",
	  sum / 10 / 1000, mn / 1000, mx / 1000);

	P("\n  (b) synchronize_rcu_expedited()：用 IPI 逼所有 CPU 立刻回報靜止狀態\n");
	sum = 0; mn = ~0ULL; mx = 0;
	for (i = 0; i < 10; i++) {
		t0 = local_clock();
		synchronize_rcu_expedited();
		t1 = local_clock();
		sum += t1 - t0;
		if (t1 - t0 < mn) mn = t1 - t0;
		if (t1 - t0 > mx) mx = t1 - t0;
	}
	P("      平均 %llu us，最短 %llu us，最長 %llu us\n",
	  sum / 10 / 1000, mn / 1000, mx / 1000);
	P("      -> expedited 快很多，但代價是對每顆 CPU 送 IPI，干擾實時性，\n");
	P("         所以只用在關鍵路徑（如 CPU 熱插拔、模組卸載）。\n");

	P("\n  (c) call_rcu()：非同步註冊回呼，量「註冊 -> 回呼被叫到」的延遲\n");
	init_completion(&cb_done);
	cb_start = local_clock();
	call_rcu(&probe_rh, probe_cb);
	wait_for_completion(&cb_done);
	P("      call_rcu() 回呼延遲 = %llu us\n", cb_lat / 1000);
	P("      -> call_rcu() 本身「立刻返回」，回呼要等 GP 結束＋批次處理，\n");
	P("         所以延遲比 synchronize_rcu() 還長是正常的。\n");

	P("\n  (d) rcu_barrier()：等所有已註冊的回呼都跑完\n");
	t0 = local_clock();
	rcu_barrier();
	t1 = local_clock();
	P("      rcu_barrier() 花了 %llu us\n", (t1 - t0) / 1000);

	P("\n  怎麼解讀（Q28/Q32）：\n");
	P("    一個 GP 的生命週期 = 「開始 -> 每顆 CPU 各報一次靜止狀態 -> 結束 -> 跑回呼」。\n");
	P("    在不可搶佔 RCU 上，靜止狀態靠時鐘節拍（HZ=%d，每 %d ms 一次）去偵測，\n",
	  HZ, 1000 / HZ);
	P("    所以一個 GP 的下限大約就是幾個節拍，實測值與此吻合。\n");
	P("    用 ftrace 的 rcu:rcu_grace_period 事件可以看到 GP 的狀態機轉換。\n");
}

/* ================================================================== */
/* mode 2：SLAB_TYPESAFE_BY_RCU                                         */
/* ================================================================== */
struct tsobj {
	unsigned long key;
	atomic_t refcount;
};
static struct kmem_cache *ts_cache;

static void ts_ctor(void *p)
{
	struct tsobj *o = p;

	atomic_set(&o->refcount, 0);
}

static void run_typesafe(void)
{
	struct tsobj *a, *b;

	P("=========================================================\n");
	P("== Q36：SLAB_TYPESAFE_BY_RCU（書上寫的 SLAB_DESTROY_BY_RCU）==\n");
	P("=========================================================\n");
	P("  ⚠ 書上用的名字 SLAB_DESTROY_BY_RCU 在 Linux 4.9 已改名為\n");
	P("     SLAB_TYPESAFE_BY_RCU（commit 5f0d5a3ae7cf），語意不變。\n\n");

	ts_cache = kmem_cache_create("rcu_demo_ts", sizeof(struct tsobj), 0,
				     SLAB_TYPESAFE_BY_RCU, ts_ctor);
	if (!ts_cache) { P("  建立 slab cache 失敗\n"); return; }

	a = kmem_cache_alloc(ts_cache, GFP_KERNEL);
	if (!a) goto out;
	a->key = 0xdeadbeef;
	atomic_set(&a->refcount, 1);
	/* 註：%px 後面要留一個空格，否則 printk 的 %p 擴充解析會吃掉後面多位元組字元的第一個 byte */
	P("  配置物件 a = %px  key = 0x%lx  refcount = %d\n",
	  a, READ_ONCE(a->key), atomic_read(&a->refcount));

	kmem_cache_free(ts_cache, a);
	P("  kmem_cache_free(a) 之後：\n");
	P("    * 物件「立刻」還給 slab，可以馬上被重新配置出去\n");
	P("    * 但物件所在的『頁面』要等一個 GP 之後才會還給伙伴系統\n");
	P("    * 所以 a 這個位址一定還是「可安全讀取」的記憶體，不會 oops\n");

	P("  free 之後用舊指標 a 讀 key（記憶體仍然有效，不會 oops）= 0x%lx\n",
	  READ_ONCE(a->key));

	b = kmem_cache_alloc(ts_cache, GFP_KERNEL);
	P("  再配置一個 b = %px  %s\n", b,
	  b == a ? "<-- ★ 拿到同一塊記憶體！" : "(這次拿到別塊)");
	if (b) {
		WRITE_ONCE(b->key, 0x12345678);
		/* 用 READ_ONCE 逼編譯器真的重讀，否則它會把舊值留在暫存器裡 */
		P("    寫入 b->key = 0x%lx 之後，用「舊指標 a」讀出來的 key = 0x%lx\n",
		  READ_ONCE(b->key), READ_ONCE(a->key));
		P("    -> 位址還活著，但內容已經是別人的了；這就是 type-safe 而非 value-safe\n");
		kmem_cache_free(ts_cache, b);
	}

	P("\n  這就是為什麼 page_get_anon_vma() 光有 rcu_read_lock() 還不夠，\n");
	P("  一定要再做兩道驗證（mm/rmap.c）：\n");
	P("     1) atomic_inc_not_zero(&anon_vma->refcount)  —— 搶到參考計數才算數\n");
	P("     2) 再檢查一次 page_mapped(page)              —— 確認拿到的還是同一個物件\n");
	P("  對應書上的樣板：lockless_lookup -> try_get_ref -> 驗證 obj->key。\n");
out:
	rcu_barrier();
	kmem_cache_destroy(ts_cache);
	ts_cache = NULL;
}

/* ================================================================== */
/* mode 3：RCU stall                                                    */
/* ================================================================== */
static int stall_thread(void *data)
{
	P("  [stall] 在 CPU%d 上進入 RCU 讀者臨界區，然後 mdelay(%d)…\n",
	  smp_processor_id(), stall_ms);
	rcu_read_lock();
	/*
	 * mdelay() 是忙等，不會呼叫 schedule()/cond_resched()，
	 * 所以這顆 CPU 一直不會經歷靜止狀態 -> GP 無法結束 -> 觸發停滯偵測。
	 */
	{
		int left = stall_ms;
		while (left > 0) {
			mdelay(100);
			left -= 100;
		}
	}
	rcu_read_unlock();
	P("  [stall] 離開臨界區，GP 現在可以結束了\n");
	atomic_inc(&done);
	return 0;
}

static void run_stall(void)
{
	struct task_struct *t;

	P("=========================================================\n");
	P("== §1.11.7 / Q29：故意觸發 RCU 停滯偵測 ==\n");
	P("=========================================================\n");
	P("  ⚠ 先在使用者態把偵測門檻調小，再 insmod：\n");
	P("      echo 5 > /sys/module/rcupdate/parameters/rcu_cpu_stall_timeout\n");
	P("    跑完記得還原：\n");
	P("      echo 60 > /sys/module/rcupdate/parameters/rcu_cpu_stall_timeout\n");
	P("  （rcu_cpu_stall_timeout 沒有匯出給模組，只能從 sysfs 改）\n");
	atomic_set(&done, 0);

	t = kthread_create(stall_thread, NULL, "rcu_staller");
	if (IS_ERR(t)) return;
	kthread_bind(t, num_online_cpus() - 1);	/* 挑最後一顆，別動 CPU0 */
	wake_up_process(t);

	while (atomic_read(&done) < 1)
		msleep(200);

	P("\n  上面應該會出現 \"rcu: INFO: rcu_sched self-detected stall on CPU\"，\n");
	P("  以及發生停滯時的函式呼叫堆疊 —— 那正是書上 §1.11.7 的輸出。\n");
	P("  常見成因：讀者臨界區裡死迴圈、長時間關中斷/關搶佔、\n");
	P("            不可搶佔核心裡的長迴圈沒有 cond_resched()。\n");
}

/* ================================================================== */
static int __init rcu_demo_init(void)
{
	P("######## RK3588 RCU 實驗 mode=%d ########\n", mode);
	switch (mode) {
	case 0: run_example();  break;
	case 1: run_timing();   break;
	case 2: run_typesafe(); break;
	case 3: run_stall();    break;
	default: P("mode 只能是 0/1/2/3\n"); break;
	}
	P("######## done ########\n");
	return 0;
}

static void __exit rcu_demo_exit(void)
{
	if (mode == 0)
		cleanup_example();
	rcu_barrier();
	P("bye\n");
}

module_init(rcu_demo_init);
module_exit(rcu_demo_exit);
