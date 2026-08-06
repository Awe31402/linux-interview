// SPDX-License-Identifier: GPL-2.0
/*
 * x86_oops_case.c —— 把書上 §4.10 案例5 的那段核心模組原始碼，
 *                    原封不動用 x86_64 編譯器編一次，看它是不是真的長成書上那樣：
 *
 *      0xffffffffc0d130e1 <init_module+225>:  mov    -0x80(%rbp),%rax
 *      0xffffffffc0d130e5 <init_module+229>:  lea    0x78(%rax),%rdx
 *      0xffffffffc0d130e9 <init_module+233>:  lea    -0x68(%rbp),%rcx
 *      0xffffffffc0d130ed <init_module+237>:  mov    -0x88(%rbp),%rax
 *      0xffffffffc0d130f4 <init_module+244>:  mov    %rcx,%rsi
 *      0xffffffffc0d130f7 <init_module+247>:  mov    %rax,%rdi
 *      0xffffffffc0d130fa <init_module+250>:  callq  create_oops
 *
 * 本機沒有 x86_64 的核心可以插模組，但「這段組合語言長什麼樣」只跟編譯器＋ABI 有關，
 * 所以用同一套規則（-O2 -fno-omit-frame-pointer，等同 CentOS 3.10 核心的
 * CONFIG_FRAME_POINTER=y）在真的 x86_64 機器上編一次就能驗證。
 *
 * 核心型別用最小假身分頂替，只保證 mmap_sem 在 mm_struct 裡的偏移量是 0x78（和書上一樣）。
 *
 * 編譯：gcc -O2 -fno-omit-frame-pointer -c x86_oops_case.c -o x86_oops_case.o
 *       objdump -d x86_oops_case.o
 */
#include <string.h>

struct rw_semaphore { long count; void *owner; void *wait_list[2]; };
struct vm_area_struct { unsigned long vm_start, vm_end; unsigned long vm_flags; };

/* 讓 mmap_sem 的偏移量剛好是 0x78，和書上 `struct -o mm_struct` 的輸出一致 */
struct mm_struct {
	char			pad[0x78];
	struct rw_semaphore	mmap_sem;
	void			*owner;
};

struct mydev_priv {
	char name[64];
	int i;
	struct mm_struct *mm;
	struct rw_semaphore *sem;
};

/* 這些在核心裡是真函式，這裡只留宣告，不讓編譯器最佳化掉呼叫 */
extern void down_read(struct rw_semaphore *sem);
extern void down_write(struct rw_semaphore *sem);
extern struct mm_struct *get_task_mm(void);
extern void *kmalloc(unsigned long size, unsigned int flags);
extern void kfree(void *p);
extern int printk(const char *fmt, ...);

__attribute__((noinline))
int create_oops(struct vm_area_struct *vma, struct mydev_priv *priv,
		struct rw_semaphore *sem)
{
	unsigned long flags;

	down_read(sem);
	flags = vma->vm_flags;
	printk("flags=0x%lx, name=%s\n", flags, priv->name);
	return 0;
}

int my_oops_init(void)			/* = 書上的 init_module */
{
	int ret;
	struct vm_area_struct *vma = NULL;
	struct mydev_priv priv;
	struct mm_struct *mm;

	mm = get_task_mm();

	priv.mm  = mm;
	priv.sem = &mm->mmap_sem;

	down_write(&mm->mmap_sem);

	vma = kmalloc(sizeof(*vma), 0xcc0);
	if (!vma)
		return -12;

	kfree(vma);
	vma = NULL;

	__asm__ __volatile__("mfence" ::: "memory");	/* = smp_mb() */

	memcpy(priv.name, "benshushu", sizeof("benshushu"));
	priv.i = 10;

	ret = create_oops(vma, &priv, &mm->mmap_sem);

	return ret;
}
