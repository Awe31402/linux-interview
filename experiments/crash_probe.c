// SPDX-License-Identifier: GPL-2.0
/*
 * crash_probe.c —— 「沒有 Kdump/Crash 的機器上，自己當 crash 工具」
 *
 * 卷2 第4章 Q10~Q13 全部要用 `crash` 的 bt -f / rd / struct / list / task -R / runq -t
 * 這幾個子命令。可是本機（RK3588 + Rockchip 6.1.115）：
 *   # CONFIG_KEXEC is not set / # CONFIG_CRASH_DUMP is not set  → 根本沒有 vmcore
 *   CONFIG_DEBUG_INFO_NONE=y                                    → 也沒有帶符號的 vmlinux
 * 所以這支模組把 crash 的那幾個子命令「用核心模組自己實作一遍」，對照表：
 *
 *   crash 子命令          本模組指令                說明
 *   ------------------    ----------------------    ----------------------------------
 *   ps / ps | grep UN     ps                        列出所有 D 狀態（TASK_UNINTERRUPTIBLE）行程
 *   bt <pid>              bt <pid>                  用 x29 框架鏈回溯（= x86 的 RBP 鏈）
 *   bt -f <pid>           btf <pid>                 逐格 dump 核心堆疊原始內容 + 符號標註
 *   rd <addr>             rd <addr> [n]             讀核心記憶體
 *   struct rw_semaphore   rwsem <addr>              解碼 count/owner + 走訪 wait_list（Q11、Q12）
 *   struct mm_struct      mm <pid>                  印 mm 與 mmap_lock 的位址、擁有者
 *   task -R sched_info    sched <pid>               印 sched_info + 算出「被阻塞了多久」（Q13）
 *   runq -t               runq                      印每顆 CPU 的 rq clock（sched_clock_cpu）
 *   sym <name>            sym <name>                kallsyms 查符號
 *
 * 用法（輸出全部在 dmesg）：
 *   sudo insmod crash_probe.ko
 *   echo 'ps'          | sudo tee /proc/crash_probe
 *   echo 'btf 1234'    | sudo tee /proc/crash_probe
 *   echo 'rwsem ffff0000c1234478' | sudo tee /proc/crash_probe
 *   echo 'sched 1234'  | sudo tee /proc/crash_probe
 *
 * 對應書目：《奔跑吧 Linux 內核》第二版 卷2 §4.4（crash 命令）、§4.10、§4.11
 */
#define pr_fmt(fmt) "crash_probe: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/sched/clock.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/rwsem.h>
#include <linux/kallsyms.h>
#include <linux/mm.h>
#include <linux/mm_types.h>

#include "ksym.h"		/* kallsyms_lookup_name 自 5.7 起不再 export，用 kprobe 取回 */

/*
 * kernel/locking/rwsem.c 是私有實作，這些定義沒有 export 到標頭檔，
 * 照抄自本機核心原始碼 6.1：kernel/locking/rwsem.c:63-65、117-127、336-347
 */
#define RP_RWSEM_READER_OWNED	(1UL << 0)
#define RP_RWSEM_NONSPINNABLE	(1UL << 1)
#define RP_RWSEM_OWNER_FLAGS	(RP_RWSEM_READER_OWNED | RP_RWSEM_NONSPINNABLE)

#define RP_RWSEM_WRITER_LOCKED	(1UL << 0)
#define RP_RWSEM_FLAG_WAITERS	(1UL << 1)
#define RP_RWSEM_FLAG_HANDOFF	(1UL << 2)
#define RP_RWSEM_FLAG_READFAIL	(1UL << (BITS_PER_LONG - 1))
#define RP_RWSEM_READER_SHIFT	8

enum rp_rwsem_waiter_type {
	RP_RWSEM_WAITING_FOR_WRITE,
	RP_RWSEM_WAITING_FOR_READ
};

struct rp_rwsem_waiter {		/* = struct rwsem_waiter，rwsem.c:341-347 */
	struct list_head list;
	struct task_struct *task;
	enum rp_rwsem_waiter_type type;
	unsigned long timeout;
	bool handoff_set;
};

/*
 * __kernel_text_address() 與 sched_clock_cpu() 在本機都沒有 EXPORT
 * （實測 modpost 直接報 undefined），所以：
 *   - 判斷「是不是核心程式碼位址」改用 sprint_symbol()：查不到符號時它會印 "0x...."，
 *     所以只要看第一個字元是不是 '0' 就知道。
 *   - 每顆 CPU 的 rq clock 改用 sched_clock()（arm64 上是全域 arch counter，
 *     和 rq_clock 同一個時間基準）。
 */
