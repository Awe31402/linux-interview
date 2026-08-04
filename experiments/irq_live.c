// SPDX-License-Identifier: GPL-2.0
/*
 * irq_live.ko —— 抓「真正發生中的硬體中斷」的現場
 * 平台：Radxa ROCK 5B (RK3588, GIC-600 / GICv3)，Linux 6.1.115+ aarch64
 *
 * 用 kprobe 掛在 ARM64 中斷處理路徑的三個點上，把一個真實中斷的現場印出來：
 *
 *   gic_handle_irq()            ← handle_arch_irq，中斷處理的第一站
 *        x0 = 被中斷者的 struct pt_regs *（就是「中斷現場」本體！）
 *   generic_handle_domain_irq() ← 硬體中斷號 → Linux IRQ 號的轉換點
 *        x0 = irq_domain *, x1 = hwirq
 *   handle_irq_event()          ← 真正呼叫 action->handler 之前
 *        x0 = irq_desc *
 *
 * 這樣可以【實測】而不是「照著書講」：
 *   Q1  硬體在中斷時把 PSTATE 存到 SPSR_EL1、返回位址存到 ELR_EL1、DAIF 全部設 1
 *   Q2  hwirq → virq 的實際轉換（GIC INTID ↔ Linux IRQ 號）
 *   Q3  gic_handle_irq → generic_handle_domain_irq → handle_fasteoi_irq → handler
 *   Q4  中斷上下文中 irqs_disabled()==1、current 是「無辜的受害者」
 *   Q14 中斷現場 = pt_regs 的實際內容
 *   Q15 pt_regs 落在【被中斷行程的核心棧】，而 handler 已經跑在【Per-CPU 中斷棧】
 *
 * 用法：
 *   sudo insmod irq_live.ko                 # 抓 6 筆任意中斷
 *   sudo insmod irq_live.ko from_el0=1      # 只抓中斷點在使用者態(EL0)的
 *   sudo insmod irq_live.ko want_hwirq=26   # 只抓 arch_timer (PPI 26)
 *   sudo insmod irq_live.ko samples=16 stackmap=1   # 統計每顆 CPU 的中斷棧位址
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 *   sudo rmmod irq_live
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/hardirq.h>
#include <linux/percpu.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>
#include <asm/memory.h>
#include <asm/irq_regs.h>
#include "ksym.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Live hardware-interrupt frame capture on RK3588 (GICv3)");

static int samples = 6;
static int from_el0;
static long want_hwirq = -1;
static int stackmap;
module_param(samples, int, 0444);
module_param(from_el0, int, 0444);
module_param(want_hwirq, long, 0444);
module_param(stackmap, int, 0444);

#define P(fmt, ...) pr_info("irq_live: " fmt, ##__VA_ARGS__)

static struct irq_desc *(*p_irq_to_desc)(unsigned int irq);
#define irq_to_desc(i) p_irq_to_desc(i)

static atomic_t got = ATOMIC_INIT(0);
static DEFINE_RAW_SPINLOCK(prlock);  /* 讓多顆 CPU 的輸出不要交錯 */
static atomic_t evt_got = ATOMIC_INIT(0);

/* 每顆 CPU 記錄「在 gic_handle_irq 裡看到的 SP」的最小/最大值 —— 用來證明中斷棧是 Per-CPU */
static DEFINE_PER_CPU(unsigned long, sp_min);
static DEFINE_PER_CPU(unsigned long, sp_max);
static DEFINE_PER_CPU(unsigned long, sp_hits);

static void decode_pstate(unsigned long ps, char *buf, size_t n)
{
	static const char * const mode[16] = {
		"EL0t", "?", "?", "?", "EL1t", "EL1h", "?", "?",
		"EL2t", "EL2h", "?", "?", "EL3t", "EL3h", "?", "?"
	};

	scnprintf(buf, n,
		  "%c%c%c%c .. D=%lu A=%lu I=%lu F=%lu .. M=%s",
		  (ps & (1UL << 31)) ? 'N' : 'n',
		  (ps & (1UL << 30)) ? 'Z' : 'z',
		  (ps & (1UL << 29)) ? 'C' : 'c',
		  (ps & (1UL << 28)) ? 'V' : 'v',
		  (ps >> 9) & 1, (ps >> 8) & 1, (ps >> 7) & 1, (ps >> 6) & 1,
		  mode[ps & 0xf]);
}

