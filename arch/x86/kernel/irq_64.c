// SPDX-License-Identifier: GPL-2.0
/*
 *	Copyright (C) 1992, 1998 Linus Torvalds, Ingo Molnar
 *
 * This file contains the lowest level x86_64-specific interrupt
 * entry and irq statistics code. All the remaining irq logic is
 * done by the generic kernel/irq/ code and in the
 * x86_64-specific irq controller code. (e.g. i8259.c and
 * io_apic.c.)
 */

#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/ftrace.h>
#include <linux/uaccess.h>
#include <linux/smp.h>
#include <linux/sched/task_stack.h>
#include <asm/io_apic.h>
#include <asm/apic.h>
#include <asm/unwind.h>

int sysctl_panic_on_stackoverflow;

#ifdef CONFIG_DEBUG_STACK_DEPTH

/*
 * Check for too deep call nesting in the kernel that might
 * overflow the 16 entry return stack buffer.
 */

#define MAX_CALL_DEPTH 16

static void check_stack_depth(struct pt_regs *regs)
{
	struct unwind_state state;
	int count = 0;
	static int num_printed;

	if (test_thread_flag(TIF_DEEP_STACK))
		return;
	/* System init code can have deep chains but that's fine */
	if (system_state != SYSTEM_RUNNING)
		return;

	for (unwind_start(&state, current, regs, NULL);
	     !unwind_done(&state) && unwind_get_return_address(&state);
	     unwind_next_frame(&state))
		count++;
	if (count > MAX_CALL_DEPTH && num_printed < 10) {
		WARN(1, "Call depth %d deeper than %d\n",
				count, MAX_CALL_DEPTH);
		num_printed++;
	}
}
#endif

#ifdef CONFIG_DEBUG_STACKOVERFLOW
/*
 * Probabilistic stack overflow check:
 *
 * Only check the stack in process context, because everything else
 * runs on the big interrupt stacks. Checking reliably is too expensive,
 * so we just check from interrupts.
 */
void stack_overflow_check(struct pt_regs *regs)
{
#define STACK_TOP_MARGIN	128
	struct orig_ist *oist;
	u64 irq_stack_top, irq_stack_bottom;
	u64 estack_top, estack_bottom;
	u64 curbase = (u64)task_stack_page(current);

	if (user_mode(regs))
		return;

#ifdef CONFIG_DEBUG_STACK_DEPTH
	check_stack_depth(regs);
#endif

	if (regs->sp >= curbase + sizeof(struct pt_regs) + STACK_TOP_MARGIN &&
	    regs->sp <= curbase + THREAD_SIZE)
		return;

	irq_stack_top = (u64)this_cpu_ptr(irq_stack_union.irq_stack) +
			STACK_TOP_MARGIN;
	irq_stack_bottom = (u64)__this_cpu_read(irq_stack_ptr);
	if (regs->sp >= irq_stack_top && regs->sp <= irq_stack_bottom)
		return;

	oist = this_cpu_ptr(&orig_ist);
	estack_top = (u64)oist->ist[0] - EXCEPTION_STKSZ + STACK_TOP_MARGIN;
	estack_bottom = (u64)oist->ist[N_EXCEPTION_STACKS - 1];
	if (regs->sp >= estack_top && regs->sp <= estack_bottom)
		return;

	WARN_ONCE(1, "do_IRQ(): %s has overflown the kernel stack (cur:%Lx,sp:%lx,irq stk top-bottom:%Lx-%Lx,exception stk top-bottom:%Lx-%Lx,ip:%pF)\n",
		current->comm, curbase, regs->sp,
		irq_stack_top, irq_stack_bottom,
		estack_top, estack_bottom, (void *)regs->ip);

	if (sysctl_panic_on_stackoverflow)
		panic("low stack detected by irq handler - check messages\n");
}
#endif

bool handle_irq(struct irq_desc *desc, struct pt_regs *regs)
{
	stack_overflow_check(regs);

	if (IS_ERR_OR_NULL(desc))
		return false;

	generic_handle_irq_desc(desc);
	return true;
}
