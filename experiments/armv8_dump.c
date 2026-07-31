// SPDX-License-Identifier: GPL-2.0
/*
 * armv8_dump.ko —— 《奔跑吧 Linux 內核》卷1 第2章 / 第3章 實驗模組
 * 平台：Radxa ROCK 5B (RK3588), Linux 6.1.115+ aarch64
 *
 * 直接讀取 ARMv8 系統暫存器與核心記憶體佈局巨集，並對指定虛擬位址做
 * 「軟體頁表巡覽」，用來驗證以下面試題：
 *
 *   Ch2 Q1  TTBR0/TTBR1 的分工        Ch2 Q2  4 級頁表映射過程
 *   Ch2 Q3  block vs table 描述符      Ch2 Q4  使用者/核心空間劃分
 *   Ch2 Q5  PAGE_OFFSET                Ch2 Q6  KIMAGE_VADDR
 *   Ch2 Q9  核心記憶體佈局             Ch2 Q10 __pa() vs __pa_symbol()
 *   Ch2 Q12 kimage_voffset             Ch2 Q13 PoC / PoU (CLIDR_EL1)
 *   Ch2 Q14 ASID (TCR_EL1.AS)          Ch2 Q15 記憶體屬性 (MAIR_EL1)
 *   Ch2 Q16 shareability (SH 欄位)     Ch2 Q22 下級頁表基址是實體位址
 *   Ch2 Q23 軟體巡覽如何用 __va()      Ch3 Q4  核心映像映射位置
 *   Ch3 Q6  memblock / PHYS_OFFSET
 *
 * 注意：本機核心 CONFIG_KALLSYMS_ALL 未開啟，資料符號（init_mm、
 * swapper_pg_dir…）查不到，因此本模組改用兩個更可靠的來源：
 *   - swapper_pg_dir 的實體位址直接從 TTBR1_EL1 讀，VA 用 kimage_voffset 換算
 *   - _stext 由外部（/proc/kallsyms）以模組參數傳入
 *
 * 用法：
 *   STEXT=0x$(sudo grep -w _stext /proc/kallsyms | cut -d' ' -f1)
 *   sudo insmod armv8_dump.ko stext=$STEXT
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 *   sudo rmmod armv8_dump
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <asm/sysreg.h>
#include <asm/memory.h>
#include <asm/pgtable.h>
#include <asm/cputype.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ARMv8 sysreg / page-table dumper for RK3588 kernel study notes");

static unsigned long walk_va;
static unsigned long stext;
module_param(walk_va, ulong, 0444);
MODULE_PARM_DESC(walk_va, "extra kernel VA to software-walk");
module_param(stext, ulong, 0444);
MODULE_PARM_DESC(stext, "address of _stext, taken from /proc/kallsyms");

#define P(fmt, ...) pr_info("armv8_dump: " fmt, ##__VA_ARGS__)

static unsigned long swapper_va;   /* swapper_pg_dir 的核心虛擬位址 */
static u64           swapper_pa;   /* 其實體位址（來自 TTBR1_EL1）  */

