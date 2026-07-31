/* Ch3 Q3 的配角：分配一頁匿名記憶體、fork 一個子行程共享它（COW 前 mapcount=2），
 * 印出 PID / VA / PFN 之後停住，讓 mm_convert.ko 有東西可以查。
 *   sudo ./hold_page &
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>

int main(void)
{
	char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		       MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	uint64_t e = 0;
	int fd;

	p[0] = 0x5a;
	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd >= 0) {
		pread(fd, &e, 8, ((unsigned long)p / 4096) * 8);
		close(fd);
	}
	printf("PID       = %d\n", getpid());
	printf("VA        = 0x%lx\n", (unsigned long)p);
	printf("PFN       = 0x%llx   PA = 0x%llx\n",
	       (unsigned long long)(e & ((1ULL << 55) - 1)),
	       (unsigned long long)((e & ((1ULL << 55) - 1)) << 12));
	printf("=> sudo insmod mm_convert.ko target_pid=%d target_va=0x%lx\n",
	       getpid(), (unsigned long)p);
	fflush(stdout);

	if (fork() == 0) { pause(); _exit(0); }   /* 子行程共享同一頁（COW）*/
	sleep(60);
	return 0;
}
