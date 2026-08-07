/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sidechannel.h —— aarch64 高速緩存側信道原語（Flush+Reload）
 * 供 flush_reload.c / spectre_v1.c / meltdown_test.c / branch_pred.c 共用。
 *
 * 高精度時鐘：ROCK 5B 上 EL0 能讀的 CNTVCT_EL0 只有 24 MHz（一 tick≈41ns），
 * 量不出 cache 命中/未命中的幾十 ns 差。所以改讀 PMU 週期計數器 PMCCNTR_EL0。
 *
 * 取得週期計數器有兩條路，sc_timer_init() 會自動挑：
 *   (1) perf 自我監控（推薦，最穩）：perf_event_open(CPU_CYCLES, self) + mmap，
 *       核心 arm_pmu 驅動會在情境切換/深度 idle 後幫我們維持 EL0 存取權限。
 *       需要 `sudo sysctl kernel.perf_user_access=1`。讀取用 mmap 頁給的
 *       index 直接 `mrs pmccntr_el0`。
 *   (2) 直接讀（備援）：先 `sudo insmod pmu_user.ko` 打開 PMUSERENR_EL0，
 *       再 `mrs pmccntr_el0`。缺點是 idle 電源崩塌後權限會被清掉。
 */
#ifndef _EXP_SIDECHANNEL_H
#define _EXP_SIDECHANNEL_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <signal.h>
#include <setjmp.h>
#include <linux/perf_event.h>

/* --- 底層原語 --- */
static inline void flush(void *p)   { asm volatile("dc civac, %0" :: "r"(p) : "memory"); }
static inline void mfence(void)     { asm volatile("dsb sy; isb" ::: "memory"); }
static inline void maccess(void *p) { volatile uint8_t x; asm volatile("ldrb %w0,[%1]" : "=r"(x) : "r"(p) : "memory"); }

static inline uint64_t raw_pmccntr(void)
{
	uint64_t v; asm volatile("isb; mrs %0, pmccntr_el0" : "=r"(v)); return v;
}

/* perf mmap 自我監控狀態 */
static struct perf_event_mmap_page *g_pc;

static long perf_open_self(void)
{
	struct perf_event_attr a;
	memset(&a, 0, sizeof(a));
	a.type = PERF_TYPE_HARDWARE;
	a.size = sizeof(a);
	a.config = PERF_COUNT_HW_CPU_CYCLES;
	a.exclude_kernel = 1;   /* arm64 直讀 PMU 要求只算使用者態 */
	a.exclude_hv = 1;
	a.pinned = 1;
	a.config1 = 0x2;        /* rdpmc 格式位：arm64 授予 EL0 直讀的必要條件 */
	return syscall(__NR_perf_event_open, &a, 0 /*self*/, -1 /*any cpu*/, -1, 0);
}

/* 用 SIGILL handler 安全地探測「原始 mrs pmccntr 是否可讀」 */
static sigjmp_buf g_illjb;
static void sc_ill(int s) { siglongjmp(g_illjb, 1); }
static int raw_pmccntr_usable(void)
{
	struct sigaction old, sa;
	int ok = 0;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sc_ill;
	sigaction(SIGILL, &sa, &old);
	if (!sigsetjmp(g_illjb, 1)) {
		uint64_t c0 = raw_pmccntr(), c1 = raw_pmccntr();
		ok = (c1 != c0) || 1;   /* 沒 SIGILL 就算可用 */
	}
	sigaction(SIGILL, &old, NULL);
	return ok;
}

/* 回傳 0=用 perf mmap，1=用原始 mrs（需 pmu_user.ko），-1=都不行 */
static int sc_timer_init(void)
{
	long fd = perf_open_self();
	if (fd >= 0) {
		void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
		if (m != MAP_FAILED) {
			g_pc = (struct perf_event_mmap_page *)m;
			if (g_pc->cap_user_rdpmc && g_pc->index)
				return 0;               /* perf 直讀可用 */
		}
		g_pc = NULL;
	}
	/* 試原始 mrs（pmu_user.ko），用 SIGILL handler 保護 */
	return raw_pmccntr_usable() ? 1 : -1;
}

static inline uint64_t rdcycle(void)
{
	if (g_pc) {
		uint32_t idx = g_pc->index;
		if (idx) {
			/* CPU_CYCLES 事件分到專屬週期計數器 -> 直接讀 PMCCNTR_EL0 */
			uint64_t v;
			asm volatile("isb; mrs %0, pmccntr_el0" : "=r"(v));
			return g_pc->offset + v;
		}
	}
	return raw_pmccntr();
}

/* 量「讀取 p 這條 line」花幾週期：命中≈幾十，未命中≈兩三百 */
static inline uint64_t probe_time(void *p)
{
	uint64_t t0, t1;
	mfence();
	t0 = rdcycle();
	maccess(p);
	asm volatile("dsb sy" ::: "memory");
	t1 = rdcycle();
	return t1 - t0;
}

#endif /* _EXP_SIDECHANNEL_H */
