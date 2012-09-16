#ifndef _LINUX_RTM
#define _LINUX_RTM 1

#ifdef CONFIG_RTM_LOCKS
#include <asm/rtm.h>
#else
/* Make transactions appear as always abort */
#define _XBEGIN_STARTED 0
#define _xbegin() 1
#define _xtest()  0
#define _xend()   do {} while (0)
#define _xabort(x) do {} while (0)
#endif

#endif
