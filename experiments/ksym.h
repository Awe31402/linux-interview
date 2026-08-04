/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ksym.h —— 用 kprobe 取回 kallsyms_lookup_name()，再拿它解析「沒有 EXPORT_SYMBOL
 * 但存在於 kallsyms 的核心函式」。
 *
 * 為什麼需要？本機 6.1 上：
 *   - irq_to_desc()      只在 CONFIG_KVM_BOOK3S_64_HV_MODULE 時才 EXPORT（PowerPC 專用），
 *                        aarch64 上模組連不到（kernel/irq/irqdesc.c:358）
 *   - irq_work_queue_on() 沒有 EXPORT（kernel/irq_work.c，只 EXPORT 了 irq_work_queue）
 * 但兩者都是 text 符號，`grep " irq_to_desc$" /proc/kallsyms` 找得到
 * （本機 CONFIG_KALLSYMS_ALL=n，所以只有 text 符號在 kallsyms 裡，data 符號查不到）。
 *
 * kallsyms_lookup_name() 自 5.7 起不再 EXPORT（commit 0bd476e6c671），
 * 標準作法是用 kprobe 拿它的位址。
 */
#ifndef _EXP_KSYM_H
#define _EXP_KSYM_H

#include <linux/kprobes.h>

static unsigned long (*exp_kallsyms_lookup_name)(const char *name);

static int exp_ksym_init(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	int ret = register_kprobe(&kp);

	if (ret < 0)
		return ret;
	exp_kallsyms_lookup_name = (void *)kp.addr;
	unregister_kprobe(&kp);
	return exp_kallsyms_lookup_name ? 0 : -ENOENT;
}

#define EXP_LOOKUP(name)  ((void *)exp_kallsyms_lookup_name(name))

#endif /* _EXP_KSYM_H */
