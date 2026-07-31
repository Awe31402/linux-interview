/* Ch7 Q4: PID vs TGID -- getpid() returns TGID, gettid() returns the per-thread PID */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
static void *tf(void *a){
  printf("  thread : getpid()=%d  gettid()=%ld\n", getpid(), syscall(SYS_gettid));
  sleep(3); return NULL; }
int main(void){
  printf("  main   : getpid()=%d  gettid()=%ld\n", getpid(), syscall(SYS_gettid));
  pthread_t t[2]; for(int i=0;i<2;i++) pthread_create(&t[i],NULL,tf,NULL);
  sleep(1);
  char c[128]; snprintf(c,sizeof c,"ls /proc/%d/task; grep -E '^(Tgid|Pid|Threads):' /proc/%d/status",getpid(),getpid());
  fflush(stdout); system(c);
  for(int i=0;i<2;i++) pthread_join(t[i],NULL); return 0; }