static bool is_ktext(unsigned long v, char *buf)
{
	sprint_symbol(buf, v);
	return buf[0] != '0';
}

static char *state_str(struct task_struct *t)
{
	unsigned int st = READ_ONCE(t->__state);

	if (st == TASK_RUNNING)			return "RU";
	if (st & TASK_UNINTERRUPTIBLE)		return "UN(D)";
	if (st & TASK_INTERRUPTIBLE)		return "IN(S)";
	if (st & __TASK_STOPPED)		return "ST";
	if (st & TASK_DEAD)			return "DE";
	return "??";
}

/* ---------------- ps：找出所有 D 狀態行程（= crash 的 ps | grep UN）------------- */
static void cmd_ps(void)
{
	struct task_struct *g, *t;
	int n = 0, idle = 0, killable = 0;

	/*
	 * 注意：光看 TASK_UNINTERRUPTIBLE 會被一大票閒置的 kworker 洗版
	 * ——它們是 TASK_IDLE = TASK_UNINTERRUPTIBLE | TASK_NOLOAD（5.x 以後才有）。
	 * kernel/hung_task.c:203-210 的判斷式就是排掉 TASK_NOLOAD 和 TASK_WAKEKILL，
	 * 這裡照抄同一個判斷式，才等價於書上 3.10 的 `ps | grep UN`。
	 */
	pr_info("=== 真正的 D 狀態行程（UNINTERRUPTIBLE && !NOLOAD && !WAKEKILL）===\n");
	pr_info("%8s %8s %4s %-18s %s\n", "PID", "PPID", "CPU", "TASK", "COMM");
	rcu_read_lock();
	for_each_process_thread(g, t) {
		unsigned int st = READ_ONCE(t->__state);

		if (!(st & TASK_UNINTERRUPTIBLE))
			continue;
		if (st & TASK_NOLOAD) {
			idle++;			/* TASK_IDLE：閒置 kworker，不算 */
			continue;
		}
		if (st & TASK_WAKEKILL) {
			killable++;		/* TASK_KILLABLE：殺得掉，hung_task 也跳過 */
			continue;
		}
		pr_info("%8d %8d %4d %px %s\n", t->pid,
			t->real_parent ? t->real_parent->pid : 0,
			task_cpu(t), t, t->comm);
		n++;
	}
	rcu_read_unlock();
	pr_info("共 %d 個真 D 狀態；另有 %d 個 TASK_IDLE、%d 個 TASK_KILLABLE 被濾掉\n",
		n, idle, killable);
}

static struct task_struct *get_task(pid_t pid)
{
	struct pid *p = find_get_pid(pid);
	struct task_struct *t;

	if (!p)
		return NULL;
	t = get_pid_task(p, PIDTYPE_PID);
	put_pid(p);
	return t;
}

