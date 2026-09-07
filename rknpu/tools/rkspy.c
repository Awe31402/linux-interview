/* rkspy.c — LD_PRELOAD shim: 攔 rknpu ioctl，把 regcmd 挖出來 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#define RKNPU_ACTION     0x00
#define RKNPU_SUBMIT     0x01
#define RKNPU_MEM_CREATE 0x02
#define RKNPU_MEM_MAP    0x03
#define RKNPU_MEM_DESTROY 0x04
#define RKNPU_MEM_SYNC   0x05
#define DRM_COMMAND_BASE 0x40

struct rknpu_mem_create { uint32_t handle; uint32_t flags; uint64_t size;
	uint64_t obj_addr; uint64_t dma_addr; uint64_t sram_size;
	int32_t iommu_domain_id; uint32_t core_mask; };
struct rknpu_mem_map { uint32_t handle; uint32_t reserved; uint64_t offset; };
struct rknpu_task { uint32_t flags, op_idx, enable_mask, int_mask, int_clear,
	int_status, regcfg_amount, regcfg_offset; uint64_t regcmd_addr; } __attribute__((packed));
struct rknpu_subcore_task { uint32_t task_start, task_number; };
struct rknpu_submit { uint32_t flags, timeout, task_start, task_number, task_counter;
	int32_t priority; uint64_t task_obj_addr; uint32_t iommu_domain_id, reserved;
	uint64_t task_base_addr; int64_t hw_elapse_time; uint32_t core_mask; int32_t fence_fd;
	struct rknpu_subcore_task subcore_task[5]; };

/* enum e_rknpu_mem_type — rknpu_ioctl.h:53 */
static const struct { unsigned bit; const char *nm; } memflags[] = {
	{ 1<<0,  "NON_CONTIG" }, { 1<<1,  "CACHEABLE" }, { 1<<2,  "WRITE_COMBINE" },
	{ 1<<3,  "KERNEL_MAPPING" }, { 1<<4, "IOMMU" }, { 1<<5, "ZEROING" },
	{ 1<<6,  "SECURE" }, { 1<<7, "DMA32" }, { 1<<8, "TRY_SRAM" },
	{ 1<<9,  "TRY_NBUF" }, { 1<<10, "IOMMU_LIMIT_IOVA_ALIGN" },
};
static void flagstr(unsigned f, char *out, size_t n)
{
	out[0]=0;
	if (!(f & 1)) snprintf(out, n, "CONTIG");     /* bit0=0 就是連續 */
	for (unsigned i=0;i<sizeof(memflags)/sizeof(memflags[0]);i++)
		if (f & memflags[i].bit) {
			if (out[0]) strncat(out, "|", n-strlen(out)-1);
			strncat(out, memflags[i].nm, n-strlen(out)-1);
		}
	if (!out[0]) snprintf(out, n, "(none)");
}

#define MAXB 512
static struct { uint32_t handle, flags; uint64_t obj_addr, dma_addr, size, moff, va; } bufs[MAXB];
static int nbuf;
static int submit_seen;

static void *(*real_mmap)(void*,size_t,int,int,int,off_t);
static int (*real_ioctl)(int,unsigned long,...);
static FILE *lg;

/* 用 /proc/self/pagemap 把實體頁框號挖出來，證明實體是散的但 IOVA 是連續的。
 * 必須在緩衝區還活著的時候呼叫（程式結束時早就 munmap 了）。需要 root。 */
static void dump_physical(void)
{
	static int done = 0;
	if (done++) return;
	int pm = open("/proc/self/pagemap", O_RDONLY);
	if (pm < 0) {
		fprintf(lg,"\n（開 /proc/self/pagemap 失敗：%s —— 要 root 才看得到實體位址）\n",
			strerror(errno));
		return;
	}
	long ps = sysconf(_SC_PAGESIZE);
	fprintf(lg,"\n=== 實體頁面真的連續嗎？（每塊取前 8 頁）===\n");
	for (int i=0;i<nbuf;i++){
		if(!bufs[i].va) continue;
		fprintf(lg,"h=%u  IOVA=0x%llx  size=%llu\n",
			bufs[i].handle,(unsigned long long)bufs[i].dma_addr,
			(unsigned long long)bufs[i].size);
		unsigned long long prev=0; int shown=0;
		int npg = bufs[i].size/ps; if(npg>8) npg=8;
		for(int k=0;k<npg;k++){
			unsigned long long va = bufs[i].va + (unsigned long long)k*ps;
			unsigned long long ent=0;
			if(pread(pm,&ent,8,(va/ps)*8)!=8) break;
			unsigned long long iova = bufs[i].dma_addr + (unsigned long long)k*ps;
			if(!(ent>>63)){ fprintf(lg,"   +%-2d IOVA=0x%010llx -> (尚未配實體頁)\n",k,iova); continue; }
			unsigned long long pa = (ent & ((1ULL<<55)-1)) * ps;
			fprintf(lg,"   +%-2d IOVA=0x%010llx -> 實體 0x%010llx%s\n", k, iova, pa,
				(shown && pa != prev + (unsigned long long)ps) ? "   ← 實體不連續" : "");
			prev = pa; shown = 1;
		}
	}
	close(pm);
	fflush(lg);
}

