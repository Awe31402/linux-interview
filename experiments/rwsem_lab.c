// SPDX-License-Identifier: GPL-2.0
/*
 * rwsem_lab.c —— 讀寫信號量死鎖現場（卷2 第4章 Q10、Q11、Q12、Q13 的實驗對象）
 *
 * mode=1（預設，可回收）：模組自己的 rw_semaphore
 *     kthread rwsem_holder : down_write() 後抱著鎖睡覺
 *     kthread rwsem_rd1    : down_read()  → 卡住（RWSEM_WAITING_FOR_READ）
 *     kthread rwsem_wr2    : down_write() → 卡住（RWSEM_WAITING_FOR_WRITE）
 *     → wait_list 上剛好兩個型別不同的等待者，正好對應書上 §4.11.2 的
 *       `list -s rwsem_waiter.task,type -h <addr>` 輸出。
 *     rmmod 時 holder 放鎖、三條執行緒依序退出，機器乾乾淨淨。
 *
 * mode=2（書上 §4.10 原版，不可回收，做完要重開機）：
 *     module_init（也就是 insmod 這個行程本人）先 down_write(&mm->mmap_lock)，
 *     再呼叫 create_oops(vma, &priv, &mm->mmap_lock)，裡面 down_read() 同一把鎖
 *     → 自己鎖死自己，insmod 永遠停在 D 狀態，rmmod 也移不掉。
 *     這一版才有「局部變數 priv 在堆疊上」可以推導（Q10）。
 *
 * 用法：
 *     sudo insmod rwsem_lab.ko          # mode=1
 *     sudo insmod rwsem_lab.ko mode=2   # 書上原版，會卡死 insmod
 *
 * 對應書目：《奔跑吧 Linux 內核》第二版 卷2 §4.10（案例5）、§4.11（案例6）
 */
#define pr_fmt(fmt) "rwsem_lab: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/rwsem.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/mm_types.h>

static int mode = 1;
module_param(mode, int, 0444);
/* 把 lab_sem 的位址透過 sysfs 露出來：/sys/module/rwsem_lab/parameters/sem_addr
 * （腳本要拿這個位址餵給 crash_probe，從 dmesg 撈太脆弱）*/
static unsigned long sem_addr;
module_param(sem_addr, ulong, 0444);
MODULE_PARM_DESC(mode, "1 = 模組自己的 rwsem（可 rmmod）；2 = 書上 §4.10 的 mmap_lock 自鎖（要重開機）");

/* 書上 §4.10 的資料結構，兩個 mode 共用 */
struct mydev_priv {
	char name[64];
	int i;
	struct mm_struct *mm;
	struct rw_semaphore *sem;
};

/* ---------------- mode=1：模組自己的 rw_semaphore ---------------------------- */
static DECLARE_RWSEM(lab_sem);
static struct task_struct *th_holder, *th_rd1, *th_wr2;
static bool release_now;

static int holder_fn(void *arg)
{
	pr_info("[holder ] pid=%d 準備 down_write(&lab_sem)\n", current->pid);
	down_write(&lab_sem);
	pr_info("[holder ] pid=%d 拿到寫者鎖，開始抱著不放\n", current->pid);
	while (!kthread_should_stop() && !release_now)
		msleep(200);
	up_write(&lab_sem);
	pr_info("[holder ] pid=%d 放掉寫者鎖\n", current->pid);
	while (!kthread_should_stop())
		msleep(100);
	return 0;
}

/*
 * 和書上 §4.10 的 create_oops() 同樣的長相：三個參數、第一件事就是 down_read()。
 * 差別只在鎖換成模組自己的 lab_sem（這樣 rmmod 得掉，不必重開機）。
 * Q10 要推導的就是「第 2 個參數 priv 存在 reader_fn 堆疊的哪個位置」。
 */
static noinline int lab_create_oops(struct vm_area_struct *vma,
				    struct mydev_priv *priv,
				    struct rw_semaphore *sem)
{
	unsigned long flags;

	pr_info("[reader ] lab_create_oops: priv=%px sem=%px，準備 down_read（會卡住）\n",
		priv, sem);
	down_read(sem);
	flags = vma ? vma->vm_flags : 0;
	pr_info("[reader ] flags=0x%lx name=%s\n", flags, priv->name);
	up_read(sem);
	return 0;
}

