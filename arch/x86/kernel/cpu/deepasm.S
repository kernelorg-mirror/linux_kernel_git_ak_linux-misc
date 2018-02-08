/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <linux/compiler.h>
#include <asm/nospec-branch.h>
#include <asm/types.h>

/*
 * Maintain call depth for Spectre deep chain workarounds.
 * This is very optimized because it runs by default for most
 * functions.  This version is used when no other ftrace hook user
 * (ftrace, kprobes etc.) is active.
 */

ENTRY(calldepth_hook)
	decl	PER_CPU_VAR(__call_depth__)
	jz	1f
	ret
1:
	pushq	%rbx
	call	__fill_rsb
	movl	$CALL_DEPTH_INIT, PER_CPU_VAR(__call_depth__)
	popq	%rbx
	ret
END(calldepth_hook)
