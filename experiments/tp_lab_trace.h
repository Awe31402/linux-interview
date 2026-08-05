/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tp_lab_trace.h —— 卷2 第3章 Q8：「如何在內核代碼中添加一個跟蹤點？」
 *
 * 書上（§3.2.5）是把 TRACE_EVENT() 寫進 include/trace/events/sched.h 再重編內核；
 * 這裡示範**完全等價、但不用重編內核**的樹外寫法：
 * 自己帶一份 trace 標頭檔，並用 TRACE_INCLUDE_PATH 告訴 define_trace.h 去哪裡找。
 *
 * 三個必備的樣板：
 *   #undef TRACE_SYSTEM  + #define TRACE_SYSTEM <子系統名>  → 決定 events/<這裡>/
 *   TRACE_INCLUDE_PATH / TRACE_INCLUDE_FILE                → 給 define_trace.h 用
 *   #include <trace/define_trace.h>                        → 真正生成程式碼
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM tp_lab

#if !defined(_TP_LAB_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TP_LAB_TRACE_H

#include <linux/tracepoint.h>
#include <linux/sched.h>

/* 對照書上 sched_stat_minvruntime 的六個欄位：name/proto/args/struct/assign/print */
TRACE_EVENT(tp_lab_alloc,

	TP_PROTO(struct task_struct *tsk, u64 size, unsigned long addr),

	TP_ARGS(tsk, size, addr),

	TP_STRUCT__entry(
		__array(char,		comm,	TASK_COMM_LEN)
		__field(pid_t,		pid)
		__field(u64,		size)
		__field(unsigned long,	addr)
	),

	TP_fast_assign(
		memcpy(__entry->comm, tsk->comm, TASK_COMM_LEN);
		__entry->pid  = tsk->pid;
		__entry->size = size;
		__entry->addr = addr;
	),

	TP_printk("comm=%s pid=%d size=%llu addr=0x%lx",
		  __entry->comm, __entry->pid,
		  (unsigned long long)__entry->size, __entry->addr)
);

/* 帶條件的跟蹤點：只有 size >= 1024 才會記錄（書上提到的 TRACE_EVENT_CONDITION）*/
TRACE_EVENT_CONDITION(tp_lab_big_alloc,

	TP_PROTO(u64 size),

	TP_ARGS(size),

	TP_CONDITION(size >= 1024),

	TP_STRUCT__entry(
		__field(u64, size)
	),

	TP_fast_assign(
		__entry->size = size;
	),

	TP_printk("big size=%llu", (unsigned long long)__entry->size)
);

#endif /* _TP_LAB_TRACE_H */

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE tp_lab_trace
#include <trace/define_trace.h>