/* ================================================================== */
/* 1. ARMv8 系統暫存器                                                 */
/* ================================================================== */
static void dump_sysregs(void)
{
	u64 tcr   = read_sysreg(tcr_el1);
	u64 ttbr0 = read_sysreg(ttbr0_el1);
	u64 ttbr1 = read_sysreg(ttbr1_el1);
	u64 mair  = read_sysreg(mair_el1);
	u64 mmfr0 = read_sysreg(id_aa64mmfr0_el1);
	u64 sctlr = read_sysreg(sctlr_el1);
	u64 ctr   = read_sysreg(ctr_el0);
	u64 clidr = read_sysreg(clidr_el1);
	u32 midr  = read_cpuid_id();
	static const char * const parange[] = {
		"32bit/4GB", "36bit/64GB", "40bit/1TB", "42bit/4TB",
		"44bit/16TB", "48bit/256TB", "52bit/4PB", "reserved" };
	int i;

	P("=============== [1] ARMv8 system registers (EL1, CPU%d) ===============\n",
	  smp_processor_id());

	P("MIDR_EL1  = 0x%08x   part=0x%03x -> %s\n", midr, MIDR_PARTNUM(midr),
	  MIDR_PARTNUM(midr) == 0xd05 ? "Cortex-A55 (LITTLE)" :
	  MIDR_PARTNUM(midr) == 0xd0b ? "Cortex-A76 (big)" : "unknown");

	/* ---- Ch2 Q1 / Q4: TTBR0 vs TTBR1, T0SZ/T1SZ ---- */
	P("TCR_EL1   = 0x%016llx\n", tcr);
	P("   T0SZ = %2llu  -> TTBR0(user)   有效 VA = %llu bits = %llu TB\n",
	  tcr & 0x3f, 64 - (tcr & 0x3f), 1ULL << (64 - (tcr & 0x3f) - 40));
	P("   T1SZ = %2llu  -> TTBR1(kernel) 有效 VA = %llu bits = %llu TB\n",
	  (tcr >> 16) & 0x3f, 64 - ((tcr >> 16) & 0x3f),
	  1ULL << (64 - ((tcr >> 16) & 0x3f) - 40));
	P("   TG0  = %llu (0=4KB,1=64KB,2=16KB)    TG1 = %llu (1=16KB,2=4KB,3=64KB)\n",
	  (tcr >> 14) & 0x3, (tcr >> 30) & 0x3);
	P("   IPS  = %llu -> 中間實體位址寬度 %s\n",
	  (tcr >> 32) & 0x7, parange[(tcr >> 32) & 0x7]);
	P("   AS   = %llu -> ASID 寬度 %s bits   (Ch2 Q14)\n",
	  (tcr >> 36) & 0x1, ((tcr >> 36) & 0x1) ? "16" : "8");
	P("   A1   = %llu (0 = ASID 由 TTBR0_EL1 提供)\n", (tcr >> 22) & 0x1);
	P("   EPD0 = %llu  EPD1 = %llu  (1 = 停用該 TTBR 的 table walk)\n",
	  (tcr >> 7) & 0x1, (tcr >> 23) & 0x1);

	P("TTBR0_EL1 = 0x%016llx   BADDR(PA)=0x%012llx   ASID=%llu\n",
	  ttbr0, ttbr0 & GENMASK_ULL(47, 1), ttbr0 >> 48);
	P("TTBR1_EL1 = 0x%016llx   BADDR(PA)=0x%012llx   ASID=%llu\n",
	  ttbr1, ttbr1 & GENMASK_ULL(47, 1), ttbr1 >> 48);
	P("   -> TTBR1 的 BADDR 就是 swapper_pg_dir 的實體位址（全域核心頁表）\n");
	if (current->mm)
		P("   -> current(%s) mm->pgd VA=0x%px  __pa=0x%llx  應等於 TTBR0 BADDR\n",
		  current->comm, current->mm->pgd, (u64)__pa(current->mm->pgd));

	/* ---- Ch2 Q15: 記憶體屬性 ---- */
	P("MAIR_EL1  = 0x%016llx   (8 個 attribute slot，每 slot 1 byte)  (Ch2 Q15)\n", mair);
	for (i = 0; i < 8; i++) {
		u8 a = (mair >> (i * 8)) & 0xff;
		const char *d;
		switch (a) {
		case 0x00: d = "Device-nGnRnE  最嚴格/強序:不合併,不重排,不提前應答"; break;
		case 0x04: d = "Device-nGnRE   不合併,不重排,允許提前應答(ioremap 預設)"; break;
		case 0x08: d = "Device-nGRE    不合併,允許重排"; break;
		case 0x0c: d = "Device-GRE     最寬鬆"; break;
		case 0x44: d = "Normal Non-Cacheable (inner+outer NC)  ioremap_wc/dma"; break;
		case 0xff: d = "Normal WB RW-Allocate (inner+outer)    一般 RAM"; break;
		case 0xbb: d = "Normal Write-Through"; break;
		case 0xf0: d = "Normal (tagged / MTE)"; break;
		default:   d = (a == 0) ? "unused" : "(其他 Normal 編碼)"; break;
		}
		P("   MAIR Attr%d = 0x%02x  %s\n", i, a, d);
	}

	/* ---- Ch2 Q14: ASID 硬體能力; Q22: PA 寬度 ---- */
	P("ID_AA64MMFR0_EL1 = 0x%016llx\n", mmfr0);
	P("   PARange  = %llu -> 硬體實體位址寬度 %s\n",
	  mmfr0 & 0xf, parange[(mmfr0 & 0xf) < 7 ? (mmfr0 & 0xf) : 7]);
	P("   ASIDBits = %llu -> 硬體支援 %s-bit ASID (%s 個)  (Ch2 Q14)\n",
	  (mmfr0 >> 4) & 0xf, (((mmfr0 >> 4) & 0xf) == 2) ? "16" : "8",
	  (((mmfr0 >> 4) & 0xf) == 2) ? "65536" : "256");
	P("   TGran4 = %llu  TGran16 = %llu  TGran64 = %llu\n",
	  (mmfr0 >> 28) & 0xf, (mmfr0 >> 20) & 0xf, (mmfr0 >> 24) & 0xf);

	P("SCTLR_EL1 = 0x%016llx   M(MMU)=%llu  C(D-cache)=%llu  I(I-cache)=%llu  (Ch2 Q20)\n",
	  sctlr, sctlr & 1, (sctlr >> 2) & 1, (sctlr >> 12) & 1);

	/* ---- Ch2 Q13: PoU / PoC ---- */
	P("CTR_EL0   = 0x%016llx\n", ctr);
	P("   IminLine=%llu -> I-cache line = %llu bytes\n",
	  ctr & 0xf, (u64)(4UL << (ctr & 0xf)));
	P("   DminLine=%llu -> D-cache line = %llu bytes\n",
	  (ctr >> 16) & 0xf, (u64)(4UL << ((ctr >> 16) & 0xf)));
	P("   L1Ip=%llu (2=VIPT, 3=PIPT)   CWG=%llu   ERG=%llu\n",
	  (ctr >> 14) & 0x3, (ctr >> 24) & 0xf, (ctr >> 20) & 0xf);
	P("CLIDR_EL1 = 0x%016llx   (Ch2 Q13: PoU/PoC 落在第幾級)\n", clidr);
	P("   LoUU  = %llu  <- PoU  (Level of Unification, Uniprocessor)\n",
	  (clidr >> 27) & 0x7);
	P("   LoC   = %llu  <- PoC  (Level of Coherency)\n", (clidr >> 24) & 0x7);
	P("   LoUIS = %llu  <- PoU for Inner Shareable\n", (clidr >> 21) & 0x7);
	for (i = 0; i < 7; i++) {
		u64 ctype = (clidr >> (i * 3)) & 0x7;
		static const char * const ct[] = { "none", "I only", "D only",
						   "I+D separate", "unified",
						   "?", "?", "?" };
		if (ctype)
			P("   L%d cache type = %llu (%s)\n", i + 1, ctype, ct[ctype]);
	}
}

