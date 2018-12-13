/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_CLEARCPU_H
#define _ASM_CLEARCPU_H 1

#ifndef __ASSEMBLY__

#include <linux/jump_label.h>
#include <linux/sched/smt.h>
#include <asm/alternative.h>
#include <linux/thread_info.h>

/*
 * Clear CPU buffers to avoid side channels.
 * We use microcode as a side effect of the obsolete VERW instruction
 */

static inline void clear_cpu(void)
{
	unsigned kernel_ds = __KERNEL_DS;
	/* Has to be memory form, don't modify to use an register */
	alternative_input("verw %[kernelds]", "", X86_FEATURE_NO_VERW,
		[kernelds] "m" (kernel_ds));
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
		clear_cpu();
	}
}

static inline void lazy_clear_cpu(void)
{
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
