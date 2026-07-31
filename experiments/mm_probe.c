// SPDX-License-Identifier: GPL-2.0
/*
 * mm_probe.ko —— 《奔跑吧 Linux 內核》卷1 第4章 / 第5章 實驗模組
 * 平台：Radxa ROCK 5B (RK3588), Linux 6.1.115+ aarch64
 *
 * 涵蓋題目：
 *   Ch4 Q1  伙伴系統分配        Ch4 Q2  gfp_mask -> zone
 *   Ch4 Q3  zonelist 掃描方向    Ch4 Q6  zone_watermark_ok
 *   Ch4 Q7  空閒頁面合併         Ch4 Q13/14/16/17 slab(SLUB) 佈局與並發
 *   Ch4 Q18 kmalloc vs vmalloc   Ch4 Q27 vm_normal_page / ZERO_PAGE
 *   Ch5 Q1/Q5 _refcount/_mapcount  Ch5 Q4  page->flags 佈局
 *   Ch5 Q6  page->mapping        Ch5 Q27 LRU / 非 LRU 頁面
 *   Ch5 Q38/44/45/46 伙伴系統與遷移類型
 *   Ch5 Q47 外碎片化指標
 *
 * 用法：
 *   sudo insmod mm_probe.ko
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 *   sudo rmmod mm_probe
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/page-flags.h>
#include <linux/pageblock-flags.h>
#include <linux/mm_inline.h>
#include <linux/sched/mm.h>
#include <asm/memory.h>
#include <asm/pgtable.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Buddy / SLUB / page-flags prober for RK3588 kernel study notes");

#define P(fmt, ...) pr_info("mm_probe: " fmt, ##__VA_ARGS__)

/*
 * for_each_populated_zone() 會用到未匯出的 first_online_pgdat()/next_zone()，
 * 這裡自己走 pgdat->node_zones[]（本機 UMA，只有一個 node）。
 */
static pg_data_t *g_pgdat;
#define for_each_probe_zone(z)						\
	for ((z) = g_pgdat->node_zones;					\
	     (z) < g_pgdat->node_zones + MAX_NR_ZONES; (z)++)	\
		if (!populated_zone(z)) continue; else

/* gfp_migratetype() 會參考未匯出的 page_group_by_mobility_disabled */
static int my_gfp_mt(gfp_t g)
{
	if (g & __GFP_MOVABLE)     return MIGRATE_MOVABLE;
	if (g & __GFP_RECLAIMABLE) return MIGRATE_RECLAIMABLE;
	return MIGRATE_UNMOVABLE;
}

static const char * const mt_names[MIGRATE_TYPES] = {
	[MIGRATE_UNMOVABLE]   = "Unmovable",
	[MIGRATE_MOVABLE]     = "Movable",
	[MIGRATE_RECLAIMABLE] = "Reclaimable",
	[MIGRATE_HIGHATOMIC]  = "HighAtomic",
#ifdef CONFIG_CMA
	[MIGRATE_CMA]         = "CMA",
#endif
#ifdef CONFIG_MEMORY_ISOLATION
	[MIGRATE_ISOLATE]     = "Isolate",
#endif
};

/* ================================================================== */
/* [1] gfp_mask -> zone   (Ch4 Q2)                                     */
/* ================================================================== */
static void probe_gfp_zone(void)
{
	struct { const char *name; gfp_t g; } tbl[] = {
		{ "GFP_KERNEL",             GFP_KERNEL },
		{ "GFP_ATOMIC",             GFP_ATOMIC },
		{ "GFP_NOWAIT",             GFP_NOWAIT },
		{ "GFP_NOIO",               GFP_NOIO },
		{ "GFP_NOFS",               GFP_NOFS },
		{ "GFP_USER",               GFP_USER },
		{ "GFP_HIGHUSER",           GFP_HIGHUSER },
		{ "GFP_HIGHUSER_MOVABLE",   GFP_HIGHUSER_MOVABLE },
		{ "GFP_DMA",                GFP_DMA },
		{ "GFP_DMA32",              GFP_DMA32 },
		{ "GFP_KERNEL|__GFP_MOVABLE", GFP_KERNEL | __GFP_MOVABLE },
	};
	static const char * const zn[] = { "ZONE_DMA", "ZONE_DMA32",
					   "ZONE_NORMAL", "ZONE_MOVABLE" };
	int i;

	P("=============== [1] Ch4 Q2: gfp_mask -> 最高允許 zone ===============\n");
	P("  GFP_ZONE_TABLE 解出來的 gfp_zone()：\n");
	P("  %-28s %-12s %-12s  遷移類型\n", "gfp_mask", "gfp_zone()", "zone 名稱");
	for (i = 0; i < ARRAY_SIZE(tbl); i++) {
		enum zone_type z = gfp_zone(tbl[i].g);
		int mt = my_gfp_mt(tbl[i].g);

		P("  %-28s %-12d %-12s  %s\n", tbl[i].name, z,
		  z < ARRAY_SIZE(zn) ? zn[z] : "?",
		  (mt < MIGRATE_TYPES && mt_names[mt]) ? mt_names[mt] : "?");
	}
	P("  註：gfp_zone() 給的是【上限】，實際分配會從這個 zone 開始沿 zonelist 往下回退\n");
	P("  註：本機 ZONE_DMA32 與 ZONE_MOVABLE 都是空的，所以 GFP_DMA32 實際會落到 ZONE_DMA\n");
}