static int reader_fn(void *arg)
{
	struct mydev_priv priv;

	memcpy(priv.name, "benshushu", sizeof("benshushu"));
	priv.i = 10;
	priv.mm = NULL;
	priv.sem = &lab_sem;

	pr_info("[reader ] pid=%d 局部變數 &priv=%px（Q10 的答案，等一下用堆疊反推）\n",
		current->pid, &priv);
	lab_create_oops(NULL, &priv, &lab_sem);
	while (!kthread_should_stop())
		msleep(100);
	return 0;
}

static int writer2_fn(void *arg)
{
	pr_info("[writer2] pid=%d 準備 down_write(&lab_sem)（預期卡住）\n", current->pid);
	down_write(&lab_sem);
	pr_info("[writer2] pid=%d 終於拿到寫者鎖\n", current->pid);
	up_write(&lab_sem);
	while (!kthread_should_stop())
		msleep(100);
	return 0;
}

/* ---------------- mode=2：書上 §4.10 原版（連變數名都照抄）------------------- */
static noinline int create_oops(struct vm_area_struct *vma,
				struct mydev_priv *priv,
				struct rw_semaphore *sem)
{
	unsigned long flags;

	pr_info("create_oops: 進來了，priv=%px sem=%px，準備 down_read（會卡死在這裡）\n",
		priv, sem);
	down_read(sem);			/* ← 自己已經持有寫者鎖，這裡永遠回不來 */

	flags = vma->vm_flags;		/* 書上原本要在這裡踩空指標 */
	pr_info("flags=0x%lx, name=%s\n", flags, priv->name);
	return 0;
}

static noinline int book_case(void)
{
	struct vm_area_struct *vma = NULL;
	struct mydev_priv priv;
	struct mm_struct *mm;

	mm = get_task_mm(current);
	if (!mm) {
		pr_err("拿不到 mm\n");
		return -EINVAL;
	}

	priv.mm  = mm;
	priv.sem = &mm->mmap_lock;	/* 3.10 叫 mmap_sem，5.8 之後改名 mmap_lock */

	pr_info("insmod pid=%d task=%px mm=%px &mm->mmap_lock=%px (offset 0x%lx)\n",
		current->pid, current, mm, &mm->mmap_lock,
		offsetof(struct mm_struct, mmap_lock));

	down_write(&mm->mmap_lock);
	pr_info("已持有 mmap_lock 寫者鎖\n");

	vma = kmalloc(sizeof(*vma), GFP_KERNEL);
	if (!vma) {
		up_write(&mm->mmap_lock);
		mmput(mm);
		return -ENOMEM;
	}
	kfree(vma);
	vma = NULL;
	smp_mb();

	memcpy(priv.name, "benshushu", sizeof("benshushu"));
	priv.i = 10;
	pr_info("局部變數 &priv=%px（Q10 要推導的就是這個位址）\n", &priv);

	return create_oops(vma, &priv, &mm->mmap_lock);
}

static int __init lab_init(void)
{
	if (mode == 2) {
		pr_info("=== mode=2：書上 §4.10 原版自鎖，insmod 會永遠卡住 ===\n");
		return book_case();	/* 不會回來 */
	}

	sem_addr = (unsigned long)&lab_sem;
	pr_info("=== mode=1：模組自己的 rwsem ===\n");
	pr_info("&lab_sem = %px  （這個位址等一下餵給 crash_probe 的 rwsem 指令）\n", &lab_sem);
	pr_info("sizeof(struct rw_semaphore)=%zu\n", sizeof(struct rw_semaphore));

	th_holder = kthread_run(holder_fn, NULL, "rwsem_holder");
	msleep(300);				/* 確保 holder 先拿到鎖 */
	th_rd1 = kthread_run(reader_fn, NULL, "rwsem_rd1");
	msleep(200);
	th_wr2 = kthread_run(writer2_fn, NULL, "rwsem_wr2");
	msleep(200);
	pr_info("holder=%d rd1=%d wr2=%d  現在去 dmesg 之外用 crash_probe 觀察\n",
		th_holder ? th_holder->pid : -1,
		th_rd1 ? th_rd1->pid : -1,
		th_wr2 ? th_wr2->pid : -1);
	return 0;
}

static void __exit lab_exit(void)
{
	release_now = true;		/* 讓 holder 放鎖，等待者才醒得過來 */
	msleep(500);
	if (th_rd1)
		kthread_stop(th_rd1);
	if (th_wr2)
		kthread_stop(th_wr2);
	if (th_holder)
		kthread_stop(th_holder);
	pr_info("卸載完成\n");
}

module_init(lab_init);
module_exit(lab_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("rw_semaphore 死鎖現場（卷2 第4章 Q10~Q13）");