/* ================================================================== */
/* 2. 核心虛擬記憶體佈局 (Ch2 Q4/Q5/Q6/Q9, Ch3 Q4/Q6)                  */
/* ================================================================== */
static void dump_layout(void)
{
	P("=============== [2] kernel VA layout (VA_BITS=%d, PAGE_SIZE=%luK) ===============\n",
	  VA_BITS, PAGE_SIZE >> 10);
	P("  user space   : 0x0000000000000000 - 0x%016lx  (TTBR0, 每行程獨立)\n",
	  (1UL << VA_BITS) - 1);
	P("  ---- 以下屬核心空間 (TTBR1, 全域共享) ----\n");
	P("  MODULES_VADDR: 0x%016lx\n", (unsigned long)MODULES_VADDR);
	P("  MODULES_END  : 0x%016lx   (MODULES_VSIZE = %lu MB)\n",
	  (unsigned long)MODULES_END, (unsigned long)MODULES_VSIZE >> 20);
	P("  KIMAGE_VADDR : 0x%016lx   <- Ch2 Q6: == MODULES_END\n",
	  (unsigned long)KIMAGE_VADDR);
	P("  VMALLOC_START: 0x%016lx\n", (unsigned long)VMALLOC_START);
	P("  VMALLOC_END  : 0x%016lx\n", (unsigned long)VMALLOC_END);
	P("  PAGE_OFFSET  : 0x%016lx   <- Ch2 Q5: 線性映射起點 = -(1<<VA_BITS)\n",
	  (unsigned long)PAGE_OFFSET);
	P("  PAGE_END     : 0x%016lx\n", (unsigned long)PAGE_END);
	P("  FIXADDR_TOP  : 0x%016lx\n", (unsigned long)FIXADDR_TOP);
	P("  PCI_IO_START : 0x%016lx   PCI_IO_END = 0x%016lx\n",
	  (unsigned long)PCI_IO_START, (unsigned long)PCI_IO_END);
	P("  VMEMMAP_START: 0x%016lx   VMEMMAP_END= 0x%016lx  (struct page 陣列)\n",
	  (unsigned long)VMEMMAP_START, (unsigned long)VMEMMAP_END);

	P("---- 位址轉換錨點 (Ch2 Q10/Q12, Ch3 Q6) ----\n");
	P("  memstart_addr (PHYS_OFFSET) = 0x%llx   <- DTB /memory 第一段的起始 PA\n",
	  (u64)memstart_addr);
	P("  kimage_voffset              = 0x%llx   <- Ch2 Q12: 映像 VA - 映像 PA\n",
	  (u64)kimage_voffset);
	P("  vabits_actual               = %llu\n", (u64)vabits_actual);
	P("  swapper_pg_dir PA (TTBR1)   = 0x%llx\n", swapper_pa);
	P("  swapper_pg_dir VA (推算)    = 0x%lx  (= PA + kimage_voffset)\n", swapper_va);

	if (stext) {
		u64 pa_sym   = (u64)__pa_symbol(stext);
		u64 pa_wrong = (u64)(stext - PAGE_OFFSET + memstart_addr);

		P("---- Ch2 Q10: __pa_symbol() vs __pa() 用在核心映像符號上 ----\n");
		P("  _stext (由 /proc/kallsyms 傳入) = 0x%lx\n", stext);
		P("  __pa_symbol(_stext)             = 0x%llx  <- 正確: VA - kimage_voffset\n",
		  pa_sym);
		P("  手算 _stext - kimage_voffset    = 0x%llx  <- 應完全相同\n",
		  (u64)(stext - kimage_voffset));
		P("  若誤用 __pa(_stext)             = 0x%llx  <- 錯誤答案\n", pa_wrong);
		P("  兩者相差 0x%llx\n", pa_wrong - pa_sym);
		P("  原因: kimage_voffset=0x%llx, 但線性映射偏移=PAGE_OFFSET-PHYS_OFFSET=0x%llx\n",
		  (u64)kimage_voffset, (u64)(PAGE_OFFSET - memstart_addr));
	}
	P("---- 線性映射區自我檢驗 ----\n");
	P("  __pa(PAGE_OFFSET) = 0x%llx  (應 == memstart_addr)\n",
	  (u64)__pa((void *)PAGE_OFFSET));
	P("  __va(memstart_addr) = 0x%px  (應 == PAGE_OFFSET)\n", __va(memstart_addr));
}

