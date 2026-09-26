// SPDX-License-Identifier: GPL-2.0
/*
 * wbtest.c — 不接螢幕，用 DRM Writeback connector 驗證 RK3588 VOP2 的合成／縮放／格式轉換。
 *
 * 只用核心 uapi 標頭檔（./drm/）＋原生 ioctl，不依賴 libdrm。
 * 每個情境：建 dumb buffer 當圖層來源 → atomic commit（CRTC + 圖層 + Writeback）→
 *            等 out-fence → 把寫回的影像存成 /tmp/wb_<name>.raw，並印出 meta 給 analyze.py。
 *
 * 需要 DRM master：Xorg/logind 佔著時先 `chvt` 到別的 VT，讓 logind 放掉 master。
 * 用法：sudo ./wbtest [情境名稱...]   （不給就全跑）
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
/* 直接用核心原始碼的 uapi 標頭（沒經 headers_install），要自己拿掉 __user 標記 */
#define __user
#include "drm/drm.h"
#include "drm/drm_fourcc.h"

static int fd;

static int xioctl(unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

static double now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---------- 屬性 ---------- */
static uint32_t prop_find(uint32_t obj, uint32_t type, const char *name, uint64_t *val)
{
	struct drm_mode_obj_get_properties op = { .obj_id = obj, .obj_type = type };
	uint32_t ids[128];
	uint64_t vals[128];

	if (xioctl(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op) || op.count_props > 128)
		return 0;
	op.props_ptr = (uintptr_t)ids;
	op.prop_values_ptr = (uintptr_t)vals;
	if (xioctl(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
		return 0;
	for (uint32_t i = 0; i < op.count_props; i++) {
		struct drm_mode_get_property gp = { .prop_id = ids[i] };

		if (xioctl(DRM_IOCTL_MODE_GETPROPERTY, &gp))
			continue;
		if (!strcmp(gp.name, name)) {
			if (val)
				*val = vals[i];
			return ids[i];
		}
	}
	return 0;
}

/* enum 屬性：名稱 → 數值；找不到回 -1 */
static int64_t prop_enum(uint32_t prop, const char *name)
{
	struct drm_mode_get_property gp = { .prop_id = prop };
	struct drm_mode_property_enum en[32];

	if (xioctl(DRM_IOCTL_MODE_GETPROPERTY, &gp) || gp.count_enum_blobs > 32)
		return -1;
	gp.enum_blob_ptr = (uintptr_t)en;
	gp.count_values = 0;
	if (xioctl(DRM_IOCTL_MODE_GETPROPERTY, &gp))
		return -1;
	for (uint32_t i = 0; i < gp.count_enum_blobs; i++)
		if (getenv("DUMP_ENUMS"))
			printf("    enum[%u] value=%llu name='%s'\n", i, (unsigned long long)en[i].value, en[i].name);
	for (uint32_t i = 0; i < gp.count_enum_blobs; i++)
		if (!strcmp(en[i].name, name))
			return en[i].value;
	return -1;
}

/* ---------- atomic 請求 ---------- */
#define MAXP 96
static uint32_t a_objs[MAXP], a_cnts[MAXP], a_props[MAXP];
static uint64_t a_vals[MAXP];
static int a_nobj, a_nprop;

static void aadd(uint32_t obj, uint32_t prop, uint64_t val)
{
	if (!prop) {
		fprintf(stderr, "  (warn) missing property on obj %u\n", obj);
		return;
	}
	if (!a_nobj || a_objs[a_nobj - 1] != obj) {
		a_objs[a_nobj] = obj;
		a_cnts[a_nobj++] = 0;
	}
	a_props[a_nprop] = prop;
	a_vals[a_nprop++] = val;
	a_cnts[a_nobj - 1]++;
}

static int acommit(uint32_t flags)
{
	struct drm_mode_atomic a = {
		.flags = flags, .count_objs = a_nobj,
		.objs_ptr = (uintptr_t)a_objs, .count_props_ptr = (uintptr_t)a_cnts,
		.props_ptr = (uintptr_t)a_props, .prop_values_ptr = (uintptr_t)a_vals,
	};
	int r = xioctl(DRM_IOCTL_MODE_ATOMIC, &a);

	a_nobj = a_nprop = 0;
	return r ? -errno : 0;
}

/* ---------- dumb buffer ---------- */
struct buf {
	uint32_t w, h, fmt, handle, pitch, fb;
	uint64_t size;
	uint8_t *map;
};

static int mkbuf(struct buf *b, uint32_t w, uint32_t h, uint32_t fmt)
{
	struct drm_mode_create_dumb cd = { .width = w, .height = h };
	struct drm_mode_fb_cmd2 fc = { .width = w, .height = h, .pixel_format = fmt };
	struct drm_mode_map_dumb md = { 0 };

	switch (fmt) {
	case DRM_FORMAT_XRGB8888: case DRM_FORMAT_ARGB8888: cd.bpp = 32; break;
	case DRM_FORMAT_BGR888: cd.bpp = 24; break;
	case DRM_FORMAT_RGB565: cd.bpp = 16; break;
	case DRM_FORMAT_NV12: cd.bpp = 8; cd.height = h * 3 / 2; break;
	default: return -EINVAL;
	}
	if (xioctl(DRM_IOCTL_MODE_CREATE_DUMB, &cd))
		return -errno;
	b->w = w; b->h = h; b->fmt = fmt; b->handle = cd.handle; b->pitch = cd.pitch; b->size = cd.size;
	fc.handles[0] = cd.handle;
	fc.pitches[0] = cd.pitch;
	if (fmt == DRM_FORMAT_NV12) {
		fc.handles[1] = cd.handle;
		fc.pitches[1] = cd.pitch;
		fc.offsets[1] = cd.pitch * h;
	}
	if (xioctl(DRM_IOCTL_MODE_ADDFB2, &fc))
		return -errno;
	b->fb = fc.fb_id;
	md.handle = cd.handle;
	if (xioctl(DRM_IOCTL_MODE_MAP_DUMB, &md))
		return -errno;
	b->map = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (b->map == MAP_FAILED)
		return -errno;
	memset(b->map, 0x5a, cd.size);	/* 先填垃圾：沒被寫到的地方看得出來 */
	return 0;
}

static void rmbuf(struct buf *b)
{
	struct drm_mode_destroy_dumb dd = { .handle = b->handle };

	if (b->map)
		munmap(b->map, b->size);
	xioctl(DRM_IOCTL_MODE_RMFB, &b->fb);
	xioctl(DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
	memset(b, 0, sizeof(*b));
}

static void put32(struct buf *b, uint32_t x, uint32_t y, uint32_t argb)
{
	*(uint32_t *)(b->map + y * b->pitch + x * 4) = argb;
}

/* 圖樣 */
enum { P_GRAD, P_STRIPES_1PX, P_COLORBARS };
static void fill(struct buf *b, int pat)
{
	static const uint32_t bars[8] = { 0x000000, 0xffffff, 0xff0000, 0x00ff00,
					  0x0000ff, 0x00ffff, 0xff00ff, 0xffff00 };

	for (uint32_t y = 0; y < b->h; y++)
		for (uint32_t x = 0; x < b->w; x++) {
			uint32_t c;

			if (pat == P_GRAD)	/* R 隨 x、G 隨 y、B 固定 64 */
				c = ((x * 255 / (b->w - 1)) << 16) | ((y * 255 / (b->h - 1)) << 8) | 64;
			else if (pat == P_STRIPES_1PX)	/* R：奇偶欄 255/0；G：奇偶列 255/0 */
				c = ((x & 1) ? 0xff0000 : 0) | ((y & 1) ? 0x00ff00 : 0);
			else
				c = bars[x * 8 / b->w];
			put32(b, x, y, 0xff000000 | c);
		}
}

/* ---------- 資源 ---------- */
static uint32_t crtc_id, crtc_idx, wb_conn, prim_plane, ovl_plane;
static struct drm_mode_modeinfo wb_modes[4];
static int n_wb_modes;

static int find_resources(void)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t crtcs[8], conns[16], encs[16];
	struct drm_mode_get_plane_res pr = { 0 };
	uint32_t planes[32];

	if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res))
		return -errno;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)conns;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.count_fbs = 0;
	if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res))
		return -errno;
	printf("resources: %u crtcs, %u connectors, %u encoders\n", res.count_crtcs, res.count_connectors, res.count_encoders);

	uint32_t possible = 0;

	for (uint32_t i = 0; i < res.count_connectors; i++) {
		struct drm_mode_get_connector c = { .connector_id = conns[i] };
		uint32_t enc_ids[4];

		if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c))
			continue;
		printf("  connector %u type %u status %u modes %u\n", conns[i], c.connector_type, c.connection, c.count_modes);
		if (c.connector_type != DRM_MODE_CONNECTOR_WRITEBACK)
			continue;
		wb_conn = conns[i];
		n_wb_modes = c.count_modes > 4 ? 4 : c.count_modes;
		c.count_modes = n_wb_modes;
		c.modes_ptr = (uintptr_t)wb_modes;
		c.count_encoders = c.count_encoders > 4 ? 4 : c.count_encoders;
		c.encoders_ptr = (uintptr_t)enc_ids;
		c.count_props = 0;
		xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c);
		for (uint32_t e = 0; e < c.count_encoders; e++) {
			struct drm_mode_get_encoder ge = { .encoder_id = enc_ids[e] };

			if (!xioctl(DRM_IOCTL_MODE_GETENCODER, &ge))
				possible |= ge.possible_crtcs;
		}
	}
	if (!wb_conn) {
		fprintf(stderr, "no writeback connector (client cap?)\n");
		return -ENODEV;
	}
	if (!n_wb_modes) {
		/* 非 master 呼叫 GETCONNECTOR 不會觸發 fill_modes；照 vop2_wb_connector_get_modes() 自己造兩個 */
		for (int i = 0; i < 2; i++) {
			struct drm_mode_modeinfo *m = &wb_modes[i];

			memset(m, 0, sizeof(*m));
			m->clock = 148500 >> i;
			m->hdisplay = 1920 >> i; m->hsync_start = 1930 >> i; m->hsync_end = 1940 >> i; m->htotal = 1990 >> i;
			m->vdisplay = 1080 >> i; m->vsync_start = 1090 >> i; m->vsync_end = 1100 >> i; m->vtotal = 1110 >> i;
			m->vrefresh = 67;
			m->type = DRM_MODE_TYPE_DRIVER;
			snprintf(m->name, sizeof(m->name), "%ux%u", m->hdisplay, m->vdisplay);
		}
		n_wb_modes = 2;
		printf("  (wb connector reported 0 modes; using the 2 modes from vop2_wb_connector_get_modes())\n");
	}
	for (int m = 0; m < n_wb_modes; m++)
		printf("  wb mode[%d] %s %ux%u clock %u kHz htotal %u vtotal %u → %.3f Hz\n", m, wb_modes[m].name,
		       wb_modes[m].hdisplay, wb_modes[m].vdisplay, wb_modes[m].clock, wb_modes[m].htotal,
		       wb_modes[m].vtotal, wb_modes[m].clock * 1000.0 / wb_modes[m].htotal / wb_modes[m].vtotal);
	printf("  wb encoder possible_crtcs = 0x%x\n", possible);
	crtc_idx = 0;
	crtc_id = crtcs[crtc_idx];

	if (xioctl(DRM_IOCTL_MODE_GETPLANERESOURCES, &pr))
		return -errno;
	pr.plane_id_ptr = (uintptr_t)planes;
	if (xioctl(DRM_IOCTL_MODE_GETPLANERESOURCES, &pr))
		return -errno;
	for (uint32_t i = 0; i < pr.count_planes; i++) {
		struct drm_mode_get_plane gp = { .plane_id = planes[i] };
		uint64_t type = 99, zpos = 99;

		xioctl(DRM_IOCTL_MODE_GETPLANE, &gp);
		prop_find(planes[i], DRM_MODE_OBJECT_PLANE, "type", &type);
		prop_find(planes[i], DRM_MODE_OBJECT_PLANE, "zpos", &zpos);
		printf("  plane %u type %llu zpos %llu possible_crtcs 0x%x formats %u\n", planes[i],
		       (unsigned long long)type, (unsigned long long)zpos, gp.possible_crtcs, gp.count_format_types);
		if (!(gp.possible_crtcs & (1u << crtc_idx)))
			continue;
		if (type == 1 && !prim_plane)
			prim_plane = planes[i];
		else if (type == 0 && !ovl_plane)
			ovl_plane = planes[i];
	}
	if (getenv("OVL_PLANE"))	/* 指定疊加圖層，例如 Esmart3 */
		ovl_plane = strtoul(getenv("OVL_PLANE"), NULL, 0);
	printf("use crtc %u (idx %u), primary plane %u, overlay plane %u, wb connector %u\n",
	       crtc_id, crtc_idx, prim_plane, ovl_plane, wb_conn);
	return prim_plane ? 0 : -ENODEV;
}

