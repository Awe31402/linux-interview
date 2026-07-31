/* Ch5 Q31-Q37 —— KSM（Kernel Samepage Merging）合併實測
 *
 * 配置 N 份「內容完全相同」的頁面 + 1 份內容不同的對照組，
 * 用 madvise(MADV_MERGEABLE) 交給 KSM，然後觀察：
 *   /sys/kernel/mm/ksm/pages_shared    有幾個「被共用的實體頁」（stable node）
 *   /sys/kernel/mm/ksm/pages_sharing   有幾個虛擬頁指向那些共用頁
 *   /sys/kernel/mm/ksm/pages_unshared  掃過但沒找到相同內容的
 *   /sys/kernel/mm/ksm/pages_volatile  內容一直在變、放棄合併的
 *
 * 再用 /proc/self/pagemap 直接證明「不同的虛擬位址映射到同一個 PFN」，
 * 並且寫入其中一份會觸發 COW（PFN 又變回不同）。
 *
 *   sudo ./ksm_test            # 需要 root 來開 /sys/kernel/mm/ksm/run
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define NCOPY   64                 /* 64 份相同內容 */
#define NDIFF   16                 /* 16 份互不相同（對照組） */
#define PGSZ    4096

static int pmfd = -1;

static unsigned long va2pfn(void *va)
{
	uint64_t e = 0;
	if (pread(pmfd, &e, 8, ((unsigned long)va / PGSZ) * 8) != 8) return 0;
	return (e >> 63) ? (e & ((1ULL << 55) - 1)) : 0;
}

static long ksm_get(const char *f)
{
	char p[128]; FILE *fp; long v = -1;
	snprintf(p, sizeof p, "/sys/kernel/mm/ksm/%s", f);
	fp = fopen(p, "r");
	if (fp) { if (fscanf(fp, "%ld", &v) != 1) v = -1; fclose(fp); }
	return v;
}

static int ksm_set(const char *f, const char *val)
{
	char p[128]; int fd, r;
	snprintf(p, sizeof p, "/sys/kernel/mm/ksm/%s", f);
	fd = open(p, O_WRONLY);
	if (fd < 0) return -1;
	r = write(fd, val, strlen(val));
	close(fd);
	return r > 0 ? 0 : -1;
}

static void ksm_stat(const char *tag)
{
	printf("%-26s shared=%-6ld sharing=%-6ld unshared=%-6ld volatile=%-6ld full_scans=%ld\n",
	       tag, ksm_get("pages_shared"), ksm_get("pages_sharing"),
	       ksm_get("pages_unshared"), ksm_get("pages_volatile"),
	       ksm_get("full_scans"));
}

