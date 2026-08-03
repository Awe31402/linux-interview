// SPDX-License-Identifier: GPL-2.0
/*
 * sync_probe.ko —— 《奔跑吧 Linux 內核》卷2 第 1 章「併發與同步」實驗模組
 * 平台：Radxa ROCK 5B (RK3588, 4×A76 + 4×A55)，Linux 6.1.115+ aarch64
 *
 * 這個模組最重要的功能：**把已經被 alternatives 動態打補丁過的指令從記憶體裡讀出來**，
 * 直接證明 RK3588 上用的是 ARMv8.1 LSE 原子指令（stadd/casal/swpal…），
 * 而不是 ldxr/stxr 的 LL/SC 迴圈。
 *
 * 涵蓋題目：
 *   Q1        ARM64 如何實現獨佔存取（LL/SC vs LSE）
 *   Q2~Q5     cmpxchg / xchg / try_cmpxchg 與 acquire/release/relaxed 變體
 *   Q6~Q8     記憶體屏障、smp_cond_load_relaxed、smp_mb__before/after_atomic
 *   Q9/Q11    自旋鎖臨界區為何不能睡眠、不能搶佔（本機 PREEMPT_COUNT=n 的實測）
 *   Q13       spin_lock_irqsave 對 DAIF 的實際影響
 *   Q12/Q15   qspinlock 的 val 欄位劃分與三元組
 *   Q14/Q23   MCS / OSQ 鎖的資料結構
 *   Q17/Q18   semaphore
 *   Q19~Q22   mutex 與樂觀自旋等待
 *   Q25/Q26   rwsem 的 count 欄位佈局
 *   Q31       ULONG_CMP_GE()/ULONG_CMP_LT() 的迴繞問題
 *   Q35       PG_locked
 *
 * 用法：
 *   sudo insmod sync_probe.ko
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 *   sudo insmod sync_probe.ko dump_insn=1   # 額外印出可餵給 objdump 的 hex
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/rwsem.h>
#include <linux/osq_lock.h>
#include <linux/rcupdate.h>
#include <linux/rculist.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/delay.h>
#include <linux/cpu.h>
#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/sysreg.h>
#include <asm/qspinlock.h>
#include <generated/utsrelease.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Concurrency & synchronization prober for RK3588 kernel study notes");

static int dump_insn = 1;
module_param(dump_insn, int, 0444);

#define P(fmt, ...) pr_info("sync_probe: " fmt, ##__VA_ARGS__)

/* ================================================================== */
/* Q1~Q5：把「已經被 alternatives patch 過」的原子指令從記憶體讀出來      */
/* ================================================================== */

static atomic_t g_atomic = ATOMIC_INIT(0);
static atomic_t g_cmp    = ATOMIC_INIT(0);
static unsigned long g_xchg;

/* 每個 demo 函式都刻意只放一個原子操作，方便對照反組譯 */
static noinline void demo_atomic_add(void)      { atomic_add(1, &g_atomic); }
static noinline int  demo_atomic_add_return(void){ return atomic_add_return(1, &g_atomic); }
static noinline int  demo_atomic_fetch_add(void) { return atomic_fetch_add(1, &g_atomic); }
static noinline int  demo_cmpxchg(void)         { return atomic_cmpxchg(&g_cmp, 0, 1); }
static noinline int  demo_cmpxchg_acquire(void) { return atomic_cmpxchg_acquire(&g_cmp, 0, 1); }
static noinline int  demo_cmpxchg_release(void) { return atomic_cmpxchg_release(&g_cmp, 0, 1); }
static noinline int  demo_cmpxchg_relaxed(void) { return atomic_cmpxchg_relaxed(&g_cmp, 0, 1); }
static noinline bool demo_try_cmpxchg(int *old) { return atomic_try_cmpxchg(&g_cmp, old, 1); }
static noinline unsigned long demo_xchg(void)   { return xchg(&g_xchg, 1UL); }
static noinline void demo_smp_mb(void)          { smp_mb(); }
static noinline void demo_smp_rmb(void)         { smp_rmb(); }
static noinline void demo_smp_wmb(void)         { smp_wmb(); }
static noinline void demo_mb(void)              { mb(); }
static noinline void demo_dma_wmb(void)         { dma_wmb(); }
static noinline void demo_mb_before_atomic(void){ smp_mb__before_atomic(); atomic_dec(&g_atomic); }

/* Q7：smp_cond_load_relaxed / smp_cond_load_acquire —— 排隊自旋鎖的自旋等待原語 */
static unsigned long g_cond = 1;	/* 先設成 1，條件立刻成立，不會真的卡住 */
static noinline unsigned long demo_cond_load_relaxed(void)
{
	return smp_cond_load_relaxed(&g_cond, VAL != 0);
}
static noinline unsigned long demo_cond_load_acquire(void)
{
	return smp_cond_load_acquire(&g_cond, VAL != 0);
}

