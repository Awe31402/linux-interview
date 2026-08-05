// SPDX-License-Identifier: GPL-2.0
/*
 * addr_probe.ko —— 卷2 第3章 Q2/Q5/Q7：
 *   把「正在跑的這顆核心」的加載地址 / 運行地址 / 鏈接地址一次攤開，
 *   並驗證 arm64 的核心映像重定位（__relocate_kernel）到底做了什麼。
 *
 * 平台：Radxa ROCK 5B (RK3588)，Linux 6.1.115+ aarch64，
 *      CONFIG_RELOCATABLE=y、CONFIG_RANDOMIZE_BASE=n、VA_BITS=48
 *
 * 用法：
 *   sudo insmod addr_probe.ko          （模組刻意回傳 -EAGAIN，不必 rmmod）
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <asm/memory.h>
#include <asm/pgtable.h>
#include <asm/sections.h>
#include <generated/utsrelease.h>
#include "ksym.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("kernel load/run/link address + relocation probe (ch12 Q2/Q5/Q7)");

static void show_sym(const char *name)
{
	unsigned long va = (unsigned long)EXP_LOOKUP(name);

	if (!va) {
		pr_info("  %-20s <不在 kallsyms>\n", name);
		return;
	}
	pr_info("  %-20s 鏈接/運行虛擬位址 = 0x%016lx  實體位址 = 0x%016llx  "
		"距 KIMAGE_VADDR +0x%lx\n",
		name, va, (u64)__pa_symbol(va), va - KIMAGE_VADDR);
}

static int __init addr_probe_init(void)
{
	void *modbase = THIS_MODULE->core_layout.base;
	unsigned long text;

	if (exp_ksym_init() < 0) {
		pr_err("kallsyms_lookup_name 取得失敗\n");
		return -ENOENT;
	}
	text = (unsigned long)EXP_LOOKUP("_stext");

	pr_info("======== addr_probe: %s ========\n", UTS_RELEASE);

	pr_info("--- [1] 編譯期就決定的「鏈接地址」骨架（arch/arm64/include/asm/memory.h）---\n");
	pr_info("  VA_BITS              = %d\n", VA_BITS);
	pr_info("  PAGE_OFFSET          = 0x%016lx  (線性映射起點)\n", (unsigned long)PAGE_OFFSET);
	pr_info("  KIMAGE_VADDR         = 0x%016lx  (核心映像 _text 的鏈接地址)\n",
		(unsigned long)KIMAGE_VADDR);
	pr_info("  MODULES_VADDR        = 0x%016lx\n", (unsigned long)MODULES_VADDR);
	pr_info("  MODULES_END          = 0x%016lx  (模組區 %lu MB)\n",
		(unsigned long)MODULES_END,
		((unsigned long)MODULES_END - (unsigned long)MODULES_VADDR) >> 20);
	pr_info("  VMALLOC_START        = 0x%016lx\n", (unsigned long)VMALLOC_START);
	pr_info("  VMALLOC_END          = 0x%016lx\n", (unsigned long)VMALLOC_END);

	pr_info("--- [2] 執行期才知道的「加載地址 / 運行地址」---\n");
	pr_info("  memstart_addr(PHYS_OFFSET) = 0x%016llx  (DRAM 起點)\n",
		(u64)PHYS_OFFSET);
	pr_info("  kimage_voffset       = 0x%016llx  (= 虛擬 - 實體，核心映像專用)\n",
		kimage_voffset);
	pr_info("  kimage_vaddr         = 0x%016lx\n", (unsigned long)kimage_vaddr);
	pr_info("  kaslr_offset()       = 0x%016lx  %s\n", kaslr_offset(),
		kaslr_offset() ? "(KASLR 有位移)" : "(CONFIG_RANDOMIZE_BASE=n → 位移 0)");
	pr_info("  PHYS(_text) = 0x%016llx  (U-Boot 把 Image 放到的實體位址=加載地址)\n",
		(u64)__pa_symbol((unsigned long)KIMAGE_VADDR + kaslr_offset()));

	pr_info("--- [3] 各段的鏈接地址 ↔ 實體加載地址（對照 /proc/iomem）---\n");
	show_sym("_text");
	show_sym("_stext");
	show_sym("_etext");
	show_sym("__init_begin");
	show_sym("__primary_switch");
	show_sym("__relocate_kernel");
	show_sym("__primary_switched");
	show_sym("_end");

	pr_info("--- [4] 這個模組自己（模組永遠要重定位）---\n");
	pr_info("  模組 .text 運行地址   = 0x%016lx  (MODULES_VADDR + 0x%lx)\n",
		(unsigned long)modbase,
		(unsigned long)modbase - (unsigned long)MODULES_VADDR);
	pr_info("  模組 → 核心 _stext 距離 = %ld MB  (bl/R_AARCH64_CALL26 上限 ±128 MB)\n",
		text ? ((long)modbase - (long)text) >> 20 : 0);
	pr_info("  addr_probe_init 的位址 = 0x%016lx\n", (unsigned long)addr_probe_init);

	pr_info("--- [5] 結論 ---\n");
	pr_info("  鏈接地址 = %#lx (vmlinux 裡看到的)，"
		"運行地址 = %#lx，兩者差 %#lx\n",
		(unsigned long)KIMAGE_VADDR,
		(unsigned long)KIMAGE_VADDR + kaslr_offset(), kaslr_offset());
	pr_info("  加載地址 = %#llx（實體），靠 MMU + kimage_voffset 把它擺回鏈接地址\n",
		(u64)__pa_symbol((unsigned long)KIMAGE_VADDR + kaslr_offset()));

	return -EAGAIN;	/* 只是印資訊，不要真的留在系統裡 */
}

static void __exit addr_probe_exit(void) { }

module_init(addr_probe_init);
module_exit(addr_probe_exit);
