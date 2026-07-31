/* Ch4 Q24/Q25/Q38/Q39/Q40/Q41/Q42 —— 四種缺頁異常的實測
 *
 *   1. 匿名頁 minor fault        （malloc/mmap 後第一次寫）
 *   2. 檔案映射 minor fault      （page cache 已在記憶體）
 *   3. 檔案映射 MAJOR fault      （page cache 被丟掉，要讀磁碟）
 *   4. 寫時複製 COW fault        （fork 後寫共享頁 / 寫私有檔案映射）
 *   5. 唯讀零頁 fault            （只讀不寫的匿名頁 -> 共用 ZERO_PAGE）
 *
 * 每一項都用 getrusage() 的 ru_minflt / ru_majflt 精準計數。
 *
 *   gcc -O2 -o fault_types fault_types.c && ./fault_types
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/stat.h>

#define MB (1024UL * 1024UL)
#define PAGES(n) ((n) / 4096)

static long g_min, g_maj;

static void mark(void)
{
	struct rusage r;
	getrusage(RUSAGE_SELF, &r);
	g_min = r.ru_minflt;
	g_maj = r.ru_majflt;
}

static void report(const char *tag, unsigned long expect_pages)
{
	struct rusage r;
	getrusage(RUSAGE_SELF, &r);
	printf("%-46s minor=%-8ld major=%-6ld  (預期 %lu 頁)\n",
	       tag, r.ru_minflt - g_min, r.ru_majflt - g_maj, expect_pages);
}

/* /proc/self/statm 的 RSS，單位是頁 */
static long rss_pages(void)
{
	FILE *f = fopen("/proc/self/statm", "r");
	long sz, rss = -1;
	if (f) { if (fscanf(f, "%ld %ld", &sz, &rss) != 2) rss = -1; fclose(f); }
	return rss;
}