/* ================================================================== */
/* 3. 軟體頁表巡覽 (Ch2 Q2/Q3/Q22/Q23)                                 */
/* ================================================================== */
static void decode_desc(const char *lvl, u64 desc, int level)
{
	const char *type;
	u64 lo2 = desc & 3;

	if (!(desc & 1))
		type = "INVALID -> Translation Fault";
	else if (lo2 == 3)
		type = (level == 3) ? "PAGE  descriptor ([1:0]=0b11)"
				    : "TABLE descriptor ([1:0]=0b11)";
	else
		type = "BLOCK descriptor ([1:0]=0b01)";

	P("   %-2s desc = 0x%016llx   [1:0]=0b%llu%llu   %s\n",
	  lvl, desc, (desc >> 1) & 1, desc & 1, type);
	if (!(desc & 1))
		return;
	P("        bits[47:12] = 0x%012llx  <- Ch2 Q22: 這是【實體位址】\n",
	  desc & GENMASK_ULL(47, 12));
	if (lo2 == 3 && level < 3) {
		P("        -> 軟體要讀下一級, 必須先 __va(0x%012llx) 換成虛擬位址 (Ch2 Q23)\n",
		  desc & GENMASK_ULL(47, 12));
		return;
	}
	P("        AttrIndx=%llu (指向 MAIR Attr%llu)  NS=%llu  AP=%llu(%s)  SH=%llu(%s)\n",
	  (desc >> 2) & 7, (desc >> 2) & 7, (desc >> 5) & 1,
	  (desc >> 6) & 3,
	  (((desc >> 6) & 3) == 0) ? "EL1 RW, EL0 none" :
	  (((desc >> 6) & 3) == 1) ? "EL1 RW, EL0 RW"   :
	  (((desc >> 6) & 3) == 2) ? "EL1 RO, EL0 none" : "EL1 RO, EL0 RO",
	  (desc >> 8) & 3,
	  (((desc >> 8) & 3) == 0) ? "Non-shareable" :
	  (((desc >> 8) & 3) == 2) ? "Outer Shareable" :
	  (((desc >> 8) & 3) == 3) ? "Inner Shareable" : "reserved");
	P("        AF=%llu  nG=%llu  DBM=%llu  Contig=%llu  PXN=%llu  UXN=%llu\n",
	  (desc >> 10) & 1, (desc >> 11) & 1, (desc >> 51) & 1,
	  (desc >> 52) & 1, (desc >> 53) & 1, (desc >> 54) & 1);
}