/* ================================================================== */
/* [2] zonelist 掃描方向  (Ch4 Q3, Ch5 Q13)                            */
/* ================================================================== */
static void probe_zonelist(struct page *pg)
{
	pg_data_t *pgdat = page_zone(pg)->zone_pgdat;
	struct zonelist *zl = &pgdat->node_zonelists[ZONELIST_FALLBACK];
	struct zoneref *z;
	int i = 0;

	P("=============== [2] Ch4 Q3: zonelist 的掃描順序 ===============\n");
	P("  node %d 的 ZONELIST_FALLBACK（回退列表）：\n", pgdat->node_id);
	for (z = zl->_zonerefs; z->zone; z++) {
		P("   [%d] zone_idx=%d  \"%-7s\"  managed=%-9lu  free=%lu\n",
		  i++, zonelist_zone_idx(z), zonelist_zone(z)->name,
		  (unsigned long)zone_managed_pages(zonelist_zone(z)),
		  (unsigned long)zone_page_state(zonelist_zone(z), NR_FREE_PAGES));
	}
	P("  -> 從 zone_idx 大的排到小的：先用高端 zone，把稀缺的低端 zone 留到最後\n");
	P("     這樣普通分配不會把只有低端 zone 能滿足的 DMA 需求擠光\n");
}

/* ================================================================== */
/* [3] 伙伴系統 free_area  (Ch4 Q1/Q7, Ch5 Q38/44/45/46/47)             */
/* ================================================================== */
static void dump_free_area(const char *tag)
{
	struct zone *zone;
	int order, mt;

	P("---- 伙伴系統 free_area (%s) ----\n", tag);
	for_each_probe_zone(zone) {
		unsigned long total = 0;

		P("  zone \"%s\"  free=%lu 頁\n", zone->name,
		  (unsigned long)zone_page_state(zone, NR_FREE_PAGES));
		P("     order:  %8s", "");
		for (order = 0; order < MAX_ORDER; order++)
			pr_cont("%6d", order);
		pr_cont("\n");
		for (mt = 0; mt < MIGRATE_TYPES; mt++) {
			bool any = false;

			for (order = 0; order < MAX_ORDER; order++)
				if (zone->free_area[order].nr_free) { any = true; break; }
			if (!any && mt) continue;
			P("     %-12s", mt_names[mt] ? mt_names[mt] : "?");
			for (order = 0; order < MAX_ORDER; order++) {
				unsigned long n = 0;
				struct list_head *l;

				list_for_each(l, &zone->free_area[order].free_list[mt])
					n++;
				pr_cont("%6lu", n);
				total += n << order;
			}
			pr_cont("\n");
		}
		P("     各階空閒塊加總 = %lu 頁\n", total);
	}
}