/* ---------------- 1. 中斷現場本體 ---------------- */
/*
 * 註：原本想直接 kprobe gic_handle_irq()（它的 x0 就是中斷現場），
 * 但實測 register_kprobe() 回 -EINVAL —— 因為 gic_handle_irq 帶
 * __exception_irq_entry 屬性，被連結到 .irqentry.text 區段
 * （實機：__irqentry_text_start = gic_handle_irq 的位址），
 * 這段是 kprobe 黑名單。改掛在下一站 generic_handle_domain_irq()，
 * 用 get_irq_regs() 取同一個 pt_regs —— 它是 do_interrupt_handler()
 * 進來時用 set_irq_regs(regs) 存進 Per-CPU 變數 __irq_regs 的。
 */
static int pre_gic(struct kprobe *kp, struct pt_regs *regs)
{
	struct pt_regs *f = get_irq_regs();
	unsigned long sp_now = current_stack_pointer;
	unsigned long tlo, thi;
	char ps[96];
	int el0;

	if (stackmap) {
		unsigned long *mn = this_cpu_ptr(&sp_min);
		unsigned long *mx = this_cpu_ptr(&sp_max);

		if (!*mn || sp_now < *mn)
			*mn = sp_now;
		if (sp_now > *mx)
			*mx = sp_now;
		(*this_cpu_ptr(&sp_hits))++;
	}

	if (atomic_read(&got) >= samples)
		return 0;
	if (!f)
		return 0;

	el0 = ((f->pstate & 0xc) == 0);		/* M[3:2] == 00 → EL0 */
	if (from_el0 && !el0)
		return 0;
	if (want_hwirq >= 0 && regs->regs[1] != (unsigned long)want_hwirq)
		return 0;

	raw_spin_lock(&prlock);
	if (atomic_read(&got) >= samples) {
		raw_spin_unlock(&prlock);
		return 0;
	}
	atomic_inc(&got);

	tlo = (unsigned long)current->stack;
	thi = tlo + THREAD_SIZE;
	decode_pstate(f->pstate, ps, sizeof(ps));

	P("---------------- 中斷現場 #%d  (cpu%d) ----------------",
	  atomic_read(&got), raw_smp_processor_id());
	P("  被中斷者 current      = %s/%d", current->comm, current->pid);
	P("  中斷點的 exception level = %s（PSTATE.M[3:0]=0x%llx）",
	  el0 ? "EL0 使用者態" :
	  (((f->pstate & 0xc) == 0x4) ? "EL1 核心態" : "EL2 核心態（VHE：核心跑在 EL2）"),
	  (unsigned long long)(f->pstate & 0xf));
	P("");
	P("  [硬體自動做的事 — Q1]");
	P("    pt_regs->pstate     = 0x%016llx  ← 硬體存進 SPSR_EL1 的中斷點 PSTATE",
	  (unsigned long long)f->pstate);
	P("                          %s", ps);
	P("                          ↑ 這是【中斷點】的 DAIF；硬體另外把【當前】DAIF 全設 1");
	P("    pt_regs->pc         = 0x%016llx  ← 硬體存進 ELR_EL1 的返回位址",
	  (unsigned long long)f->pc);
	P("                          %pS", (void *)f->pc);
	P("    現在的 DAIF(EL1)    = 0x%08llx  irqs_disabled()=%d  ← 進中斷後 I 已被硬體設 1",
	  (unsigned long long)read_sysreg(daif), irqs_disabled());
	P("");
	P("  [軟體 kernel_entry 存的 — Q14]");
	P("    pt_regs->sp         = 0x%016llx", (unsigned long long)f->sp);
	P("    pt_regs->regs[30]/lr= 0x%016llx  %pS",
	  (unsigned long long)f->regs[30], (void *)f->regs[30]);
	P("    pt_regs->regs[29]/fp= 0x%016llx", (unsigned long long)f->regs[29]);
	P("    pt_regs->regs[0..3] = %016llx %016llx %016llx %016llx",
	  (unsigned long long)f->regs[0], (unsigned long long)f->regs[1],
	  (unsigned long long)f->regs[2], (unsigned long long)f->regs[3]);
	P("    pt_regs->stackframe = { 0x%016llx, 0x%016llx }  ← 給 backtrace 用",
	  (unsigned long long)f->stackframe[0], (unsigned long long)f->stackframe[1]);
	P("    pt_regs->pmr_save   = 0x%016llx  (只有 ARM64_HAS_IRQ_PRIO_MASKING 時才有意義)",
	  (unsigned long long)f->pmr_save);
	P("");
	P("  [現場放在哪 — Q15]");
	P("    &pt_regs            = 0x%016lx", (unsigned long)f);
	P("    current->stack      = 0x%016lx .. 0x%016lx (THREAD_SIZE=%lu)",
	  tlo, thi, (unsigned long)THREAD_SIZE);
	P("    → pt_regs %s被中斷行程的核心棧內；距棧頂 %ld B（= sizeof(pt_regs)=%zu）",
	  ((unsigned long)f >= tlo && (unsigned long)f < thi) ? "【在】" : "【不在】",
	  thi - (unsigned long)f, sizeof(struct pt_regs));
	P("    現在的 SP           = 0x%016lx", sp_now);
	P("    → SP %s行程核心棧內  ← gic_handle_irq 已被 call_on_irq_stack() 切到 Per-CPU 中斷棧",
	  (sp_now >= tlo && sp_now < thi) ? "【在】" : "【不在】");
	P("");
	{
		struct irq_domain *dm = (struct irq_domain *)regs->regs[0];
		unsigned long hwirq = regs->regs[1];
		unsigned int virq = irq_find_mapping(dm, hwirq);
		struct irq_desc *dsc = virq ? irq_to_desc(virq) : NULL;

		P("  [硬體中斷號 → Linux IRQ 號 — Q2]");
		P("    generic_handle_domain_irq(domain=\"%s\", hwirq=%lu)",
		  dm && dm->name ? dm->name : "?", hwirq);
		P("    irq_find_mapping() → virq %u   %s", virq,
		  dsc ? "" : "(無 desc)");
		if (dsc)
			P("    desc: irq_data.irq=%u irq_data.hwirq=%lu chip=%s handle_irq=%pS action=\"%s\"",
			  dsc->irq_data.irq, dsc->irq_data.hwirq,
			  dsc->irq_data.chip ? dsc->irq_data.chip->name : "-",
			  dsc->handle_irq,
			  dsc->action && dsc->action->name ? dsc->action->name : "-");
		P("");
	}
	P("  [上下文 — Q3/Q4]");
	P("    preempt_count       = 0x%08x", preempt_count());
	P("    in_hardirq()=%d in_softirq()=%d in_serving_softirq()=%d in_interrupt()=%d in_task()=%d",
	  !!in_hardirq(), !!in_softirq(), !!in_serving_softirq(),
	  !!in_interrupt(), !!in_task());
	raw_spin_unlock(&prlock);
	return 0;
}

