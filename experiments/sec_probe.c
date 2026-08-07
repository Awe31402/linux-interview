// SPDX-License-Identifier: GPL-2.0
/*
 * sec_probe.ko —— 《奔跑吧》卷2 第6章〈安全漏洞分析〉的核心側證據模組。
 * 平台：Radxa ROCK 5B (RK3588, A55×4 + A76×4), Linux 6.1.115+ aarch64
 *
 * 直接讀系統暫存器 + 軟體巡覽頁表 + 反組譯核心程式，回答：
 *   Q3  熔斷是否可行     -> ID_AA64PFR0_EL1.CSV3（每顆 CPU）
 *   Q4  KPTI 實作原理    -> 核心頁 PTE 的 nG 位、跳板/向量表符號
 *   Q5  copy_*_user ASID -> TCR_EL1.A1、TTBR0/1 的 ASID 欄位
 *   Q7  幽靈 v2 防護      -> ID_AA64PFR0_EL1.CSV2、VBAR_EL1 指向哪張向量表
 *   Q8  CSDB 指令        -> 反組譯 invoke_syscall，抓 csdb(0xd503229f)/sbc
 *
 * 資料符號查不到（CONFIG_KALLSYMS_ALL=n），需要的核心位址用模組參數傳入，
 * 由 ch15_run_all.sh 從 /proc/kallsyms 抓好：
 *   stext, vectors, bp_harden(=__bp_harden_el1_vectors),
 *   tramp_vectors, invoke_syscall(=el0_svc_common 亦可)
 *
 * 用法：見 ch15_run_all.sh。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/sysreg.h>
#include <asm/cputype.h>
#include <asm/pgtable.h>
#include <asm/memory.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RK3588 security-vulnerability sysreg / pgtable / disasm probe");

static unsigned long stext, vectors, bp_harden, tramp_vectors, invoke_syscall;
module_param(stext, ulong, 0444);
module_param(vectors, ulong, 0444);
module_param(bp_harden, ulong, 0444);
module_param(tramp_vectors, ulong, 0444);
module_param(invoke_syscall, ulong, 0444);

#define P(fmt, ...) pr_info("sec_probe: " fmt, ##__VA_ARGS__)

/* ---- 每顆 CPU 的 MIDR 與推測側信道相關 ID 欄位 ---- */
struct cpu_id { u32 midr; u64 pfr0; u64 vbar; };
static struct cpu_id ids[NR_CPUS];

static void read_id_this_cpu(void *info)
{
	int cpu = smp_processor_id();
	ids[cpu].midr = read_cpuid_id();
	ids[cpu].pfr0 = read_sysreg_s(SYS_ID_AA64PFR0_EL1);
	ids[cpu].vbar = read_sysreg(vbar_el1);
}

static const char *part_name(u32 midr)
{
	switch (MIDR_PARTNUM(midr)) {
	case 0xd05: return "Cortex-A55";
	case 0xd0b: return "Cortex-A76";
	default:    return "unknown";
	}
}

static void dump_cpu_ids(void)
{
	int cpu;

	P("=============== [Q3/Q7] 每顆 CPU 的推測側信道抵抗力 ===============\n");
	P("ID_AA64PFR0_EL1.CSV2/CSV3 : CSV3>=1 => 對熔斷(rogue cache load)免疫;\n");
	P("                            CSV2>=1 => 對幽靈v2(分支注入)有硬體緩解\n");
	for_each_online_cpu(cpu) {
		u64 pfr0 = ids[cpu].pfr0;
		u32 csv2 = (pfr0 >> 56) & 0xf;
		u32 csv3 = (pfr0 >> 60) & 0xf;
		P("  CPU%d %-11s MIDR=0x%08x  CSV2=%u  CSV3=%u %s\n",
		  cpu, part_name(ids[cpu].midr), ids[cpu].midr, csv2, csv3,
		  csv3 ? "(熔斷免疫)" : "(ID未宣告，靠核心白名單判定)");
	}
	P("--- 每顆 CPU 的 VBAR_EL1（幽靈v2/BHB 硬化向量表在哪些核上）---\n");
	for_each_online_cpu(cpu) {
		u64 v = ids[cpu].vbar;
		const char *which = "未知";
		if (v == vectors)         which = "vectors (原始)";
		else if (v == bp_harden)  which = "__bp_harden_el1_vectors (BHB/v2 硬化)";
		else if (v == tramp_vectors) which = "tramp_vectors (KPTI 跳板)";
		P("  CPU%d %-11s VBAR_EL1=0x%016llx = %s\n",
		  cpu, part_name(ids[cpu].midr), v, which);
	}
}