static void probe_buddy(void)
{
	struct page *p8;
	struct zone *z;

	P("=============== [3] Ch4 Q1/Q7: 伙伴系統的分裂與合併 ===============\n");
	P("  MAX_ORDER=%d  -> 最大一塊 = 2^%d 頁 = %lu KB\n",
	  MAX_ORDER, MAX_ORDER - 1, (PAGE_SIZE << (MAX_ORDER - 1)) >> 10);
	P("  pageblock_order=%d -> 一個 pageblock = %lu 頁 = %lu MB (Ch5 Q45)\n",
	  pageblock_order, (unsigned long)pageblock_nr_pages,
	  ((unsigned long)pageblock_nr_pages << PAGE_SHIFT) >> 20);

	dump_free_area("配置前");

	/* 配一塊 order-8 (1MB)，觀察它從哪一階被切出來 */
	p8 = alloc_pages(GFP_KERNEL | __GFP_NOWARN, 8);
	if (p8) {
		z = page_zone(p8);
		P("  ---- 剛配了一塊 order-8 (%lu KB) ----\n",
		  (PAGE_SIZE << 8) >> 10);
		P("     PFN = 0x%lx  PA = 0x%llx  zone=\"%s\"\n",
		  page_to_pfn(p8), (u64)page_to_phys(p8), z->name);
		P("     PFN 是否 order-8 對齊？ 0x%lx %% 256 = %lu  %s\n",
		  page_to_pfn(p8), page_to_pfn(p8) % 256,
		  (page_to_pfn(p8) % 256) ? "否(不該發生)" : "是 ✔");
		P("     伙伴 PFN = pfn XOR (1<<8) = 0x%lx  (Ch4 Q7 的異或找伙伴)\n",
		  page_to_pfn(p8) ^ (1UL << 8));
		P("     （pageblock migratetype 請看 sudo cat /proc/pagetypeinfo）\n");
		dump_free_area("配置後");
		__free_pages(p8, 8);
		P("  ---- 已釋放，觸發 __free_one_page() 逐級合併 ----\n");
		dump_free_area("釋放後");
	} else {
		P("  配置 order-8 失敗（記憶體碎片化嚴重）\n");
	}
}

/* ================================================================== */
/* [4] zone watermark  (Ch4 Q6)                                        */
/* ================================================================== */
static void probe_watermark(void)
{
	struct zone *zone;
	int order;

	P("=============== [4] Ch4 Q6: zone 水位與是否滿足分配 ===============\n");
	for_each_probe_zone(zone) {
		unsigned long free = zone_page_state(zone, NR_FREE_PAGES);

		P("  zone \"%s\"  free=%lu\n", zone->name, free);
		P("     _watermark[MIN] =%-8lu  +boost=%-8lu -> 顯示值 %lu\n",
		  (unsigned long)zone->_watermark[WMARK_MIN],
		  (unsigned long)zone->watermark_boost,
		  (unsigned long)min_wmark_pages(zone));
		P("     _watermark[LOW] =%-8lu               -> 顯示值 %lu\n",
		  (unsigned long)zone->_watermark[WMARK_LOW],
		  (unsigned long)low_wmark_pages(zone));
		P("     _watermark[HIGH]=%-8lu               -> 顯示值 %lu\n",
		  (unsigned long)zone->_watermark[WMARK_HIGH],
		  (unsigned long)high_wmark_pages(zone));
		P("     lowmem_reserve[] = %lu %lu %lu %lu\n",
		  zone->lowmem_reserve[0], zone->lowmem_reserve[1],
		  zone->lowmem_reserve[2], zone->lowmem_reserve[3]);
		P("     nr_reserved_highatomic = %lu 頁 (Ch5 Q43)\n",
		  (unsigned long)zone->nr_reserved_highatomic);
		/* Ch4 Q6：高階分配不只看總量，還要該階真的有塊 */
		P("     各階是否有空閒塊：");
		for (order = 0; order < MAX_ORDER; order++)
			pr_cont("o%d=%lu ", order,
				(unsigned long)zone->free_area[order].nr_free);
		pr_cont("\n");
	}
	P("  -> __zone_watermark_ok() 的兩個條件：\n");
	P("     (a) free - (1<<order) - lowmem_reserve[highest_zoneidx] >= mark\n");
	P("     (b) 對 order>0，還要 free_area[o>=order] 真的有可用塊（否則只是外碎片）\n");
}

