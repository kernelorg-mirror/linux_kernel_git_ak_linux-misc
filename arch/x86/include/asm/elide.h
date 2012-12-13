#ifndef _ASM_ELIDE_H
#define _ASM_ELIDE_H 1

#ifdef CONFIG_RTM_LOCKS
#include <asm/rtm.h>

/*
 * These are out of line unfortunately, just to avoid
 * a nasty include loop with per cpu data.
 * (FIXME)
 */
extern int __elide_lock(void);
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
	int flag = 0;				\
	if (state) {				\
		if (!(check))			\
			_xabort(0xfe);		\
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
