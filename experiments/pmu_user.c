// SPDX-License-Identifier: GPL-2.0
/*
 * pmu_user.ko —— 打開 EL0 直接讀 PMU 週期計數器 (PMCCNTR_EL0) 的權限。
 *
 * 《奔跑吧 Linux 內核》卷2 第6章〈安全漏洞分析〉高頻面試題 Q1/Q3/Q7 的
 * 「高速緩存側信道」全都要「用高精度時鐘量單次記憶體存取延遲」。
 *
 * 本機 (RK3588) 的問題：使用者態能讀的 CNTVCT_EL0 只有 24 MHz
 * (CNTFRQ_EL0 = 0x016E3600 = 24000000)，一個 tick ≈ 41.6 ns，
 * 而 L1 命中 (~4 cycle @ 2.4GHz ≈ 1.7 ns) vs DRAM (~100 ns) 的差異
 * 只有 2~3 個 tick，量不準。所以側信道實驗需要「週期級」時鐘。
 *
 * ARMv8 的 PMU 週期計數器 PMCCNTR_EL0 預設只有 EL1 能讀，EL0 讀會觸發
 * 未定義指令例外。本模組在每顆 CPU 上：
 *   1. PMCR_EL0.E=1  啟用 PMU、.C=1 清零週期計數器、.LC=1 用 64 位元計數
 *   2. PMCNTENSET_EL0.C(bit31)=1  打開週期計數器
 *   3. PMCCFILTR_EL0 = 0          在所有 EL 都計數
 *   4. PMUSERENR_EL0.EN(bit0)=1, .CR(bit2)=1  允許 EL0 讀 PMCCNTR
 *
 * 之後使用者態就能 `mrs x0, pmccntr_el0` 拿到週期數。
 * rmmod 時把 PMUSERENR_EL0 清 0，收回 EL0 權限。
 *
 * 用法：
 *   sudo insmod pmu_user.ko
 *   ./flush_reload   # 或 spectre_v1 / meltdown_test
 *   sudo rmmod pmu_user
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/smp.h>
#include <asm/sysreg.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Enable EL0 access to PMCCNTR_EL0 for side-channel timing");

static void pmu_enable_this_cpu(void *info)
{
	u64 v;

	/* 在所有 EL 都計數（不過濾） */
	write_sysreg(0, pmccfiltr_el0);

	/* 啟用 PMU、清零週期計數、64 位元長計數器 */
	v = read_sysreg(pmcr_el0);
	v |= (1U << 0) | (1U << 2) | (1U << 6);   /* E | C | LC */
	write_sysreg(v, pmcr_el0);

	/* 打開週期計數器 (bit31) */
	write_sysreg((u64)1U << 31, pmcntenset_el0);

	/* 允許 EL0：EN(bit0) 讀 PMU 暫存器、CR(bit2) 讀週期計數器 */
	write_sysreg((1U << 0) | (1U << 2), pmuserenr_el0);
}

static void pmu_disable_this_cpu(void *info)
{
	write_sysreg(0, pmuserenr_el0);   /* 收回 EL0 讀取權限 */
}

static int __init pmu_user_init(void)
{
	on_each_cpu(pmu_enable_this_cpu, NULL, 1);
	pr_info("pmu_user: EL0 access to PMCCNTR_EL0 enabled on all CPUs\n");
	return 0;
}

static void __exit pmu_user_exit(void)
{
	on_each_cpu(pmu_disable_this_cpu, NULL, 1);
	pr_info("pmu_user: EL0 access disabled\n");
}

module_init(pmu_user_init);
module_exit(pmu_user_exit);
