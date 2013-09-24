#ifndef _ASM_ELIDE_H
#define _ASM_ELIDE_H 1

#ifdef CONFIG_RTM_LOCKS
#include <asm/rtm.h>

/* Per CPU statistics */
struct elision_stat {
	int total_skips;		/* total number of skips */
	int start_skips;		/* number of start skippings */
};

/* Configuration parameters for adaptive elision algorithm */
struct elision_config {
	/* skipping means not eliding the lock for the next N invocations. */
	short internal_abort_skip;	/* skip on internal abort. */
	short lock_busy_skip;		/* skip on lock-busy */
	short other_abort_skip;		/* skip on other aborts */
	short conflict_abort_skip;	/* skip on conflicts */
	short capacity_abort_skip;	/* skip on capacity overflows */
	int conflict_retry;		/* number of retries on conflict */
	int retry_timeout;		/* number of spins waiting for lock
					   free on retry. */
	int lock_busy_retry;		/* number of retries on lock busy */
	struct elision_stat *stat __percpu;
};

/* Tuning preliminary */
#define DEFAULT_ELISION_CONFIG(prefix, ...) {	\
	.internal_abort_skip = 5,	\
	.lock_busy_skip = 3,		\
	.other_abort_skip = 5,		\
	.conflict_abort_skip = 5,	\
	.conflict_retry = 3,		\
	.retry_timeout = 500,		\
	.lock_busy_retry = 3,		\
	.capacity_abort_skip = 10,	\
	.stat = &prefix ## _el_stat, ## __VA_ARGS__ \
}

#define DEFINE_ELISION_CONFIG(ST, prefix, name, ...)			\
	ST __read_mostly struct elision_config name;			\
	module_param_named(prefix ## _internal_abort_skip,		\
			   name.internal_abort_skip, short, 0644);\
	module_param_named(prefix ## _lock_busy_skip,			\
			   name.lock_busy_skip, short, 0644);		\
	module_param_named(prefix ## _other_abort_skip,			\
			   name.other_abort_skip, short, 0644);		\
	module_param_named(prefix ## _conflict_abort_skip,		\
			   name.conflict_abort_skip, short, 0644);	\
	module_param_named(prefix ## _capacity_abort_skip,		\
			   name.capacity_abort_skip, short, 0644);	\
	module_param_named(prefix ## _conflict_retry,			\
			   name.conflict_retry, int, 0644);		\
	module_param_named(prefix ## _retry_timeout,			\
			   name.retry_timeout, int, 0644);		\
	module_param_named(prefix ## _lock_busy_retry,			\
			   name.lock_busy_retry, int, 0644);		\
	static DEFINE_PER_CPU(struct elision_stat, prefix ## _el_stat);	\
	module_param_cb(prefix ## _total_skips, &param_ops_percpu_uint,	\
			&prefix ## _el_stat.total_skips, 0644);		\
	module_param_cb(prefix ## _start_skips, &param_ops_percpu_uint,	\
			&prefix ## _el_stat.start_skips, 0644);		\
	ST __read_mostly struct elision_config name =			\
		DEFAULT_ELISION_CONFIG(prefix, ## __VA_ARGS__)

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
 */

#define elide_lock(f, l) ({		\
	int flag = 0;			\
	if (static_key_false(&(f)) && __elide_lock()) {	\
		if (l)			\
			flag = 1;	\
		else			\
			_xabort(0xff);	\
	}				\
	flag;				\
})

enum { ELIDE_TXN, ELIDE_STOP, ELIDE_RETRY_WAIT, ELIDE_RETRY_FAST };

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
	if (static_key_false(&(f))) {				\
		int retry = (config)->conflict_retry;		\
		int timeout = (config)->retry_timeout;		\
		int status;					\
		again:						\
		status = __elide_lock_adapt(a, config, &retry); \
		/* lock-busy retries wait until the lock is free. */ \
		/* Right now we just spin, even for sleeping locks*/ \
		/* To prevent wasting too much time use a timeout */ \
		if (unlikely(status == ELIDE_RETRY_WAIT)) {	\
			while (!(l) && --timeout > 0)		\
				cpu_relax();			\
			if (timeout > 0)			\
				goto again;			\
		}						\
		/* Conflict retries retry immediately */	\
		if (unlikely(status == ELIDE_RETRY_FAST))	\
			goto again;				\
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
 * Check the lock variable only before commit.
 * Advantage: smaller window for conflicts
 * Disadvantage: More mis-speculation.
 *
 * When not using stateful elision set state to 1.
 */
#define elide_unlock_check(state, check) ({	\
	if (state) {				\
		if (!(check))			\
			_xabort(0xfe);		\
		__elide_unlock();		\
	}					\
	state;					\
})

/*
 * Use for code that cannot elide, primarily code that queries
 * the lock state.
 */
#define elide_abort() _xabort(0xfe)

#endif
#endif
