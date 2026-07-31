/* Ch7 Q12: when are the page-table levels allocated? VmPTE == mm->pgtables_bytes */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#define MB (1024UL*1024UL)
#define GB (1024UL*MB)
static long pte(void){FILE*f=fopen("/proc/self/status","r");char l[256];long v=-1;
 while(fgets(l,sizeof l,f)) if(!strncmp(l,"VmPTE:",6)){sscanf(l+6,"%ld",&v);break;} fclose(f);return v;}
static void s(const char*t){ printf("%-46s VmPTE=%4ld kB (%2ld pages)\n", t, pte(), pte()/4); }
int main(void){
  s("at main() entry (pgd+levels for exec image)");
  char *a = mmap(NULL, 1*GB, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_NORESERVE, -1, 0);
  s("after mmap 1GB (no touch)");
  for (int i=0;i<8;i++) a[i*4096] = 1;              /* 8 pages, same 2MB -> 1 PTE table */
  s("after touching 8 pages inside one 2MB range");
  for (int i=0;i<8;i++) a[i*2*MB + 4096] = 1;       /* 8 pages, 8 different 2MB -> 8 PTE tables */
  s("after touching 8 pages in 8 different 2MB");
  char *b = mmap(NULL, 4*GB, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_NORESERVE, -1, 0);
  for (int i=0;i<4;i++) b[(size_t)i*GB] = 1;        /* 4 pages, 4 different 1GB -> +4 PUD-level PMD tables */
  s("after touching 4 pages in 4 different 1GB");
  long before = pte();
  pid_t c = fork();
  if (c == 0) { printf("%-46s VmPTE=%4ld kB   <-- fork() COPIED the page tables\n","[child] right after fork",pte()); fflush(stdout); _exit(0);}
  wait(NULL);
  printf("[parent] VmPTE before fork = %ld kB\n", before);
  return 0; }