/* ---------------- 3. handle_irq_event：呼叫 handler 之前 ---------------- */

static int pre_evt(struct kprobe *kp, struct pt_regs *regs)
{
	struct irq_desc *desc = (struct irq_desc *)regs->regs[0];
	unsigned long tlo = (unsigned long)current->stack;

	if (atomic_read(&evt_got) >= samples)
		return 0;
	if (want_hwirq >= 0 && desc->irq_data.hwirq != (unsigned long)want_hwirq)
		return 0;
	if (atomic_inc_return(&evt_got) > samples)
		return 0;

	P("[Q3 派發 #%d] cpu%d  virq=%u hwirq=%lu  action=\"%s\" handler=%pS thread_fn=%pS",
	  atomic_read(&evt_got), raw_smp_processor_id(),
	  desc->irq_data.irq, desc->irq_data.hwirq,
	  desc->action && desc->action->name ? desc->action->name : "-",
	  desc->action ? desc->action->handler : NULL,
	  desc->action ? desc->action->thread_fn : NULL);
	P("            preempt_count=0x%08x in_hardirq=%d irqs_disabled=%d  SP=0x%016lx (%s行程棧)",
	  preempt_count(), !!in_hardirq(), irqs_disabled(),
	  current_stack_pointer,
	  (current_stack_pointer >= tlo &&
	   current_stack_pointer < tlo + THREAD_SIZE) ? "在" : "不在");
	return 0;
}