/* ================================================================== */
/* [5] kmalloc vs vmalloc 的實體連續性  (Ch4 Q18)                       */
/* ================================================================== */
static void probe_kmalloc_vmalloc(void)
{
	const size_t SZ = 64 * 1024;      /* 64KB：kmalloc 還配得到，vmalloc 也夠大 */
	void *k, *v;
	int i, discont = 0;

	P("=============== [5] Ch4 Q18: kmalloc vs vmalloc 的實體連續性 ===============\n");

	k = kmalloc(SZ, GFP_KERNEL);
	if (k) {
		phys_addr_t base = virt_to_phys(k);

		P("  kmalloc(%zu)  VA=0x%px\n", SZ, k);
		P("     在線性映射區嗎？ %s\n",
		  ((unsigned long)k >= PAGE_OFFSET &&
		   (unsigned long)k < (unsigned long)PAGE_END) ? "是 ✔" : "否");
		P("     實體位址逐頁檢查：\n");
		for (i = 0; i < SZ / PAGE_SIZE; i++) {
			phys_addr_t pa = virt_to_phys((char *)k + i * PAGE_SIZE);

			if (pa != base + i * PAGE_SIZE) discont++;
			if (i < 4 || i == SZ / PAGE_SIZE - 1)
				P("        page[%2d] VA=0x%px  PA=0x%llx\n",
				  i, (char *)k + i * PAGE_SIZE, (u64)pa);
		}
		P("     不連續的頁數 = %d  -> %s\n", discont,
		  discont ? "不連續（不該發生）" : "★ 實體完全連續 ✔");
		kfree(k);
	}

	v = vmalloc(SZ);
	if (v) {
		phys_addr_t base;
		struct page *pg0 = vmalloc_to_page(v);

		base = page_to_phys(pg0);
		discont = 0;
		P("  vmalloc(%zu)  VA=0x%px\n", SZ, v);
		P("     在 vmalloc 區嗎？ %s (VMALLOC_START=0x%lx)\n",
		  ((unsigned long)v >= VMALLOC_START &&
		   (unsigned long)v < VMALLOC_END) ? "是 ✔" : "否",
		  (unsigned long)VMALLOC_START);
		P("     __pa() 對它有效嗎？ __pa(v)=0x%llx  <- 不可信，vmalloc 區不是線性映射\n",
		  (u64)__pa(v));
		P("     必須用 vmalloc_to_page() 逐頁查頁表：\n");
		for (i = 0; i < SZ / PAGE_SIZE; i++) {
			struct page *pg = vmalloc_to_page((char *)v + i * PAGE_SIZE);
			phys_addr_t pa = page_to_phys(pg);

			if (pa != base + i * PAGE_SIZE) discont++;
			if (i < 4 || i == SZ / PAGE_SIZE - 1)
				P("        page[%2d] VA=0x%px  PA=0x%llx  PFN=0x%lx\n",
				  i, (char *)v + i * PAGE_SIZE, (u64)pa, page_to_pfn(pg));
		}
		P("     不連續的頁數 = %d / %zu  -> %s\n", discont, SZ / PAGE_SIZE,
		  discont ? "★ 實體不連續，只有虛擬位址連續 ✔" : "剛好連續（偶發）");
		vfree(v);
	}
}

/* ================================================================== */
/* [6] SLUB 內部  (Ch4 Q13/14/15/16/17)                                */
/* ================================================================== */
static void probe_slub(void)
{
	struct kmem_cache *c;
	void *o1, *o2;
	size_t sizes[] = { 8, 16, 24, 32, 65, 100, 192, 200, 1000, 4000, 8000 };
	int i;

	P("=============== [6] Ch4 Q8-Q17: SLUB 分配器 ===============\n");
	P("  本機 CONFIG_SLUB=y（不是經典 SLAB），所以：\n");
	P("   - 沒有 cache colouring（Ch4 Q10 是 SLAB 專有概念）\n");
	P("   - 沒有 正常/OFF_SLAB/OBJFREELIST_SLAB 三種佈局（Ch4 Q14 是 SLAB 專有概念）\n");
	P("   - SLUB 的 freelist 是「把下一個空閒物件的指標寫進物件自己裡面」\n");
	P("   - struct kmem_cache 對模組不可見，內部欄位請看 /sys/kernel/slab/<name>/\n");

	c = kmem_cache_create("mm_probe_test", 100, 0, 0, NULL);
	if (!c) { P("  建立測試 cache 失敗\n"); goto ksize_test; }

	o1 = kmem_cache_alloc(c, GFP_KERNEL);
	o2 = kmem_cache_alloc(c, GFP_KERNEL);
	if (o1 && o2) {
		P("  kmem_cache_create(\"mm_probe_test\", object_size=100) 連配兩個物件：\n");
		P("     obj1 = 0x%px   ksize=%zu\n", o1, ksize(o1));
		P("     obj2 = 0x%px   兩者相距 %ld bytes  <- 這就是 SLUB 的 c->size\n",
		  o2, (long)abs((long)((char *)o1 - (char *)o2)));
		P("     同一個 slab 頁嗎？ %s   PFN=0x%lx PageSlab=%d\n",
		  (virt_to_page(o1) == virt_to_page(o2)) ? "是 ✔" : "否",
		  page_to_pfn(virt_to_page(o1)), PageSlab(virt_to_page(o1)));
		P("     slab 頁的 _refcount=%d  在 LRU 上嗎=%d  (Ch5 Q27：slab 是非 LRU 頁面)\n",
		  page_ref_count(virt_to_page(o1)), PageLRU(virt_to_page(o1)));
		kmem_cache_free(c, o1);
		kmem_cache_free(c, o2);
	}
	kmem_cache_destroy(c);

ksize_test:
	P("  Ch4 Q8：kmalloc 的尺寸階梯（比純 2^n 精細，內部碎片更小）：\n");
	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		void *m = kmalloc(sizes[i], GFP_KERNEL);

		if (m) {
			P("     kmalloc(%5zu) -> ksize()=%-6zu  內部碎片 %4zu bytes (%2zu%%)  slab頁PFN=0x%lx\n",
			  sizes[i], ksize(m), ksize(m) - sizes[i],
			  (ksize(m) - sizes[i]) * 100 / ksize(m),
			  page_to_pfn(virt_to_page(m)));
			kfree(m);
		}
	}
	P("     ^ 96/192 這種非 2^n 的尺寸就是 SLUB 為了降低內部碎片特別加的\n");
	P("  Ch4 Q15：slab 的實體頁是【惰性】配置的——只有現有 slab 都滿了才向\n");
	P("           伙伴系統要新的 2^order 頁（allocate_slab() -> alloc_slab_page()）\n");
}

