# 《奔跑吧 Linux內核》（第二版）卷1 第6章 內存管理之實戰案例分析 高頻面試題

> **說明**：收錄自第 6 章開篇「本章的高頻面試題」，共計 14 題。


### 1. Linux内核的内存管理模块都对哪些页面进行了统计？

### 2. 请解释/proc/meminfo节点中每一项的含义。

### 3. 为什么/proc/meminfo节点中的MemTotal不等于QEMU虚拟机中分配的内 存大小？

### 4. 为什么slab要区分SReclaimable和SUnreclaim？

### 5. 在/proc/meminfo节点中，为什么Active(anon)+Inactive(anon)不等于 AnonPages？

### 6. 在/proc/meminfo节点中，为什么Active(file) + Inactive(file)不等于 Mapped？

### 7. 在/proc/meminfo节点中，为什么Active(file) + Inactive(file)不等于 Cached？

### 8. /proc/PID/status（PID表示进程的ID）节点中有不少和具体进程内存相关 的信息，请简述这些信息的含义。

### 9. /proc/meminfo节点中SwapTotal减去SwapFree等于系统中已经使用的swap 内存大小，我们称之为S_swap。另外，我们写一个小程序来遍历系统中所有的 进程，并把进程中/proc/PID/status节点的VmSwap值都累加起来，我们把它称为 P_swap，为什么这两个值不相等？

### 10. 请简述min_free_kbytes的含义和作用。

### 11. 请简述lowmem_reserve_ratio的含义和作用。

### 12. 请简述zone_reclaim_mode的含义和作用。

### 13. 请简述watermark_boost_factor的含义和作用。

### 14. 请简述影响脏页回写的参数有哪些？它们的含义和作用分别是什么？
