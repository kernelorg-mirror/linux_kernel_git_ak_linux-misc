#ifndef _ASM_RTM_LOCKS
#define _ASM_RTM_LOCKS 1

#include <asm/rwlock.h>

/* rwlocks */

void rtm_read_lock(arch_rwlock_t *rw);
void rtm_read_unlock(arch_rwlock_t *rw);
void rtm_read_unlock_irq(arch_rwlock_t *rw);
void rtm_read_unlock_irqrestore(arch_rwlock_t *rw, unsigned long flags);
int rtm_read_trylock(arch_rwlock_t *rw);
void rtm_write_lock(arch_rwlock_t *rw);
void rtm_write_unlock(arch_rwlock_t *rw);
void rtm_write_unlock_irq(arch_rwlock_t *rw);
void rtm_write_unlock_irqrestore(arch_rwlock_t *rw, unsigned long flags);

#endif
