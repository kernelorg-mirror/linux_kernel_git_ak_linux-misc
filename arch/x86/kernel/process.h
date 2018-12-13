// SPDX-License-Identifier: GPL-2.0
//
// Code shared between 32 and 64 bit

#include <linux/clearcpu.h>
#include <asm/spec-ctrl.h>

void __switch_to_xtra(struct task_struct *prev_p, struct task_struct *next_p);

/*
 * This needs to be inline to optimize for the common case where no extra
 * work needs to be done.
 */
static inline void switch_to_extra(struct task_struct *prev,
				   struct task_struct *next)
{
	unsigned long next_tif = task_thread_info(next)->flags;
	unsigned long prev_tif = task_thread_info(prev)->flags;

	if (IS_ENABLED(CONFIG_SMP)) {
		/*
		 * Avoid __switch_to_xtra() invocation when conditional
		 * STIBP is disabled and the only different bit is
		 * TIF_SPEC_IB. For CONFIG_SMP=n TIF_SPEC_IB is not
		 * in the TIF_WORK_CTXSW masks.
		 */
		if (!static_branch_likely(&switch_to_cond_stibp)) {
			prev_tif &= ~_TIF_SPEC_IB;
			next_tif &= ~_TIF_SPEC_IB;
		}
	}

	/*
	 * When we switch to a different process, or we switch
	 * from a kernel thread that was not idle, clear the CPU
	 * buffers on next kernel exit.
	 *
	 * We assume that idle does not touch user data, except
	 * for interrupts, which schedule their own clears as needed.
	 * But other kernel threads, like work queues, might
	 * touch user data, so flush in this case.
	 *
	 * This has to be here because switch_mm doesn't get
	 * called in the kernel thread case.
	 */
	if (static_cpu_has(X86_BUG_MDS)) {
		if (prev->pid && (next->mm != prev->mm || prev->mm == NULL))
			lazy_clear_cpu();
		/*
		 * Also transfer the clearcpu flag from the previous task.
		 * Can be done non atomically because interrupts are off.
		 */
		task_thread_info(next)->status |=
			task_thread_info(prev)->status & _TIF_CLEAR_CPU;
		task_thread_info(prev)->status &= ~_TIF_CLEAR_CPU;
	}


	/*
	 * __switch_to_xtra() handles debug registers, i/o bitmaps,
	 * speculation mitigations etc.
	 */
	if (unlikely(next_tif & _TIF_WORK_CTXSW_NEXT ||
		     prev_tif & _TIF_WORK_CTXSW_PREV))
		__switch_to_xtra(prev, next);
}
