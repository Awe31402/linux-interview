/* same loop, but label who prints what so the process tree is visible */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void){ int i;
  for(i=0;i<2;i++){ pid_t r=fork(); printf("i=%d pid=%d ppid=%d fork_ret=%d\n", i, getpid(), getppid(), r); fflush(stdout); }
  wait(NULL); wait(NULL); return 0; }
