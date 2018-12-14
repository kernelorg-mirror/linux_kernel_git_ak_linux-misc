#undef TRACE_SYSTEM
#define TRACE_SYSTEM clearcpu

#if !defined(_TRACE_CLEARCPU_H) || defined(TRACE_HEADER_MULTI_READ)

#include <linux/tracepoint.h>

DECLARE_EVENT_CLASS(clear_cpu,
		    TP_PROTO(int dummy),
		    TP_ARGS(dummy),
		    TP_STRUCT__entry(__field(int, dummy)),
		    TP_fast_assign(),
		    TP_printk("%d", __entry->dummy));

DEFINE_EVENT(clear_cpu, clear_cpu, TP_PROTO(int dummy), TP_ARGS(dummy));
DEFINE_EVENT(clear_cpu, lazy_clear_cpu, TP_PROTO(int dummy), TP_ARGS(dummy));

#define _TRACE_CLEARCPU_H

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH asm/trace/
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE clearcpu
#endif /* _TRACE_CLEARCPU_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
