/* Ch6 Q9: prove shmem swap-out is counted in S_swap but NOT in VmSwap(P_swap) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MB (1024UL*1024UL)
static size_t SZ = 256*MB;

static long get_kv(const char *file, const char *key) {
    FILE *f = fopen(file, "r"); char l[256]; long v = -1;
    if (!f) return -1;
    while (fgets(l, sizeof l, f)) {
        if (!strncmp(l, key, strlen(key))) { sscanf(l+strlen(key), "%ld", &v); break; }
    }
    fclose(f); return v;
}
static void snap(const char *tag) {
    printf("%-28s VmSwap=%7ld kB  VmRSS=%8ld kB | SwapFree=%9ld kB  Shmem=%8ld kB  AnonPages=%8ld kB\n",
        tag, get_kv("/proc/self/status","VmSwap:"), get_kv("/proc/self/status","VmRSS:"),
        get_kv("/proc/meminfo","SwapFree:"), get_kv("/proc/meminfo","Shmem:"),
        get_kv("/proc/meminfo","AnonPages:"));
}

int main(int argc, char **argv) {
    int shared = (argc > 1 && !strcmp(argv[1], "shmem"));
    int flags = MAP_ANONYMOUS | (shared ? MAP_SHARED : MAP_PRIVATE);
    printf("=== mapping %zu MB  %s ===\n", SZ/MB, shared ? "MAP_SHARED|MAP_ANONYMOUS (shmem)" : "MAP_PRIVATE|MAP_ANONYMOUS (anon)");
    snap("before mmap");
    char *p = mmap(NULL, SZ, PROT_READ|PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    for (size_t i = 0; i < SZ; i += 4096) p[i] = (char)i;   /* touch */
    snap("after touch");
    if (madvise(p, SZ, MADV_PAGEOUT)) { perror("MADV_PAGEOUT"); return 1; }
    sleep(2);
    snap("after MADV_PAGEOUT");
    long swapped = get_kv("/proc/self/status","VmSwap:");
    printf(">>> this process VmSwap = %ld kB  (contributes to P_swap)\n", swapped);
    munmap(p, SZ);
    return 0;
}
