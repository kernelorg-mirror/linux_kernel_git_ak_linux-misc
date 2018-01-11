#ifndef DEEPSTACK_H
#define DEEPSTACK_H 1

/* Annotation for deep call chains */

#include <linux/thread_info.h>

#ifdef CONFIG_RETPOLINE
#include <asm/nospec-branch.h>
#endif

/* Mark the stack so that the detector doesn't do a false positive */
static inline void start_deep_call_chain(void)
{
#ifdef CONFIG_DEBUG_STACK_DEPTH
	set_thread_flag(TIF_DEEP_STACK);
#endif
}

static inline void end_deep_call_chain(void)
{
#ifdef CONFIG_RETPOLINE
	fill_return_buffer();
#endif
}

#endif
