// SPDX-License-Identifier: GPL-2.0
/*
 * irq_probe.ko —— 《奔跑吧 Linux 內核》卷2 第 2 章「中斷管理」結構探測模組
 * 平台：Radxa ROCK 5B (RK3588, GIC-600 / GICv3)，Linux 6.1.115+ aarch64
 *
 * 這個模組不需要任何中斷發生，純粹把「中斷管理相關的靜態結構」讀出來：
 *   1. 從 VBAR_EL1 讀出 ARM64 異常向量表本體（16 個表項 × 128 B），
 *      逐項解出符號名稱與第一條指令 —— 直接證明書上表 2.5 的佈局。
 *   2. struct pt_regs（中斷現場 / 棧框）的大小與每個欄位的偏移量。
 *   3. preempt_count 的欄位劃分（PREEMPT / SOFTIRQ / HARDIRQ / NMI）。
 *   4. 掃描所有 irq_desc：virq ↔ hwirq ↔ irq_chip ↔ irq_domain ↔ handle_irq
 *      ↔ action（主處理常式 / 中斷線程），即硬體中斷號到 Linux IRQ 號的映射。
 *   5. 中斷棧 / 行程核心棧的大小與位址。
 *
 * 涵蓋題目：Q1、Q2、Q3、Q4、Q14、Q15
 *
 * 用法：
 *   sudo insmod irq_probe.ko            # 全部
 *   sudo insmod irq_probe.ko show_irqs=0    # 不掃 irq_desc（輸出較短）
 *   sudo insmod irq_probe.ko one_irq=160    # 只細看某一條 IRQ
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/preempt.h>
#include <linux/hardirq.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/cpu.h>
#include <asm/ptrace.h>
#include <asm/sysreg.h>
#include <asm/memory.h>
#include <asm/thread_info.h>
#include <generated/utsrelease.h>
#include "ksym.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Interrupt-management structure prober for RK3588 kernel study notes");

static int show_irqs = 1;
static int one_irq = -1;
module_param(show_irqs, int, 0444);
module_param(one_irq, int, 0444);

#define P(fmt, ...) pr_info("irq_probe: " fmt, ##__VA_ARGS__)

/* irq_to_desc() 在 aarch64 沒有 EXPORT，用 kallsyms 取（見 ksym.h） */
static struct irq_desc *(*p_irq_to_desc)(unsigned int irq);
#define irq_to_desc(i) p_irq_to_desc(i)

/* ================================================================== */
/* Q1 / Q14：ARM64 異常向量表                                          */
/* ================================================================== */

static const char * const vec_name[16] = {
	"EL1t sync",  "EL1t IRQ",  "EL1t FIQ",  "EL1t error",
	"EL1h sync",  "EL1h IRQ",  "EL1h FIQ",  "EL1h error",
	"EL0 64 sync", "EL0 64 IRQ", "EL0 64 FIQ", "EL0 64 error",
	"EL0 32 sync", "EL0 32 IRQ", "EL0 32 FIQ", "EL0 32 error",
};

/*
 * 每個表項 128 B。第一條指令在 EL1 表項是 "sub sp, sp, #PT_REGS_SIZE"。
 * 我們把整條 32-bit 指令印出來，順便解碼 sub 的立即數，
 * 用來反推核心編譯時的 PT_REGS_SIZE。
 */
