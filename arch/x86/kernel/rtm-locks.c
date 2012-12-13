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
 * For spinlocks use paravirt ops to hook in the RTM lock elision.  For
 * interrupt disabling we also use the pvops to patch in our own code
 * that avoids aborts. For other locks that are not supported by pvops
 * use direct hooks.
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
#include <asm/rtm.h>
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

static struct static_key spinlock_elision = STATIC_KEY_INIT_TRUE;
module_param(spinlock_elision, static_key, 0644);

static int rtm_spin_trylock(struct arch_spinlock *lock)
{
	if (elide_lock(spinlock_elision, !__ticket_spin_is_locked(lock)))
		return 1;
	return __ticket_spin_trylock(lock);
}

static inline void rtm_spin_lock(struct arch_spinlock *lock)
{
	if (!elide_lock(spinlock_elision, !__ticket_spin_is_locked(lock)))
		__ticket_spin_lock(lock);
}

static void rtm_spin_lock_flags(struct arch_spinlock *lock, unsigned long flags)
{
	rtm_spin_lock(lock);
}

static inline void
rtm_spin_unlock_check(struct arch_spinlock *lock, bool not_enabling)
{
	/*
	 * Note when you get a #GP here this usually means that you
	 * unlocked a lock that was not locked. Please fix your code.
	 */
	if (!__ticket_spin_is_locked(lock)) {
		/*
		 * Unlock without restoring interrupts without restoring
		 * interrupts that were disabled nested.
		 * In this case we have to abort.
		 */
		if (not_enabling && this_cpu_read(cli_elided) &&
		    this_cpu_read(in_tx) == 1)
			_xabort(0xfc);
		end_in_tx();
		_xend();
	} else
		__ticket_spin_unlock(lock);
}

static void rtm_spin_unlock(struct arch_spinlock *lock)
{
	rtm_spin_unlock_check(lock, true);
}

static void rtm_spin_unlock_flags(struct arch_spinlock *lock,
				  unsigned long flags)
{
	rtm_spin_unlock_check(lock, !(flags & X86_EFLAGS_IF));
	local_irq_restore(flags);
}

static void rtm_spin_unlock_irq(struct arch_spinlock *lock)
{
	rtm_spin_unlock_check(lock, false);
	local_irq_enable();
}

static int rtm_spin_is_locked(struct arch_spinlock *lock)
{
	/*
	 * Cannot tell reliably if the lock is locked or not
	 * when we're in a transaction. So abort instead.
	 */
	_xabort(0xfe);
	return __ticket_spin_is_locked(lock);
}

/*
 * rwlocks: both readers and writers freely speculate.
 * This uses direct calls with static patching, not pvops.
 */

__read_mostly bool rwlock_elision = true;
module_param(rwlock_elision, bool, 0644);

void rtm_read_lock(arch_rwlock_t *rw)
{
	/*
	 * Abort when there is a writer.
	 * In principle we don't care about readers here,
	 * but since they are on the same cache line they
	 * would abort anyways.
	 */

	if (!elide_lock(rwlock_elision, !arch_rwlock_is_locked(rw)))
		arch_do_read_lock(rw);
}
EXPORT_SYMBOL(rtm_read_lock);

static inline void rtm_read_unlock_check(arch_rwlock_t *rw, bool not_enabling)
{
	/*
	 * Note when you get a #GP here this usually means that you
	 * unlocked a lock that was not locked. Please fix your code.
	 */
	if (!arch_rwlock_is_locked(rw)) {
		if (not_enabling && this_cpu_read(cli_elided) &&
		    this_cpu_read(in_tx) == 1)
			_xabort(0xfd);
		end_in_tx();
		_xend();
	} else
		arch_do_read_unlock(rw);
}

void rtm_read_unlock(arch_rwlock_t *rw)
{
	rtm_read_unlock_check(rw, true);
}
EXPORT_SYMBOL(rtm_read_unlock);

void rtm_read_unlock_irq(arch_rwlock_t *rw)
{
	rtm_read_unlock_check(rw, false);
	local_irq_enable();
}
EXPORT_SYMBOL(rtm_read_unlock_irq);

void rtm_read_unlock_irqrestore(arch_rwlock_t *rw, unsigned long flags)
{
	rtm_read_unlock_check(rw, !(flags & X86_EFLAGS_IF));
	local_irq_restore(flags);
}
EXPORT_SYMBOL(rtm_read_unlock_irqrestore);