static void walk(unsigned long va, const char *what)
{
	pgd_t *l0; pud_t *l1; pmd_t *l2; pte_t *l3;
	u64 pa;
	int user = !((va >> 63) & 1);
	struct mm_struct *mm = current->mm;

	P("=============== [3] software page-table walk: %s ===============\n", what);
	P("  VA = 0x%016lx\n", va);
	P("  VA[63] = %lu  ->  使用 %s\n", (va >> 63) & 1,
	  user ? "TTBR0_EL1 (使用者空間)" : "TTBR1_EL1 (核心空間)");
	P("  索引拆解 (4KB granule, 48-bit VA):\n");
	P("     L0(PGD)=VA[47:39]=%3lu  L1(PUD)=VA[38:30]=%3lu  L2(PMD)=VA[29:21]=%3lu  L3(PTE)=VA[20:12]=%3lu  off=0x%03lx\n",
	  (va >> 39) & 0x1ff, (va >> 30) & 0x1ff, (va >> 21) & 0x1ff,
	  (va >> 12) & 0x1ff, va & 0xfff);

	if (user) {
		if (!mm) { P("  (current 沒有 mm)\n"); return; }
		l0 = pgd_offset(mm, va);
		P("  L0 表基址 VA = 0x%px (current->mm->pgd)\n", mm->pgd);
	} else {
		l0 = (pgd_t *)swapper_va + pgd_index(va);
		P("  L0 表基址 VA = 0x%lx (swapper_pg_dir)\n", swapper_va);
	}
	P("  -> L0 entry 的 VA = 0x%px\n", l0);
	decode_desc("L0", pgd_val(*l0), 0);
	if (pgd_none(*l0)) return;

	l1 = pud_offset(p4d_offset(l0, va), va);
	P("  pud_offset() 回傳的 L1 entry VA = 0x%px\n", l1);
	decode_desc("L1", pud_val(*l1), 1);
	if (pud_none(*l1)) return;
	if (pud_sect(*l1)) {
		pa = (pud_val(*l1) & GENMASK_ULL(47, 30)) | (va & 0x3fffffffUL);
		P("  ==> L1 是 1GB BLOCK, 轉換到此結束。PA = 0x%llx\n", pa);
		goto verify;
	}

	l2 = pmd_offset(l1, va);
	P("  pmd_offset() 回傳的 L2 entry VA = 0x%px\n", l2);
	decode_desc("L2", pmd_val(*l2), 2);
	if (pmd_none(*l2)) return;
	if (pmd_sect(*l2)) {
		pa = (pmd_val(*l2) & GENMASK_ULL(47, 21)) | (va & 0x1fffffUL);
		P("  ==> L2 是 2MB BLOCK, 轉換到此結束 (Ch2 Q3)。PA = 0x%llx\n", pa);
		goto verify;
	}

	l3 = pte_offset_kernel(l2, va);
	P("  pte_offset_kernel() 回傳的 L3 entry VA = 0x%px\n", l3);
	decode_desc("L3", pte_val(*l3), 3);
	if (pte_none(*l3)) return;
	pa = (pte_val(*l3) & GENMASK_ULL(47, 12)) | (va & 0xfffUL);
	P("  ==> L3 是 4KB PAGE 描述符。PA = 0x%llx\n", pa);

verify:
	if (!user && va >= PAGE_OFFSET)
		P("  交叉驗證 __pa(va)        = 0x%llx\n", (u64)__pa((void *)va));
	else if (!user && stext && va >= KIMAGE_VADDR && va < VMALLOC_START)
		P("  交叉驗證 __pa_symbol(va) = 0x%llx\n", (u64)__pa_symbol(va));
}