static void dump_vectors(void)
{
	unsigned long vbar = read_sysreg(vbar_el1);
	int i;

	unsigned long sym_vectors = (unsigned long)EXP_LOOKUP("vectors");
	unsigned long sym_bp = (unsigned long)EXP_LOOKUP("__bp_harden_el1_vectors");

	P("========== Q1/Q14  ARM64 異常向量表（書上表 2.5）==========");
	P("VBAR_EL1                    = 0x%016lx  (%pS)", vbar, (void *)vbar);
	P("entry.S 的 SYM_CODE(vectors)= 0x%016lx", sym_vectors);
	P("__bp_harden_el1_vectors     = 0x%016lx", sym_bp);
	if (sym_vectors && vbar != sym_vectors)
		P("★ VBAR_EL1 【不是】entry.S 裡那張 vectors！差 0x%lx；"
		  "本機 spectre_v2 = \"Mitigation: CSV2, BHB\"，", vbar - sym_vectors);
	if (sym_bp && vbar == sym_bp)
		P("  開機時 spectre_bhb_enable_mitigation() 把 VBAR_EL1 改指到"
		  " __bp_harden_el1_vectors 這張加了 BHB 清除序列的副本"
		  "（arch/arm64/kernel/proton-pack.c）。");
	P("每個表項 128 B（.align 7），共 16 項 = 2048 B");
	P("%-3s %-14s %-18s %-10s %s", "#", "類型", "位址", "第一條指令", "反組譯/符號");

	for (i = 0; i < 16; i++) {
		unsigned long ent = vbar + i * 0x80;
		u32 insn = *(u32 *)ent;
		char note[80];

		note[0] = '\0';
		/* sub sp, sp, #imm12 → 0xD10003FF | (imm12 << 10) */
		if ((insn & 0xFFC003FFu) == 0xD10003FFu)
			scnprintf(note, sizeof(note),
				  "sub sp, sp, #%u  → PT_REGS_SIZE = %u",
				  (insn >> 10) & 0xFFF, (insn >> 10) & 0xFFF);
		else if ((insn & 0xFC000000u) == 0x14000000u)
			scnprintf(note, sizeof(note), "b  %pS",
				  (void *)(ent + (((s32)(insn << 6)) >> 4)));

		P("%-3d %-14s +0x%03x = %pS  0x%08x  %s",
		  i, vec_name[i], i * 0x80, (void *)ent, insn, note);
	}
	P("");
	P("→ 這 16 項與書上表 2.5 完全對應：");
	P("  0x000/0x080/0x100/0x180 = 當前 EL 用 SP0（EL1t）的 sync/IRQ/FIQ/SError");
	P("  0x200/0x280/0x300/0x380 = 當前 EL 用 SPx（EL1h）的 sync/IRQ/FIQ/SError");
	P("  0x400.. = 來自低 EL 的 AArch64；0x600.. = 來自低 EL 的 AArch32");
	P("→ 核心態發生的外設中斷走的是第 5 項（+0x280, EL1h IRQ）");
	P("");
}

/* ================================================================== */
/* Q14：中斷現場 = struct pt_regs（棧框）                              */
/* ================================================================== */

static void dump_ptregs_layout(void)
{
	P("========== Q14  中斷現場：struct pt_regs 棧框佈局 ==========");
	P("sizeof(struct pt_regs)  = %zu  (= 組語裡的 PT_REGS_SIZE / 書上 S_FRAME_SIZE)",
	  sizeof(struct pt_regs));
	P("  regs[0..30]  (x0~x30)  offset %3zu .. %3zu  (%zu B，31 個通用暫存器)",
	  offsetof(struct pt_regs, regs[0]), offsetof(struct pt_regs, regs[30]),
	  31 * sizeof(u64));
	P("  sp                     offset %3zu  ← 中斷點的 SP（EL0 時是 SP_EL0）",
	  offsetof(struct pt_regs, sp));
	P("  pc                     offset %3zu  ← 硬體存進 ELR_EL1 的返回位址",
	  offsetof(struct pt_regs, pc));
	P("  pstate                 offset %3zu  ← 硬體存進 SPSR_EL1 的處理器狀態",
	  offsetof(struct pt_regs, pstate));
	P("  orig_x0                offset %3zu", offsetof(struct pt_regs, orig_x0));
	P("  syscallno              offset %3zu", offsetof(struct pt_regs, syscallno));
	P("  sdei_ttbr1             offset %3zu", offsetof(struct pt_regs, sdei_ttbr1));
	P("  pmr_save               offset %3zu  ← 6.1 新增（pseudo-NMI 用 ICC_PMR_EL1）",
	  offsetof(struct pt_regs, pmr_save));
	P("  stackframe[2]          offset %3zu  ← 給 backtrace 用的假 frame record",
	  offsetof(struct pt_regs, stackframe));
	P("  lockdep_hardirqs       offset %3zu", offsetof(struct pt_regs, lockdep_hardirqs));
	P("  exit_rcu               offset %3zu", offsetof(struct pt_regs, exit_rcu));
	P("");
	P("→ 書上 5.0 的 pt_regs 有 orig_addr_limit（set_fs 機制），6.1 已刪除；");
	P("   6.1 多出 sdei_ttbr1 / pmr_save / lockdep_hardirqs / exit_rcu。");
	P("→ 書上的 S_LR/S_SP/S_PC/S_PSTATE/S_STACKFRAME/S_FRAME_SIZE 巨集");
	P("   對應 arch/arm64/kernel/asm-offsets.c，值就是上面這些偏移量。");
	P("");
}