/*
 * 極簡 AArch64 解碼器 —— 只認得本章關心的那幾條指令。
 * 精確的欄位定義見 ARM ARM v8-A：
 *   C6.2.x  LDADD/STADD、CAS、SWP、LDXR/STXR、DMB/DSB/ISB
 */
static const char *bar_domain(u32 crm)
{
	switch (crm) {
	case 15: return "sy";  case 14: return "st";  case 13: return "ld";
	case 11: return "ish"; case 10: return "ishst"; case 9: return "ishld";
	case 7:  return "nsh"; case 6:  return "nshst"; case 5: return "nshld";
	case 3:  return "osh"; case 2:  return "oshst"; case 1: return "oshld";
	default: return "?";
	}
}

static const char *decode(u32 insn, char *buf, size_t n)
{
	u32 size = insn >> 30;
	const char *w = (size == 2) ? "32-bit" : (size == 3) ? "64-bit" : "8/16-bit";

	/* DMB / DSB / ISB : 1101 0101 0000 0011 0011 CRm 1 opc2 11111 */
	if ((insn & 0xFFFFF01F) == 0xD503301F) {
		u32 crm = (insn >> 8) & 0xf, op2 = (insn >> 5) & 0x7;
		const char *nm = op2 == 4 ? "DSB" : op2 == 5 ? "DMB" : op2 == 6 ? "ISB" : "BAR?";
		snprintf(buf, n, "%s %s   <-- 記憶體屏障", nm, bar_domain(crm));
		return buf;
	}
	/* LSE atomic RMW: size 111000 A R 1 Rs o3 opc 00 Rn Rt  */
	if ((insn & 0x3F200C00) == 0x38200000) {
		static const char * const opc[] = { "ADD","CLR","EOR","SET","SMAX","SMIN","UMAX","UMIN" };
		u32 A = (insn >> 23) & 1, R = (insn >> 22) & 1;
		u32 o3 = (insn >> 15) & 1, op = (insn >> 12) & 7, Rt = insn & 0x1f;
		if (o3) {	/* o3=1 -> SWP */
			snprintf(buf, n, "SWP%s%s  (%s)  <-- LSE 交換指令 (xchg)",
				 A ? "A" : "", R ? "L" : "", w);
		} else {
			snprintf(buf, n, "%s%s%s%s  (%s)  <-- LSE 原子指令%s",
				 Rt == 31 ? "ST" : "LD", opc[op],
				 A ? "A" : "", R ? "L" : "", w,
				 Rt == 31 ? "（無回傳值，ST 別名）" : "（有回傳值）");
		}
		return buf;
	}
	/* CAS: size 001000 1 L 1 Rs o0 11111 Rn Rt */
	if ((insn & 0x3FA07C00) == 0x08A07C00) {
		u32 L = (insn >> 22) & 1, o0 = (insn >> 15) & 1;
		snprintf(buf, n, "CAS%s%s  (%s)  <-- LSE 比較並交換指令",
			 L ? "A" : "", o0 ? "L" : "", w);
		return buf;
	}
	/* LDXR/LDAXR: size 001000 0 1 0 11111 o0 11111 Rn Rt */
	if ((insn & 0x3FE07C00) == 0x08400000 || (insn & 0x3FE07C00) == 0x08405C00) {
		snprintf(buf, n, "LD%sXR (%s)  <-- LL/SC 獨佔載入", (insn >> 15) & 1 ? "A" : "", w);
		return buf;
	}
	if ((insn & 0x3FE00000) == 0x08000000) {
		snprintf(buf, n, "ST%sXR (%s)  <-- LL/SC 獨佔儲存", (insn >> 15) & 1 ? "L" : "", w);
		return buf;
	}
	if (insn == 0xD503201F) { snprintf(buf, n, "NOP   <-- alternatives 把 LL/SC 分支改寫成 nop"); return buf; }
	if (insn == 0xD503205F) { snprintf(buf, n, "WFE   <-- 進入低功耗等待，被 SEV/獨佔監視器喚醒"); return buf; }
	if (insn == 0xD50320BF) { snprintf(buf, n, "SEVL  <-- 先送一次本地事件，避免錯過喚醒"); return buf; }
	if (insn == 0xD503209F) { snprintf(buf, n, "SEV   <-- 喚醒其他 CPU 的 WFE"); return buf; }
	if (insn == 0xD503203F) { snprintf(buf, n, "YIELD <-- cpu_relax()"); return buf; }
	if ((insn & 0xFC000000) == 0x94000000) {
		s32 off = (s32)(insn << 6) >> 4;
		snprintf(buf, n, "BL   %+d   <-- 外呼（全屏障版本被編譯器外聯）", off);
		return buf;
	}
	if ((insn & 0xFC000000) == 0x14000000) {
		s32 off = (s32)(insn << 6) >> 4;
		snprintf(buf, n, "B    %+d   <-- 若沒有 LSE，這裡會跳去 LL/SC 迴圈", off);
		return buf;
	}
	if ((insn & 0xFFFFFC1F) == 0xD65F0000) { snprintf(buf, n, "RET"); return buf; }
	if ((insn & 0xFF000000) == 0xF9000000) { snprintf(buf, n, "STR (imm)"); return buf; }
	if ((insn & 0xFF000000) == 0xF8000000) { snprintf(buf, n, "LDR/STR"); return buf; }
	if ((insn & 0x9F000000) == 0x90000000) { snprintf(buf, n, "ADRP"); return buf; }
	if ((insn & 0xFF800000) == 0x91000000) { snprintf(buf, n, "ADD (imm)"); return buf; }
	if ((insn & 0x7F800000) == 0x52800000) { snprintf(buf, n, "MOVZ"); return buf; }
	snprintf(buf, n, "-");
	return buf;
}

