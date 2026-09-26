// SPDX-License-Identifier: GPL-2.0
/* 注意：2026-09-26 因板上 build tree 被切到 6.12（見 05 §2）未能編譯，改用 peek.py 讀同一組暫存器。 */
/*
 * pvtm_probe.c — 讀 RK3588 各 CPU 叢集的 PVTPLL/PVTM 計數暫存器（就是開機時
 * rockchip_opp_select.c 用來決定「體質分數 pvtm」的那個暫存器），
 * 連同當下的溫度、頻率、電壓一起印出來，並套用和核心一樣的溫度補償公式。
 *
 * 只讀，不改任何頻率或電壓。
 *   insmod pvtm_probe.ko ; dmesg | tail ; rmmod pvtm_probe
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/cpufreq.h>
#include <linux/thermal.h>
#include <linux/regulator/consumer.h>
#include <linux/cpu.h>
#include <linux/delay.h>

struct clus {
	const char *name;
	int cpu;
	phys_addr_t grf;
	u32 off;
	const char *tz;
	int ref_temp, prop_lo, prop_hi;	/* 與 DT 同名屬性相同 */
};

/* 數值抄自 arch/arm64/boot/dts/rockchip/rk3588s.dtsi 的 clusterN-opp-table */
static const struct clus tbl[] = {
	{ "cluster0(A55 cpu0-3)", 0, 0xfd594000, 0x64, "soc-thermal", 25, 0, 0 },
	{ "cluster1(A76 cpu4-5)", 4, 0xfd590000, 0x18, "soc-thermal", 25, 270, 270 },
	{ "cluster2(A76 cpu6-7)", 6, 0xfd592000, 0x18, "soc-thermal", 25, 270, 270 },
};

static int samples = 8;
module_param(samples, int, 0444);

static int __init pvtm_probe_init(void)
{
	struct thermal_zone_device *tz = thermal_zone_get_zone_by_name("soc-thermal");
	int i, s;

	for (i = 0; i < ARRAY_SIZE(tbl); i++) {
		const struct clus *c = &tbl[i];
		void __iomem *b = ioremap(c->grf, 0x100);
		struct device *cdev = get_cpu_device(c->cpu);
		struct regulator *reg = regulator_get_optional(cdev, "cpu");
		int temp = 0, uv = reg && !IS_ERR(reg) ? regulator_get_voltage(reg) : -1;
		unsigned int khz = cpufreq_quick_get(c->cpu);
		u32 v[16], mn = ~0u, mx = 0;
		u64 sum = 0;

		if (!b)
			continue;
		if (!IS_ERR_OR_NULL(tz))
			thermal_zone_get_temp(tz, &temp);
		for (s = 0; s < samples && s < 16; s++) {
			v[s] = readl(b + c->off) & 0xffff;
			sum += v[s];
			mn = min(mn, v[s]);
			mx = max(mx, v[s]);
			usleep_range(1100, 1200);	/* rockchip,pvtm-sample-time = 1100 us */
		}
		{
			int avg = sum / s;
			int dt = temp / 1000 - c->ref_temp;
			int comp = dt * (dt < 0 ? c->prop_lo : c->prop_hi) / 1000;

			pr_info("pvtm_probe: %s f=%u kHz V=%d uV T=%d mC raw avg=%d (min %u max %u) comp=%+d -> pvtm=%d\n",
				c->name, khz, uv, temp, avg, mn, mx, comp, avg + comp);
			pr_info("pvtm_probe:   grf@%pa: +0x00..0x2c = %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
				&c->grf, readl(b), readl(b + 4), readl(b + 8), readl(b + 0xc),
				readl(b + 0x10), readl(b + 0x14), readl(b + 0x18), readl(b + 0x1c),
				readl(b + 0x20), readl(b + 0x24), readl(b + 0x28), readl(b + 0x2c));
		}
		if (reg && !IS_ERR(reg))
			regulator_put(reg);
		iounmap(b);
	}
	return -EAGAIN;	/* 讀完就走，不留在核心裡 */
}
module_init(pvtm_probe_init);
MODULE_LICENSE("GPL");