/* ================================================================== */
/* Q15：中斷現場存在哪裡 —— 行程核心棧 vs Per-CPU 中斷棧              */
/* ================================================================== */

static void dump_stacks(void)
{
	unsigned long sp = current_stack_pointer;
	unsigned long tsk_lo = (unsigned long)current->stack;

	P("========== Q15  中斷現場存放位置：兩個棧 ==========");
	P("THREAD_SIZE     = %lu (%lu KB)  ← 每個行程的核心棧",
	  (unsigned long)THREAD_SIZE, (unsigned long)THREAD_SIZE >> 10);
	P("IRQ_STACK_SIZE  = %lu (%lu KB)  ← Per-CPU 中斷棧（= THREAD_SIZE）",
	  (unsigned long)IRQ_STACK_SIZE, (unsigned long)IRQ_STACK_SIZE >> 10);
	P("VMAP_STACK      = %s",
	  IS_ENABLED(CONFIG_VMAP_STACK) ? "y（棧用 vmalloc 配置，帶 guard page）" : "n");
	P("");
	P("目前（行程上下文，insmod 這條路徑）：");
	P("  current             = %s/%d", current->comm, current->pid);
	P("  current->stack      = 0x%016lx .. 0x%016lx", tsk_lo, tsk_lo + THREAD_SIZE);
	P("  current_stack_pointer = 0x%016lx  → %s",
	  sp, (sp >= tsk_lo && sp < tsk_lo + THREAD_SIZE) ? "在行程核心棧內" : "不在行程核心棧內");
	P("");
	P("→ 中斷發生時：硬體 + kernel_ventry 的 `sub sp, sp, #PT_REGS_SIZE`");
	P("   先在【被中斷行程的核心棧】頂端挖出一個 pt_regs 棧框，kernel_entry 把現場存進去；");
	P("→ 之後 do_interrupt_handler() 判斷 on_thread_stack()，");
	P("   用 call_on_irq_stack() 切到【Per-CPU 中斷棧 irq_stack_ptr】才去跑 handler。");
	P("   （arch/arm64/kernel/entry-common.c:268 / entry.S: SYM_FUNC_START(call_on_irq_stack)）");
	P("   → 現場在行程棧，處理常式跑在中斷棧，兩者是分開的。用 irq_live.ko 可實測到位址。");
	P("");
}

/* ================================================================== */
/* Q3 / Q4：preempt_count 的欄位劃分                                   */
/* ================================================================== */

static void dump_preempt_count(void)
{
	unsigned long pc = preempt_count();

	P("========== Q3/Q4  preempt_count 欄位劃分（書上圖 2.8）==========");
	P("%-16s %-8s %-8s %s", "欄位", "bits", "shift", "mask");
	P("%-16s %-8d %-8d 0x%08lx", "PREEMPT", PREEMPT_BITS, PREEMPT_SHIFT, (unsigned long)PREEMPT_MASK);
	P("%-16s %-8d %-8d 0x%08lx", "SOFTIRQ", SOFTIRQ_BITS, SOFTIRQ_SHIFT, (unsigned long)SOFTIRQ_MASK);
	P("%-16s %-8d %-8d 0x%08lx", "HARDIRQ", HARDIRQ_BITS, HARDIRQ_SHIFT, (unsigned long)HARDIRQ_MASK);
	P("%-16s %-8d %-8d 0x%08lx", "NMI",     NMI_BITS,     NMI_SHIFT,     (unsigned long)NMI_MASK);
	P("HARDIRQ_OFFSET = 0x%lx   SOFTIRQ_OFFSET = 0x%lx   SOFTIRQ_DISABLE_OFFSET = 0x%lx",
	  (unsigned long)HARDIRQ_OFFSET, (unsigned long)SOFTIRQ_OFFSET,
	  (unsigned long)SOFTIRQ_DISABLE_OFFSET);
	P("");
	P("本機組態：CONFIG_PREEMPT_COUNT = %s，CONFIG_PREEMPT_VOLUNTARY = %s",
	  IS_ENABLED(CONFIG_PREEMPT_COUNT) ? "y" : "n",
	  IS_ENABLED(CONFIG_PREEMPT_VOLUNTARY) ? "y" : "n");
	P("  → PREEMPT_DISABLE_OFFSET = %d（PREEMPT_COUNT=n 時 preempt_disable() 只是 barrier()）",
	  (int)PREEMPT_DISABLE_OFFSET);
	P("  → 但 SOFTIRQ / HARDIRQ 兩個欄位【照樣維護】：irq_enter_rcu()/__do_softirq()");
	P("     直接呼叫 preempt_count_add()，與 CONFIG_PREEMPT_COUNT 無關。");
	P("");
	P("目前 preempt_count() = 0x%08lx", pc);
	P("  in_hardirq()=%d  in_softirq()=%d  in_serving_softirq()=%d  in_interrupt()=%d  in_task()=%d",
	  !!in_hardirq(), !!in_softirq(), !!in_serving_softirq(), !!in_interrupt(), !!in_task());
	P("  irqs_disabled()=%d   DAIF=0x%08llx", irqs_disabled(),
	  (unsigned long long)read_sysreg(daif));
	P("");
}

