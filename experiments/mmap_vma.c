/* Ch4 Q21/Q31/Q32/Q33 —— VMA、brk、mmap 的行為實測
 *
 *   Q21  內核如何保證位址不衝突（含 VMA 合併）
 *   Q31  使用者空間劃分 / brk 區的起止
 *   Q32  私有映射 vs 共享映射（四種組合）
 *   Q33  MAP_FIXED 靜默覆蓋 vs MAP_FIXED_NOREPLACE 報錯
 *
 *   gcc -O2 -o mmap_vma mmap_vma.c && ./mmap_vma
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define PGSZ 4096

/* 印出涵蓋 [lo,hi) 的所有 maps 行 */
static void show_maps(const char *tag, unsigned long lo, unsigned long hi)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512];
	int n = 0;

	printf("    %s\n", tag);
	while (f && fgets(line, sizeof line, f)) {
		unsigned long a, b;
		if (sscanf(line, "%lx-%lx", &a, &b) != 2) continue;
		if (b <= lo || a >= hi) continue;
		printf("      %s", line);
		n++;
	}
	if (f) fclose(f);
	if (!n) printf("      （這段位址目前沒有任何 VMA）\n");
}

static int count_vma(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char line[512]; int n = 0;
	while (f && fgets(line, sizeof line, f)) n++;
	if (f) fclose(f);
	return n;
}