static void dump_func_depth(const char *name, void *fn, int words, int depth);

static void dump_func(const char *name, void *fn, int words)
{
	dump_func_depth(name, fn, words, 0);
}

static void dump_func_depth(const char *name, void *fn, int words, int depth)
{
	u32 *p = (u32 *)fn;
	void *bl_target = NULL;
	char buf[96];
	int i;

	P("  -- %s @ %pS\n", name, fn);
	for (i = 0; i < words; i++) {
		u32 insn = READ_ONCE(p[i]);

		P("     +%02d: %08x   %s\n", i * 4, insn, decode(insn, buf, sizeof(buf)));
		if (!bl_target && (insn & 0xFC000000) == 0x94000000)
			bl_target = (void *)(p + i + (((s32)(insn << 6)) >> 6));
		if ((insn & 0xFFFFFC1F) == 0xD65F0000)	/* RET */
			break;
	}
	if (dump_insn) {
		/* 方便在機台上用 objdump 反組譯做交叉驗證 */
		char hex[9 * 12 + 1];
		int k = 0;
		for (i = 0; i < words && k < (int)sizeof(hex) - 9; i++) {
			k += scnprintf(hex + k, sizeof(hex) - k, "%08x ", READ_ONCE(p[i]));
			if ((READ_ONCE(p[i]) & 0xFFFFFC1F) == 0xD65F0000)
				break;
		}
		P("     HEX: %s\n", hex);
	}
	if (bl_target && depth == 0) {
		P("     ↓ 追進外呼目標：\n");
		dump_func_depth("（被外聯的實作）", bl_target, words, 1);
	}
}

static void show_atomics(void)
{
	int old = 0;

	P("=========================================================\n");
	P("== Q1~Q5：ARM64 的原子指令（執行期已被 alternatives 打補丁）==\n");
	P("=========================================================\n");
	P("  CONFIG_ARM64_LSE_ATOMICS = %s，system_uses_lse_atomics() = %d\n",
	  IS_ENABLED(CONFIG_ARM64_LSE_ATOMICS) ? "y" : "n",
	  alternative_has_feature_likely(ARM64_HAS_LSE_ATOMICS));
	P("  cpu_have_feature(ARM64_HAS_LSE_ATOMICS) = %d\n",
	  cpus_have_const_cap(ARM64_HAS_LSE_ATOMICS));

	/* 先真的跑一次，確認函式沒被最佳化掉 */
	demo_atomic_add();
	demo_atomic_add_return();
	demo_atomic_fetch_add();
	demo_cmpxchg();
	demo_cmpxchg_acquire();
	demo_cmpxchg_release();
	demo_cmpxchg_relaxed();
	demo_try_cmpxchg(&old);
	demo_xchg();
	P("  （跑完之後 g_atomic=%d, g_cmp=%d, g_xchg=%lu）\n",
	  atomic_read(&g_atomic), atomic_read(&g_cmp), g_xchg);

	P("\n  [Q1] atomic_add() —— 不回傳值\n");
	dump_func("atomic_add(1, v)", demo_atomic_add, 10);
	P("\n  [Q1] atomic_add_return() / atomic_fetch_add() —— 要回傳值\n");
	dump_func("atomic_add_return(1, v)", demo_atomic_add_return, 10);
	dump_func("atomic_fetch_add(1, v)", demo_atomic_fetch_add, 10);

	P("\n  [Q2][Q3][Q5] cmpxchg 的四種記憶體序\n");
	dump_func("atomic_cmpxchg()          全屏障", demo_cmpxchg, 12);
	dump_func("atomic_cmpxchg_acquire()  取得語意", demo_cmpxchg_acquire, 12);
	dump_func("atomic_cmpxchg_release()  釋放語意", demo_cmpxchg_release, 12);
	dump_func("atomic_cmpxchg_relaxed()  無屏障", demo_cmpxchg_relaxed, 12);

	P("\n  [Q4] atomic_try_cmpxchg() —— 同樣是 cas，差別在回傳值語意\n");
	dump_func("atomic_try_cmpxchg()", demo_try_cmpxchg, 14);

	P("\n  [Q2] xchg() —— 對應 LSE 的 SWP 指令\n");
	dump_func("xchg()", demo_xchg, 10);
}

