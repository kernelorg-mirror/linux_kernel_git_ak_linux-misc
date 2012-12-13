/*
 * Intel TSX RTM (Restricted Transactional Memory) lock elision.
 * Lock elision allows to run locks in parallel using transactional memory.
 *
 * (C) Copyright 2012, 2013 Intel Corporation
 * Author: Andi Kleen <ak@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 *
 * Adds a fast path for locks. Run each lock speculatively in a hardware
 * memory transaction implemented by the CPU. When the transaction succeeds
 * the lock will have executed in parallel without blocking.
 *
 * If the transaction aborts (due to memory conflicts or other causes)
 * eventually fall back to normal locking.
 *
 * For interrupt disabling we  use the paravirt ops to patch in our own code
 * that avoids aborts. Inside a transaction there is no need to disable
 * interrupts, because interrupts abort the transaction anyways.
 *
 * This use of paravirt ops implies currently that elision does not
 * work when another paravirt ops user is active.
 *
 * For the other locks we use custom hooks protected by static keys.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/elide.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/bit_spinlock.h>
#include <linux/jump_label.h>
#include <linux/rtm.h>
#include <asm/paravirt.h>

/*
 * We need a software in_tx marker, to answer the question
 * "Is this an inner nested transaction commit?" inside the transaction.
 * XTEST unfortunately does not tell us that.
 *
 * This is needed to handle
 *
 * spin_lock(x)
 * spin_lock_irqsave(y, flags)
 * spin_unlock(y)    // no _irqrestore
 * spin_unlock(x)
 * ... code that relies on interrupts disabled ...
 * local_irq_restore(flags)
 *
 * If the outermost spin_lock has the irqsave there is no problem
 * because we just disable/reenable interrupts outside the transaction.
 * But we cannot do that for a nested spin lock, because disabling
 * interrupts would abort. Normally we don't need to disable
 * interrupts in a transaction anyways because any interrupt aborts.
 * But there's no way to atomically disable the interrupts on
 * unlock/commit and keep them disabled after the transaction.
 *
 * The current solution is to detect the non matched unlock and abort
 * (and fix code which does that frequently). This needs the software
 * in_tx counter.
 */

/* TBD combine into one count */
static DEFINE_PER_CPU(int, in_tx);
static DEFINE_PER_CPU(bool, cli_elided);

#define start_in_tx() __this_cpu_inc(in_tx)
#define end_in_tx() __this_cpu_dec(in_tx)
#define is_in_tx() __this_cpu_read(in_tx)

/*
 * CLI aborts, so avoid it inside transactions
 * We don't need it because interrupts aborts anyways.
 */

inline void rtm_restore_fl(unsigned long flags)
{
	if (flags & X86_EFLAGS_IF)
		this_cpu_write(cli_elided, false);
	if (!is_in_tx())
		native_restore_fl(flags);
}
PV_CALLEE_SAVE_REGS_THUNK(rtm_restore_fl);

inline void rtm_irq_disable(void)
{
	if (!is_in_tx())
		native_irq_disable();
	else if (native_save_fl() & X86_EFLAGS_IF)
		this_cpu_write(cli_elided, true);
}
PV_CALLEE_SAVE_REGS_THUNK(rtm_irq_disable);

inline void rtm_irq_enable(void)
{
	if (!is_in_tx())
		native_irq_enable();
	this_cpu_write(cli_elided, false);
}
PV_CALLEE_SAVE_REGS_THUNK(rtm_irq_enable);

/*
 * This should be in the headers for inlining, but include loop hell
 * prevents it.
 */

inline int __elide_lock(void)
{
	if (!txn_disabled() && _xbegin() == _XBEGIN_STARTED) {
		start_in_tx();
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(__elide_lock);

inline void __elide_unlock(void)
{
	/*
	 * Note when you get a #GP here this usually means that you
	 * unlocked a lock that was not locked. Please fix your code.
	 */
	end_in_tx();
	_xend();
}
EXPORT_SYMBOL(__elide_unlock);

static unsigned rtm_patch(u8 type, u16 clobbers, void *ibuf,
			  unsigned long addr, unsigned len)
{
	switch (type) {
	case PARAVIRT_PATCH(pv_irq_ops.irq_enable):
	case PARAVIRT_PATCH(pv_irq_ops.irq_disable):
	case PARAVIRT_PATCH(pv_irq_ops.restore_fl):
		return paravirt_patch_default(type, clobbers, ibuf, addr, len);
	default:
		return native_patch(type, clobbers, ibuf, addr, len);
	}
}

void __init init_rtm(void)
{
	if (!boot_cpu_has(X86_FEATURE_RTM))
		return;

	if (strcmp(pv_info.name, "bare hardware")) {
		pr_info("No TSX lock elision because of conflicting paravirt ops\n");
		return;
	}

	pr_info("Enabling Intel TSX lock elision\n");
	pv_info.name = "rtm locking";

	pv_irq_ops.irq_disable = PV_CALLEE_SAVE(rtm_irq_disable);
	pv_irq_ops.irq_enable = PV_CALLEE_SAVE(rtm_irq_enable);
	pv_irq_ops.restore_fl = PV_CALLEE_SAVE(rtm_restore_fl);
	pv_init_ops.patch = rtm_patch;
}

/* jump_labels can be only initialized late */
static int __init init_rtm_late(void)
{
	if (strcmp(pv_info.name, "rtm locking"))
		return 0;
	return 0;
}
__initcall(init_rtm_late);
