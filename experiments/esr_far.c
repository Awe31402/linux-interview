/* Ch4 Q34/Q35/Q36/Q37 —— ARM64 缺頁異常時，處理器把資訊放在哪裡？
 *
 * 用 SIGSEGV/SIGBUS 的 sigaction(SA_SIGINFO) 處理常式，直接讀出：
 *   - si_addr           = 核心從 FAR_EL1 抄過來的「出錯的虛擬位址」
 *   - si_code           = SEGV_MAPERR（沒有 VMA）/ SEGV_ACCERR（權限不符）
 *   - ucontext 的 esr   = 核心從 ESR_EL1 抄過來的例外症狀暫存器
 *
 * ESR_EL1 解碼（arch/arm64/include/asm/esr.h）：
 *   EC   = ESR[31:26]  例外類別：0x24 = Data Abort from lower EL
 *                                0x20 = Instruction Abort from lower EL
 *   WnR  = ESR[6]      1 = 寫入造成，0 = 讀取造成      <- Q35 的答案
 *   DFSC = ESR[5:0]    錯誤狀態碼：0b0001xx = Translation fault (level xx)
 *                                  0b0011xx = Access flag fault
 *                                  0b0010xx = Permission fault    <- COW 走這條
 *
 *   gcc -O2 -o esr_far esr_far.c && ./esr_far
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <ucontext.h>
#include <sys/mman.h>

static sigjmp_buf jb;
static const char *cur;

static const char *dfsc_str(unsigned long d)
{
	switch (d & 0x3f) {
	case 0x00: case 0x01: case 0x02: case 0x03:
		return "Address size fault";
	case 0x04: return "Translation fault, level 0";
	case 0x05: return "Translation fault, level 1";
	case 0x06: return "Translation fault, level 2";
	case 0x07: return "Translation fault, level 3   <- 頁表項無效";
	case 0x08: return "Access flag fault, level 0";
	case 0x09: return "Access flag fault, level 1";
	case 0x0a: return "Access flag fault, level 2";
	case 0x0b: return "Access flag fault, level 3   <- AF=0，硬體 AF 更新";
	case 0x0c: return "Permission fault, level 0";
	case 0x0d: return "Permission fault, level 1";
	case 0x0e: return "Permission fault, level 2";
	case 0x0f: return "Permission fault, level 3    <- 權限不符（COW/唯讀）";
	case 0x21: return "Alignment fault";
	default:   return "(其他)";
	}
}

static void handler(int sig, siginfo_t *si, void *uc)
{
	ucontext_t *u = uc;
	unsigned long esr = 0, far = 0;
	struct _aarch64_ctx *h;

	/* aarch64 的 esr/far 放在 uc_mcontext.__reserved 的 esr_context 裡 */
	h = (struct _aarch64_ctx *)u->uc_mcontext.__reserved;
	while (h->magic) {
		if (h->magic == 0x45535201 /* ESR_MAGIC */) {
			esr = ((struct esr_context *)h)->esr;
			break;
		}
		if (!h->size) break;
		h = (struct _aarch64_ctx *)((char *)h + h->size);
	}
	far = (unsigned long)si->si_addr;

	printf("  ┌─ 捕捉到 %s：%s\n", sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", cur);
	printf("  │  si_addr (<- FAR_EL1) = 0x%016lx\n", far);
	printf("  │  si_code              = %d (%s)\n", si->si_code,
	       si->si_code == 1 ? "SEGV_MAPERR 該位址沒有 VMA" :
	       si->si_code == 2 ? "SEGV_ACCERR 有 VMA 但權限不符" :
	       si->si_code == 3 ? "BUS_ADRERR" : "?");
	if (esr) {
		unsigned long ec = (esr >> 26) & 0x3f;
		printf("  │  ESR_EL1              = 0x%016lx\n", esr);
		printf("  │    EC   = 0x%02lx  (%s)\n", ec,
		       ec == 0x24 ? "Data Abort from lower EL" :
		       ec == 0x20 ? "Instruction Abort from lower EL" : "?");
		printf("  │    WnR  = %lu     (%s)      <- Q35\n", (esr >> 6) & 1,
		       ((esr >> 6) & 1) ? "寫入造成" : "讀取/取指造成");
		printf("  │    CM   = %lu     ISV = %lu\n", (esr >> 8) & 1, (esr >> 24) & 1);
		printf("  │    DFSC = 0x%02lx  %s\n", esr & 0x3f, dfsc_str(esr));
	} else {
		printf("  │  （這個 libc 的 ucontext 沒帶 esr_context）\n");
	}
	printf("  └─\n\n");
	siglongjmp(jb, 1);
}

#define TRY(desc) cur = desc; if (sigsetjmp(jb, 1) == 0)

int main(void)
{
	struct sigaction sa = { 0 };
	char *ro, *none, *p;

	sa.sa_sigaction = handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);

	printf("=========== Ch4 Q34-Q37：ARM64 缺頁異常資訊 ===========\n");
	printf("FAR_EL1 -> si_addr，ESR_EL1 -> ucontext 的 esr_context\n\n");

	/* --- 1. 存取完全沒有映射的位址 -> Translation fault + SEGV_MAPERR --- */
	none = (char *)0x0000123456789000UL;
	TRY("讀一個沒有任何 VMA 的位址 0x123456789000") {
		volatile char c = *none; (void)c;
	}

	TRY("寫一個沒有任何 VMA 的位址 0x123456789000") {
		*none = 1;
	}

	/* --- 2. 有 VMA 但唯讀，寫它 -> Permission fault + SEGV_ACCERR --- */
	ro = mmap(NULL, 4096, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	{ volatile char c = *ro; (void)c; }            /* 先讀一次，建立映射 */
	TRY("寫一段 PROT_READ 的映射（有 VMA，權限不符）") {
		*ro = 1;
	}

	/* --- 3. 存取 PROT_NONE --- */
	p = mmap(NULL, 4096, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	TRY("讀一段 PROT_NONE 的映射") {
		volatile char c = *p; (void)c;
	}

	/* --- 4. NULL 指標 --- */
	TRY("解參考 NULL 指標") {
		volatile char c = *(char *)0; (void)c;
	}

	/* --- 5. 核心空間位址（使用者態不可碰） --- */
	TRY("從使用者態讀核心位址 0xffff800008010000") {
		volatile char c = *(char *)0xffff800008010000UL; (void)c;
	}

	printf("=========== 對照：可以修復的缺頁（Q36）===========\n");
	printf("上面每一個都是【不可修復】的：核心找不到合法 VMA 或權限不符，\n");
	printf("於是 do_page_fault() 走 __do_user_fault() 送 SIGSEGV。\n");
	printf("可修復的情況（正常 demand paging / COW）不會走到訊號，\n");
	printf("請看 fault_types 的 minor/major fault 統計。\n");
	return 0;
}