/* ================================================================== */
/* Q6~Q8：記憶體屏障                                                    */
/* ================================================================== */
static void show_barriers(void)
{
	P("=========================================================\n");
	P("== Q6~Q8：記憶體屏障在 ARM64 上到底變成哪一條指令 ==\n");
	P("=========================================================\n");
	demo_smp_mb(); demo_smp_rmb(); demo_smp_wmb(); demo_mb(); demo_dma_wmb();
	demo_mb_before_atomic();
	dump_func("smp_mb()", demo_smp_mb, 4);
	dump_func("smp_rmb()", demo_smp_rmb, 4);
	dump_func("smp_wmb()", demo_smp_wmb, 4);
	dump_func("mb()", demo_mb, 4);
	dump_func("dma_wmb()", demo_dma_wmb, 4);
	dump_func("smp_mb__before_atomic() + atomic_dec()", demo_mb_before_atomic, 10);
	P("\n  [Q7] smp_cond_load_relaxed()/_acquire() —— 排隊自旋鎖用的自旋等待原語\n");
	demo_cond_load_relaxed();
	demo_cond_load_acquire();
	dump_func("smp_cond_load_relaxed(ptr, VAL != 0)", demo_cond_load_relaxed, 16);
	dump_func("smp_cond_load_acquire(ptr, VAL != 0)", demo_cond_load_acquire, 16);
	P("     ARM64 覆寫了通用版本：不是單純的 cpu_relax() 忙等，而是\n");
	P("       sevl; wfe; ldxr; eor; cbnz 1f; wfe; 1:\n");
	P("     （arch/arm64/include/asm/cmpxchg.h 的 __CMPWAIT_CASE）\n");
	P("     ldxr 會把該位址設成獨佔監視狀態，別的 CPU 一寫入就自動觸發事件把 WFE 喚醒，\n");
	P("     所以等待期間 CPU 幾乎不耗電、也不會一直打 cache line。\n");
	P("     smp_cond_load_acquire() 只是在跳出迴圈後多一道 acquire 屏障。\n");
	P("\n  說明：dmb ish  = inner-shareable 全屏障（smp_mb）\n");
	P("        dmb ishld = inner-shareable 讀屏障（smp_rmb）\n");
	P("        dmb ishst = inner-shareable 寫屏障（smp_wmb）\n");
	P("        dsb sy    = 系統範圍同步屏障（mb，含裝置存取）\n");
}

/* ================================================================== */
/* Q9/Q11/Q13：自旋鎖的搶佔與中斷                                       */
/* ================================================================== */
static DEFINE_SPINLOCK(g_spin);
static DEFINE_RAW_SPINLOCK(g_raw_spin);

