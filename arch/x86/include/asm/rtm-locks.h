#ifndef _ASM_RTM_LOCKS
#define _ASM_RTM_LOCKS 1

/* spinlocks */
void rtm_spin_unlock(struct arch_spinlock *lock);
void rtm_spin_unlock_flags(struct arch_spinlock *lock,
			   unsigned long flags);
void rtm_spin_unlock_irq(struct arch_spinlock *lock);

#endif
