/*
 * exp00_whoami.c — 實驗 0.1
 *
 * 問題：/dev/dri/ 底下一堆節點，哪個是 NPU？而且 NPU 真的不畫圖嗎？
 *
 * 作法：對每個 card 節點問兩句話
 *   1. DRM_IOCTL_VERSION          —— 你叫什麼名字？
 *   2. DRM_IOCTL_MODE_GETRESOURCES —— 你有幾個螢幕輸出？
 *
 * 不需要 libdrm，結構自己宣告（順便看清楚 ioctl 編號怎麼組出來的）。
 *
 *   gcc -O1 -o exp00_whoami exp00_whoami.c
 *   sudo ./exp00_whoami
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>

/* DRM 的 ioctl type 是 'd' (0x64) */
struct drm_version {
	int version_major, version_minor, version_patchlevel;
	size_t name_len;  char *name;
	size_t date_len;  char *date;
	size_t desc_len;  char *desc;
};
struct drm_mode_card_res {
	uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
	uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
	uint32_t min_width, max_width, min_height, max_height;
};

#define DRM_IOCTL_VERSION            _IOWR('d', 0x00, struct drm_version)
#define DRM_IOCTL_MODE_GETRESOURCES  _IOWR('d', 0xA0, struct drm_mode_card_res)

int main(void)
{
	char path[64];
	printf("%-18s %-14s %-9s %-10s %s\n",
	       "node", "driver", "version", "connectors", "does KMS?");
	printf("--------------------------------------------------------------------\n");

	for (int i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		int fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		char name[64] = {0}, date[64] = {0}, desc[128] = {0};
		struct drm_version v = {0};
		v.name = name; v.name_len = sizeof(name) - 1;
		v.date = date; v.date_len = sizeof(date) - 1;
		v.desc = desc; v.desc_len = sizeof(desc) - 1;
		if (ioctl(fd, DRM_IOCTL_VERSION, &v) < 0) {
			printf("%-18s (VERSION 失敗: %s)\n", path, strerror(errno));
			close(fd);
			continue;
		}

		char ver[16];
		snprintf(ver, sizeof(ver), "%d.%d.%d",
			 v.version_major, v.version_minor, v.version_patchlevel);

		/* 問它有幾個螢幕輸出。不支援 KMS 的驅動會直接拒絕。 */
		struct drm_mode_card_res r = {0};
		int ok = ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &r) == 0;

		printf("%-18s %-14s %-9s ", path, name, ver);
		if (ok)
			printf("%-10u yes\n", r.count_connectors);
		else
			printf("%-10s NO  <- ioctl rejected: %s\n", "-", strerror(errno));

		printf("%-18s   desc: %s\n", "", desc);
		close(fd);
	}
	return 0;
}