/* ---- Q5：ASID 從哪個 TTBR 來、目前 TTBR0/1 的 ASID ---- */
static void dump_asid(void)
{
	u64 tcr   = read_sysreg(tcr_el1);
	u64 ttbr0 = read_sysreg(ttbr0_el1);
	u64 ttbr1 = read_sysreg(ttbr1_el1);
	u32 a1    = (tcr >> 22) & 1;
	u64 asid0 = ttbr0 >> 48;
	u64 asid1 = ttbr1 >> 48;

	P("=============== [Q5] copy_*_user 用哪個 ASID 查 TLB ===============\n");
	P("TCR_EL1.A1 = %u  -> ASID 由 %s 提供\n", a1, a1 ? "TTBR1_EL1" : "TTBR0_EL1");
	P("TTBR0_EL1 = 0x%016llx  ASID=%llu (%s)\n",
	  ttbr0, asid0, (asid0 & 1) ? "奇數=使用者頁表" : "偶數=核心頁表");
	P("TTBR1_EL1 = 0x%016llx  ASID=%llu (%s)\n",
	  ttbr1, asid1, (asid1 & 1) ? "奇數" : "偶數=核心頁表");
	P("解讀：KPTI 下核心態用偶數 ASID；copy_to/from_user 存取使用者位址時\n");
	P("      仍帶著這個偶數(核心)ASID 查 TLB，配不上使用者頁的奇數 ASID,\n");
	P("      多半 TLB miss，需走 MMU 翻頁 -> 有一點效能損失。\n");
}

/* ---- Q4：軟體巡覽核心頁表，看 nG 位（KPTI 把核心頁改成 non-global） ---- */
static void check_kpti_ng(void)
{
	pgd_t *l0; pud_t *l1; pmd_t *l2; pte_t *l3;
	unsigned long va = stext;
	u64 desc; int ng_seen = 0;
	u64 ttbr1 = read_sysreg(ttbr1_el1);
	unsigned long swapper_va =
		(unsigned long)((ttbr1 & GENMASK_ULL(47, 1)) + kimage_voffset);

	P("=============== [Q4] KPTI：核心頁是否為 non-global (nG) ===============\n");
	if (!stext) { P("(未提供 stext，略過)\n"); goto vbar; }

	/* 從 TTBR1 取 swapper_pg_dir 的 VA，避開未匯出的 init_mm */
	l0 = (pgd_t *)swapper_va + pgd_index(va);
	if (pgd_none(*l0)) { P("L0 empty\n"); goto vbar; }
	l1 = pud_offset(p4d_offset(l0, va), va);
	if (pud_none(*l1)) { P("L1 empty\n"); goto vbar; }
	if (pud_sect(*l1)) { desc = pud_val(*l1); goto decode; }
	l2 = pmd_offset(l1, va);
	if (pmd_none(*l2)) { P("L2 empty\n"); goto vbar; }
	if (pmd_sect(*l2)) { desc = pmd_val(*l2); goto decode; }
	l3 = pte_offset_kernel(l2, va);
	if (pte_none(*l3)) { P("L3 empty\n"); goto vbar; }
	desc = pte_val(*l3);
decode:
	ng_seen = !!(desc & PTE_NG);        /* PTE_NG = 1<<11 */
	P("_stext(0x%lx) 末級描述符 = 0x%016llx\n", va, desc);
	P("  nG(bit11) = %d -> %s\n", ng_seen,
	  ng_seen ? "non-global：KPTI 生效，核心頁帶 ASID(進程獨有)"
		  : "global：核心頁是全域 TLB（KPTI 未在此頁生效）");

vbar:
	P("=============== [Q4/Q7] 目前 VBAR_EL1 指向哪張向量表 ===============\n");
	{
		u64 vbar = read_sysreg(vbar_el1);
		const char *which = "未知";
		if (vbar == vectors)             which = "vectors (原始核心向量表)";
		else if (vbar == bp_harden)      which = "__bp_harden_el1_vectors (Spectre-BHB/v2 硬化副本)";
		else if (vbar == tramp_vectors)  which = "tramp_vectors (KPTI 跳板向量表)";
		P("VBAR_EL1 = 0x%016llx  = %s\n", vbar, which);
		P("  vectors=0x%lx  bp_harden=0x%lx  tramp_vectors=0x%lx\n",
		  vectors, bp_harden, tramp_vectors);
	}
}