/* ================================================================== */
/* Q2 / Q3：硬體中斷號 ↔ Linux IRQ 號的映射                            */
/* ================================================================== */

static const char *domain_name_of(struct irq_data *d)
{
	if (!d || !d->domain)
		return "(none)";
	return d->domain->name ? d->domain->name : "(unnamed)";
}

static void dump_one_irq(unsigned int irq, struct irq_desc *desc, bool verbose)
{
	struct irq_data *d = &desc->irq_data;
	struct irqaction *a;
	unsigned long total = 0;
	int cpu;

	for_each_possible_cpu(cpu)
		if (desc->kstat_irqs)
			total += *per_cpu_ptr(desc->kstat_irqs, cpu);

	P("irq %-4u  hwirq %-10lu  chip=%-14s domain=%-18s handle_irq=%pS  count=%lu",
	  irq, d->hwirq,
	  d->chip && d->chip->name ? d->chip->name : "-",
	  domain_name_of(d), desc->handle_irq, total);

	for (a = desc->action; a; a = a->next)
		P("           action \"%s\"  handler=%pS  thread_fn=%pS  flags=0x%08x%s%s%s",
		  a->name ? a->name : "-", a->handler, a->thread_fn, a->flags,
		  (a->flags & IRQF_SHARED) ? " SHARED" : "",
		  (a->flags & IRQF_ONESHOT) ? " ONESHOT" : "",
		  a->thread ? " [threaded]" : "");

	if (!verbose)
		return;

	P("           irq_data.irq=%u  irq_data.hwirq=%lu  state=0x%08x",
	  d->irq, d->hwirq, desc->status_use_accessors);
	if (d->domain) {
		struct irq_domain *dm = d->domain;

		P("           domain: name=%s  hwirq_max=%lu  revmap_size=%u  parent=%s",
		  dm->name ? dm->name : "-", (unsigned long)dm->hwirq_max,
		  dm->revmap_size,
		  dm->parent ? (dm->parent->name ? dm->parent->name : "(unnamed)") : "(none)");
		P("           domain->ops: translate=%pS alloc=%pS map=%pS",
		  dm->ops ? dm->ops->translate : NULL,
		  dm->ops ? dm->ops->alloc : NULL,
		  dm->ops ? dm->ops->map : NULL);
	}
	if (d->chip) {
		P("           chip ops: irq_mask=%pS irq_unmask=%pS irq_eoi=%pS irq_set_type=%pS",
		  d->chip->irq_mask, d->chip->irq_unmask,
		  d->chip->irq_eoi, d->chip->irq_set_type);
		P("           chip ops: irq_ack=%pS irq_set_affinity=%pS",
		  d->chip->irq_ack, d->chip->irq_set_affinity);
	}
	/* 反查：用 hwirq 去 domain 找回 virq，證明是雙向映射 */
	if (d->domain) {
		unsigned int back = irq_find_mapping(d->domain, d->hwirq);

		P("           irq_find_mapping(domain, hwirq=%lu) = %u  %s",
		  d->hwirq, back, back == irq ? "✓ 與 virq 相符" : "✗");
	}
}