/* ================================================================== */
static int __init armv8_dump_init(void)
{
	u64 ttbr1 = read_sysreg(ttbr1_el1);

	swapper_pa = ttbr1 & GENMASK_ULL(47, 1);
	swapper_va = (unsigned long)(swapper_pa + kimage_voffset);

	dump_sysregs();
	dump_layout();

	/* (a) 核心映像映射區：_stext —— 預期是 2MB block 映射 */
	if (stext)
		walk(stext, "_stext (核心映像映射區, KIMAGE_VADDR 之後)");
	/* (b) 線性映射區 */
	walk((unsigned long)PAGE_OFFSET + 0x200000,
	     "PAGE_OFFSET+2MB (實體記憶體線性映射區)");
	/* (c) vmalloc / module 區：本模組自己的程式碼 */
	walk((unsigned long)armv8_dump_init, "本模組的 init 函式 (module/vmalloc 區)");
	/* (d) 使用者空間：insmod 行程的程式碼段 */
	if (current->mm && current->mm->start_code)
		walk(current->mm->start_code, "insmod 行程的使用者空間程式碼段 (TTBR0)");
	/* (e) 額外指定 */
	if (walk_va)
		walk(walk_va, "walk_va 參數指定的位址");

	P("=============== done ===============\n");
	return 0;
}

static void __exit armv8_dump_exit(void)
{
	P("unloaded\n");
}

module_init(armv8_dump_init);
module_exit(armv8_dump_exit);