/* ---- Q8：反組譯 invoke_syscall，找 csdb / sbc（array_index_mask_nospec） ---- */
static void scan_csdb(void)
{
	u32 *code = (u32 *)invoke_syscall;
	int i, found = 0;

	P("=============== [Q8] 反組譯核心程式找 CSDB 指令 ===============\n");
	if (!invoke_syscall) { P("(未提供 invoke_syscall 位址，略過)\n"); return; }
	P("掃描 invoke_syscall @ 0x%lx，csdb 機器碼 = 0xd503229f\n", invoke_syscall);
	for (i = 0; i < 64; i++) {          /* 掃前 64 條指令 */
		u32 insn = READ_ONCE(code[i]);
		if (insn == 0xd503229f) {        /* CSDB */
			P("  +0x%02x: 0x%08x  CSDB  <-- 消費預測資料屏障\n", i * 4, insn);
			/* 印出前三條，通常是 cmp/csel 或 subs/sbc */
			if (i >= 3)
				P("    前文  +0x%02x:%08x  +0x%02x:%08x  +0x%02x:%08x\n",
				  (i-3)*4, code[i-3], (i-2)*4, code[i-2], (i-1)*4, code[i-1]);
			found++;
		}
		/* sbc Xd, xzr, xzr = 0xda1f03e0 | Rd（Rm=Rn=xzr，只有 Rd 變） */
		if ((insn & 0xffffffe0) == 0xda1f03e0)
			P("  +0x%02x: 0x%08x  SBC (mask=0-0-!C, array_index_mask_nospec)\n",
			  i * 4, insn);
	}
	if (!found)
		P("  這 64 條裡沒抓到 csdb（可能該路徑未內聯到此位址；改看 syscall 表存取處）\n");
}

/* ---- Q3 靶子：在核心空間放一個「已知值」的祕密，供 meltdown_test 攻擊 ---- */
static u8 *secret_page;
static void plant_secret(void)
{
	secret_page = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!secret_page) { P("(kmalloc 失敗，略過祕密植入)\n"); return; }
	secret_page[0] = 0x5a;   /* 已知祕密位元組 = 0x5a ('Z') */
	P("=============== [Q3] 已在核心空間植入祕密供攻擊 ===============\n");
	P("secret VA = 0x%px  值 = 0x%02x ('%c')  <- 拿這個位址當 meltdown 靶\n",
	  secret_page, secret_page[0], secret_page[0]);
	P("  用法：KADDR=0x%px taskset -c 4 ./meltdown_test\n", secret_page);
	P("  本機預期：signal 法還原不出 0x5a（CSV3=1 熔斷免疫）\n");
}

static int __init sec_probe_init(void)
{
	P("==================== RK3588 安全漏洞分析：實機證據 ====================\n");
	on_each_cpu(read_id_this_cpu, NULL, 1);
	dump_cpu_ids();
	dump_asid();
	check_kpti_ng();
	scan_csdb();
	plant_secret();
	P("==================== 完成，請看上面 dmesg ====================\n");
	return 0;
}

static void __exit sec_probe_exit(void) { kfree(secret_page); P("unloaded\n"); }

module_init(sec_probe_init);
module_exit(sec_probe_exit);