static void dump_irq_map(void)
{
	unsigned int irq;
	struct irq_desc *desc;
	int n = 0;

	P("========== Q2/Q3  硬體中斷號 ↔ Linux IRQ 號映射 ==========");
	P("CONFIG_SPARSE_IRQ = %s  → irq_desc 用 radix tree（maple tree）存，不是陣列",
	  IS_ENABLED(CONFIG_SPARSE_IRQ) ? "y" : "n");
	P("nr_irqs = %d   NR_IRQS = %d", nr_irqs, NR_IRQS);
	P("");

	if (one_irq >= 0) {
		desc = irq_to_desc(one_irq);
		if (desc)
			dump_one_irq(one_irq, desc, true);
		else
			P("irq %d 沒有對應的 irq_desc", one_irq);
		P("");
		return;
	}

	if (!show_irqs)
		return;

	for (irq = 0; irq < nr_irqs; irq++) {
		desc = irq_to_desc(irq);
		if (!desc || !desc->action)
			continue;
		dump_one_irq(irq, desc, false);
		n++;
	}
	P("");
	P("共 %d 條已註冊 action 的 IRQ。", n);
	P("→ chip=GICv3 的 hwirq 就是 GIC INTID（SPI = DTS 的 interrupts 第二欄 + 32）；");
	P("→ chip=ITS-MSI 的走 GICv3 的 ITS，hwirq 是 LPI/DevID 編碼；");
	P("→ chip=rockchip_gpio_irq 的是二級（chained）中斷控制器，自己有一個 irq_domain。");
	P("");
}

/* ================================================================== */
/* Q5~Q9：軟中斷靜態資訊                                               */
/* ================================================================== */

static const char * const sirq_name[NR_SOFTIRQS] = {
	"HI", "TIMER", "NET_TX", "NET_RX", "BLOCK",
	"IRQ_POLL", "TASKLET", "SCHED", "HRTIMER", "RCU"
};

static void dump_softirq(void)
{
	int i, cpu;

	P("========== Q5~Q9  軟中斷靜態資訊 ==========");
	P("NR_SOFTIRQS = %d（本機 6.1 的枚舉，索引即優先序，越小越先跑）", NR_SOFTIRQS);
	for (i = 0; i < NR_SOFTIRQS; i++)
		P("  [%d] %s_SOFTIRQ", i, sirq_name[i]);
	P("→ 與書上 5.0 的差異：BLOCK_IOPOLL_SOFTIRQ 已更名為 IRQ_POLL_SOFTIRQ（4.5, commit 903cd12ee9e5）");
	P("");
	P("__do_softirq() 的退場條件（kernel/softirq.c）：");
	P("  MAX_SOFTIRQ_TIME    = 2 ms   MAX_SOFTIRQ_RESTART = 10 次");
	P("  三個條件任一不滿足 → wakeup_softirqd() 丟給 ksoftirqd/N");
	P("");
	P("本機 local_softirq_pending() = 0x%08x（現在是行程上下文，通常為 0）",
	  local_softirq_pending());
	P("每 CPU 的 __softirq_pending（irq_stat.__softirq_pending，EXPORT_PER_CPU_SYMBOL）：");
	for_each_online_cpu(cpu)
		P("  cpu%d: 0x%08x", cpu, per_cpu(irq_stat.__softirq_pending, cpu));
	P("");
	P("CONFIG_SOFTIRQ_ON_OWN_STACK = %s  → arm64 軟中斷也跑在 Per-CPU 中斷棧上",
	  IS_ENABLED(CONFIG_SOFTIRQ_ON_OWN_STACK) ? "y" : "n");
	P("");
}

static int __init irq_probe_init(void)
{
	int r = exp_ksym_init();

	if (r) {
		pr_err("irq_probe: 取不到 kallsyms_lookup_name: %d\n", r);
		return r;
	}
	p_irq_to_desc = EXP_LOOKUP("irq_to_desc");
	if (!p_irq_to_desc) {
		pr_err("irq_probe: kallsyms 找不到 irq_to_desc\n");
		return -ENOENT;
	}

	P("================================================================");
	P("irq_probe.ko  中斷管理結構探測   kernel %s  nr_cpus=%d",
	  UTS_RELEASE, num_online_cpus());
	P("================================================================");
	dump_vectors();
	dump_ptregs_layout();
	dump_stacks();
	dump_preempt_count();
	dump_softirq();
	dump_irq_map();
	P("探測完畢（模組刻意回傳 -EAGAIN，不需要 rmmod）。");
	return -EAGAIN;
}

static void __exit irq_probe_exit(void) { }

module_init(irq_probe_init);
module_exit(irq_probe_exit);
