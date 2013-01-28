#ifndef _ASM_ELIDE_H
#define _ASM_ELIDE_H 1

#ifdef CONFIG_RTM_LOCKS
#include <asm/rtm.h>

struct elision_config {
	short internal_abort_skip;
	short lock_busy_skip;
	short other_abort_skip;
	short conflict_abort_skip;
	int conflict_retry;
	int retry_timeout;
	int lock_busy_retry;
};

/* Tuning preliminary */
#define DEFAULT_ELISION_CONFIG {	\
	.internal_abort_skip = 5,	\
	.lock_busy_skip = 3,		\
	.other_abort_skip = 3,		\
	.conflict_abort_skip = 3,	\
	.conflict_retry = 3,		\
	.retry_timeout = 500,		\
	.lock_busy_retry = 3,		\
}

#define TUNE_ELISION_CONFIG(prefix, name)				\
	module_param_named(prefix ## _internal_abort_skip,		\
			   name.internal_abort_skip, short, 0644);	\
	module_param_named(prefix ## _lock_busy_skip,			\
			   name.lock_busy_skip, short, 0644);		\
	module_param_named(prefix ## _other_abort_skip,			\
			   name.other_abort_skip, short, 0644);		\
	module_param_named(prefix ## _conflict_abort_skip,		\
			   name.conflict_abort_skip, short, 0644);	\
	module_param_named(prefix ## _conflict_retry,			\
			   name.conflict_retry, int, 0644);		\
	module_param_named(prefix ## _retry_timeout,			\
			   name.retry_timeout, int, 0644);		\
	module_param_named(prefix ## _lock_busy_retry,			\
			   name.lock_busy_retry, int, 0644)


/*
 * These are out of line unfortunately, just to avoid
 * a nasty include loop with per cpu data.
 * (FIXME)
 */
extern int __elide_lock(void);
extern int __elide_lock_adapt(short *adapt, struct elision_config *config,
			      int *retry);
extern void __elide_unlock(void);

/*
 * Simple lock elision wrappers for locks.
 * f is the static key that enables/disables elision
 * l must be evaluated by the macro later, and yield 1
 * when the lock is free.
 *
 * TBD should use static_keys too, but that needs
 * more changes to avoid include loop hell with users.
 */

#define elide_lock(f, l) ({		\
	int flag = 0;			\
	if ((f) && __elide_lock()) {	\
		if (l)			\
			flag = 1;	\
		else			\
			_xabort(0xff);	\
	}				\
	flag;				\
})

enum { ELIDE_TXN, ELIDE_STOP, ELIDE_RETRY };

/*
 * Adaptive elision lock wrapper
 *
 * Like above. a is a pointer to a
 * short adaption count stored in the lock.
 * config is a pointer to a elision_config for the lock type
 *
 * This is a bit convulted because we need a retry loop with a lock test
 * to wait for the lock freeing again. Right now we just spin up to
 * a defined number of iterations.
 *
 * Ideally every lock that can afford to have a 16 bit count stored
 * in it should use this variant.
 */
#define elide_lock_adapt(f, l, a, config) ({			\
	int flag = 0;						\
	if (static_key_true(&(f))) {				\
		int retry = (config)->conflict_retry;		\
		int timeout = (config)->retry_timeout;		\
		int status;					\
		again:						\
		status = __elide_lock_adapt(a, config, &retry); \
		/* Retries wait until the lock is free. */	\
		if (unlikely(status == ELIDE_RETRY)) {		\
			while (!(l) && --timeout > 0)		\
				cpu_relax();			\
			if (timeout > 0)			\
				goto again;			\
		}						\
		if (likely(status != ELIDE_STOP)) {		\
			/* in transaction. check now if the lock is free. */ \
			if (likely(l))				\
				flag = 1;			\
			else					\
				_xabort(0xff);			\
		}						\
	}							\
	flag;							\
})


/*
 * Note that if you see a general protection fault
 * in the _xend you have a unmatched unlock. Please fix
 * your code.
 */

#define elide_unlock(l) ({			\
	int flag = 0;				\
	if (l)	{				\
		__elide_unlock();		\
		flag = 1;			\
	}					\
	flag;					\
})

/*
 * Use for code that cannot elide, primarily code that queries
 * the lock state.
 */
#define elide_abort() _xabort(0xfe)

#endif
#endif
