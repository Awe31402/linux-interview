// SPDX-License-Identifier: GPL-2.0
/*
 * slub_lab.ko —— 卷2 第3章 Q9：「slub_debug 可以檢測哪些類型的記憶體錯誤？」
 *
 * 書上（§3.3.1）的作法是重編內核打開 CONFIG_SLUB_DEBUG_ON，再從 cmdline
 * 傳 slub_debug=UFPZ。本機 6.1.115+ 是 CONFIG_SLUB_DEBUG=y 但
 * CONFIG_SLUB_DEBUG_ON=n、cmdline 也沒有 slub_debug，
 * 所以「框架在、預設關」。
 *
 * 不用改 cmdline、不用重開機也能做完整實驗的辦法：
 *   kmem_cache_create() 時自己帶上 SLAB_RED_ZONE | SLAB_POISON |
 *   SLAB_STORE_USER | SLAB_CONSISTENCY_CHECKS
 *   —— 這四個旗標正好就是 slub_debug=Z,P,U,F 這四個字母
 *      （mm/slub.c: parse_slub_debug_flags() 的對照表）。
 *   走的是 mm/slub.c 裡完全相同的 check_object()/free_debug_processing() 路徑，
 *   所以 dmesg 印出來的 BUG 報告格式與書上一模一樣。
 *
 * mode（可疊加，用加總）：
 *   1  越界寫（out-of-bounds）      → BUG ... Redzone overwritten
 *   2  釋放後使用（use-after-free） → BUG ... Poison overwritten
 *   4  重複釋放（double free）      → BUG ... Object already free
 *   8  改壞 free pointer            → BUG ... Freepointer corrupt
 *   16 記憶體洩漏（leak）           → 不會有 BUG，只能從 objects 計數看出來
 *
 * 用法：
 *   sudo insmod slub_lab.ko mode=1 ; sudo rmmod slub_lab
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 *   cat /sys/kernel/slab/slub_lab/{red_zone,poison,store_user,sanity_checks,objects}
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("slub_debug error taxonomy on a private cache (ch12 Q9)");

static int mode = 1;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "1=越界 2=UAF 4=double free 8=freepointer 16=leak（可相加）");

static int objsize = 32;
module_param(objsize, int, 0444);

static int overflow = 8;
module_param(overflow, int, 0444);
MODULE_PARM_DESC(overflow, "mode=1 時越界寫幾個位元組（8=只踩到右 Redzone；"
			   ">8 會連 SLAB_STORE_USER 的 track 一起踩壞）");

static int nodebug;
module_param(nodebug, int, 0444);
MODULE_PARM_DESC(nodebug, "1=不加 debug 旗標，對照組（錯誤會靜靜地被吃掉）");

static struct kmem_cache *cache;
static void *leaked;

static void banner(const char *what)
{
	pr_info("############ slub_lab: %s ############\n", what);
}

static int __init slub_lab_init(void)
{
	slab_flags_t flags = 0;
	void *p;

	if (!nodebug)
		flags = SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER |
			SLAB_CONSISTENCY_CHECKS;

	cache = kmem_cache_create("slub_lab", objsize, 0, flags, NULL);
	if (!cache)
		return -ENOMEM;

	pr_info("slub_lab: cache 建立完成 object_size=%u size=%u flags=%#x %s\n",
		kmem_cache_size(cache), objsize, (unsigned)flags,
		nodebug ? "(對照組：無 debug)" : "(= slub_debug=FPUZ)");

	if (mode & 1) {
		banner("(1) 越界寫 out-of-bounds");
		pr_info("slub_lab: 配 %d B，卻寫 %d B\n", objsize, objsize + overflow);
		p = kmem_cache_alloc(cache, GFP_KERNEL);
		memset(p, 0x55, objsize + overflow);	/* 越界！ */
		kmem_cache_free(cache, p);	/* 釋放時 check_object() 會抓到 */
	}

	if (mode & 2) {
		banner("(2) 釋放後使用 use-after-free：kfree 之後再 memset");
		p = kmem_cache_alloc(cache, GFP_KERNEL);
		kmem_cache_free(cache, p);
		memset(p, 0x55, objsize);	/* 改寫 POISON_FREE(0x6b) */
		/* 再配一次同一個物件，alloc 路徑的 check_object() 會抓到 */
		p = kmem_cache_alloc(cache, GFP_KERNEL);
		kmem_cache_free(cache, p);
	}

	if (mode & 4) {
		banner("(3) 重複釋放 double free");
		p = kmem_cache_alloc(cache, GFP_KERNEL);
		kmem_cache_free(cache, p);
		kmem_cache_free(cache, p);	/* 第二次！ */
	}

	if (mode & 8) {
		banner("(4) 改壞 free pointer（SLUB 把 next 指標存在物件裡）");
		p = kmem_cache_alloc(cache, GFP_KERNEL);
		kmem_cache_free(cache, p);
		*(unsigned long *)p = 0x4141414141414141UL;
		p = kmem_cache_alloc(cache, GFP_KERNEL);
		if (p)
			kmem_cache_free(cache, p);
	}

	if (mode & 16) {
		banner("(5) 記憶體洩漏：配了不放 —— slub_debug 抓不到，只能看計數");
		leaked = kmem_cache_alloc(cache, GFP_KERNEL);
		pr_info("slub_lab: 洩漏了一個物件 @ %px（rmmod 時故意不釋放）\n", leaked);
	}

	pr_info("slub_lab: init 完成，請看 /sys/kernel/slab/slub_lab/\n");
	return 0;
}

static void __exit slub_lab_exit(void)
{
	if (cache) {
		/* 有洩漏時 kmem_cache_destroy() 會警告 "Objects remaining" */
		kmem_cache_destroy(cache);
	}
	pr_info("slub_lab: exit\n");
}

module_init(slub_lab_init);
module_exit(slub_lab_exit);