static void show_spinlock_semantics(void)
{
	unsigned long flags;
	int pc_before, pc_in, pc_after;
	int irq_before, irq_in, irq_after;

	P("=========================================================\n");
	P("== Q9/Q11/Q13：自旋鎖的搶佔與中斷語意（本機實測） ==\n");
	P("=========================================================\n");
	P("  CONFIG_PREEMPT_COUNT = %s      <-- ★ 決定 preempt_disable() 有沒有作用\n",
	  IS_ENABLED(CONFIG_PREEMPT_COUNT) ? "y" : "n");
	P("  CONFIG_PREEMPTION    = %s\n", IS_ENABLED(CONFIG_PREEMPTION) ? "y" : "n");
	P("  CONFIG_DEBUG_SPINLOCK= %s\n", IS_ENABLED(CONFIG_DEBUG_SPINLOCK) ? "y" : "n");
	P("  CONFIG_LOCKDEP       = %s\n", IS_ENABLED(CONFIG_LOCKDEP) ? "y" : "n");

	/* (a) spin_lock()：只關搶佔，不關中斷 */
	pc_before = preempt_count();  irq_before = irqs_disabled();
	spin_lock(&g_spin);
	pc_in = preempt_count();      irq_in = irqs_disabled();
	spin_unlock(&g_spin);
	pc_after = preempt_count();   irq_after = irqs_disabled();
	P("\n  (a) spin_lock() / spin_unlock()\n");
	P("      preempt_count : 前=0x%x  臨界區內=0x%x  後=0x%x\n", pc_before, pc_in, pc_after);
	P("      irqs_disabled : 前=%d    臨界區內=%d     後=%d\n", irq_before, irq_in, irq_after);

	/* (b) spin_lock_irqsave()：連中斷一起關 */
	irq_before = irqs_disabled();
	spin_lock_irqsave(&g_spin, flags);
	pc_in = preempt_count();   irq_in = irqs_disabled();
	spin_unlock_irqrestore(&g_spin, flags);
	irq_after = irqs_disabled();
	P("\n  (b) spin_lock_irqsave() / spin_unlock_irqrestore()\n");
	P("      preempt_count : 臨界區內=0x%x\n", pc_in);
	P("      irqs_disabled : 前=%d  臨界區內=%d  後=%d   <-- ★ 中斷真的被關掉了\n",
	  irq_before, irq_in, irq_after);
	P("      flags = 0x%lx（PSR_I_BIT=0x%x；flags 為 0 表示「進來之前中斷是開的」，\n",
	  flags, (unsigned int)PSR_I_BIT);
	P("             所以 unlock_irqrestore 會把中斷「還原成開」，而不是無條件開）\n");

	/* (c) raw_spin_lock 與 spin_lock 在非 RT 核心上完全相同 */
	raw_spin_lock(&g_raw_spin);
	P("\n  (c) raw_spin_lock()：非 RT 核心上 spin_lock() 直接就是它\n");
	raw_spin_unlock(&g_raw_spin);

	P("\n  結論（Q11）：本機 CONFIG_PREEMPT_COUNT=n，spin_lock() 裡的\n");
	P("      preempt_disable() 只剩 barrier()，preempt_count 全程是 0。\n");
	P("      「自旋鎖臨界區不可搶佔」在這台機器上是靠\n");
	P("      「核心本身就不可搶佔（PREEMPT_VOLUNTARY）」來保證的。\n");
	P("  結論（Q9）：臨界區睡眠 -> 換到別的行程 -> 它也來搶同一把鎖 -> 死鎖；\n");
	P("      而且 schedule() 會無條件開中斷（見卷1 第9章 Q13），語意全毀。\n");
}

