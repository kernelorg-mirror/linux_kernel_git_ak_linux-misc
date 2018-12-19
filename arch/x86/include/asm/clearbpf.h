/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_CLEARBPF_H
#define _ASM_CLEARBPF_H 1

#include <linux/clearcpu.h>
#include <linux/cred.h>
#include <asm/cpufeatures.h>

/*
 * When the BPF program was loaded unprivileged, clear the CPU
 * to prevent any exploits written in BPF using side channels to read
 * data leaked from other kernel code. In some cases, like
 * process context with the same uid, we can avoid it.
 *
 * See Documentation/clearcpu.txt for more details.
 */
static inline void arch_bpf_prepare_nonpriv(kuid_t uid)
{
	if (!static_cpu_has(X86_BUG_MDS))
		return;
	if (in_interrupt() ||
	    __this_cpu_read(clear_cpu_flag) ||
	    !uid_eq(current_euid(), uid)) {
		clear_cpu();
		__this_cpu_write(clear_cpu_flag, 0);
	}
}

#endif