int main(void)
{
	void *hint = (void *)0x0000700000000000UL;   /* 找一段幾乎不會被用到的位址 */
	void *a, *b, *c;
	int fd;

	printf("=========== Ch4 Q21/Q31/Q32/Q33：VMA / brk / mmap ===========\n\n");

	/* ================= Q31：使用者空間劃分與 brk ================= */
	printf("--- [Q31] 使用者空間劃分 ---\n");
	{
		FILE *f = fopen("/proc/self/maps", "r");
		char line[512];
		while (f && fgets(line, sizeof line, f))
			if (strstr(line, "[heap]") || strstr(line, "[stack]") ||
			    strstr(line, "[vdso]") || strstr(line, "mmap_vma"))
				printf("      %s", line);
		if (f) fclose(f);
	}
	printf("    初始 brk (sbrk(0)) = %p\n", sbrk(0));
	{
		void *b0 = sbrk(0);
		sbrk(1024 * 1024);
		printf("    sbrk(+1MB) 後      = %p   (差 %ld bytes)\n",
		       sbrk(0), (char *)sbrk(0) - (char *)b0);
		show_maps("擴大後的 [heap]：", (unsigned long)b0 - PGSZ,
			  (unsigned long)sbrk(0) + PGSZ);
		sbrk(-1024 * 1024);
		printf("    sbrk(-1MB) 後      = %p   <- mm->brk 可增可減，"
		       "但 mm->start_brk 不變\n", sbrk(0));
	}
	printf("\n");

	/* ================= Q33：MAP_FIXED ================= */
	printf("--- [Q33] MAP_FIXED 會靜默覆蓋，MAP_FIXED_NOREPLACE 才會報錯 ---\n");

	printf("[1] mmap(hint, 800KB, MAP_FIXED)\n");
	a = mmap(hint, 819200, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
	printf("    -> %p   (VMA 總數 = %d)\n", a, count_vma());
	show_maps("目前的 maps：", (unsigned long)hint, (unsigned long)hint + 819200);

	printf("[2] mmap(同一位址, 4KB, MAP_FIXED)   <- 落在 [1] 的正中間之前\n");
	b = mmap(hint, PGSZ, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
	printf("    -> %p   errno=%d   ★ 成功！沒有回報任何錯誤\n", b, errno);
	show_maps("覆蓋後的 maps（原本一段被切開）：",
		  (unsigned long)hint, (unsigned long)hint + 819200);

	printf("[3] mmap(同一位址, 4KB, MAP_FIXED_NOREPLACE)\n");
	errno = 0;
	c = mmap(hint, PGSZ, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_FIXED_NOREPLACE | MAP_ANONYMOUS, -1, 0);
	printf("    -> %p   errno=%d (%s)   %s\n", c, errno, strerror(errno),
	       (c == MAP_FAILED && errno == EEXIST) ?
	       "★ 正確地偵測到重疊並回報 EEXIST" : "(未如預期)");

	printf("[4] mmap(同一位址, 4KB, 不帶 MAP_FIXED)\n");
	errno = 0;
	c = mmap(hint, PGSZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	printf("    -> %p   %s\n", c,
	       (c != hint) ? "★ hint 只是建議，衝突時核心自動另尋位址" : "(剛好給了 hint)");
	if (c != MAP_FAILED) munmap(c, PGSZ);
	munmap(hint, 819200);
	printf("\n");

	/* ================= Q21：VMA 合併 ================= */
	printf("--- [Q21] 相鄰且屬性相同的 VMA 會被自動合併 ---\n");
	{
		char *base = (char *)0x0000700100000000UL;
		int n0 = count_vma();

		mmap(base,             PGSZ, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
		printf("    第 1 塊 4KB     -> VMA 總數 %d (+%d)\n",
		       count_vma(), count_vma() - n0);
		mmap(base + PGSZ,      PGSZ, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
		printf("    第 2 塊（緊鄰、屬性相同）-> VMA 總數 %d\n", count_vma());
		mmap(base + 2 * PGSZ,  PGSZ, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
		printf("    第 3 塊（緊鄰、屬性相同）-> VMA 總數 %d\n", count_vma());
		show_maps("三塊合併的結果：", (unsigned long)base,
			  (unsigned long)base + 3 * PGSZ);

		mmap(base + 3 * PGSZ,  PGSZ, PROT_READ,          /* 屬性不同！ */
		     MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS, -1, 0);
		printf("    第 4 塊（緊鄰但 PROT_READ）-> VMA 總數 %d  ★ 屬性不同就不能合併\n",
		       count_vma());
		show_maps("屬性不同無法合併：", (unsigned long)base,
			  (unsigned long)base + 4 * PGSZ);
		munmap(base, 4 * PGSZ);
	}
	printf("\n");

	/* ================= Q32：四種 mmap 組合 ================= */
	printf("--- [Q32] 四種 mmap 組合（私有/共享 × 匿名/檔案）---\n");
	fd = open("/tmp/mmap_vma_test.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
	{
		char buf[PGSZ];
		memset(buf, 0xAB, sizeof buf);
		if (write(fd, buf, sizeof buf) != sizeof buf) perror("write");
		fsync(fd);
	}

	struct { const char *name; int flags; int use_fd; } cases[] = {
		{ "MAP_PRIVATE|MAP_ANONYMOUS", MAP_PRIVATE | MAP_ANONYMOUS, 0 },
		{ "MAP_SHARED |MAP_ANONYMOUS", MAP_SHARED  | MAP_ANONYMOUS, 0 },
		{ "MAP_PRIVATE|檔案",          MAP_PRIVATE,                 1 },
		{ "MAP_SHARED |檔案",          MAP_SHARED,                  1 },
	};
	for (int i = 0; i < 4; i++) {
		char *m = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE,
			       cases[i].flags, cases[i].use_fd ? fd : -1, 0);
		unsigned char before, after;
		FILE *f; char line[512]; char perm[8] = "?";

		if (m == MAP_FAILED) { printf("  %-28s mmap 失敗\n", cases[i].name); continue; }
		before = m[0];
		m[0] = 0x5a;
		msync(m, PGSZ, MS_SYNC);
		if (cases[i].use_fd) pread(fd, &after, 1, 0); else after = 0;

		/* 從 maps 讀出這段的權限字串（p 還是 s）*/
		f = fopen("/proc/self/maps", "r");
		while (f && fgets(line, sizeof line, f)) {
			unsigned long lo, hi;
			if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) == 3 &&
			    (unsigned long)m >= lo && (unsigned long)m < hi) break;
		}
		if (f) fclose(f);

		printf("  %-28s maps權限=%-5s 映射前內容=0x%02x 寫入後=0x%02x",
		       cases[i].name, perm, before, (unsigned char)m[0]);
		if (cases[i].use_fd)
			printf("  檔案變成=0x%02x %s", after,
			       after == 0x5a ? "★會回寫" : "★不回寫(COW)");
		printf("\n");
		munmap(m, PGSZ);
		/* 每次測完把檔案還原 */
		{ unsigned char v = 0xAB; pwrite(fd, &v, 1, 0); fsync(fd); }
	}
	close(fd);
	unlink("/tmp/mmap_vma_test.bin");
	printf("\n  註：MAP_SHARED|MAP_ANONYMOUS 就是 shmem——maps 權限顯示 's'，\n");
	printf("      它掛在匿名 LRU 上但計入 Shmem 而非 AnonPages（見 Ch6 Q5）\n");
	return 0;
}