int main(void)
{
	char *same, *diff;
	int i, saved_run, saved_pages;
	unsigned long pfn0, pfn1;

	pmfd = open("/proc/self/pagemap", O_RDONLY);
	if (pmfd < 0) { perror("pagemap（需要 root）"); return 1; }
	if (ksm_get("run") < 0) { fprintf(stderr, "本核心沒有 KSM\n"); return 1; }

	saved_run   = ksm_get("run");
	saved_pages = ksm_get("pages_to_scan");
	printf("=========== Ch5 Q31-Q37：KSM 合併實測 ===========\n");
	printf("實驗前：run=%d  pages_to_scan=%d\n\n", saved_run, saved_pages);

	/* ---- 準備資料 ---- */
	same = mmap(NULL, (size_t)NCOPY * PGSZ, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	diff = mmap(NULL, (size_t)NDIFF * PGSZ, PROT_READ | PROT_WRITE,
		    MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	for (i = 0; i < NCOPY; i++)
		memset(same + (size_t)i * PGSZ, 0x5a, PGSZ);      /* 全部一樣 */
	for (i = 0; i < NDIFF; i++)
		memset(diff + (size_t)i * PGSZ, i + 1, PGSZ);     /* 互不相同 */

	printf("--- 合併前：%d 個相同頁的 PFN（應該全都不同）---\n", NCOPY);
	for (i = 0; i < 4; i++)
		printf("  same[%d] VA=%p PFN=0x%lx\n", i,
		       same + (size_t)i * PGSZ, va2pfn(same + (size_t)i * PGSZ));
	pfn0 = va2pfn(same);
	pfn1 = va2pfn(same + PGSZ);
	printf("  -> same[0]=0x%lx, same[1]=0x%lx  %s\n\n", pfn0, pfn1,
	       pfn0 != pfn1 ? "不同（尚未合併）" : "已相同?!");

	/* ---- 標記為可合併 ---- */
	if (madvise(same, (size_t)NCOPY * PGSZ, MADV_MERGEABLE) ||
	    madvise(diff, (size_t)NDIFF * PGSZ, MADV_MERGEABLE)) {
		perror("madvise(MADV_MERGEABLE)");
		return 1;
	}
	printf("已對兩塊區域下 madvise(MADV_MERGEABLE)\n");

	ksm_stat("啟用 KSM 前");

	/* ---- 開啟 KSM 並加快掃描 ---- */
	if (ksm_set("pages_to_scan", "2000") || ksm_set("sleep_millisecs", "20") ||
	    ksm_set("run", "1")) {
		perror("寫 /sys/kernel/mm/ksm/*（需要 root）");
		return 1;
	}
	printf("已設定 run=1, pages_to_scan=2000, sleep_millisecs=20\n\n");

	for (i = 0; i < 12; i++) {
		char tag[64];
		usleep(500000);
		snprintf(tag, sizeof tag, "  掃描中 %4.1f 秒", (i + 1) * 0.5);
		ksm_stat(tag);
		if (ksm_get("pages_sharing") >= NCOPY - 1) break;
	}

	/* ---- 驗證：不同 VA 是否指向同一個 PFN ---- */
	printf("\n--- 合併後：同樣 4 個虛擬頁的 PFN ---\n");
	for (i = 0; i < 4; i++)
		printf("  same[%d] VA=%p PFN=0x%lx\n", i,
		       same + (size_t)i * PGSZ, va2pfn(same + (size_t)i * PGSZ));
	{
		int merged = 1;
		unsigned long p0 = va2pfn(same);

		for (i = 1; i < NCOPY; i++)
			if (va2pfn(same + (size_t)i * PGSZ) != p0) { merged = 0; break; }
		printf("  -> %d 個相同內容的虛擬頁 %s 同一個實體頁 (PFN=0x%lx) %s\n",
		       NCOPY, merged ? "全部指向" : "沒有全部指向", p0,
		       merged ? "★ KSM 合併成功" : "(可能還沒掃完)");
	}
	printf("--- 對照組：內容互不相同的 %d 頁 ---\n", NDIFF);
	for (i = 0; i < 3; i++)
		printf("  diff[%d] PFN=0x%lx\n", i, va2pfn(diff + (size_t)i * PGSZ));
	printf("  -> 內容不同 -> 不會被合併，計入 pages_unshared\n");

	/* ---- 寫入合併後的頁 -> 觸發 COW，重新分家 (Ch5 Q35/Q37) ---- */
	printf("\n--- 對合併後的 same[1] 寫入一個 byte（觸發 COW）---\n");
	printf("  寫入前 same[1] PFN = 0x%lx\n", va2pfn(same + PGSZ));
	same[PGSZ] = 0x77;
	printf("  寫入後 same[1] PFN = 0x%lx\n", va2pfn(same + PGSZ));
	printf("  same[0]        PFN = 0x%lx（不受影響）\n", va2pfn(same));
	printf("  -> PFN 變了就證明 KSM 頁是【唯讀共享 + 寫時複製】(Ch5 Q37)\n");
	ksm_stat("  寫入後");

	/* ---- 還原 ---- */
	printf("\n--- 還原設定 ---\n");
	munmap(same, (size_t)NCOPY * PGSZ);
	munmap(diff, (size_t)NDIFF * PGSZ);
	{
		char buf[32];
		snprintf(buf, sizeof buf, "%d", saved_pages);
		ksm_set("pages_to_scan", buf);
		snprintf(buf, sizeof buf, "%d", saved_run);
		ksm_set("run", buf);
		ksm_set("sleep_millisecs", "20");
	}
	printf("run 還原為 %d，pages_to_scan 還原為 %d\n",
	       ksm_get("run"), ksm_get("pages_to_scan"));
	printf("（run=2 表示 unmerge 全部；這裡還原成實驗前的 %d）\n", saved_run);
	return 0;
}