static struct kprobe kp_gic = { .symbol_name = "generic_handle_domain_irq", .pre_handler = pre_gic };
static struct kprobe kp_evt = { .symbol_name = "handle_irq_event",          .pre_handler = pre_evt };

static int __init irq_live_init(void)
{
	int r;

	r = exp_ksym_init();
	if (r) {
		pr_err("irq_live: 取不到 kallsyms_lookup_name: %d\n", r);
		return r;
	}
	p_irq_to_desc = EXP_LOOKUP("irq_to_desc");
	if (!p_irq_to_desc)
		return -ENOENT;

	P("================================================================");
	P("irq_live.ko  抓真實硬體中斷現場   samples=%d from_el0=%d want_hwirq=%ld",
	  samples, from_el0, want_hwirq);
	P("================================================================");

	{
		struct kprobe probe_gic = { .symbol_name = "gic_handle_irq" };
		int rc = register_kprobe(&probe_gic);

		if (rc == 0) {
			P("（意外）gic_handle_irq 可以掛 kprobe");
			unregister_kprobe(&probe_gic);
		} else {
			P("kprobe @ gic_handle_irq 掛不上（%d）—— 它在 .irqentry.text，"
			  "是 kprobe 黑名單；改掛下一站。", rc);
		}
	}

	r = register_kprobe(&kp_gic);
	if (r) {
		P("register_kprobe(generic_handle_domain_irq) 失敗: %d", r);
		return r;
	}
	P("kprobe @ generic_handle_domain_irq = %pS （現場由 get_irq_regs() 取得）", kp_gic.addr);

	r = register_kprobe(&kp_evt);
	if (r)
		P("register_kprobe(handle_irq_event) 失敗: %d（略過）", r);
	else
		P("kprobe @ handle_irq_event          = %pS", kp_evt.addr);

	P("已掛好，等中斷發生……（rmmod 時印統計）");
	return 0;
}

static void __exit irq_live_exit(void)
{
	int cpu;

	unregister_kprobe(&kp_gic);
	if (kp_evt.addr)
		unregister_kprobe(&kp_evt);

	if (stackmap) {
		P("---- Q15：每顆 CPU 在 gic_handle_irq 裡看到的 SP 範圍 ----");
		P("（同一顆 CPU 不論被中斷的是哪個行程，SP 都落在同一個窄區間 → Per-CPU 中斷棧）");
		for_each_online_cpu(cpu)
			P("  cpu%d: hits=%-8lu SP 0x%016lx .. 0x%016lx  (跨度 %lu B)",
			  cpu, per_cpu(sp_hits, cpu),
			  per_cpu(sp_min, cpu), per_cpu(sp_max, cpu),
			  per_cpu(sp_max, cpu) - per_cpu(sp_min, cpu));
	}
	P("卸載完畢：現場 %d 筆、派發 %d 筆",
	  atomic_read(&got), atomic_read(&evt_got));
}

module_init(irq_live_init);
module_exit(irq_live_exit);