/* ================================================================== */
/* [7] page->flags 佈局  (Ch5 Q4)                                      */
/* ================================================================== */
static void probe_page_flags(struct page *pg)
{
	P("=============== [7] Ch5 Q4: page->flags 佈局 ===============\n");
	P("  BITS_PER_LONG = %d,  NR_PAGEFLAGS = %d\n", BITS_PER_LONG, __NR_PAGEFLAGS);
	P("  高位欄位（從最高位往下排）：\n");
	P("     SECTIONS_WIDTH    = %-3d  SECTIONS_PGSHIFT    = %d\n",
	  (int)SECTIONS_WIDTH, (int)SECTIONS_PGSHIFT);
	P("     NODES_WIDTH       = %-3d  NODES_PGSHIFT       = %d\n",
	  (int)NODES_WIDTH, (int)NODES_PGSHIFT);
	P("     ZONES_WIDTH       = %-3d  ZONES_PGSHIFT       = %d\n",
	  (int)ZONES_WIDTH, (int)ZONES_PGSHIFT);
	P("     LAST_CPUPID_WIDTH = %-3d  LAST_CPUPID_PGSHIFT = %d\n",
	  (int)LAST_CPUPID_WIDTH, (int)LAST_CPUPID_PGSHIFT);
	P("     KASAN_TAG_WIDTH   = %-3d\n", (int)KASAN_TAG_WIDTH);
	P("  低位 0..%d 是各種 PG_* 旗標\n", __NR_PAGEFLAGS - 1);
	P("  範例 page (PFN 0x%lx)：flags = 0x%016lx\n",
	  page_to_pfn(pg), pg->flags);
	P("     zone id  = %u  (從 flags 位 %d 取 %d bits)\n",
	  (unsigned int)page_zonenum(pg), (int)ZONES_PGSHIFT, (int)ZONES_WIDTH);
	P("     node id  = %d\n", page_to_nid(pg));
	P("     PG_locked=%d PG_referenced=%d PG_uptodate=%d PG_dirty=%d\n",
	  PageLocked(pg), PageReferenced(pg), PageUptodate(pg), PageDirty(pg));
	P("     PG_lru=%d PG_active=%d PG_slab=%d PG_reserved=%d\n",
	  PageLRU(pg), PageActive(pg), PageSlab(pg), PageReserved(pg));
	P("     PG_swapbacked=%d PG_unevictable=%d PG_writeback=%d PG_reclaim=%d\n",
	  PageSwapBacked(pg), PageUnevictable(pg), PageWriteback(pg), PageReclaim(pg));
}

