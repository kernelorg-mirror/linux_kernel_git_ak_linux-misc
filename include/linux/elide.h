#ifndef _LINUX_ELIDE_H
#define _LINUX_ELIDE_H 1

#include <linux/rtm.h>

#ifdef CONFIG_RTM_LOCKS
#include <asm/elide.h>
#else
#define elide_lock(l, f) 0
#define elide_lock_adapt(f, l, a, ac) 0
#define elide_unlock(l) 0
#define elide_abort() do {} while (0)
struct elision_config {};
#define DEFAULT_ELISION_CONFIG {}
#define TUNE_ELISION_CONFIG(a, b)
#endif

#endif
