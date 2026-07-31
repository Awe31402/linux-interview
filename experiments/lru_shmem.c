/* Ch6 Q5/Q6/Q7: shmem lands on the ANON LRU but is counted in NR_FILE_PAGES/Shmem, not AnonPages */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define MB (1024UL*1024UL)
static size_t SZ = 512*MB;
static long g(const char*k){FILE*f=fopen("/proc/meminfo","r");char l[256];long v=-1;
 while(fgets(l,sizeof l,f)) if(!strncmp(l,k,strlen(k))){sscanf(l+strlen(k),"%ld",&v);break;} fclose(f);return v;}
static void snap(const char*t){
 long aa=g("Active(anon):"),ia=g("Inactive(anon):"),af=g("Active(file):"),iff=g("Inactive(file):");
 printf("%-16s anonLRU=%8ld  fileLRU=%8ld | AnonPages=%8ld Shmem=%7ld Cached=%8ld Buffers=%7ld Mapped=%7ld\n",
   t,aa+ia,af+iff,g("AnonPages:"),g("Shmem:"),g("Cached:"),g("Buffers:"),g("Mapped:"));}
int main(void){
 snap("baseline");
 char*p=mmap(NULL,SZ,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_SHARED,-1,0);
 for(size_t i=0;i<SZ;i+=4096) p[i]=1;
 snap("+512MB shmem");
 munmap(p,SZ); sleep(1);
 char*q=mmap(NULL,SZ,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
 for(size_t i=0;i<SZ;i+=4096) q[i]=1;
 snap("+512MB anon");
 munmap(q,SZ); sleep(1);
 snap("after munmap");
 /* identity check on the live values */
 long aa=g("Active(anon):")+g("Inactive(anon):"), af=g("Active(file):")+g("Inactive(file):");
 printf("\n[Q7] fileLRU(%ld) == Cached(%ld) - Shmem(%ld) + Buffers(%ld) = %ld\n",
   af,g("Cached:"),g("Shmem:"),g("Buffers:"),g("Cached:")-g("Shmem:")+g("Buffers:"));
 printf("[Q5] anonLRU(%ld) == AnonPages(%ld) + Shmem(%ld) + SwapCached(%ld) - Unevictable(%ld) = %ld\n",
   aa,g("AnonPages:"),g("Shmem:"),g("SwapCached:"),g("Unevictable:"),
   g("AnonPages:")+g("Shmem:")+g("SwapCached:")-g("Unevictable:"));
 printf("[Q6] Mapped(%ld) is only the mapped subset of page cache; fileLRU=%ld\n", g("Mapped:"), af);
 return 0;}
