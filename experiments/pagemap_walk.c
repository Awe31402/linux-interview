/* Ch2 Q2/Q22/Q23, Ch3 Q3: 用 /proc/self/pagemap 把使用者 VA 翻成 PFN/PA，
 * 並印出 4 級頁表的索引拆解，可與 armv8_dump.ko 的核心側巡覽互相對照。
 * 需要 root（Linux 4.0 之後非 root 讀 pagemap 會被遮成 0）。
 *   sudo ./pagemap_walk
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE_ (1UL << PAGE_SHIFT)

static int pmfd = -1;

/* pagemap 每個 PFN 佔 8 bytes；bit63=present, bit62=swapped, bits[54:0]=PFN */
static int va2pfn(unsigned long va, unsigned long *pfn, int *present, int *swapped)
{
	uint64_t e;
	off_t off = (va / PAGE_SIZE_) * sizeof(uint64_t);

	if (pread(pmfd, &e, sizeof e, off) != sizeof e)
		return -1;
	*present = !!(e >> 63);
	*swapped = !!((e >> 62) & 1);
	*pfn     = e & ((1ULL << 55) - 1);
	return 0;
}

static void show(const char *tag, unsigned long va)
{
	unsigned long pfn; int present, swapped;

	if (va2pfn(va, &pfn, &present, &swapped)) { printf("%-28s read fail\n", tag); return; }
	printf("%-28s VA=0x%016lx  present=%d  PFN=0x%-9lx  PA=0x%011lx\n",
	       tag, va, present, pfn, (pfn << PAGE_SHIFT) | (va & (PAGE_SIZE_ - 1)));
	printf("%-28s  VA[63]=%lu(%s)  L0=%3lu L1=%3lu L2=%3lu L3=%3lu off=0x%03lx\n", "",
	       (va >> 63) & 1, ((va >> 63) & 1) ? "TTBR1" : "TTBR0",
	       (va >> 39) & 0x1ff, (va >> 30) & 0x1ff, (va >> 21) & 0x1ff,
	       (va >> 12) & 0x1ff, va & 0xfff);
}

int main(void)
{
	static int bss_var;
	static int data_var = 7;
	char *heap = malloc(4096);
	char *anon = mmap(NULL, 2 * 1024 * 1024, PROT_READ | PROT_WRITE,
			  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	char stack_var = 1;

	pmfd = open("/proc/self/pagemap", O_RDONLY);
	if (pmfd < 0) { perror("open pagemap (需要 root)"); return 1; }

	*heap = 1; anon[0] = 1; anon[1024 * 1024] = 1; bss_var = 1;

	puts("=== 使用者空間各區段的 VA -> PFN -> PA（透過 /proc/self/pagemap）===");
	printf("PAGE_SIZE = %lu, 48-bit VA, 4 級頁表\n\n", PAGE_SIZE_);
	show(".text (main)",        (unsigned long)main);
	show(".data (data_var)",    (unsigned long)&data_var);
	show(".bss  (bss_var)",     (unsigned long)&bss_var);
	show("heap  (malloc)",      (unsigned long)heap);
	show("stack (stack_var)",   (unsigned long)&stack_var);
	show("mmap anon +0",        (unsigned long)anon);
	show("mmap anon +1MB",      (unsigned long)(anon + 1024 * 1024));

	puts("\n=== 同一實體頁被兩個 VA 映射（MAP_SHARED）—— PA 應相同 ===");
	{
		char *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		a[0] = 42;
		show("shared map A", (unsigned long)a);
	}

	puts("\n=== 未觸碰的 mmap：present=0，沒有實體頁（demand paging）===");
	{
		char *lazy = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
				  MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		show("mmap 未觸碰", (unsigned long)lazy);
		lazy[0] = 1;
		show("mmap 觸碰後", (unsigned long)lazy);
	}

	printf("\n提示：把上面任一 VA 傳給核心模組做交叉驗證：\n");
	printf("  sudo insmod armv8_dump.ko walk_va=0x%lx\n", (unsigned long)main);
	return 0;
}
