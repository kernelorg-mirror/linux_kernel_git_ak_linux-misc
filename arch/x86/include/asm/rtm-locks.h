#ifndef _ASM_RTM_LOCKS
#define _ASM_RTM_LOCKS 1

#include <asm/rwlock.h>

/* spinlocks */
void rtm_spin_unlock(struct arch_spinlock *lock);
void rtm_spin_unlock_flags(struct arch_spinlock *lock,
			   unsigned long flags);
void rtm_spin_unlock_irq(struct arch_spinlock *lock);

/* rwlocks */

void rtm_read_unlock(arch_rwlock_t *rw);
void rtm_read_unlock_irq(arch_rwlock_t *rw);
void rtm_read_unlock_irqrestore(arch_rwlock_t *rw, unsigned long flags);
void rtm_write_unlock(arch_rwlock_t *rw);
void rtm_write_unlock_irq(arch_rwlock_t *rw);
void rtm_write_unlock_irqrestore(arch_rwlock_t *rw, unsigned long flags);

#endif