static void summary(void)
{
	if(!lg || !nbuf) return;
	fprintf(lg,"\n================ 記憶體總表 ================\n");
	fprintf(lg,"%-3s %-10s %-6s %-26s %-12s %-14s %s\n",
		"h","size","flags","flags 解開","dma_addr","mmap offset","user VA");
	for(int i=0;i<nbuf;i++){
		char fs[96]; flagstr(bufs[i].flags, fs, sizeof(fs));
		fprintf(lg,"%-3u %-10llu 0x%02x   %-26s 0x%010llx 0x%012llx 0x%llx\n",
			bufs[i].handle,(unsigned long long)bufs[i].size,bufs[i].flags,fs,
			(unsigned long long)bufs[i].dma_addr,
			(unsigned long long)bufs[i].moff,
			(unsigned long long)bufs[i].va);
	}
	fprintf(lg,"\n同一塊記憶體的三個名字：handle（使用者的號碼牌）／"
		   "dma_addr（硬體看的位址）／user VA（mmap 後 CPU 用的指標）\n");

	fflush(lg);
}

static void init(void){
	if(!real_ioctl) real_ioctl = dlsym(RTLD_NEXT,"ioctl");
	if(!real_mmap)  real_mmap  = dlsym(RTLD_NEXT,"mmap");
	if(!lg){ lg = fopen(getenv("RKSPY_LOG")?:"/tmp/rkspy.log","w"); atexit(summary); }
}
/* 用 dma_addr 找出哪塊 buffer 裝著這段位址 */
static int find_by_dma(uint64_t d){
	for(int i=0;i<nbuf;i++) if(bufs[i].dma_addr && d>=bufs[i].dma_addr && d<bufs[i].dma_addr+bufs[i].size) return i;
	return -1;
}
static int find_by_obj(uint64_t o){
	for(int i=0;i<nbuf;i++) if(bufs[i].obj_addr==o) return i;
	return -1;
}

static void note_map(void *r, off_t off){
	if(r==MAP_FAILED || (uint64_t)off < 0x100000000ULL) return;
	for(int i=0;i<nbuf;i++) if(bufs[i].moff==(uint64_t)off){
		bufs[i].va=(uint64_t)r;
		fprintf(lg,"  -> mmap 綁定 h=%u va=0x%llx (off=0x%llx)\n",
			bufs[i].handle,(unsigned long long)r,(unsigned long long)off);
	}
}
void *mmap(void *a,size_t l,int p,int f,int fd,off_t off){
	init(); void *r = real_mmap(a,l,p,f,fd,off); note_map(r,off); return r;
}
void *mmap64(void *a,size_t l,int p,int f,int fd,off_t off){
	init(); void *r = real_mmap(a,l,p,f,fd,off); note_map(r,off); return r;
}


/* 在一個 task 的 regcmd 裡找某個暫存器 offset，回傳它被寫入的值。
 * regcmd 每筆 8 bytes：[15:0]=offset、[47:16]=值、[63:48]=區塊標籤。 */
static int regcmd_find(struct rknpu_task *tk, unsigned want, uint32_t *out)
{
	int ri = find_by_dma(tk->regcmd_addr);
	if (ri < 0 || !bufs[ri].va) return 0;
	uint64_t *e = (uint64_t *)(bufs[ri].va + (tk->regcmd_addr - bufs[ri].dma_addr));
	uint32_t n = tk->regcfg_amount + 4;
	for (uint32_t i = 0; i < n; i++) {
		if ((unsigned)(e[i] & 0xffff) == want) {
			*out = (uint32_t)((e[i] >> 16) & 0xffffffffULL);
			return 1;
		}
	}
	return 0;
}

