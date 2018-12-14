/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_CLEARCPU_H
#define _ASM_CLEARCPU_H 1

#ifndef __ASSEMBLY__

#include <linux/jump_label.h>
#include <linux/sched/smt.h>
#include <asm/alternative.h>
#include <linux/thread_info.h>

/*
 * We cannot directly include the trace point header here
 * because it leads to include loops with other trace point
 * files pulling this one in. Define the static
 * key manually here, which handles noping the fast path,
 * and the actual tracing is done out of line.
 */
#ifdef CONFIG_TRACEPOINTS
#include <asm/atomic.h>
#include <linux/tracepoint-defs.h>

extern struct tracepoint __tracepoint_clear_cpu;
extern struct tracepoint __tracepoint_lazy_clear_cpu;
#define cc_tracepoint_active(t) static_key_false(&(t).key)

extern void do_trace_clear_cpu(void);
extern void do_trace_lazy_clear_cpu(void);
#else
#define cc_tracepoint_active(t) false
static inline void do_trace_clear_cpu(void) {}
static inline void do_trace_lazy_clear_cpu(void) {}
#endif

/*
 * Clear CPU buffers to avoid side channels.
 * We use microcode as a side effect of the obsolete VERW instruction
 */

static inline void __clear_cpu(void)
{
	unsigned kernel_ds = __KERNEL_DS;
	/* Has to be memory form, don't modify to use an register */
	alternative_input("verw %[kernelds]", "", X86_FEATURE_NO_VERW,
		[kernelds] "m" (kernel_ds));
}

static inline void clear_cpu(void)
{
	if (cc_tracepoint_active(__tracepoint_clear_cpu))
		do_trace_clear_cpu();
	__clear_cpu();
}

/*
 * Clear CPU buffers before going idle, so that no state is leaked to SMT
 * siblings taking over thread resources.
 * Out of line to avoid include hell.
 *
 * Assumes that interrupts are disabled and only get reenabled
 * before idle, otherwise the data from a racing interrupt might not
 * get cleared. There are some callers who violate this,
 * but they are only used in unattackable cases.
 */

static inline void clear_cpu_idle(void)
{
	if (sched_smt_active()) {
		clear_thread_flag(TIF_CLEAR_CPU);
		__clear_cpu();
	}
}

static inline void lazy_clear_cpu(void)
{
	if (cc_tracepoint_active(__tracepoint_lazy_clear_cpu))
		do_trace_lazy_clear_cpu();
	set_thread_flag(TIF_CLEAR_CPU);
}

DECLARE_STATIC_KEY_FALSE(force_cpu_clear);

#else

.macro CLEAR_CPU
	ALTERNATIVE __stringify(push $__USER_DS ; verw (% _ASM_SP ) ; add $8, % _ASM_SP ),\
		"", X86_FEATURE_NO_VERW
.endm

#endif

#endif