/* ================================================================== */
/* Q12/Q15/Q16：qspinlock 的欄位劃分                                    */
/* ================================================================== */
static void show_qspinlock(void)
{
	P("=========================================================\n");
	P("== Q12/Q15/Q16：排隊自旋鎖 qspinlock 的 32 位元劃分 ==\n");
	P("=========================================================\n");
	P("  CONFIG_QUEUED_SPINLOCKS = %s，CONFIG_QUEUED_RWLOCKS = %s\n",
	  IS_ENABLED(CONFIG_QUEUED_SPINLOCKS) ? "y" : "n",
	  IS_ENABLED(CONFIG_QUEUED_RWLOCKS) ? "y" : "n");
	P("  sizeof(spinlock_t)=%zu  sizeof(raw_spinlock_t)=%zu  sizeof(arch_spinlock_t)=%zu\n",
	  sizeof(spinlock_t), sizeof(raw_spinlock_t), sizeof(arch_spinlock_t));
	P("\n  欄位          位元範圍      OFFSET  BITS  MASK\n");
	P("  locked      bit[%2d:%2d]    %2d     %2d    0x%08x\n",
	  _Q_LOCKED_OFFSET + _Q_LOCKED_BITS - 1, _Q_LOCKED_OFFSET,
	  _Q_LOCKED_OFFSET, _Q_LOCKED_BITS, _Q_LOCKED_MASK);
	P("  pending     bit[%2d:%2d]    %2d     %2d    0x%08x\n",
	  _Q_PENDING_OFFSET + _Q_PENDING_BITS - 1, _Q_PENDING_OFFSET,
	  _Q_PENDING_OFFSET, _Q_PENDING_BITS, _Q_PENDING_MASK);
	P("  tail_idx    bit[%2d:%2d]    %2d     %2d    0x%08x\n",
	  _Q_TAIL_IDX_OFFSET + _Q_TAIL_IDX_BITS - 1, _Q_TAIL_IDX_OFFSET,
	  _Q_TAIL_IDX_OFFSET, _Q_TAIL_IDX_BITS, _Q_TAIL_IDX_MASK);
	P("  tail_cpu    bit[%2d:%2d]    %2d     %2d    0x%08x\n",
	  _Q_TAIL_CPU_OFFSET + _Q_TAIL_CPU_BITS - 1, _Q_TAIL_CPU_OFFSET,
	  _Q_TAIL_CPU_OFFSET, _Q_TAIL_CPU_BITS, _Q_TAIL_CPU_MASK);
	P("  _Q_LOCKED_VAL = 0x%x, _Q_PENDING_VAL = 0x%x, _Q_TAIL_MASK = 0x%08x\n",
	  _Q_LOCKED_VAL, _Q_PENDING_VAL, _Q_TAIL_MASK);
	P("  NR_CPUS=%d\n", NR_CPUS);
	P("  ★ 注意：本機 _Q_PENDING_BITS = %d，不是書上表 1.4 說的 1！\n", _Q_PENDING_BITS);
	P("     include/asm-generic/qspinlock_types.h:\n");
	P("         #if CONFIG_NR_CPUS < (1U << 14)\n");
	P("         #define _Q_PENDING_BITS  8      <-- 本機走這條\n");
	P("         #else\n");
	P("         #define _Q_PENDING_BITS  1\n");
	P("         #endif\n");
	P("     NR_CPUS 小的時候 pending 佔滿一個位元組，locked_pending 就能用\n");
	P("     一次 16 位元的存取同時清 pending、設 locked（見 qspinlock.c 的\n");
	P("     clear_pending_set_locked()）。\n");

	/* 拿一把真的鎖來看 val */
	{
		spinlock_t l;
		spin_lock_init(&l);
		P("\n  一把剛初始化的鎖： val = 0x%08x  三元組 {tail, pending, locked} = {%u, %u, %u}\n",
		  atomic_read(&l.rlock.raw_lock.val),
		  (atomic_read(&l.rlock.raw_lock.val) & _Q_TAIL_MASK) >> _Q_TAIL_OFFSET,
		  (atomic_read(&l.rlock.raw_lock.val) & _Q_PENDING_MASK) >> _Q_PENDING_OFFSET,
		  atomic_read(&l.rlock.raw_lock.val) & _Q_LOCKED_MASK);
		spin_lock(&l);
		P("  spin_lock() 之後：  val = 0x%08x  三元組 = {%u, %u, %u}   <-- 快速通道，只設 locked\n",
		  atomic_read(&l.rlock.raw_lock.val),
		  (atomic_read(&l.rlock.raw_lock.val) & _Q_TAIL_MASK) >> _Q_TAIL_OFFSET,
		  (atomic_read(&l.rlock.raw_lock.val) & _Q_PENDING_MASK) >> _Q_PENDING_OFFSET,
		  atomic_read(&l.rlock.raw_lock.val) & _Q_LOCKED_MASK);
		spin_unlock(&l);
		P("  spin_unlock() 之後：val = 0x%08x\n",
		  atomic_read(&l.rlock.raw_lock.val));
	}
	P("\n  每 CPU 4 個 mcs 節點（task/softirq/hardirq/nmi），sizeof(struct mcs_spinlock) 見 Q14。\n");
}

/* ================================================================== */
/* Q14/Q23：MCS / OSQ                                                   */
/* ================================================================== */
static struct optimistic_spin_queue g_osq;

static void show_mcs(void)
{
	P("=========================================================\n");
	P("== Q14/Q23：MCS 鎖 / OSQ 鎖 ==\n");
	P("=========================================================\n");
	P("  sizeof(struct optimistic_spin_queue) = %zu  (只有一個 atomic_t tail)\n",
	  sizeof(struct optimistic_spin_queue));
	P("  sizeof(struct optimistic_spin_node)  = %zu  (next/prev/locked/cpu)\n",
	  sizeof(struct optimistic_spin_node));
	osq_lock_init(&g_osq);
	P("  osq_lock_init() 之後 tail = %d （0 = OSQ_UNLOCKED_VAL）\n",
	  atomic_read(&g_osq.tail));
	P("  encode_cpu(cpu) = cpu + 1，所以 0 保留給「沒有 CPU」\n");
	P("  本 CPU(%d) 的編碼值 = %d\n", smp_processor_id(), smp_processor_id() + 1);
	P("\n  為什麼 MCS 只用在 mutex/rwsem，不用在傳統 spinlock？\n");
	P("    optimistic_spin_node 有 %zu 位元組，而 arch_spinlock_t 只有 %zu 位元組。\n",
	  sizeof(struct optimistic_spin_node), sizeof(arch_spinlock_t));
	P("    （本機 sizeof(spinlock_t)=%zu 是因為 CONFIG_DEBUG_SPINLOCK=y 多了\n",
	  sizeof(spinlock_t));
	P("      magic/owner/owner_cpu 三個除錯欄位；關掉除錯就只有 4 位元組。）\n");
	P("    spinlock 被內嵌在 struct page 等對大小極敏感的結構裡，塞不下 MCS 節點。\n");
	P("    qspinlock 的解法：節點放 per-CPU 陣列（qnodes[4]），鎖本體仍然只有 4 位元組。\n");
}

