/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CLEARCPU_H
#define _LINUX_CLEARCPU_H 1

#include <linux/preempt.h>

#ifdef CONFIG_ARCH_HAS_CLEAR_CPU
#include <asm/clearcpu.h>
#else
static inline void lazy_clear_cpu(void)
{
}
#endif

/*
 * Use this function when potentially touching (reading or writing)
 * user data in an interrupt. In this case schedule to clear the
 * CPU buffers on kernel exit to avoid any potential side channels.
 *
 * If not in an interrupt we assume the touched data belongs to the
 * current process and doesn't need to be cleared.
 *
 * This version is for code who might be in an interrupt.
 * If you know for sure you're in interrupt context call
 * lazy_clear_cpu directly.
 *
 * lazy_clear_cpu is reasonably cheap (just sets a bit) and
 * can be used in fast paths.
 */
static inline void lazy_clear_cpu_interrupt(void)
{
	if (in_interrupt())
		lazy_clear_cpu();
}

#endif
