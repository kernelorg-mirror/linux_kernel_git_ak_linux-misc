#undef TRACE_SYSTEM
#define TRACE_SYSTEM elision

#if !defined(_TRACE_ELISION_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_ELISION_H

#include <linux/lockdep.h>
#include <linux/tracepoint.h>

#ifdef CONFIG_RTM_LOCKS

TRACE_EVENT(elision_skip_start,
	    TP_PROTO(void *lock, u32 status),
	    TP_ARGS(lock, status),
	    TP_STRUCT__entry(
		__field(void *, lock)
		__field(u32, status)
	    ),
	    TP_fast_assign(
		__entry->lock = lock;
		__entry->status = status;
	    ),
	    TP_printk("%p %x", __entry->lock, __entry->status)
);

#endif

#endif

/* This part must be outside protection */
#include <trace/define_trace.h>