int rtm_read_trylock(arch_rwlock_t *rw)
{
	if (elide_lock(rwlock_elision, !arch_rwlock_is_locked(rw)))
		return 1;
	return arch_do_read_trylock(rw);
}
EXPORT_SYMBOL(rtm_read_trylock);

void rtm_write_lock(arch_rwlock_t *rw)
{
	if (!elide_lock(rwlock_elision, !arch_write_can_lock(rw)))
		arch_do_write_lock(rw);
}
EXPORT_SYMBOL(rtm_write_lock);

static inline void rtm_write_unlock_check(arch_rwlock_t *rw, bool not_enabling)
{
	/*
	 * Note when you get a #GP here this usually means that you
	 * unlocked a lock that was not locked. Please fix your code.
	 */
	if (!arch_rwlock_is_locked(rw)) {
		if (not_enabling && this_cpu_read(cli_elided) &&
		    this_cpu_read(in_tx) == 1)
			_xabort(0xfd);
		end_in_tx();
		_xend();
	} else
		arch_do_write_unlock(rw);
}

void rtm_write_unlock(arch_rwlock_t *rw)
{
	rtm_write_unlock_check(rw, true);
}
EXPORT_SYMBOL(rtm_write_unlock);

void rtm_write_unlock_irq(arch_rwlock_t *rw)
{
	rtm_write_unlock_check(rw, false);
	local_irq_enable();
}
EXPORT_SYMBOL(rtm_write_unlock_irq);

void rtm_write_unlock_irqrestore(arch_rwlock_t *rw, unsigned long flags)
{
	rtm_write_unlock_check(rw, !(flags & X86_EFLAGS_IF));
	local_irq_restore(flags);
}
EXPORT_SYMBOL(rtm_write_unlock_irqrestore);

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


/*
 * CLI aborts, so avoid it inside transactions
 *
 * Could also turn non txn cli into transactions?
 */

static void rtm_restore_fl(unsigned long flags)
{
	if (flags & X86_EFLAGS_IF)
		this_cpu_write(cli_elided, false);
	if (!_xtest())
		native_restore_fl(flags);
}
PV_CALLEE_SAVE_REGS_THUNK(rtm_restore_fl);

static void rtm_irq_disable(void)
{
	if (!_xtest())
		native_irq_disable();
	else if (native_save_fl() & X86_EFLAGS_IF)
		this_cpu_write(cli_elided, true);
}
PV_CALLEE_SAVE_REGS_THUNK(rtm_irq_disable);

static void rtm_irq_enable(void)
{
	if (!_xtest())
		native_irq_enable();
	this_cpu_write(cli_elided, false);
}
PV_CALLEE_SAVE_REGS_THUNK(rtm_irq_enable);

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

void __init init_rtm_spinlocks(void)
{
	if (!boot_cpu_has(X86_FEATURE_RTM))
		return;

	if (strcmp(pv_info.name, "bare hardware")) {
		pr_info("No TSX lock elision because of conflicting paravirt ops\n");
		return;
	}

	pr_info("Enabling TSX based elided spinlocks\n");
	pv_info.name = "rtm locking";
	/* spin_is_contended will lie now */
	pv_lock_ops.spin_lock = rtm_spin_lock;
	pv_lock_ops.spin_lock_flags = rtm_spin_lock_flags;
	pv_lock_ops.spin_trylock = rtm_spin_trylock;
	pv_lock_ops.spin_unlock = rtm_spin_unlock;
	pv_lock_ops.spin_unlock_flags = rtm_spin_unlock_flags;
	pv_lock_ops.spin_unlock_irq = rtm_spin_unlock_irq;
	pv_lock_ops.spin_is_locked = rtm_spin_is_locked;

	pv_irq_ops.irq_disable = PV_CALLEE_SAVE(rtm_irq_disable);
	pv_irq_ops.irq_enable = PV_CALLEE_SAVE(rtm_irq_enable);
	pv_irq_ops.restore_fl = PV_CALLEE_SAVE(rtm_restore_fl);
	pv_init_ops.patch = rtm_patch;
}

__read_mostly bool mutex_elision = true;
module_param(mutex_elision, bool, 0644);