static void plane_set(uint32_t plane, struct buf *b, uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch)
{
	uint32_t P = DRM_MODE_OBJECT_PLANE;

	aadd(plane, prop_find(plane, P, "FB_ID", NULL), b ? b->fb : 0);
	aadd(plane, prop_find(plane, P, "CRTC_ID", NULL), b ? crtc_id : 0);
	if (!b)
		return;
	aadd(plane, prop_find(plane, P, "SRC_X", NULL), 0);
	aadd(plane, prop_find(plane, P, "SRC_Y", NULL), 0);
	aadd(plane, prop_find(plane, P, "SRC_W", NULL), (uint64_t)b->w << 16);
	aadd(plane, prop_find(plane, P, "SRC_H", NULL), (uint64_t)b->h << 16);
	aadd(plane, prop_find(plane, P, "CRTC_X", NULL), cx);
	aadd(plane, prop_find(plane, P, "CRTC_Y", NULL), cy);
	aadd(plane, prop_find(plane, P, "CRTC_W", NULL), cw);
	aadd(plane, prop_find(plane, P, "CRTC_H", NULL), ch);
}

/* ---------- 情境 ---------- */
struct scen {
	const char *name;
	int mode;			/* wb_modes[] 索引 */
	uint32_t prim_w, prim_h;	/* 主圖層來源大小（放到整個畫面） */
	int prim_pat;
	int overlay;			/* 1：疊一塊 512x512 半透明藍 */
	const char *blend;		/* overlay 的 pixel blend mode；NULL 不設 */
	uint32_t wb_w, wb_h, wb_fmt;
	int frames;			/* 連續送幾次（量時間） */
};

