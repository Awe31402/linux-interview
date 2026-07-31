/* Ch5 Q23-Q30 —— 頁面遷移（migration）與記憶體規整（compaction）實測
 *
 * 原理：規整的本質就是「把可移動的頁搬到 zone 的一端，把空閒頁湊到另一端」，
 * 而搬動的手段就是頁面遷移。所以只要：
 *   1. 配一大塊匿名記憶體（MIGRATE_MOVABLE），記下每一頁的 PFN
 *   2. echo 1 > /proc/sys/vm/compact_memory 觸發全系統規整
 *   3. 再讀一次 PFN —— 有變的就是【被遷移過】的頁
 * 就能同時證明 Q23（遷移原理）、Q24（哪些頁可遷移）、Q28（規整原理）。
 *
 * 同時取樣 /proc/vmstat 的 compact_* / pgmigrate_* 計數與 /proc/buddyinfo，
 * 觀察規整對高階空閒塊的影響（Q29/Q30/Q48）。
 *
 *   sudo ./migrate_compact
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define NPAGES 8192                /* 32 MB */
#define PGSZ   4096

static int pmfd = -1;

static unsigned long va2pfn(void *va)
{
	uint64_t e = 0;
	if (pread(pmfd, &e, 8, ((unsigned long)va / PGSZ) * 8) != 8) return 0;
	return (e >> 63) ? (e & ((1ULL << 55) - 1)) : 0;
}

static long vmstat(const char *key)
{
	FILE *f = fopen("/proc/vmstat", "r");
	char k[64]; long v, r = -1;
	if (!f) return -1;
	while (fscanf(f, "%63s %ld", k, &v) == 2)
		if (!strcmp(k, key)) { r = v; break; }
	fclose(f);
	return r;
}

static void show_vmstat(const char *tag)
{
	printf("%-18s compact_stall=%-6ld success=%-6ld fail=%-6ld migrate_scanned=%-9ld free_scanned=%-9ld isolated=%-8ld\n",
	       tag, vmstat("compact_stall"), vmstat("compact_success"),
	       vmstat("compact_fail"), vmstat("compact_migrate_scanned"),
	       vmstat("compact_free_scanned"), vmstat("compact_isolated"));
	printf("%-18s pgmigrate_success=%-9ld pgmigrate_fail=%ld\n", "",
	       vmstat("pgmigrate_success"), vmstat("pgmigrate_fail"));
}

static void show_buddy(const char *tag)
{
	FILE *f = fopen("/proc/buddyinfo", "r");
	char line[256];
	printf("--- /proc/buddyinfo (%s) ---\n", tag);
	while (f && fgets(line, sizeof line, f)) fputs(line, stdout);
	if (f) fclose(f);
}

int main(void)
{
	char *p;
	unsigned long *before, *after;
	int i, moved = 0, fd;

	pmfd = open("/proc/self/pagemap", O_RDONLY);
	if (pmfd < 0) { perror("pagemap（需要 root）"); return 1; }

	printf("=========== Ch5 Q23-Q30：頁面遷移與記憶體規整 ===========\n\n");

	before = calloc(NPAGES, sizeof(*before));
	after  = calloc(NPAGES, sizeof(*after));

	/* 1. 配一大塊匿名記憶體並全部觸碰（匿名頁 = MIGRATE_MOVABLE，可遷移）*/
	p = mmap(NULL, (size_t)NPAGES * PGSZ, PROT_READ | PROT_WRITE,
		 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED) { perror("mmap"); return 1; }
	for (i = 0; i < NPAGES; i++) p[(size_t)i * PGSZ] = (char)i;
	for (i = 0; i < NPAGES; i++) before[i] = va2pfn(p + (size_t)i * PGSZ);

	printf("已配置 %d 頁（%d MB）匿名記憶體並全部觸碰\n", NPAGES, NPAGES * PGSZ / 1024 / 1024);
	printf("前 5 頁的 PFN：");
	for (i = 0; i < 5; i++) printf("0x%lx ", before[i]);
	printf("\n\n");

	show_buddy("規整前");
	printf("\n");
	show_vmstat("規整前");

	/* 2. 觸發全系統規整（連跑數輪，讓掃描器掃過整個 zone）*/
	printf("\n>>> for r in 1..4: echo 1 > /proc/sys/vm/compact_memory\n");
	for (int r = 0; r < 4; r++) {
		fd = open("/proc/sys/vm/compact_memory", O_WRONLY);
		if (fd < 0) { perror("compact_memory（需要 root）"); return 1; }
		if (write(fd, "1\n", 2) < 0) perror("write");
		close(fd);
		sleep(2);
		{
			int m = 0;
			for (i = 0; i < NPAGES; i++)
				if (before[i] != va2pfn(p + (size_t)i * PGSZ)) m++;
			printf("    第 %d 輪後：已被搬動 %d / %d 頁，pgmigrate_success=%ld\n",
			       r + 1, m, NPAGES, vmstat("pgmigrate_success"));
		}
	}

	show_vmstat("規整後");
	printf("\n");
	show_buddy("規整後");

	/* 3. 比對 PFN */
	for (i = 0; i < NPAGES; i++) after[i] = va2pfn(p + (size_t)i * PGSZ);
	for (i = 0; i < NPAGES; i++) if (before[i] != after[i]) moved++;

	printf("\n=========== 結果 ===========\n");
	printf("虛擬位址完全沒變，但實體頁被搬動了 %d / %d 頁 (%.1f%%)\n",
	       moved, NPAGES, 100.0 * moved / NPAGES);
	if (moved) {
		int shown = 0;
		printf("被搬動的頁（前 8 個）：\n");
		for (i = 0; i < NPAGES && shown < 8; i++)
			if (before[i] != after[i]) {
				printf("  VA=%p   PFN 0x%-8lx -> 0x%-8lx   (搬了 %+ld 頁)\n",
				       p + (size_t)i * PGSZ, before[i], after[i],
				       (long)after[i] - (long)before[i]);
				shown++;
			}
		printf("\n★ 這就是頁面遷移：核心把實體頁複製到新位置、改掉 PTE，\n");
		printf("  使用者行程完全無感（VA 不變、資料不變）——Ch5 Q23/Q26\n");
	} else {
		printf("這一輪沒有頁面被搬動（系統當下不夠碎片化，規整提早結束）\n");
	}

	/* 4. 驗證資料沒被搞壞 */
	{
		int bad = 0;
		for (i = 0; i < NPAGES; i++)
			if (p[(size_t)i * PGSZ] != (char)i) bad++;
		printf("\n資料完整性檢查：%d / %d 頁內容錯誤  %s\n", bad, NPAGES,
		       bad ? "✘" : "✔ 遷移過程資料完全正確");
	}

	munmap(p, (size_t)NPAGES * PGSZ);
	free(before); free(after);
	printf("\n註：compact_memory 是 write-only 的觸發點，寫入後立刻恢復，無需還原。\n");
	return 0;
}