/* 從 CNA 的 data_size 暫存器還原每一層的張量形狀。
 *   0x1020 RKNN_cna_data_size0: [26:16]=datain_width, [10:0]=datain_height
 *   0x1024 RKNN_cna_data_size1: [29:16]=datain_channel_real, [15:0]=datain_channel
 */
static void dump_shapes(struct rknpu_submit *s, struct rknpu_task *t)
{
	fprintf(lg,"\n  === 每個 task 的卷積參數（從 CNA 暫存器還原）===\n");
	fprintf(lg,"  0x1020 data_size0: [26:16]=width [10:0]=height   0x1024 data_size1: [29:16]=ch_real [15:0]=ch\n");
	fprintf(lg,"  0x1010 conv_con2 : [13:4]=feature_grains          0x1014 conv_con3 : [5:3]=y_stride [2:0]=x_stride\n");
	fprintf(lg,"  0x1038 wt_size2  : [28:24]=k_w [20:16]=k_h        0x1068 pad_con0 : [7:4]=pad_left [3:0]=pad_top\n");
	fprintf(lg,"  %-5s %-6s %5s %5s %6s %6s %6s %4s %4s %4s %4s %4s %4s\n",
		"task","op","W","H","ch","chreal","grains","k_w","k_h","sx","sy","pl","pt");
	int prev_op = -1;
	for (uint32_t k = 0; k < s->task_number; k++) {
		struct rknpu_task *tk = &t[s->task_start + k];
		uint32_t sz0 = 0, sz1 = 0;
		int has0 = regcmd_find(tk, 0x1020, &sz0);
		int has1 = regcmd_find(tk, 0x1024, &sz1);
		if ((int)tk->op_idx != prev_op && prev_op >= 0)
			fprintf(lg,"  %s\n","  ----");
		prev_op = tk->op_idx;
		uint32_t cc2=0, cc3=0, ws2=0, pad=0;
		int hcc2 = regcmd_find(tk, 0x1010, &cc2);
		int hcc3 = regcmd_find(tk, 0x1014, &cc3);
		int hws2 = regcmd_find(tk, 0x1038, &ws2);
		int hpad = regcmd_find(tk, 0x1068, &pad);

		fprintf(lg,"  %-5u %-6u", s->task_start + k, tk->op_idx);
		if (has0) fprintf(lg," %5u %5u", (sz0 >> 16) & 0x7ff, sz0 & 0x7ff);
		else      fprintf(lg," %5s %5s", "-", "-");
		if (has1) fprintf(lg," %6u %6u", sz1 & 0xffff, (sz1 >> 16) & 0x3fff);
		else      fprintf(lg," %6s %6s", "-", "-");
		if (hcc2) fprintf(lg," %6u", (cc2 >> 4) & 0x3ff);
		else      fprintf(lg," %6s", "-");
		if (hws2) fprintf(lg," %4u %4u", (ws2 >> 24) & 0x1f, (ws2 >> 16) & 0x1f);
		else      fprintf(lg," %4s %4s", "-", "-");
		if (hcc3) fprintf(lg," %4u %4u", cc3 & 0x7, (cc3 >> 3) & 0x7);
		else      fprintf(lg," %4s %4s", "-", "-");
		if (hpad) fprintf(lg," %4u %4u", (pad >> 4) & 0xf, pad & 0xf);
		else      fprintf(lg," %4s %4s", "-", "-");
		fprintf(lg,"\n");
	}
	fflush(lg);
}

