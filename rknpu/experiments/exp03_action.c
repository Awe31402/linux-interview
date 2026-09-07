/*
 * exp03_action.c — 實驗 3.2 / 3.3
 *
 * 問題：
 *   (a) 那六個 ioctl 的號碼到底是多少？（strace 會叫錯名字，得自己算）
 *   (b) ACTION 底下那 26 個子命令，實際問下去會拿到什麼？
 *
 * 不需要任何函式庫，結構從 rknpu_ioctl.h 抄過來。
 *
 *   gcc -O1 -o exp03_action exp03_action.c
 *   sudo ./exp03_action
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>

/* ---- 照抄自 drivers/rknpu/include/rknpu_ioctl.h ---- */
struct rknpu_action      { uint32_t flags; uint32_t value; };
struct rknpu_mem_create  { uint32_t handle; uint32_t flags; uint64_t size;
                           uint64_t obj_addr; uint64_t dma_addr; uint64_t sram_size;
                           int32_t iommu_domain_id; uint32_t core_mask; };
struct rknpu_mem_map     { uint32_t handle; uint32_t reserved; uint64_t offset; };
struct rknpu_mem_destroy { uint32_t handle; uint32_t reserved; uint64_t obj_addr; };
struct rknpu_mem_sync    { uint32_t flags; uint32_t reserved; uint64_t obj_addr;
                           uint64_t offset; uint64_t size; };
struct rknpu_subcore_task { uint32_t task_start, task_number; };
struct rknpu_submit      { uint32_t flags, timeout, task_start, task_number, task_counter;
                           int32_t priority; uint64_t task_obj_addr;
                           uint32_t iommu_domain_id, reserved;
                           uint64_t task_base_addr; int64_t hw_elapse_time;
                           uint32_t core_mask; int32_t fence_fd;
                           struct rknpu_subcore_task subcore_task[5]; };

#define RKNPU_ACTION      0x00
#define RKNPU_SUBMIT      0x01
#define RKNPU_MEM_CREATE  0x02
#define RKNPU_MEM_MAP     0x03
#define RKNPU_MEM_DESTROY 0x04
#define RKNPU_MEM_SYNC    0x05
#define DRM_COMMAND_BASE  0x40

#define RKNPU_IOCTL(nr, type) _IOWR('d', DRM_COMMAND_BASE + (nr), type)

/* enum e_rknpu_action */
static const struct { int id; const char *name; int is_set; } actions[] = {
	{  0, "GET_HW_VERSION",        0 },
	{  1, "GET_DRV_VERSION",       0 },
	{  2, "GET_FREQ",              0 },
	{  3, "SET_FREQ",              1 },
	{  4, "GET_VOLT",              0 },
	{  5, "SET_VOLT",              1 },
	{  7, "GET_BW_PRIORITY",       0 },
	{  9, "GET_BW_EXPECT",         0 },
	{ 11, "GET_BW_TW",             0 },
	{ 14, "GET_DT_WR_AMOUNT",      0 },
	{ 15, "GET_DT_RD_AMOUNT",      0 },
	{ 16, "GET_WT_RD_AMOUNT",      0 },
	{ 17, "GET_TOTAL_RW_AMOUNT",   0 },
	{ 18, "GET_IOMMU_EN",          0 },
	{ 20, "POWER_ON",              1 },
	{ 21, "POWER_OFF",             1 },
	{ 22, "GET_TOTAL_SRAM_SIZE",   0 },
	{ 23, "GET_FREE_SRAM_SIZE",    0 },
	{ 24, "GET_IOMMU_DOMAIN_ID",   0 },
};

int main(void)
{
	/* ---- (a) 六個 ioctl 的號碼 ---- */
	struct { const char *n; unsigned long cmd; size_t sz; } cmds[] = {
		{ "RKNPU_ACTION",      RKNPU_IOCTL(RKNPU_ACTION,      struct rknpu_action),      sizeof(struct rknpu_action) },
		{ "RKNPU_SUBMIT",      RKNPU_IOCTL(RKNPU_SUBMIT,      struct rknpu_submit),      sizeof(struct rknpu_submit) },
		{ "RKNPU_MEM_CREATE",  RKNPU_IOCTL(RKNPU_MEM_CREATE,  struct rknpu_mem_create),  sizeof(struct rknpu_mem_create) },
		{ "RKNPU_MEM_MAP",     RKNPU_IOCTL(RKNPU_MEM_MAP,     struct rknpu_mem_map),     sizeof(struct rknpu_mem_map) },
		{ "RKNPU_MEM_DESTROY", RKNPU_IOCTL(RKNPU_MEM_DESTROY, struct rknpu_mem_destroy), sizeof(struct rknpu_mem_destroy) },
		{ "RKNPU_MEM_SYNC",    RKNPU_IOCTL(RKNPU_MEM_SYNC,    struct rknpu_mem_sync),    sizeof(struct rknpu_mem_sync) },
	};

	printf("=== 六個 ioctl 的號碼（拿去比對 strace）===\n");
	printf("%-20s %-12s %-6s %-6s %s\n", "name", "cmd", "type", "nr", "size");
	for (unsigned i = 0; i < 6; i++)
		printf("%-20s 0x%08lx   0x%02x   0x%02x   %zu (0x%02zx)\n",
		       cmds[i].n, cmds[i].cmd,
		       (unsigned)_IOC_TYPE(cmds[i].cmd),
		       (unsigned)_IOC_NR(cmds[i].cmd),
		       cmds[i].sz, cmds[i].sz);

	/* ---- (b) 問 ACTION ---- */
	int fd = open("/dev/dri/card1", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "\n開 /dev/dri/card1 失敗: %s（要 root，或節點編號不同）\n",
			strerror(errno));
		return 1;
	}

	printf("\n=== ACTION 的子命令，一個一個問 ===\n");
	printf("%-3s %-22s %-6s %s\n", "id", "name", "ret", "value");
	for (unsigned i = 0; i < sizeof(actions)/sizeof(actions[0]); i++) {
		struct rknpu_action a = { .flags = actions[i].id, .value = 0 };
		int r = ioctl(fd, RKNPU_IOCTL(RKNPU_ACTION, struct rknpu_action), &a);
		printf("%-3d %-22s %-6s ", actions[i].id, actions[i].name,
		       r == 0 ? "ok" : "FAIL");
		if (r == 0)
			printf("%u (0x%08x)\n", a.value, a.value);
		else
			printf("-- %s%s\n", strerror(errno),
			       actions[i].is_set ? "   ← 預期如此" : "");
	}
	close(fd);
	return 0;
}
