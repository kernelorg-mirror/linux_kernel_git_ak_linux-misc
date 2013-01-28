#ifndef _ASM_MUTEX_H
#define _ASM_MUTEX_H 1

#ifdef CONFIG_X86_32
# include <asm/mutex_32.h>
#else
# include <asm/mutex_64.h>
#endif

#define  ARCH_HAS_MUTEX_AND_OWN 1

#include <linux/elide.h>
#include <linux/jump_label.h>

extern struct static_key mutex_elision;
extern struct elision_config mutex_elision_config;

/*
 * Try speculation first and only do the normal locking and owner setting
 * if that fails.
 */

#define mutex_free(l) (atomic_read(&(l)->count) == 1)

#define __mutex_fastpath_lock_and_own(l, s) ({				\
			if (!elide_lock_adapt(mutex_elision,		\
					mutex_free(l),			\
					&l->elision_adapt,		\
					&mutex_elision_config)) {	\
				__mutex_fastpath_lock(&(l)->count, s);	\
				mutex_set_owner(l);			\
			}						\
		})

#define __mutex_fastpath_unlock_and_unown(l, s) ({			\
			if (!elide_unlock(mutex_free(l))) {		\
				mutex_unlock_clear_owner(l);		\
				__mutex_fastpath_unlock(&(l)->count, s); \
			}						\
		})

#define __mutex_fastpath_lock_retval_and_own(l, s) ({			\
			int ret = 0;					\
			if (!elide_lock_adapt(mutex_elision,		\
					      mutex_free(l),		\
					      &l->elision_adapt,	\
					      &mutex_elision_config)) { \
				ret = __mutex_fastpath_lock_retval(&(l)->count, s); \
				if (!ret)				\
					mutex_set_owner(l);		\
			}						\
			ret; })

#define __mutex_fastpath_trylock_and_own(l, s) ({			\
			int ret = 1;					\
			if (!elide_lock_adapt(mutex_elision,		\
					      mutex_free(l),		\
					      &l->elision_adapt,	\
					      &mutex_elision_config)) { \
				ret = __mutex_fastpath_trylock(&(l)->count, s);	\
				if (ret)				\
					mutex_set_owner(l);		\
			}						\
			ret; })

#endif
