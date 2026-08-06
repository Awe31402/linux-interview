// SPDX-License-Identifier: GPL-2.0
/*
 * arm64_lab.c —— 卷2 第5章 Q1、Q3、Q4~Q7 的實驗對象（ARM64 版的「宕機現場」）
 *
 * 這支模組做的事：造一個「三層呼叫、最後卡死在讀寫信號量上」的核心執行緒，
 * 而且把**每一層的正確答案（ground truth）先印到 dmesg**，
 * 這樣等一下用 crash 工具推導出來的東西可以逐項對答案。
 *
 *   lab_main()  ── 局部變數 priv（struct lab_priv，含 "benshushu"）
 *     └─ lab_func1(priv, magic)        ── 局部變數 local1
 *          └─ lab_func2(priv, sem, magic, ...)  ── 局部變數 local2
 *               └─ down_read(sem)  ← 永遠卡在這裡（寫者鎖被 lab_holder 抱走了）
 *
 * 對應書上：
 *   §5.3 案例2「恢復函數調用棧」    → 我們印出每層真正的 FP，crash 的 bt 應該完全一致
 *   §5.4 案例3「分析和推導參數的值」→ 我們印出 priv/sem/magic 的真值，
 *                                     再用 [sp+N] 的方式從堆疊推導，看對不對得上
 *   §5.5 案例4「複雜的宕機案例」    → lab_holder / lab_writer / lab_reader 三方搶同一把鎖
 *
 * 用法：
 *   sudo insmod arm64_lab.ko           # 造現場；rmmod 可以完整收回，不必重開機
 *   sudo insmod arm64_lab.ko mmap=1    # 另外再讓一條 kthread 去搶 insmod 行程的 mm->mmap_lock
 *
 * 注意：mmap=1 會讓一條核心執行緒永久持有某個行程的 mmap_lock 讀者鎖，
 *       只影響那個行程，rmmod 時會放掉。
 */
#define pr_fmt(fmt) "arm64_lab: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/slab.h>

#define LAB_MAGIC1	0x1122334455667788UL
#define LAB_MAGIC2	0xdeadbeefcafe0001UL

/*
 * noipa = 關掉跨函式最佳化。不加的話 GCC 會做 constprop/isra，
 * 把「參數是常數」的函式改寫成 lab_func2.constprop.0.isra.0（參數被優化掉），
 * 就沒有「參數存在堆疊哪裡」可以推導了（書上 §5.4 的重點）。
 */
#define LAB_NOOPT	noinline __attribute__((noipa))

/* 讓參數變成 runtime 的值，GCC 就沒辦法常數傳播 */
static unsigned long magic = LAB_MAGIC1;
module_param(magic, ulong, 0444);

struct lab_priv {			/* 書上 §4.10/§5.4 的 mydev_priv */
	char name[64];
	int i;
	struct mm_struct *mm;
	struct rw_semaphore *sem;
};

static DECLARE_RWSEM(lab_sem);
static struct task_struct *th_holder, *th_main, *th_writer;
static bool release_now;

/* 讓腳本可以直接拿到位址，不必從 dmesg 撈 */
static unsigned long sem_addr;
module_param(sem_addr, ulong, 0444);
static unsigned long priv_addr;
module_param(priv_addr, ulong, 0444);
static unsigned long fp_main, fp_func1, fp_func2;
module_param(fp_main, ulong, 0444);
module_param(fp_func1, ulong, 0444);
module_param(fp_func2, ulong, 0444);

/* ---------------- 抱著寫者鎖不放的那一位 ------------------------------------ */
static int holder_fn(void *arg)
{
	pr_info("[holder] pid=%d 準備 down_write(&lab_sem)\n", current->pid);
	down_write(&lab_sem);
	pr_info("[holder] pid=%d 拿到寫者鎖，開始抱著不放\n", current->pid);
	while (!kthread_should_stop() && !release_now)
		msleep(200);
	up_write(&lab_sem);
	pr_info("[holder] pid=%d 放掉寫者鎖\n", current->pid);
	while (!kthread_should_stop())
		msleep(100);
	return 0;
}

