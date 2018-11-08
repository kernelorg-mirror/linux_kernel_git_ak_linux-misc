/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_CLEARCPU_H
#define _ASM_CLEARCPU_H 1

#include <linux/jump_label.h>
#include <linux/sched/smt.h>
#include <asm/alternative.h>
#include <linux/thread_info.h>

/*
 * Clear CPU buffers to avoid side channels.
 * We use either microcode (as a side effect of the obsolete
 * "VERW" instruction), or special out of line clear sequences.
 */

static inline void clear_cpu(void)
{
	unsigned kernel_ds = __KERNEL_DS;
	/* Has to be memory form, don't modify to use an register */
	alternative_input("",
		"verw %[kernelds]",
		X86_FEATURE_MB_CLEAR,
		[kernelds] "m" (kernel_ds));
}

#endif
