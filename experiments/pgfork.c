#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#define MB (1024UL*1024UL)
static long pte(void){FILE*f=fopen("/proc/self/status","r");char l[256];long v=-1;
 while(fgets(l,sizeof l,f)) if(!strncmp(l,"VmPTE:",6)){sscanf(l+6,"%ld",&v);break;} fclose(f);return v;}
int main(void){ char*p=mmap(NULL,256*MB,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
 memset(p,1,256*MB);
 printf("[parent] VmPTE = %ld kB (before fork)\n", pte()); fflush(stdout);
 if(fork()==0){ printf("[child ] VmPTE = %ld kB (immediately after fork -> page tables were COPIED)\n", pte()); fflush(stdout); _exit(0);} 
 wait(NULL); return 0;}