static void dump_submit(struct rknpu_submit *s){
	fprintf(lg,"\n================ SUBMIT #%d ================\n",++submit_seen);
	fprintf(lg,"flags=0x%x task_start=%u task_number=%u core_mask=0x%x fence_fd=%d\n",
		s->flags,s->task_start,s->task_number,s->core_mask,s->fence_fd);
	fprintf(lg,"task_obj_addr=0x%llx  task_base_addr=0x%llx  iommu_domain_id=%d\n",
		(unsigned long long)s->task_obj_addr,(unsigned long long)s->task_base_addr,s->iommu_domain_id);
	for(int i=0;i<5;i++)
		fprintf(lg,"  subcore[%d]: start=%u number=%u\n",i,
			s->subcore_task[i].task_start,s->subcore_task[i].task_number);

	dump_physical();

	int ti = find_by_obj(s->task_obj_addr);
	if(ti<0){ fprintf(lg,"  !! 找不到 task buffer\n"); return; }
	if(!bufs[ti].va){ fprintf(lg,"  !! task buffer 沒 mmap\n"); return; }
	struct rknpu_task *t = (struct rknpu_task*)bufs[ti].va;
	fprintf(lg,"  task buffer: va=0x%llx dma=0x%llx size=%llu\n",
		(unsigned long long)bufs[ti].va,(unsigned long long)bufs[ti].dma_addr,
		(unsigned long long)bufs[ti].size);

	const char *rawenv = getenv("RKSPY_RAW");
	uint32_t rawn = rawenv ? (uint32_t)atoi(rawenv) : 3;
	uint32_t n = s->task_number; if(n>rawn) n=rawn;
	for(uint32_t k=0;k<n;k++){
		struct rknpu_task *tk = &t[s->task_start+k];
		fprintf(lg,"\n  --- task[%u] op_idx=%u regcfg_amount=%u regcfg_offset=%u regcmd_addr=0x%llx int_mask=0x%x\n",
			s->task_start+k,tk->op_idx,tk->regcfg_amount,tk->regcfg_offset,
			(unsigned long long)tk->regcmd_addr,tk->int_mask);
		int ri = find_by_dma(tk->regcmd_addr);
		if(ri<0){ fprintf(lg,"      (regcmd 不在已知 buffer 內)\n"); continue; }
		if(!bufs[ri].va){ fprintf(lg,"      (regcmd buffer 沒 mmap)\n"); continue; }
		uint64_t off = tk->regcmd_addr - bufs[ri].dma_addr;
		uint32_t *w = (uint32_t*)(bufs[ri].va + off);
		uint32_t cnt = tk->regcfg_amount + 4;   /* RKNPU_PC_DATA_EXTRA_AMOUNT */
		if(cnt>24) cnt=24;                       /* 只印開頭 */
		fprintf(lg,"      regcmd buffer va=0x%llx dma=0x%llx off=0x%llx\n",
			(unsigned long long)bufs[ri].va,(unsigned long long)bufs[ri].dma_addr,
			(unsigned long long)off);
		unsigned char *b = (unsigned char*)w;
		for(uint32_t j=0;j<cnt*4 && j<96;j+=8){
			uint64_t e=0; for(int q=7;q>=0;q--) e=(e<<8)|b[j+q];
			fprintf(lg,"      raw=%02x %02x %02x %02x %02x %02x %02x %02x  |u64=0x%016llx| off=0x%04x val=0x%08x tag=0x%04x\n",
				b[j],b[j+1],b[j+2],b[j+3],b[j+4],b[j+5],b[j+6],b[j+7],
				(unsigned long long)e,
				(unsigned)(e & 0xffff),
				(unsigned)((e>>16) & 0xffffffffULL),
				(unsigned)((e>>48) & 0xffff));
		}
	}

	dump_shapes(s, t);
	fflush(lg);
}

int ioctl(int fd, unsigned long req, ...){
	init();
	va_list ap; va_start(ap,req); void *arg = va_arg(ap,void*); va_end(ap);
	int nr = _IOC_NR(req), ty = _IOC_TYPE(req);

	if(ty=='d' && nr==DRM_COMMAND_BASE+RKNPU_SUBMIT && arg) dump_submit(arg);

	int r = real_ioctl(fd,req,arg);
	if(r || ty!='d' || !arg) return r;

	if(nr==DRM_COMMAND_BASE+RKNPU_MEM_CREATE){
		struct rknpu_mem_create *c = arg;
		if(nbuf<MAXB){ bufs[nbuf].handle=c->handle; bufs[nbuf].obj_addr=c->obj_addr;
			bufs[nbuf].dma_addr=c->dma_addr; bufs[nbuf].size=c->size;
			bufs[nbuf].flags=c->flags; nbuf++; }
		fprintf(lg,"MEM_CREATE h=%u size=%llu flags=0x%x obj=0x%llx dma=0x%llx\n",
			c->handle,(unsigned long long)c->size,c->flags,
			(unsigned long long)c->obj_addr,(unsigned long long)c->dma_addr);
	} else if(nr==DRM_COMMAND_BASE+RKNPU_MEM_MAP){
		struct rknpu_mem_map *m = arg;
		for(int i=0;i<nbuf;i++) if(bufs[i].handle==m->handle) bufs[i].moff=m->offset;
		fprintf(lg,"MEM_MAP    h=%u offset=0x%llx\n",m->handle,(unsigned long long)m->offset);
	}
	return r;
}
