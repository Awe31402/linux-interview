/*
 * exp01_regs.c — 實驗 1.1
 *
 * 問題：TRM 第 36 章列的那些暫存器，真的在那個位址上嗎？
 *
 * 作法：用 /dev/mem 把 NPU 三顆核心的暫存器區間 mmap 進來，直接讀。
 *
 * ⚠️ 讀之前一定要先開電，否則匯流排會沒有回應，整台板子可能當掉：
 *      echo on | sudo tee /sys/kernel/debug/rknpu/power
 *
 *   gcc -O1 -o exp01_regs exp01_regs.c
 *   sudo ./exp01_regs
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

/* drivers/rknpu/include/rknpu_ioctl.h */
struct rknpu_action { uint32_t flags; uint32_t value; };
#define RKNPU_GET_HW_VERSION  0
#define DRM_IOCTL_RKNPU_ACTION _IOWR('d', 0x40 + 0x00, struct rknpu_action)

/* rk3588s.dtsi:3450 的三段 reg */
static const uint64_t core_base[3] = { 0xfdab0000, 0xfdac0000, 0xfdad0000 };
#define CORE_SIZE 0x10000

struct regdef { uint32_t off; const char *name; const char *note; };

static const struct regdef regs[] = {
	/* offset  TRM 36.4.2 的名字                    備註 */
	{ 0x0000, "(TRM 未記載)",                "驅動叫 RKNPU_OFFSET_VERSION" },
	{ 0x0004, "(TRM 未記載)",                "驅動叫 RKNPU_OFFSET_VERSION_NUM" },
	{ 0x0008, "RKNN_pc_operation_enable",    "Operation Enable，寫 1 開跑" },
	{ 0x0010, "RKNN_pc_base_address",        "指令清單位址" },
	{ 0x0014, "RKNN_pc_register_amounts",    "每個 task 幾筆設定" },
	{ 0x0020, "RKNN_pc_interrupt_mask",      "TRM 說 reset value = 0x0001FFFF" },
	{ 0x0024, "RKNN_pc_interrupt_clear",     "" },
	{ 0x0028, "RKNN_pc_interrupt_status",    "" },
	{ 0x002C, "RKNN_pc_interrupt_raw_status","" },
	{ 0x0030, "RKNN_pc_task_con",            "" },
	{ 0x0034, "RKNN_pc_task_dma_base_addr",  "" },
	{ 0x003C, "RKNN_pc_task_status",         "" },
	{ 0xF008, "RKNN_global_operation_enable","位址表說 GLOBAL 只到 0xF004" },
};

int main(void)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "開 /dev/mem 失敗: %s（要 root）\n", strerror(errno));
		return 1;
	}

	volatile uint32_t *core[3];
	for (int c = 0; c < 3; c++) {
		void *p = mmap(NULL, CORE_SIZE, PROT_READ, MAP_SHARED, fd, core_base[c]);
		if (p == MAP_FAILED) {
			fprintf(stderr, "mmap core%d (0x%llx) 失敗: %s\n",
				c, (unsigned long long)core_base[c], strerror(errno));
			return 1;
		}
		core[c] = p;
	}

	printf("%-8s %-30s %-12s %-12s %-12s\n",
	       "offset", "TRM 36.4.2 name", "core0", "core1", "core2");
	printf("-------------------------------------------------------------------------------\n");
	for (unsigned i = 0; i < sizeof(regs)/sizeof(regs[0]); i++) {
		printf("0x%04x   %-30s", regs[i].off, regs[i].name);
		for (int c = 0; c < 3; c++)
			printf(" 0x%08x  ", core[c][regs[i].off / 4]);
		printf("\n");
		if (regs[i].note[0])
			printf("         └─ %s\n", regs[i].note);
	}

	/* 閉迴圈：問驅動「硬體版本是多少」，看它是不是就是 0x0000 讀到的那個數 */
	printf("\n--- 交叉比對：驅動回報的硬體版本 ---\n");
	printf("rknpu_job.c:892  version = REG_READ(0x0) + (REG_READ(0x4) & 0xffff)\n");
	printf("                         = 0x%08x + 0x%04x = 0x%08x\n",
	       core[0][0], core[0][1] & 0xffff, core[0][0] + (core[0][1] & 0xffff));

	int drm = open("/dev/dri/card1", O_RDWR);
	if (drm >= 0) {
		struct rknpu_action a = { .flags = RKNPU_GET_HW_VERSION, .value = 0 };
		if (ioctl(drm, DRM_IOCTL_RKNPU_ACTION, &a) == 0)
			printf("ioctl(ACTION/GET_HW_VERSION) -> 0x%08x  %s\n", a.value,
			       a.value == core[0][0] + (core[0][1] & 0xffff) ? "✓ 一致" : "✗ 不一致");
		else
			printf("ioctl 失敗: %s\n", strerror(errno));
		close(drm);
	} else {
		printf("開 /dev/dri/card1 失敗: %s\n", strerror(errno));
	}

	for (int c = 0; c < 3; c++)
		munmap((void *)core[c], CORE_SIZE);
	close(fd);
	return 0;
}