int main(void)
{
	const size_t SZ = 16 * MB;
	char *p;
	int fd;

	printf("=========== Ch4 缺頁異常實測（頁大小 4096）===========\n\n");

	/* ---------- Q24/Q25：mmap 不配實體頁，第一次寫才配 ---------- */
	printf("--- [Q24/Q25] 惰性分配 demand paging ---\n");
	printf("mmap 前 RSS = %ld 頁\n", rss_pages());
	mark();
	p = mmap(NULL, SZ, PROT_READ | PROT_WRITE,
		 MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
	report("mmap 16MB（完全不觸碰）", 0);
	printf("mmap 後 RSS = %ld 頁   <- 完全沒漲，一個實體頁都沒配\n\n", rss_pages());

	/* ---------- Q39：匿名頁寫缺頁 ---------- */
	printf("--- [Q39] 匿名頁「寫」缺頁：每頁一次 minor fault ---\n");
	mark();
	for (size_t i = 0; i < SZ; i += 4096) p[i] = 1;
	report("寫入 16MB 匿名頁", PAGES(SZ));
	printf("RSS = %ld 頁\n\n", rss_pages());
	munmap(p, SZ);

	/* ---------- Q39 變體：只讀 -> ZERO_PAGE ---------- */
	printf("--- [Q39 變體] 匿名頁「只讀」缺頁：共用 ZERO_PAGE，不佔 RSS ---\n");
	p = mmap(NULL, SZ, PROT_READ | PROT_WRITE,
		 MAP_ANONYMOUS | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
	long rss0 = rss_pages();
	mark();
	{ volatile long s = 0; for (size_t i = 0; i < SZ; i += 4096) s += p[i]; }
	report("只讀掃過 16MB 匿名頁", PAGES(SZ));
	printf("RSS 變化 = %ld 頁  <- 幾乎不漲：全部指向同一個唯讀 ZERO_PAGE\n\n",
	       rss_pages() - rss0);
	munmap(p, SZ);

	/* ---------- 準備一個測試檔案 ---------- */
	fd = open("/tmp/fault_test.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
	{
		char *buf = malloc(SZ);
		memset(buf, 0xAB, SZ);
		if (write(fd, buf, SZ) != (ssize_t)SZ) perror("write");
		free(buf);
	}
	fsync(fd);

	/* ---------- Q40：檔案映射 minor fault（page cache 熱） ---------- */
	printf("--- [Q40] 檔案映射缺頁（page cache 命中 = minor）---\n");
	p = mmap(NULL, SZ, PROT_READ, MAP_PRIVATE, fd, 0);
	mark();
	{ volatile long s = 0; for (size_t i = 0; i < SZ; i += 4096) s += p[i]; }
	report("讀 16MB 檔案映射（cache 熱）", PAGES(SZ));
	munmap(p, SZ);
	printf("\n");

	/* ---------- Q38：MAJOR fault（先把 page cache 丟掉） ---------- */
	printf("--- [Q38] 主缺頁 major fault（page cache 被丟掉，要讀磁碟）---\n");
	posix_fadvise(fd, 0, SZ, POSIX_FADV_DONTNEED);
	sync();
	posix_fadvise(fd, 0, SZ, POSIX_FADV_DONTNEED);
	p = mmap(NULL, SZ, PROT_READ, MAP_PRIVATE, fd, 0);
	mark();
	{ volatile long s = 0; for (size_t i = 0; i < SZ; i += 4096) s += p[i]; }
	report("讀 16MB 檔案映射（cache 冷）", PAGES(SZ));
	printf("  ^ major > 0 就證明真的去讀了儲存裝置\n");
	printf("  （readahead 會一次讀進多頁，所以 major 遠少於 4096）\n\n");
	munmap(p, SZ);

	/* ---------- Q41/Q42：COW ---------- */
	printf("--- [Q41/Q42] 寫時複製 COW ---\n");
	p = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	memset(p, 0x11, SZ);                       /* 先實體化 */
	printf("父行程 fork 前 RSS = %ld 頁\n", rss_pages());
	if (fork() == 0) {
		long r0 = rss_pages();
		mark();
		{ volatile long s = 0; for (size_t i = 0; i < SZ; i += 4096) s += p[i]; }
		report("[子] fork 後【讀】16MB（不觸發 COW）", 0);
		printf("[子] RSS 變化 = %ld 頁\n", rss_pages() - r0);
		r0 = rss_pages();
		mark();
		for (size_t i = 0; i < SZ; i += 4096) p[i] = 2;
		report("[子] fork 後【寫】16MB（每頁 COW=複製）", PAGES(SZ));
		printf("[子] RSS 變化 = %ld 頁  <- 真的複製了 16MB\n", rss_pages() - r0);
		fflush(stdout);
		_exit(0);
	}
	wait(NULL);
	sleep(1);

	/* Q42：mapcount 降到 1 之後再寫 —— 仍會缺頁，但走 reuse 不複製 */
	{
		long r0 = rss_pages();
		mark();
		for (size_t i = 0; i < SZ; i += 4096) p[i] = 3;
		report("[父] 子行程結束後第 1 次寫（reuse 不複製）", PAGES(SZ));
		printf("[父] RSS 變化 = %ld 頁  <- 幾乎 0 就證明是 reuse 而非 copy ★\n",
		       rss_pages() - r0);
		mark();
		for (size_t i = 0; i < SZ; i += 4096) p[i] = 4;
		report("[父] 第 2 次寫（PTE 已可寫）", 0);
		printf("  ^ 0 次缺頁：do_wp_page() 已把 PTE 改成可寫\n\n");
	}
	munmap(p, SZ);

	/* ---------- Q32/Q41：私有檔案映射寫 -> COW ---------- */
	printf("--- [Q32] MAP_PRIVATE 檔案映射寫入 -> COW，不回寫檔案 ---\n");
	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	printf("  寫入前 p[0] = 0x%02x（檔案內容）\n", (unsigned char)p[0]);
	mark();
	p[0] = 0x5a;
	report("寫 1 頁私有檔案映射", 1);
	printf("  寫入後 p[0] = 0x%02x（記憶體中的私有副本）\n", (unsigned char)p[0]);
	msync(p, 4096, MS_SYNC);
	munmap(p, 4096);
	{
		unsigned char c;
		pread(fd, &c, 1, 0);
		printf("  msync 之後檔案第 0 byte = 0x%02x  <- %s\n\n", c,
		       c == 0xAB ? "仍是 0xAB，證明私有映射不回寫檔案 ✓" : "被改了（不符預期）");
	}

	/* ---------- Q32：共享檔案映射寫 -> 直接改 page cache，會回寫 ---------- */
	printf("--- [Q32] MAP_SHARED 檔案映射寫入 -> 直接改 page cache，會回寫 ---\n");
	p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	p[0] = 0x5a;
	msync(p, 4096, MS_SYNC);
	munmap(p, 4096);
	{
		unsigned char c;
		pread(fd, &c, 1, 0);
		printf("  msync 之後檔案第 0 byte = 0x%02x  <- %s\n\n", c,
		       c == 0x5a ? "變成 0x5a，證明共享映射會寫回檔案 ✓" : "沒改（不符預期）");
	}

	close(fd);
	unlink("/tmp/fault_test.bin");
	return 0;
}
