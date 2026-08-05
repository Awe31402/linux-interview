// SPDX-License-Identifier: GPL-2.0
/*
 * reloc_mod.ko —— 卷2 第3章 Q4：「什麼是重定位」的可執行證據
 *
 * 核心映像在本機（CONFIG_RANDOMIZE_BASE=n）重定位位移是 0，看不出效果；
 * 但**每一個核心模組都一定要重定位**：.ko 是可重定位 ELF（ET_REL），
 * 裡面的 bl / adrp 全都是留白，位址欄位由 module loader
 * （arch/arm64/kernel/module.c: apply_relocate_add()）在載入時依實際
 * 載入位址填進去。
 *
 * 這個模組做的事：
 *   1. 印出自己被載到哪裡（運行地址）。
 *   2. 把 reloc_probe_site() 這個函式**在記憶體中的機器碼**逐條 dump 出來，
 *      解出 BL 的 imm26 與 ADRP 的 immhi/immlo，算出跳轉目標。
 *   3. 與 `objdump -dr reloc_mod.o` 裡的「留白 + R_AARCH64_CALL26/ADR_PREL_PG_HI21」
 *      對照 —— 就能看到「重定位前 vs 重定位後」的同一條指令。
 *
 * 用法：
 *   sudo insmod reloc_mod.ko ; sudo dmesg | sed 's/^\[[^]]*\] //'
 *   aarch64-linux-gnu-objdump -dr reloc_mod.o      # 重定位前
 *   readelf -r reloc_mod.ko
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kallsyms.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("module relocation evidence (ch12 Q4)");

/* 模組自己的全域變數：會被 adrp+add 以 PC 相對方式定址 */
static unsigned long reloc_gvar = 0xdeadbeefcafe0000UL;

/* noinline 保證這個函式真的存在，而且裡面真的有一條 bl 到核心符號 */
static noinline void reloc_probe_site(void)
{
	/* 這一行會產生 R_AARCH64_CALL26（bl _printk）
	 * 以及 R_AARCH64_ADR_PREL_PG_HI21 / ADD_ABS_LO12_NC（取字串與 reloc_gvar）*/
	printk(KERN_INFO "reloc_mod: gvar=%#lx @ %px\n", reloc_gvar, &reloc_gvar);
}

static void dump_insn(const char *tag, u32 *pc, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		u32 insn = pc[i];
		char note[128] = "";

		/* BL: 100101 imm26 */
		if ((insn & 0xfc000000) == 0x94000000) {
			s32 imm26 = (s32)(insn << 6) >> 6;	/* 符號延伸 */
			unsigned long target = (unsigned long)&pc[i] + ((long)imm26 << 2);
			char sym[KSYM_SYMBOL_LEN];

			sprint_symbol(sym, target);
			snprintf(note, sizeof(note),
				 "BL  imm26=%#x → 0x%lx <%s>", imm26, target, sym);
		/* ADRP: 1 immlo 10000 immhi Rd */
		} else if ((insn & 0x9f000000) == 0x90000000) {
			u32 immlo = (insn >> 29) & 0x3;
			s32 immhi = (s32)((insn << 8) >> 13);	/* 19 bits, 符號延伸 */
			long imm = (((long)immhi << 2) | immlo) << 12;
			unsigned long base = ((unsigned long)&pc[i]) & ~0xfffUL;

			snprintf(note, sizeof(note),
				 "ADRP x%u, 0x%lx", insn & 0x1f, base + imm);
		} else if ((insn & 0xff000000) == 0x91000000) {
			snprintf(note, sizeof(note), "ADD  x%u, x%u, #%#x (:lo12:)",
				 insn & 0x1f, (insn >> 5) & 0x1f, (insn >> 10) & 0xfff);
		}
		pr_info("  %s +0x%02x: %08x  %s\n", tag, i * 4, insn, note);
	}
}

static int __init reloc_mod_init(void)
{
	void *base = THIS_MODULE->core_layout.base;

	pr_info("======== reloc_mod：模組重定位 ========\n");
	pr_info("模組載入位址(運行地址) core_layout.base = %px, size = %u\n",
		base, THIS_MODULE->core_layout.size);
	pr_info("reloc_probe_site  = %px  (檔案內偏移由 objdump 看)\n", reloc_probe_site);
	pr_info("reloc_gvar        = %px\n", &reloc_gvar);
	pr_info("_printk           = %px\n", _printk);
	pr_info("--- reloc_probe_site() 在記憶體中的機器碼（= 重定位「之後」）---\n");
	dump_insn("reloc_probe_site", (u32 *)reloc_probe_site, 10);
	reloc_probe_site();
	return -EAGAIN;
}

static void __exit reloc_mod_exit(void) { }

module_init(reloc_mod_init);
module_exit(reloc_mod_exit);