static const struct scen scens[] = {
	{ "compose_cov",  0, 1920, 1080, P_GRAD, 1, "Coverage",       1920, 1080, DRM_FORMAT_ARGB8888, 1 },
	{ "compose_pre",  0, 1920, 1080, P_GRAD, 1, "Pre-multiplied", 1920, 1080, DRM_FORMAT_ARGB8888, 1 },
	{ "upscale2x",    0,  960,  540, P_STRIPES_1PX, 0, NULL,      1920, 1080, DRM_FORMAT_ARGB8888, 1 },
	{ "wb_xhalf",     0, 1920, 1080, P_STRIPES_1PX, 0, NULL,       960, 1080, DRM_FORMAT_ARGB8888, 1 },
	{ "wb_yhalf",     0, 1920, 1080, P_STRIPES_1PX, 0, NULL,      1920,  540, DRM_FORMAT_ARGB8888, 1 },
	{ "fmt_bgr888",   0, 1920, 1080, P_COLORBARS, 0, NULL,        1920, 1080, DRM_FORMAT_BGR888, 1 },
	{ "fmt_rgb565",   0, 1920, 1080, P_GRAD, 0, NULL,             1920, 1080, DRM_FORMAT_RGB565, 1 },
	{ "fmt_nv12",     0, 1920, 1080, P_COLORBARS, 0, NULL,        1920, 1080, DRM_FORMAT_NV12, 1 },
	{ "timing",       1,  960,  540, P_GRAD, 0, NULL,              960,  540, DRM_FORMAT_ARGB8888, 10 },
};