/* ================================================================== */
/* [8] _refcount / _mapcount / mapping  (Ch5 Q1/Q5/Q6/Q27)             */
/* ================================================================== */
static void show_counts(struct page *pg, const char *tag)
{
	P("  %-34s _refcount=%-3d _mapcount=%-3d PageAnon=%d PageLRU=%d PageSlab=%d mapping=0x%px\n",
	  tag, page_ref_count(pg), page_mapcount(pg),
	  PageAnon(pg), PageLRU(pg), PageSlab(pg), pg->mapping);
}

static void probe_refcount(void)
{
	struct page *pg;
	void *k;

	P("=============== [8] Ch5 Q1/Q5/Q6: _refcount vs _mapcount ===============\n");
	P("  _refcount：核心對這個 page 的【引用】次數（誰在用它，不讓它被釋放）\n");
	P("  _mapcount：這個 page 被幾個【使用者態 PTE】映射（-1 表示沒有任何映射）\n");
	P("             注意 page_mapcount() 回傳的是 _mapcount+1\n");

	pg = alloc_page(GFP_KERNEL);
	if (pg) {
		show_counts(pg, "alloc_page() 剛配出來");
		get_page(pg);
		show_counts(pg, "get_page() 之後");
		put_page(pg);
		show_counts(pg, "put_page() 之後");
		__free_page(pg);
	}

	k = kmalloc(64, GFP_KERNEL);
	if (k) {
		show_counts(virt_to_page(k), "kmalloc 物件所在的 slab 頁");
		P("      ^ PageSlab=1、不在 LRU 上 -> Ch5 Q27 的「非 LRU 頁面」\n");
		kfree(k);
	}

	/* ZERO_PAGE：Ch4 Q27 */
	pg = ZERO_PAGE(0);
	show_counts(pg, "ZERO_PAGE（唯讀零頁）");
	P("      ^ PageReserved=%d，vm_normal_page() 會對它回傳 NULL (Ch4 Q27)\n",
	  PageReserved(pg));
	P("      ZERO_PAGE PFN = 0x%lx  PA = 0x%llx\n",
	  page_to_pfn(pg), (u64)page_to_phys(pg));
}

/* ================================================================== */
/* [9] 外碎片化指標  (Ch5 Q47/Q48)                                     */
/* ================================================================== */
static void probe_fragmentation(void)
{
	struct zone *zone;
	int order;

	P("=============== [9] Ch5 Q47: 外碎片化指標 ===============\n");
	P("  fragmentation index = (1 - free_blocks_suitable/free_blocks_total) 的變形\n");
	P("  直觀版本：每一階「若把所有更高階拆光，總共能湊出幾塊」\n");
	for_each_probe_zone(zone) {
		unsigned long free_pages = zone_page_state(zone, NR_FREE_PAGES);

		P("  zone \"%s\"  free=%lu 頁\n", zone->name, free_pages);
		P("     order  nr_free   該階可湊出的塊數(含拆分高階)  能否直接滿足\n");
		for (order = 0; order < MAX_ORDER; order++) {
			unsigned long o, suitable = 0, direct;

			direct = zone->free_area[order].nr_free;
			for (o = order; o < MAX_ORDER; o++)
				suitable += zone->free_area[o].nr_free << (o - order);
			P("     %-6d %-9lu %-30lu %s\n", order, direct, suitable,
			  direct ? "可以" : (suitable ? "需拆分高階" : "★不行(外碎片)"));
		}
	}
	P("  -> 「總空閒頁很多、但某一階完全湊不出」就是外碎片化\n");
	P("     核心用 fragmentation_index()（mm/vmstat.c）量化，\n");
	P("     超過 extfrag_threshold(預設500) 就傾向做 compaction 而不是直接回收\n");
}

/* ================================================================== */
static int __init mm_probe_init(void)
{
	struct page *pg = alloc_page(GFP_KERNEL);

	if (!pg) { P("alloc_page failed\n"); return -ENOMEM; }
	g_pgdat = page_zone(pg)->zone_pgdat;

	probe_gfp_zone();
	if (pg) probe_zonelist(pg);
	probe_watermark();
	probe_buddy();
	probe_kmalloc_vmalloc();
	probe_slub();
	if (pg) probe_page_flags(pg);
	probe_refcount();
	probe_fragmentation();
	if (pg) __free_page(pg);
	P("=============== done ===============\n");
	return 0;
}

static void __exit mm_probe_exit(void) { P("unloaded\n"); }

module_init(mm_probe_init);
module_exit(mm_probe_exit);