/* ---------------- bt：用 x29 框架鏈回溯（ARM64 版的 RBP 鏈）-------------------- */
static void cmd_bt(pid_t pid, bool full)
{
	struct task_struct *t = get_task(pid);
	unsigned long fp, sp, pc, stack_lo, stack_hi, addr;
	char sym[KSYM_SYMBOL_LEN];
	int frame = 0;

	if (!t) {
		pr_err("找不到 pid %d\n", pid);
		return;
	}
	if (t == current || task_is_running(t)) {
		pr_warn("pid %d 正在跑（或就是自己），堆疊是浮動的，回溯沒有意義\n", pid);
		put_task_struct(t);
		return;
	}

	stack_lo = (unsigned long)task_stack_page(t);
	stack_hi = stack_lo + THREAD_SIZE;
	fp = t->thread.cpu_context.fp;
	sp = t->thread.cpu_context.sp;
	pc = t->thread.cpu_context.pc;

	sprint_symbol(sym, pc);
	pr_info("=== PID %d COMMAND \"%s\" TASK %px CPU %d STATE %s ===\n",
		t->pid, t->comm, t, task_cpu(t), state_str(t));
	pr_info("核心堆疊 [%lx, %lx)  THREAD_SIZE=%lu\n", stack_lo, stack_hi, THREAD_SIZE);
	pr_info("thread.cpu_context: fp(x29)=%lx sp=%lx pc=%lx (%s)\n", fp, sp, pc, sym);

	/* 框架鏈：[x29] = 父框架的 x29，[x29+8] = LR，和 x86 的 [RBP]/[RBP+8] 同構 */
	while (fp >= stack_lo && fp < stack_hi - 16 && frame < 32) {
		unsigned long next_fp = *(unsigned long *)fp;
		unsigned long lr = *(unsigned long *)(fp + 8);

		sprint_symbol(sym, lr);
		pr_info("#%-2d [%lx] %s   (框架 x29=%lx → 父 x29=%lx)\n",
			frame, fp + 8, sym, fp, next_fp);
		if (next_fp <= fp)
			break;
		fp = next_fp;
		frame++;
	}

	if (full) {
		pr_info("--- 原始堆疊內容（= crash 的 bt -f）---\n");
		fp = t->thread.cpu_context.fp;
		for (addr = sp; addr < stack_hi; addr += 8) {
			unsigned long val = *(unsigned long *)addr;
			const char *tag = "";

			if (addr == t->thread.cpu_context.fp)
				tag = "  <= 切換出去時的 x29";
			if (is_ktext(val, sym)) {
				pr_info("%lx: %016lx  %s%s\n", addr, val, sym, tag);
			} else if (val >= stack_lo && val < stack_hi) {
				pr_info("%lx: %016lx  [堆疊位址 = 某個框架的 x29 或指標]%s\n",
					addr, val, tag);
			} else {
				pr_info("%lx: %016lx%s\n", addr, val, tag);
			}
		}
	}
	put_task_struct(t);
}

/* ---------------- rd：讀核心記憶體 ------------------------------------------- */
static void cmd_rd(unsigned long addr, int count)
{
	int i;

	if (count <= 0 || count > 64)
		count = 1;
	for (i = 0; i < count; i++) {
		unsigned long a = addr + i * 8;
		unsigned long v;

		if (!virt_addr_valid((void *)a) && (a < VMALLOC_START)) {
			pr_err("rd: %lx 看起來不是合法核心位址\n", a);
			return;
		}
		v = *(unsigned long *)a;
		pr_info("%lx: %016lx  %s\n", a, v,
			(v >= 0x20 && v < 0x7f000000000000UL) ? "" : "");
	}
}

/* ---------------- rwsem：Q11 持有者 + Q12 等待者 ------------------------------ */
static void cmd_rwsem(unsigned long addr)
{
	struct rw_semaphore *sem = (struct rw_semaphore *)addr;
	long count = atomic_long_read(&sem->count);
	unsigned long owner_raw = atomic_long_read(&sem->owner);
	struct task_struct *owner = (struct task_struct *)(owner_raw & ~RP_RWSEM_OWNER_FLAGS);
	struct rp_rwsem_waiter *w;
	int n = 0;

	pr_info("=== struct rw_semaphore %px ===\n", sem);
	pr_info("count = 0x%lx\n", (unsigned long)count);
	pr_info("  bit0 WRITER_LOCKED = %d   ← 有寫者持有\n", !!(count & RP_RWSEM_WRITER_LOCKED));
	pr_info("  bit1 FLAG_WAITERS  = %d   ← wait_list 非空\n", !!(count & RP_RWSEM_FLAG_WAITERS));
	pr_info("  bit2 FLAG_HANDOFF  = %d\n", !!(count & RP_RWSEM_FLAG_HANDOFF));
	pr_info("  bit63 READFAIL     = %d\n", !!(count & RP_RWSEM_FLAG_READFAIL));
	pr_info("  讀者個數 (count >> 8) = %ld\n", (count & ~0xffUL) >> RP_RWSEM_READER_SHIFT);
	pr_info("owner = 0x%lx  (旗標: READER_OWNED=%d NONSPINNABLE=%d)\n",
		owner_raw, !!(owner_raw & RP_RWSEM_READER_OWNED),
		!!(owner_raw & RP_RWSEM_NONSPINNABLE));

	if (owner_raw & RP_RWSEM_READER_OWNED) {
		if (owner)
			pr_info("→ 讀者持有；owner 只記錄「最後一個」讀者：PID %d (%s)\n",
				owner->pid, owner->comm);
		else
			pr_info("→ 讀者持有，但 owner 沒有指向任何 task\n");
	} else if (owner) {
		pr_info("→ 寫者持有：PID %d (%s) task=%px  ★ 這就是 Q11 要的鎖持有者\n",
			owner->pid, owner->comm, owner);
	} else {
		pr_info("→ 目前沒有持有者\n");
	}

	pr_info("--- wait_list（Q12：誰在等這把鎖）head=%px ---\n", &sem->wait_list);
	if (list_empty(&sem->wait_list)) {
		pr_info("(空)\n");
	} else {
		list_for_each_entry(w, &sem->wait_list, list) {
			pr_info("waiter[%d] @%px  task=%px PID %d (%s)  type=%s  state=%s\n",
				n++, w, w->task, w->task->pid, w->task->comm,
				w->type == RP_RWSEM_WAITING_FOR_READ ?
					"RWSEM_WAITING_FOR_READ" : "RWSEM_WAITING_FOR_WRITE",
				state_str(w->task));
		}
		pr_info("共 %d 個等待者\n", n);
	}
}