static int run(const struct scen *s)
{
	struct buf prim = { 0 }, ovl = { 0 }, wb = { 0 };
	const struct drm_mode_modeinfo *m = &wb_modes[s->mode];
	struct drm_mode_create_blob blob = { .data = (uintptr_t)m, .length = sizeof(*m) };
	uint32_t C = DRM_MODE_OBJECT_CRTC, K = DRM_MODE_OBJECT_CONNECTOR;
	int r, ret = 0;

	printf("\n=== %s: mode %ux%u, prim %ux%u pat %d, overlay %d (%s), wb %ux%u %.4s\n", s->name,
	       m->hdisplay, m->vdisplay, s->prim_w, s->prim_h, s->prim_pat, s->overlay,
	       s->blend ? s->blend : "-", s->wb_w, s->wb_h, (const char *)&s->wb_fmt);
	if ((r = mkbuf(&prim, s->prim_w, s->prim_h, DRM_FORMAT_XRGB8888)) ||
	    (s->overlay && (r = mkbuf(&ovl, 512, 512, DRM_FORMAT_ARGB8888))) ||
	    (r = mkbuf(&wb, s->wb_w, s->wb_h, s->wb_fmt))) {
		printf("  mkbuf failed: %d\n", r);
		return r;
	}
	fill(&prim, s->prim_pat);
	if (s->overlay)
		for (uint32_t y = 0; y < 512; y++)
			for (uint32_t x = 0; x < 512; x++)
				put32(&ovl, x, y, 0x800000ff);	/* A=0x80, R=0, G=0, B=0xff */
	if (xioctl(DRM_IOCTL_MODE_CREATEPROPBLOB, &blob)) {
		printf("  blob failed %d\n", errno);
		return -errno;
	}

	double t_first = 0, t_sum = 0;

	for (int f = 0; f < s->frames; f++) {
		int32_t out_fence = -1;
		struct buf *w = &wb;

		if (f == 0) {
			aadd(crtc_id, prop_find(crtc_id, C, "MODE_ID", NULL), blob.blob_id);
			aadd(crtc_id, prop_find(crtc_id, C, "ACTIVE", NULL), 1);
			plane_set(prim_plane, &prim, 0, 0, m->hdisplay, m->vdisplay);
			if (s->overlay) {
				plane_set(ovl_plane, &ovl, 200, 200, 512, 512);
				if (s->blend) {
					uint32_t bp = prop_find(ovl_plane, DRM_MODE_OBJECT_PLANE, "pixel blend mode", NULL);
					int64_t v = bp ? prop_enum(bp, s->blend) : -1;

					printf("  pixel blend mode prop %u: '%s' -> %lld\n", bp, s->blend, (long long)v);
					if (v >= 0)
						aadd(ovl_plane, bp, v);
					else
						printf("  (no pixel blend mode '%s')\n", s->blend);
				}
			}
		}
		aadd(wb_conn, prop_find(wb_conn, K, "CRTC_ID", NULL), crtc_id);
		aadd(wb_conn, prop_find(wb_conn, K, "WRITEBACK_FB_ID", NULL), w->fb);
		aadd(wb_conn, prop_find(wb_conn, K, "WRITEBACK_OUT_FENCE_PTR", NULL), (uintptr_t)&out_fence);

		double t0 = now_ms();

		r = acommit(f == 0 ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0);
		double t1 = now_ms();

		if (r) {
			printf("  commit failed: %d (%s)\n", r, strerror(-r));
			ret = r;
			break;
		}
		struct pollfd p = { .fd = out_fence, .events = POLLIN };
		int pr = poll(&p, 1, 2000);
		double t2 = now_ms();

		close(out_fence);
		printf("  frame %d: commit ioctl %.2f ms, fence signalled after %.2f ms (poll=%d)\n",
		       f, t1 - t0, t2 - t0, pr);
		if (f == 0)
			t_first = t2 - t0;
		else
			t_sum += t2 - t0;
		if (pr != 1) {
			ret = -ETIMEDOUT;
			break;
		}
	}
	if (s->frames > 1 && !ret)
		printf("  avg later frames: %.2f ms (vsync period %.2f ms)\n", t_sum / (s->frames - 1),
		       1000.0 * m->htotal * m->vtotal / (m->clock * 1000.0));
	(void)t_first;

	if (!ret) {
		char path[128];

		snprintf(path, sizeof(path), "/tmp/wb_%s.raw", s->name);
		FILE *fp = fopen(path, "wb");

		fwrite(wb.map, 1, wb.size, fp);
		fclose(fp);
		printf("  META name=%s w=%u h=%u pitch=%u fmt=%.4s size=%llu file=%s\n", s->name, wb.w, wb.h,
		       wb.pitch, (const char *)&wb.fmt, (unsigned long long)wb.size, path);
	}

	/* DUMP_DEBUGFS=1：關掉之前，把驅動眼中的狀態印出來（summary 與 atomic state） */
	if (!ret && getenv("DUMP_DEBUGFS")) {
		fflush(stdout);
		system("echo '--- clk'; grep -E ' dclk_vp0 | dclk_vop0 | aclk_vop |dclk_vp0_src|dclk_vop0_src' /sys/kernel/debug/clk/clk_summary; echo '--- summary'; cat /sys/kernel/debug/dri/0/summary; "
		       "echo '--- state (planes on crtc)'; grep -A14 -E '^plane\\[' /sys/kernel/debug/dri/0/state | "
		       "grep -E 'plane\\[|crtc=|fb=|format=|blend|alpha|zpos|crtc-pos|src-pos'");
	}

	/* 關掉：planes、writeback、CRTC */
	plane_set(prim_plane, NULL, 0, 0, 0, 0);
	if (s->overlay)
		plane_set(ovl_plane, NULL, 0, 0, 0, 0);
	aadd(wb_conn, prop_find(wb_conn, K, "CRTC_ID", NULL), 0);
	aadd(crtc_id, prop_find(crtc_id, C, "ACTIVE", NULL), 0);
	aadd(crtc_id, prop_find(crtc_id, C, "MODE_ID", NULL), 0);
	r = acommit(DRM_MODE_ATOMIC_ALLOW_MODESET);
	if (r)
		printf("  disable commit failed: %d\n", r);
	xioctl(DRM_IOCTL_MODE_DESTROYPROPBLOB, &(struct drm_mode_destroy_blob){ .blob_id = blob.blob_id });
	rmbuf(&prim);
	if (s->overlay)
		rmbuf(&ovl);
	rmbuf(&wb);
	return ret;
}

int main(int argc, char **argv)
{
	struct drm_set_client_cap caps[] = {
		{ DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 },
		{ DRM_CLIENT_CAP_ATOMIC, 1 },
		{ DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1 },
	};

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open card0");
		return 1;
	}
	if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0))
		printf("SET_MASTER: %s（若已自動成為 master 可忽略）\n", strerror(errno));
	for (unsigned i = 0; i < 3; i++)
		if (xioctl(DRM_IOCTL_SET_CLIENT_CAP, &caps[i]))
			printf("client cap %llu: %s\n", (unsigned long long)caps[i].capability, strerror(errno));
	if (find_resources())
		return 1;
	int fails = 0;

	for (unsigned i = 0; i < sizeof(scens) / sizeof(scens[0]); i++) {
		int want = argc < 2;

		for (int a = 1; a < argc; a++)
			want |= !strcmp(argv[a], scens[i].name);
		if (want && run(&scens[i]))
			fails++;
	}
	ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
	close(fd);
	printf("\ndone, %d failed\n", fails);
	return fails ? 2 : 0;
}