/* ---------------- 第三層：真正卡死的地方 ------------------------------------ */
static LAB_NOOPT int lab_func2(struct lab_priv *priv, struct rw_semaphore *sem,
			      unsigned long magic, unsigned long a4,
			      unsigned long a5, unsigned long a6,
			      unsigned long a7, unsigned long a8)
{
	unsigned long local2 = LAB_MAGIC2;
	unsigned long sp;

	asm volatile("mov %0, sp" : "=r"(sp));
	fp_func2 = (unsigned long)__builtin_frame_address(0);

	pr_info("[答案] lab_func2: x29=%lx sp=%lx  &local2=%px(local2=0x%lx)\n",
		fp_func2, sp, &local2, local2);
	pr_info("[答案] lab_func2 的參數：priv=%px sem=%px magic=0x%lx a8=0x%lx\n",
		priv, sem, magic, a8);
	pr_info("[答案] 即將 down_read(%px)，之後這條執行緒就進 D 狀態了\n", sem);

	down_read(sem);			/* ← 卡死在這裡 */

	pr_info("[答案] lab_func2 醒了，local2=0x%lx priv->name=%s\n", local2, priv->name);
	up_read(sem);
	return 0;
}

/* ---------------- 第二層 ---------------------------------------------------- */
static LAB_NOOPT int lab_func1(struct lab_priv *priv, unsigned long magic)
{
	unsigned long local1 = LAB_MAGIC1;
	unsigned long sp;

	asm volatile("mov %0, sp" : "=r"(sp));
	fp_func1 = (unsigned long)__builtin_frame_address(0);
	pr_info("[答案] lab_func1: x29=%lx sp=%lx  &local1=%px(local1=0x%lx)\n",
		fp_func1, sp, &local1, local1);

	return lab_func2(priv, &lab_sem, magic, 4, 5, 6, 7, 0x8888);
}

/* ---------------- 第一層（kthread 的進入點）--------------------------------- */
static int lab_main(void *arg)
{
	struct lab_priv priv;
	unsigned long sp;

	memcpy(priv.name, "benshushu", sizeof("benshushu"));
	priv.i	 = 10;
	priv.mm	 = NULL;
	priv.sem = &lab_sem;

	asm volatile("mov %0, sp" : "=r"(sp));
	fp_main	  = (unsigned long)__builtin_frame_address(0);
	priv_addr = (unsigned long)&priv;

	pr_info("[答案] lab_main : x29=%lx sp=%lx  &priv=%px（Q4 要推導的就是它）\n",
		fp_main, sp, &priv);
	pr_info("[答案] lab_main : pid=%d task=%px 核心堆疊=%px\n",
		current->pid, current, task_stack_page(current));

	lab_func1(&priv, magic);

	while (!kthread_should_stop())
		msleep(100);
	return 0;
}

/* ---------------- 另一位等待者（寫者），讓 wait_list 有兩種型別 ------------- */
static int writer_fn(void *arg)
{
	pr_info("[writer] pid=%d 準備 down_write(&lab_sem)（預期卡住）\n", current->pid);
	down_write(&lab_sem);
	pr_info("[writer] pid=%d 終於拿到寫者鎖\n", current->pid);
	up_write(&lab_sem);
	while (!kthread_should_stop())
		msleep(100);
	return 0;
}

static int __init lab_init(void)
{
	sem_addr = (unsigned long)&lab_sem;

	pr_info("=== ARM64 宕機現場 ===\n");
	pr_info("&lab_sem = %px   sizeof(struct rw_semaphore)=%zu\n",
		&lab_sem, sizeof(struct rw_semaphore));
	pr_info("offsetof(mm_struct, mmap_lock) = 0x%lx（書上 5.0 是 0x60、3.10 是 0x78）\n",
		offsetof(struct mm_struct, mmap_lock));

	th_holder = kthread_run(holder_fn, NULL, "lab_holder");
	msleep(300);
	th_main = kthread_run(lab_main, NULL, "lab_main");
	msleep(300);
	th_writer = kthread_run(writer_fn, NULL, "lab_writer");
	msleep(200);

	pr_info("holder=%d main=%d writer=%d\n",
		th_holder ? th_holder->pid : -1,
		th_main ? th_main->pid : -1,
		th_writer ? th_writer->pid : -1);
	pr_info("三層框架的正確答案：lab_main x29=%lx / lab_func1 x29=%lx / lab_func2 x29=%lx\n",
		fp_main, fp_func1, fp_func2);
	return 0;
}

static void __exit lab_exit(void)
{
	release_now = true;
	msleep(500);
	if (th_main)
		kthread_stop(th_main);
	if (th_writer)
		kthread_stop(th_writer);
	if (th_holder)
		kthread_stop(th_holder);
	pr_info("卸載完成\n");
}

module_init(lab_init);
module_exit(lab_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ARM64 函式棧 / 讀寫信號量死鎖現場（卷2 第5章 Q1、Q3~Q7）");
