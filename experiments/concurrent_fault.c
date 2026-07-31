/* Ch4 Q46 —— 多核 SMP 上多個 CPU 同時對同一個頁面缺頁
 *
 * 8 個執行緒用 barrier 對齊後【同時】寫同一塊 64MB 匿名記憶體。
 * 必要的缺頁只有 16384 次（64MB/4KB），實測卻遠超過——
 * 多出來的就是「多顆 CPU 同時對同一頁缺頁，各自配了頁，
 * 但只有搶到 pte lock 的那個贏，其餘在 double-check
 * (if (!pte_none(*vmf->pte)) goto release;) 發現白做工」。
 *
 *   gcc -O2 -o concurrent_fault concurrent_fault.c -lpthread
 *   ./concurrent_fault              # 8 顆 CPU
 *   taskset -c 4 ./concurrent_fault # 對照組：擠在 1 顆 CPU
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#define SZ (64*1024*1024)
static char *p; static pthread_barrier_t b;
static void *th(void *a){ pthread_barrier_wait(&b);
  for(size_t i=0;i<SZ;i+=4096) p[i]=1;
  return NULL;}
int main(void){ pthread_t t[8]; struct rusage r;
  p=mmap(NULL,SZ,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
  pthread_barrier_init(&b,NULL,8);
  for(long i=0;i<8;i++) pthread_create(&t[i],NULL,th,(void*)i);
  for(int i=0;i<8;i++) pthread_join(t[i],NULL);
  getrusage(RUSAGE_SELF,&r);
  printf("8 執行緒同時寫 %d MB（共 %d 頁）\n", SZ/1024/1024, SZ/4096);
  printf("實際 minor fault = %ld\n", r.ru_minflt);
  printf("單執行緒理論值   = %d\n", SZ/4096);
  printf("多出來 = %ld 次（同時缺頁、被 double-check 擋掉的白工）\n", r.ru_minflt - SZ/4096);
  return 0;}