/* ---------------- mm：mm_struct 與 mmap_lock ---------------------------------- */
static void cmd_mm(pid_t pid)
{
	struct task_struct *t = get_task(pid);
	struct mm_struct *mm;

	if (!t) {
		pr_err("找不到 pid %d\n", pid);
		return;
	}
	mm = t->mm;
	if (!mm) {
		pr_info("PID %d (%s) 是核心執行緒，沒有 mm\n", t->pid, t->comm);
		put_task_struct(t);
		return;
	}
	pr_info("=== PID %d (%s) 的 mm ===\n", t->pid, t->comm);
	pr_info("mm            = %px\n", mm);
	pr_info("&mm->mmap_lock = %px   (offsetof = 0x%lx)  ← 書上 3.10 叫 mmap_sem，偏移 0x78\n",
		&mm->mmap_lock, offsetof(struct mm_struct, mmap_lock));
	pr_info("mm->mm_users=%d mm->mm_count=%d map_count=%d\n",
		atomic_read(&mm->mm_users), atomic_read(&mm->mm_count), mm->map_count);
#ifdef CONFIG_MEMCG
	pr_info("mm->owner     = %px (PID %d %s)  ← 3.10 靠這個反查鎖的主人\n",
		mm->owner, mm->owner ? mm->owner->pid : -1,
		mm->owner ? mm->owner->comm : "?");
#else
	pr_info("本核心沒有 mm->owner（CONFIG_MEMCG=n）\n");
#endif
	put_task_struct(t);
}

/*
 * mmowner：書上 §4.11.2 的反推法 ——「鎖的位址 - mmap_lock 在 mm_struct 裡的偏移量
 * = mm_struct 的位址」，再從 mm->owner 找出持有這把鎖的行程。
 * 只有 mm->mmap_lock 這種「內嵌在 mm_struct 裡」的鎖才能這樣玩。
 */
static void cmd_mmowner(unsigned long sem_addr)
{
	unsigned long off = offsetof(struct mm_struct, mmap_lock);
	struct mm_struct *mm = (struct mm_struct *)(sem_addr - off);

	pr_info("=== 從 rw_semaphore 反推 mm_struct（書上 §4.11.2 圖 4.9）===\n");
	pr_info("mmap_lock 位址 0x%lx - offsetof(mm_struct, mmap_lock)=0x%lx → mm=%px\n",
		sem_addr, off, mm);
	pr_info("mm->mm_users=%d mm->map_count=%d（數值合理就代表推導正確）\n",
		atomic_read(&mm->mm_users), mm->map_count);
#ifdef CONFIG_MEMCG
	if (mm->owner)
		pr_info("mm->owner = %px → PID %d (%s)  ★ 鎖的主人\n",
			mm->owner, mm->owner->pid, mm->owner->comm);
	else
		pr_info("mm->owner = NULL\n");
#else
	pr_info("本核心 CONFIG_MEMCG=n，沒有 mm->owner 可用\n");
#endif
}

