/* Ch7 Q3/Q5/Q7: process life cycle -- zombie state, reaping by wait(), orphan re-parented to init */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
static void show(pid_t p,const char*tag){ char c[200];
  snprintf(c,sizeof c,"grep -HE '^(Name|State|Pid|PPid):' /proc/%d/status 2>/dev/null | tr '\\n' ' ' ; echo",p);
  printf("%-24s ",tag); fflush(stdout); system(c); }
int main(void){
  /* --- 1. zombie --- */
  pid_t z = fork();
  if (z==0) _exit(42);
  sleep(1); show(z,"child exited, no wait:");
  int st; waitpid(z,&st,0); show(z,"after wait():");
  printf("%-24s exit status = %d\n","",WEXITSTATUS(st));
  /* --- 2. orphan --- */
  pid_t a = fork();
  if (a==0){ pid_t b=fork();
      if(b==0){ printf("%-24s grandchild pid=%d ppid=%d (parent alive)\n","",getpid(),getppid()); fflush(stdout);
                sleep(2); printf("%-24s grandchild pid=%d ppid=%d (parent dead -> re-parented)\n","",getpid(),getppid()); fflush(stdout); _exit(0);} 
      _exit(0); }
  waitpid(a,NULL,0); sleep(3);
  /* --- 3. VmPTE across fork --- */
  return 0; }
