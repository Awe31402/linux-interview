/* Ch5 Q10-Q22 —— 頁面回收、LRU 鏈表、swappiness 實測
 *
 *  A. LRU 鏈表的分層與晉升            (Q11, Q27)
 *  B. 匿名頁的生命週期                (Q20, Q21, Q22)
 *  C. MADV_PAGEOUT 觸發回收 + 換出    (Q12, Q19)
 *  D. swappiness 對 anon/file 掃描比重的影響 (Q16)
 *  E. workingset refault：只讀一次的檔案      (Q17)
 *  F. 髒頁回收要先回寫                (Q18)
 *
 * 全部用 /proc/vmstat 與 /proc/meminfo 的差量說話，不製造全系統記憶體壓力。
 *
 *   sudo ./reclaim_test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define MB (1024UL * 1024UL)

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

static long meminfo(const char *key)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[256]; long v = -1;
	if (!f) return -1;
	while (fgets(line, sizeof line, f))
		if (!strncmp(line, key, strlen(key))) {
			sscanf(line + strlen(key), "%ld", &v); break;
		}
	fclose(f);
	return v;
}

static void lru(const char *tag)
{
	printf("%-30s Act(anon)=%-9ld Inact(anon)=%-9ld Act(file)=%-9ld Inact(file)=%-9ld Unevict=%ld\n",
	       tag, meminfo("Active(anon):"), meminfo("Inactive(anon):"),
	       meminfo("Active(file):"), meminfo("Inactive(file):"),
	       meminfo("Unevictable:"));
}

static long sysctl_get(const char *p)
{
	FILE *f = fopen(p, "r"); long v = -1;
	if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); }
	return v;
}

static int sysctl_set(const char *p, long v)
{
	char b[32]; int fd, r;
	fd = open(p, O_WRONLY);
	if (fd < 0) return -1;
	r = snprintf(b, sizeof b, "%ld\n", v);
	r = write(fd, b, r);
	close(fd);
	return r > 0 ? 0 : -1;
}

int main(void)
{
	const size_t SZ = 256 * MB;
	char *p;
	long sw_saved;

	printf("=========== Ch5 Q10-Q22：頁面回收與 LRU ===========\n\n");

	/* ---------- A/B：匿名頁進 LRU 的哪一條？ ---------- */
	printf("--- [Q11/Q20/Q21] 匿名頁的誕生與 LRU 歸屬 ---\n");
	lru("配置前");
	p = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	memset(p, 0xA5, SZ);                        /* Q21：寫入 -> 產生匿名頁 */
	lru("寫入 256MB 匿名頁後");
	printf("  -> 新產生的匿名頁進【Inactive(anon)】：lru_cache_add() 的預設行為\n");
	printf("     （核心的 second-chance：先放不活躍，被再次存取才晉升活躍）\n\n");

	/* 再存取一次，觀察是否晉升 */
	{
		volatile long s = 0;
		for (size_t i = 0; i < SZ; i += 4096) s += p[i];
		for (size_t i = 0; i < SZ; i += 4096) s += p[i];
	}
	sleep(1);
	lru("再讀兩遍之後");
	printf("  -> 純讀取不一定馬上晉升；晉升靠 PTE 的 Access flag + 回收時的\n");
	printf("     page_referenced() 檢查（Ch5 Q11：LRU 靠 PG_referenced + AF 判斷冷熱）\n\n");

	/* ---------- C：MADV_PAGEOUT 直接觸發回收 ---------- */
	printf("--- [Q12/Q19] MADV_PAGEOUT 觸發回收（匿名頁 -> 換出到 zram）---\n");
	{
		long s0 = vmstat("pgsteal_anon"), sc0 = vmstat("pgscan_anon");
		long sf0 = meminfo("SwapFree:"), nvw0 = vmstat("nr_vmscan_write");

		printf("  回收前 SwapFree=%ld kB  pgsteal_anon=%ld  pgscan_anon=%ld\n",
		       sf0, s0, sc0);
		if (madvise(p, SZ, MADV_PAGEOUT)) perror("MADV_PAGEOUT");
		sleep(2);
		printf("  回收後 SwapFree=%ld kB  pgsteal_anon=%ld  pgscan_anon=%ld\n",
		       meminfo("SwapFree:"), vmstat("pgsteal_anon"), vmstat("pgscan_anon"));
		printf("  差量：SwapFree %+ld kB   pgsteal_anon %+ld 頁   pgscan_anon %+ld 頁\n",
		       meminfo("SwapFree:") - sf0, vmstat("pgsteal_anon") - s0,
		       vmstat("pgscan_anon") - sc0);
		printf("  nr_vmscan_write %+ld  <- 回收路徑主動發起的回寫次數\n",
		       vmstat("nr_vmscan_write") - nvw0);
		lru("  MADV_PAGEOUT 後");
		printf("  -> 匿名頁被 try_to_unmap + swap out，Inactive(anon) 掉下來 (Q12/Q19)\n\n");
	}

	/* ---------- B：換回來（major fault / swapin）---------- */
	printf("--- [Q20] 匿名頁生命週期：換出 -> 再存取 -> 換回 ---\n");
	{
		long mj0 = vmstat("pgmajfault"), si0 = vmstat("pswpin");
		volatile long s = 0;

		for (size_t i = 0; i < SZ; i += 4096) s += p[i];
		printf("  重新讀一遍：pgmajfault %+ld   pswpin %+ld 頁\n",
		       vmstat("pgmajfault") - mj0, vmstat("pswpin") - si0);
		printf("  -> 換出的匿名頁再被存取 = major fault + 從 swap 讀回 (Q20)\n\n");
	}
	munmap(p, SZ);
	printf("  munmap 之後匿名頁的引用歸零 -> 釋放回伙伴系統 (Q22)\n\n");

	/* ---------- D：swappiness ---------- */
	printf("--- [Q16] swappiness 如何影響 anon/file 的掃描比重 ---\n");
	sw_saved = sysctl_get("/proc/sys/vm/swappiness");
	printf("  目前 swappiness = %ld\n", sw_saved);
	printf("  公式（mm/vmscan.c get_scan_count()）：\n");
	printf("     anon_prio = swappiness;  file_prio = 200 - swappiness\n");
	printf("     再各自乘上「最近的回收效率」(recent_scanned/recent_rotated)\n");
	printf("  本機累計：pgscan_anon=%ld  pgscan_file=%ld  比例 %.2f : 1\n",
	       vmstat("pgscan_anon"), vmstat("pgscan_file"),
	       (double)vmstat("pgscan_anon") / (vmstat("pgscan_file") ?: 1));
	printf("  本機累計：pgsteal_anon=%ld pgsteal_file=%ld\n",
	       vmstat("pgsteal_anon"), vmstat("pgsteal_file"));
	printf("  -> swappiness=%ld（>100 的極端值）是 zram 場景的典型設定：\n", sw_saved);
	printf("     換出到壓縮 RAM 幾乎沒有 I/O 成本，所以刻意偏向回收匿名頁\n\n");

	/* ---------- E：workingset / 只讀一次的檔案 ---------- */
	printf("--- [Q17] 只存取一次的大檔案：如何避免污染 LRU ---\n");
	{
		long r0 = vmstat("workingset_refault_file");
		long a0 = vmstat("workingset_activate_file");

		printf("  workingset_refault_file = %ld  （曾被回收、又被讀回來的檔案頁）\n", r0);
		printf("  workingset_activate_file= %ld  （refault 後被判定為熱、直接晉升活躍）\n", a0);
		printf("  workingset_nodereclaim  = %ld\n", vmstat("workingset_nodereclaim"));
		printf("  -> 核心的兩道防線：\n");
		printf("     (1) 新讀進來的檔案頁一律進 Inactive(file)，只讀一次就會先被回收，\n");
		printf("         不會擠掉 Active(file) 裡真正的熱資料\n");
		printf("     (2) shadow entry 記錄「被回收時的 LRU 距離」，若很快 refault\n");
		printf("         就代表 inactive 太小，直接晉升 active（workingset_activate）\n\n");
	}

	/* ---------- F：髒頁回收 ---------- */
	printf("--- [Q18] 回收髒的頁面高速快取時會馬上回寫嗎？---\n");
	printf("  目前 Dirty=%ld kB  Writeback=%ld kB\n",
	       meminfo("Dirty:"), meminfo("Writeback:"));
	printf("  nr_vmscan_write=%ld  nr_vmscan_immediate_reclaim=%ld\n",
	       vmstat("nr_vmscan_write"), vmstat("nr_vmscan_immediate_reclaim"));
	printf("  -> 不會馬上回寫。shrink_page_list() 對髒的檔案頁：\n");
	printf("     * 一般情況設 PG_reclaim 後放回 inactive 頭部，交給 writeback 執行緒，\n");
	printf("       自己繼續掃下一頁（避免回收路徑被磁碟 I/O 卡住）\n");
	printf("     * 只有 kswapd 掃了很多輪還是滿手髒頁時才會自己 pageout()\n");
	printf("     * nr_vmscan_immediate_reclaim 就是「遇到正在回寫的頁而跳過」的次數\n\n");

	/* ---------- kswapd ---------- */
	printf("--- [Q10/Q13/Q14] kswapd ---\n");
	printf("  kswapd_low_wmark_hit_quickly  = %ld\n", vmstat("kswapd_low_wmark_hit_quickly"));
	printf("  kswapd_high_wmark_hit_quickly = %ld\n", vmstat("kswapd_high_wmark_hit_quickly"));
	printf("  pgscan_kswapd=%-10ld pgsteal_kswapd=%ld   （背景回收）\n",
	       vmstat("pgscan_kswapd"), vmstat("pgsteal_kswapd"));
	printf("  pgscan_direct=%-10ld pgsteal_direct=%ld   （直接回收，會阻塞分配者）\n",
	       vmstat("pgscan_direct"), vmstat("pgsteal_direct"));
	printf("  allocstall_normal=%ld allocstall_movable=%ld  （被迫直接回收的次數）\n",
	       vmstat("allocstall_normal"), vmstat("allocstall_movable"));
	printf("  -> kswapd 回收效率 = pgsteal/pgscan = %.1f%%\n",
	       100.0 * vmstat("pgsteal_kswapd") / (vmstat("pgscan_kswapd") ?: 1));
	printf("  -> 直接回收效率   = %.1f%%\n",
	       100.0 * vmstat("pgsteal_direct") / (vmstat("pgscan_direct") ?: 1));

	printf("\n本程式沒有修改任何 sysctl，無需還原。\n");
	return 0;
}
