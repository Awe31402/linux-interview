// SPDX-License-Identifier: GPL-2.0
/*
 * dyndbg_lab.ko —— 卷2 第3章 Q13：內核的動態輸出（dynamic debug）
 *
 * 模組裡放了 5 條 pr_debug()，分散在 3 個函式裡。
 * 在 CONFIG_DYNAMIC_DEBUG=y 的核心上，這 5 條在編譯時被登記成
 * struct _ddebug（放在 __dyndbg section），預設**一條都不會印**，
 * 執行期可以用 file/func/line/module 四種選擇器逐條打開。
 *
 * 用法：
 *   sudo insmod dyndbg_lab.ko                       # 什麼都不會印
 *   sudo grep dyndbg_lab /proc/dynamic_debug/control
 *   echo 'module dyndbg_lab +p'   | sudo tee /proc/dynamic_debug/control
 *   echo 'func dyndbg_two +pflmt' | sudo tee /proc/dynamic_debug/control
 *   echo 1 | sudo tee /sys/module/dyndbg_lab/parameters/fire
 *   sudo insmod dyndbg_lab.ko dyndbg=+pfl           # 載入時就打開
 */
#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("dynamic debug lab (ch12 Q13)");

static void dyndbg_one(int v)
{
	pr_debug("dyndbg_one: 第 1 條，v=%d\n", v);
	pr_debug("dyndbg_one: 第 2 條，v*2=%d\n", v * 2);
}

static void dyndbg_two(int v)
{
	pr_debug("dyndbg_two: 第 3 條，v=%d\n", v);
	pr_debug("dyndbg_two: 第 4 條，v=%d\n", v);
}

static void dyndbg_three(void)
{
	pr_debug("dyndbg_three: 第 5 條，沒有參數\n");
}

static void fire_all(void)
{
	pr_info("dyndbg_lab: 觸發 5 條 pr_debug()（有沒有印出來看 control 設定）\n");
	dyndbg_one(1);
	dyndbg_two(2);
	dyndbg_three();
}

static int fire_set(const char *val, const struct kernel_param *kp)
{
	fire_all();
	return 0;
}
static const struct kernel_param_ops fire_ops = { .set = fire_set };
module_param_cb(fire, &fire_ops, NULL, 0644);

static int __init dyndbg_lab_init(void)
{
	pr_info("dyndbg_lab: init（CONFIG_DYNAMIC_DEBUG=%s）\n",
#ifdef CONFIG_DYNAMIC_DEBUG
		"y");
#else
		"n");
#endif
	fire_all();
	return 0;
}

static void __exit dyndbg_lab_exit(void)
{
	pr_info("dyndbg_lab: exit\n");
}

module_init(dyndbg_lab_init);
module_exit(dyndbg_lab_exit);
