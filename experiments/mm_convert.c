// SPDX-License-Identifier: GPL-2.0
/*
 * mm_convert.ko —— 《奔跑吧 Linux 內核》卷1 第3章 第 3 題實驗模組
 * 平台：Radxa ROCK 5B (RK3588), Linux 6.1.115+ aarch64
 *
 * 把第 3 章第 3 題問到的 9 種轉換全部「跑」一遍，印出實際數值：
 *   mm_struct + vaddr -> VMA        VMA + vaddr -> page
 *   page + VMA -> vaddr             page -> 所有映射它的 VMA (rmap)
 *   page <-> PFN                    PFN  <-> paddr
 *   page <-> PTE                    zone <-> page
 *   zone <-> pg_data_t
 *
 * 用法：
 *   # 先讓一支使用者程式印出自己的某個 VA 與 PID，再餵給模組
 *   sudo insmod mm_convert.ko target_pid=<PID> target_va=0x<VA>
 *   # 不給參數時，改用 insmod 自己的 mm 與一個新配置的核心頁面
 *   sudo insmod mm_convert.ko
 *   sudo dmesg | sed 's/^\[[^]]*\] //'
 *   sudo rmmod mm_convert
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/sched/mm.h>
#include <linux/rmap.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <asm/memory.h>
#include <asm/pgtable.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Demonstrate mm/page/pfn/zone/pgdat conversions on RK3588");

static int target_pid;
static unsigned long target_va;
module_param(target_pid, int, 0444);
module_param(target_va, ulong, 0444);

#define P(fmt, ...) pr_info("mm_convert: " fmt, ##__VA_ARGS__)

static void show_page(struct page *page, const char *tag)
{
	unsigned long pfn   = page_to_pfn(page);
	phys_addr_t   paddr = PFN_PHYS(pfn);
	struct zone  *zone  = page_zone(page);
	pg_data_t    *pgdat = zone->zone_pgdat;

	P("---- %s ----\n", tag);
	P("  struct page *      = 0x%px\n", page);
	P("  [5] page -> PFN    : page_to_pfn()  = 0x%lx (%lu)\n", pfn, pfn);
	P("  [5] PFN  -> page   : pfn_to_page()  = 0x%px   %s\n",
	  pfn_to_page(pfn), pfn_to_page(pfn) == page ? "✔ 往返一致" : "✘");
	P("      推導：vmemmap = 0x%lx，sizeof(struct page)=%zu\n",
	  (unsigned long)VMEMMAP_START, sizeof(struct page));
	P("             vmemmap + pfn*%zu = 0x%lx  %s\n", sizeof(struct page),
	  (unsigned long)VMEMMAP_START + pfn * sizeof(struct page),
	  ((unsigned long)VMEMMAP_START + pfn * sizeof(struct page) == (unsigned long)page)
	  ? "✔ 與 page 相同（SPARSEMEM_VMEMMAP 線性排布）" : "(memstart 偏移造成差異)");
	P("  [6] PFN  -> paddr  : PFN_PHYS()     = 0x%llx   (pfn << PAGE_SHIFT)\n",
	  (u64)paddr);
	P("  [6] paddr-> PFN    : PHYS_PFN()     = 0x%lx    %s\n",
	  (unsigned long)PHYS_PFN(paddr), (unsigned long)PHYS_PFN(paddr) == pfn ? "✔" : "✘");
	P("      page_to_phys() = 0x%llx\n", (u64)page_to_phys(page));
	P("  線性映射虛擬位址   : page_to_virt() = 0x%px  __pa()=0x%llx\n",
	  page_to_virt(page), (u64)__pa(page_to_virt(page)));
	P("      virt_to_page(page_to_virt()) = 0x%px  %s\n",
	  virt_to_page(page_to_virt(page)),
	  virt_to_page(page_to_virt(page)) == page ? "✔ 往返一致" : "✘");
	P("  [8] zone <- page   : page_zone()    = \"%s\"  zone_start_pfn=0x%lx spanned=%lu\n",
	  zone->name, (unsigned long)zone->zone_start_pfn,
	  (unsigned long)zone->spanned_pages);
	P("      PFN 0x%lx 落在 [0x%lx, 0x%lx) 之內 %s\n", pfn,
	  (unsigned long)zone->zone_start_pfn,
	  (unsigned long)(zone->zone_start_pfn + zone->spanned_pages),
	  (pfn >= zone->zone_start_pfn &&
	   pfn < zone->zone_start_pfn + zone->spanned_pages) ? "✔" : "✘");
	P("      page->flags 中編碼的 zone id = %u\n", page_zonenum(page));
	P("  [9] pg_data <- zone: zone->zone_pgdat = 0x%px  node_id=%d\n",
	  pgdat, pgdat->node_id);
	P("      pgdat->node_start_pfn=0x%lx  node_spanned_pages=%lu  nr_zones=%d\n",
	  (unsigned long)pgdat->node_start_pfn,
	  (unsigned long)pgdat->node_spanned_pages, pgdat->nr_zones);
	P("      pg_data -> zone[]：\n");
	{
		int i;
		for (i = 0; i < MAX_NR_ZONES; i++) {
			struct zone *z = &pgdat->node_zones[i];

			P("        node_zones[%d] \"%-7s\" present=%-9lu managed=%-9lu start_pfn=0x%lx\n",
			  i, z->name, (unsigned long)z->present_pages,
			  (unsigned long)zone_managed_pages(z),
			  (unsigned long)z->zone_start_pfn);
		}
	}
	P("  page 狀態：_refcount=%d  _mapcount=%d  PageAnon=%d PageLRU=%d PageSlab=%d\n",
	  page_ref_count(page), page_mapcount(page),
	  PageAnon(page), PageLRU(page), PageSlab(page));
}

static int __init mm_convert_init(void)
{
	struct task_struct *tsk = current;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long va;
	struct page *page = NULL, *kpage;

	P("================ Ch3 Q3：記憶體管理資料結構的九種轉換 ================\n");
	P("PAGE_SIZE=%lu  PAGE_SHIFT=%d  sizeof(struct page)=%zu  VMEMMAP_START=0x%lx\n",
	  PAGE_SIZE, PAGE_SHIFT, sizeof(struct page), (unsigned long)VMEMMAP_START);

	/* ---------- 先用一個核心頁面示範 5~9 ---------- */
	kpage = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (kpage) {
		show_page(kpage, "A) 核心 alloc_page() 得到的頁面");
		__free_page(kpage);
	}

	/* ---------- 再用一個使用者位址示範 1~4, 7 ---------- */
	if (target_pid) {
		struct pid *p = find_get_pid(target_pid);

		tsk = p ? get_pid_task(p, PIDTYPE_PID) : NULL;
		if (p) put_pid(p);
		if (!tsk) { P("找不到 pid %d\n", target_pid); return 0; }
	}
	mm = get_task_mm(tsk);
	if (!mm) { P("目標行程沒有 mm\n"); goto out; }
	va = target_va ? target_va : mm->start_code;

	P("================ 使用者位址側：pid=%d comm=%s va=0x%lx ================\n",
	  task_pid_nr(tsk), tsk->comm, va);

	mmap_read_lock(mm);

	/* [1] mm_struct + vaddr -> VMA */
	vma = find_vma(mm, va);
	if (!vma || va < vma->vm_start) {
		P("  [1] find_vma(mm, 0x%lx) 沒找到涵蓋此位址的 VMA\n", va);
		goto unlock;
	}
	P("  [1] mm + vaddr -> VMA : find_vma(mm, 0x%lx)\n", va);
	P("      vma = 0x%px  範圍 [0x%lx, 0x%lx)  大小 %lu KB\n",
	  vma, vma->vm_start, vma->vm_end, (vma->vm_end - vma->vm_start) >> 10);
	P("      vm_flags=0x%lx (%c%c%c%c)  vm_pgoff=0x%lx  vm_file=%s\n",
	  vma->vm_flags,
	  (vma->vm_flags & VM_READ)  ? 'r' : '-',
	  (vma->vm_flags & VM_WRITE) ? 'w' : '-',
	  (vma->vm_flags & VM_EXEC)  ? 'x' : '-',
	  (vma->vm_flags & VM_SHARED) ? 's' : 'p',
	  vma->vm_pgoff,
	  vma->vm_file ? "有（檔案映射）" : "無（匿名映射）");
	P("      mm->pgd = 0x%px   __pa = 0x%llx  <- 會被寫進 TTBR0_EL1\n",
	  mm->pgd, (u64)__pa(mm->pgd));

	/* [2] VMA + vaddr -> page（走頁表） */
	{
		pgd_t *pgd; p4d_t *p4d; pud_t *pud; pmd_t *pmd; pte_t *pte;

		pgd = pgd_offset(mm, va);
		P("  [2] VMA + vaddr -> page（逐級走頁表）\n");
		P("      pgd_offset() = 0x%px  val=0x%llx\n", pgd, (u64)pgd_val(*pgd));
		if (pgd_none(*pgd)) goto unlock;
		p4d = p4d_offset(pgd, va);
		pud = pud_offset(p4d, va);
		P("      pud_offset() = 0x%px  val=0x%llx\n", pud, (u64)pud_val(*pud));
		if (pud_none(*pud)) goto unlock;
		pmd = pmd_offset(pud, va);
		P("      pmd_offset() = 0x%px  val=0x%llx\n", pmd, (u64)pmd_val(*pmd));
		if (pmd_none(*pmd)) goto unlock;
		pte = pte_offset_map(pmd, va);
		P("      pte_offset_map() = 0x%px  val=0x%llx\n", pte, (u64)pte_val(*pte));
		if (pte_present(*pte)) {
			/* [7] page <-> PTE */
			page = pte_page(*pte);
			P("  [7] PTE -> page : pte_page()  = 0x%px   pte_pfn()=0x%lx\n",
			  page, (unsigned long)pte_pfn(*pte));
			P("  [7] page -> PTE : mk_pte(page, prot) 會用 page_to_pfn(0x%lx) 組回去\n",
			  page_to_pfn(page));
			get_page(page);
		} else {
			P("      PTE 不存在（頁面還沒被觸碰或已被換出）\n");
		}
		pte_unmap(pte);
	}

	/* [3] page + VMA -> vaddr */
	if (page) {
		unsigned long back = vma->vm_start +
			((page->index - vma->vm_pgoff) << PAGE_SHIFT);

		P("  [3] page + VMA -> vaddr : vm_start + ((page->index - vm_pgoff) << PAGE_SHIFT)\n");
		P("      = 0x%lx + ((0x%lx - 0x%lx) << 12) = 0x%lx   (原始 va=0x%lx)\n",
		  vma->vm_start, page->index, vma->vm_pgoff, back, va & PAGE_MASK);
		P("      核心的統一介面：vma_address() / page_address_in_vma()\n");
	}

	mmap_read_unlock(mm);

	/* [4] page -> 所有映射它的 VMA（反向映射 rmap） */
	if (page) {
		void *av = PageAnon(page) ?
			(void *)((unsigned long)page->mapping & ~PAGE_MAPPING_FLAGS) : NULL;

		P("  [4] page -> 所有映射它的 VMA（反向映射 rmap）\n");
		P("      page->mapping = 0x%px   PageAnon=%d  PageKsm=%d\n",
		  page->mapping, PageAnon(page), PageKsm(page));
		if (av) {
			P("      匿名頁：page->mapping 低位元帶 PAGE_MAPPING_ANON 標記，\n");
			P("              去掉標記後指向 anon_vma = 0x%px\n", av);
			P("              走訪 av->rb_root 上的 anon_vma_chain 就能找到所有 VMA\n");
		} else if (page->mapping) {
			struct address_space *as = page_mapping(page);

			P("      檔案頁：page->mapping 指向 address_space = 0x%px\n", as);
			P("              page->index = 0x%lx（在檔案中的頁偏移）\n", page->index);
			P("              走訪 as->i_mmap 這棵區間樹就能找到所有映射該偏移的 VMA\n");
		}
		P("      _mapcount = %d  =>  共有 %d 個 PTE 映射到這個頁面\n",
		  page_mapcount(page) - 1, page_mapcount(page));
		P("      （核心的統一入口是 rmap_walk()，見 mm/rmap.c；它沒有 EXPORT_SYMBOL，\n");
		P("        所以這裡只印出它會用到的兩個起點欄位）\n");
		show_page(page, "B) 使用者頁面");
		put_page(page);
	}
	mmput(mm);
	goto out;

unlock:
	mmap_read_unlock(mm);
	mmput(mm);
out:
	if (target_pid && tsk)
		put_task_struct(tsk);
	P("================ done ================\n");
	return 0;
}

static void __exit mm_convert_exit(void) { P("unloaded\n"); }

module_init(mm_convert_init);
module_exit(mm_convert_exit);