/* ================================================================== */
/* Q17~Q26：semaphore / mutex / rwsem                                   */
/* ================================================================== */
static DEFINE_SEMAPHORE(g_sem);
static DEFINE_MUTEX(g_mutex);
static DECLARE_RWSEM(g_rwsem);

static void show_sleeping_locks(void)
{
	P("=========================================================\n");
	P("== Q17~Q26：semaphore / mutex / rwsem ==\n");
	P("=========================================================\n");
	P("  sizeof(struct semaphore)    = %2zu\n", sizeof(struct semaphore));
	P("  sizeof(struct mutex)        = %2zu\n", sizeof(struct mutex));
	P("  sizeof(struct rw_semaphore) = %2zu\n", sizeof(struct rw_semaphore));
	P("  sizeof(spinlock_t)          = %2zu   <-- 對照組\n", sizeof(spinlock_t));

	P("\n  [Q19~Q22] mutex 的 owner 欄位把「持有者指標」和旗標塞在一起：\n");
	P("     MUTEX_FLAG_WAITERS   = 0x%02lx  等待佇列非空\n", 0x01UL);
	P("     MUTEX_FLAG_HANDOFF   = 0x%02lx  要求把鎖直接交棒給第一順位者\n", 0x02UL);
	P("     MUTEX_FLAG_PICKUP    = 0x%02lx  鎖已交棒，等人來撿\n", 0x04UL);
	P("     -> 因為 task_struct 至少 L1_CACHE_BYTES(%d) 對齊，低 3 位永遠是 0，可以借用\n",
	  L1_CACHE_BYTES);
	P("     CONFIG_MUTEX_SPIN_ON_OWNER = %s  <-- 樂觀自旋等待的總開關\n",
	  IS_ENABLED(CONFIG_MUTEX_SPIN_ON_OWNER) ? "y" : "n");
	P("     CONFIG_RWSEM_SPIN_ON_OWNER = %s\n",
	  IS_ENABLED(CONFIG_RWSEM_SPIN_ON_OWNER) ? "y" : "n");

	mutex_lock(&g_mutex);
	P("     mutex_lock() 之後 owner = 0x%lx，current = %px (%s/%d)\n",
	  atomic_long_read(&g_mutex.owner), current, current->comm, current->pid);
	P("       -> owner & ~0x07 = 0x%lx  %s current\n",
	  atomic_long_read(&g_mutex.owner) & ~0x07UL,
	  (atomic_long_read(&g_mutex.owner) & ~0x07UL) == (unsigned long)current ? "==" : "!=");
	mutex_unlock(&g_mutex);
	P("     mutex_unlock() 之後 owner = 0x%lx\n", atomic_long_read(&g_mutex.owner));

	P("\n  [Q25][Q26] rwsem 的 count 欄位佈局（kernel/locking/rwsem.c）：\n");
	P("     bit0   RWSEM_WRITER_LOCKED   寫者持有\n");
	P("     bit1   RWSEM_FLAG_WAITERS    等待佇列非空\n");
	P("     bit2   RWSEM_FLAG_HANDOFF    交棒給等待者（防餓死）\n");
	P("     bit7   RWSEM_FLAG_READFAIL\n");
	P("     bit8+  RWSEM_READER_BIAS     每個讀者 +256\n");
	P("     count 初始 = %ld\n", atomic_long_read(&g_rwsem.count));
	down_read(&g_rwsem);
	P("     down_read()  之後 count = %ld  (= 1 個 RWSEM_READER_BIAS)\n",
	  atomic_long_read(&g_rwsem.count));
	down_read(&g_rwsem);
	P("     再 down_read() 之後 count = %ld  (= 2 個讀者，讀者可以疊加)\n",
	  atomic_long_read(&g_rwsem.count));
	up_read(&g_rwsem); up_read(&g_rwsem);
	down_write(&g_rwsem);
	P("     down_write() 之後 count = %ld  (bit0 = RWSEM_WRITER_LOCKED)\n",
	  atomic_long_read(&g_rwsem.count));
	P("     rwsem owner = 0x%lx\n", atomic_long_read(&g_rwsem.owner));
	up_write(&g_rwsem);

	P("\n  [Q17][Q18] semaphore：count=%u，就是一個計數器 + 等待佇列 + 一把 raw_spinlock\n",
	  g_sem.count);
	P("     -> 沒有 owner 欄位，所以「誰都可以 up()」，也就無法做樂觀自旋（不知道誰在跑）。\n");
	P("        這正是 Q22「已經有信號量了為何還要 mutex」的關鍵答案。\n");
}

