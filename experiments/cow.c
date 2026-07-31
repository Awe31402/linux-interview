/* Ch7 Q9: copy-on-write.  Watch min_flt and smaps Shared/Private_Dirty across fork+write */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>
#define MB (1024UL*1024UL)
static size_t SZ = 64*MB;
static char *p;
static long kv(const char*f,const char*k){FILE*fp=fopen(f,"r");char l[512];long v=-1;
 while(fp&&fgets(l,sizeof l,fp)) if(!strncmp(l,k,strlen(k))){sscanf(l+strlen(k),"%ld",&v);break;} if(fp)fclose(fp);return v;}
static void smaps(const char*tag){
  /* find the smaps entry for our region */
  char cmd[256]; snprintf(cmd,sizeof cmd,"awk '/^%lx-/{f=1} f&&/^(Rss|Shared_Dirty|Private_Dirty):/{printf \"%%s %%s \", $1,$2} f&&/^VmFlags/{exit}' /proc/%d/smaps",(unsigned long)p,getpid());
  printf("%-34s ", tag); fflush(stdout); system(cmd);
  struct rusage r; getrusage(RUSAGE_SELF,&r);
  printf(" | min_flt=%ld  VmRSS=%ld kB\n", r.ru_minflt, kv("/proc/self/status","VmRSS:"));
}
int main(void){
  p = mmap(NULL,SZ,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
  memset(p,0xAA,SZ);
  smaps("[parent] after memset (pre-fork)");
  pid_t c = fork();
  if (c == 0) {
      smaps("[child ] right after fork");
      /* read-only pass: must NOT trigger COW */
      volatile long s=0; for(size_t i=0;i<SZ;i+=4096) s+=p[i];
      smaps("[child ] after READ-only sweep");
      /* write pass: triggers COW on every page */
      for(size_t i=0;i<SZ;i+=4096) p[i]=0x55;
      smaps("[child ] after WRITE sweep (COW)");
      _exit(0);
  }
  wait(NULL);
  sleep(1);
  smaps("[parent] after child exited");
  return 0; }