/* ---------------- sched：Q13 被阻塞了多久 ------------------------------------ */
static void cmd_sched(pid_t pid)
{
	struct task_struct *t = get_task(pid);
	u64 now, arrival, blocked;

	if (!t) {
		pr_err("找不到 pid %d\n", pid);
		return;
	}
#ifdef CONFIG_SCHED_INFO
	now     = sched_clock();		/* = crash 的 runq -t 那個時間戳 */
	arrival = t->sched_info.last_arrival;
	blocked = now > arrival ? now - arrival : 0;

	pr_info("=== PID %d (%s) state=%s cpu=%d ===\n",
		t->pid, t->comm, state_str(t), task_cpu(t));
	pr_info("sched_info = {\n");
	pr_info("  pcount       = %lu   (被排程上 CPU 的次數)\n", t->sched_info.pcount);
	pr_info("  run_delay    = %llu ns (在 runqueue 上等 CPU 的累計時間)\n",
		t->sched_info.run_delay);
	pr_info("  last_arrival = %llu ns (上次真正在 CPU 上開始跑的時間戳)\n", arrival);
	pr_info("  last_queued  = %llu ns\n", t->sched_info.last_queued);
	pr_info("}\n");
	pr_info("目前 CPU%d 的 rq clock = %llu ns\n", task_cpu(t), now);
	pr_info("★ 被阻塞時間 = %llu - %llu = %llu ns = %llu.%03llu 秒\n",
		now, arrival, blocked, blocked / NSEC_PER_SEC,
		(blocked % NSEC_PER_SEC) / 1000000);
	pr_info("（若上面 last_arrival 是 0，代表 sysctl kernel.sched_schedstats=0，"
		"要先 sysctl -w kernel.sched_schedstats=1）\n");
	pr_info("nvcsw=%lu nivcsw=%lu (自願/非自願切換次數，hung_task 就是比這個)\n",
		t->nvcsw, t->nivcsw);
#else
	pr_info("本核心沒有 CONFIG_SCHED_INFO\n");
#endif
	put_task_struct(t);
}

/* ---------------- runq：每顆 CPU 的 rq clock（= crash 的 runq -t）------------- */
static void cmd_runq(void)
{
	int cpu;

	pr_info("=== 每顆 CPU 的 sched_clock（= crash runq -t 的時間戳）===\n");
	for_each_online_cpu(cpu)
		pr_info("CPU %d: %llu\n", cpu, sched_clock());
}

static void cmd_sym(const char *name)
{
	unsigned long addr = exp_kallsyms_lookup_name ?
			     exp_kallsyms_lookup_name(name) : 0;

	if (!addr)
		pr_err("找不到符號 %s（本機 CONFIG_KALLSYMS_ALL=n，資料符號查不到）\n", name);
	else
		pr_info("%s = %lx\n", name, addr);
}

static ssize_t cp_write(struct file *f, const char __user *ubuf,
			size_t len, loff_t *off)
{
	char buf[128], arg[64];
	unsigned long addr;
	int pid, n;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (!strncmp(buf, "ps", 2))
		cmd_ps();
	else if (sscanf(buf, "btf %d", &pid) == 1)
		cmd_bt(pid, true);
	else if (sscanf(buf, "bt %d", &pid) == 1)
		cmd_bt(pid, false);
	else if (sscanf(buf, "rd %lx %d", &addr, &n) == 2)
		cmd_rd(addr, n);
	else if (sscanf(buf, "rd %lx", &addr) == 1)
		cmd_rd(addr, 1);
	else if (sscanf(buf, "rwsem %lx", &addr) == 1)
		cmd_rwsem(addr);
	else if (sscanf(buf, "mmowner %lx", &addr) == 1)
		cmd_mmowner(addr);
	else if (sscanf(buf, "mm %d", &pid) == 1)
		cmd_mm(pid);
	else if (sscanf(buf, "sched %d", &pid) == 1)
		cmd_sched(pid);
	else if (!strncmp(buf, "runq", 4))
		cmd_runq();
	else if (sscanf(buf, "sym %63s", arg) == 1)
		cmd_sym(arg);
	else
		pr_err("指令：ps | bt <pid> | btf <pid> | rd <hex> [n] | rwsem <hex> | mm <pid> | mmowner <hex> | sched <pid> | runq | sym <name>\n");

	return len;
}

static const struct proc_ops cp_ops = {
	.proc_write = cp_write,
};

static int __init cp_init(void)
{
	if (exp_ksym_init())
		pr_warn("拿不到 kallsyms_lookup_name，sym 指令不能用\n");
	if (!proc_create("crash_probe", 0222, NULL, &cp_ops))
		return -ENOMEM;
	pr_info("已載入，用法：echo 'ps' > /proc/crash_probe，輸出看 dmesg\n");
	return 0;
}

static void __exit cp_exit(void)
{
	remove_proc_entry("crash_probe", NULL);
	pr_info("卸載\n");
}

module_init(cp_init);
module_exit(cp_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("在沒有 Kdump/Crash 的機器上模擬 crash 工具的子命令（卷2 第4章 Q10~Q13）");