/* ================================================================== */
/* Q31：ULONG_CMP_GE / ULONG_CMP_LT                                     */
/* ================================================================== */
static void show_ulong_cmp(void)
{
	unsigned long a, b;

	P("=========================================================\n");
	P("== Q31：為什麼 RCU 要用 ULONG_CMP_GE()/ULONG_CMP_LT() ==\n");
	P("=========================================================\n");
	P("  定義（include/linux/rcupdate.h）：\n");
	P("     #define ULONG_CMP_GE(a, b)  (ULONG_MAX / 2 >= (a) - (b))\n");
	P("     #define ULONG_CMP_LT(a, b)  (ULONG_MAX / 2 <  (a) - (b))\n");

	a = ULONG_MAX - 2;	/* 快要溢位的舊序號 */
	b = 3;			/* 溢位之後的新序號 */
	P("\n  情境：GP 序號即將迴繞。a = %lu (ULONG_MAX-2)，b = %lu\n", a, b);
	P("    直接比大小 :  a > b  = %d   <-- ★ 錯！以為舊的比新的大\n", a > b);
	P("    ULONG_CMP_LT(a, b) = %d   <-- 正確：a 在 b 之前\n",
	  (int)ULONG_CMP_LT(a, b));
	P("    ULONG_CMP_GE(b, a) = %d   <-- 正確：b 在 a 之後\n",
	  (int)ULONG_CMP_GE(b, a));
	P("    a - b = %lu（無號減法自然迴繞），ULONG_MAX/2 = %lu\n",
	  a - b, ULONG_MAX / 2);
	P("\n  一般情況（沒有迴繞）兩者結果一致：a=100, b=50\n");
	a = 100; b = 50;
	P("    a > b = %d，ULONG_CMP_GE(a, b) = %d\n", a > b, (int)ULONG_CMP_GE(a, b));
	P("\n  結論：GP 序號 (rcu_state.gp_seq) 是單調遞增的 unsigned long，\n");
	P("        在 32 位元系統上約 4.9 天就會迴繞一次（HZ=250 且每 GP 一次）。\n");
	P("        用差值 <= ULONG_MAX/2 判斷「誰在前面」，可以正確處理迴繞。\n");
}

/* ================================================================== */
/* Q35：PG_locked                                                       */
/* ================================================================== */
static void show_pg_locked(void)
{
	struct page *pg;

	P("=========================================================\n");
	P("== Q35：PG_locked 頁面鎖 ==\n");
	P("=========================================================\n");
	pg = alloc_page(GFP_KERNEL);
	if (!pg) { P("  alloc_page 失敗\n"); return; }

	P("  剛配置的頁面 pfn=%lu  flags=0x%lx  PageLocked=%d\n",
	  page_to_pfn(pg), pg->flags, PageLocked(pg));
	P("  trylock_page() = %d\n", trylock_page(pg));
	P("  加鎖後          flags=0x%lx  PageLocked=%d   <-- PG_locked = bit %d\n",
	  pg->flags, PageLocked(pg), PG_locked);
	P("  再 trylock_page() = %d   <-- 已經被鎖住，拿不到\n", trylock_page(pg));
	unlock_page(pg);
	P("  unlock_page() 後 flags=0x%lx  PageLocked=%d\n", pg->flags, PageLocked(pg));
	__free_page(pg);
	P("\n  常見用法：lock_page() 會睡眠等待；trylock_page() 不會；\n");
	P("            wait_on_page_locked() 只等不拿鎖。\n");
	P("            PG_locked 保護的是「頁面內容正在被 I/O 或回收操作」這件事。\n");
}

/* ================================================================== */
static int __init sync_probe_init(void)
{
	P("######## RK3588 併發與同步探針，kernel %s ########\n", UTS_RELEASE);
	show_atomics();
	show_barriers();
	show_spinlock_semantics();
	show_qspinlock();
	show_mcs();
	show_sleeping_locks();
	show_ulong_cmp();
	show_pg_locked();
	P("######## done，用 rmmod 移除 ########\n");
	return 0;
}

static void __exit sync_probe_exit(void)
{
	P("bye\n");
}

module_init(sync_probe_init);
module_exit(sync_probe_exit);
