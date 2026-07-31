/* vfork(): child runs first and BLOCKS the parent until _exit/execve; shares the mm */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
int g = 111;
int main(void){
  printf("[parent] before vfork, g=%d\n", g); fflush(stdout);
  pid_t p = vfork();
  if (p == 0) { g = 222; printf("[child ] pid=%d set g=222, sleeping 1s then _exit\n", getpid()); fflush(stdout); sleep(1); _exit(0); }
  printf("[parent] resumed after child exited, g=%d  <-- vfork SHARES the mm\n", g);
  /* now the same with fork() */
  g = 111;
  p = fork();
  if (p == 0) { g = 333; _exit(0); }
  wait(NULL);
  printf("[parent] after fork()+child g=333, parent still sees g=%d  <-- fork COPIES the mm\n", g);
  return 0; }
