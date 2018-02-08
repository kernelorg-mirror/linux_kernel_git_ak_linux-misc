/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <linux/compiler.h>
#include <asm/nospec-branch.h>
#include <asm/types.h>
#include <asm/percpu.h>

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
	pushq	%rax
	/*
	 * This function is only patched in on demand when we need RSB
	 * stuffing, so an alternative doesn't make sense here. It would
	 * seem dumb to add an completely unnecessary alternative here
	 * just to shut up objfool.
	 */
	__FILL_RETURN_BUFFER(%rax, RSB_FILL_LOOPS , %rsp)
	movl	$CALL_DEPTH_INIT, PER_CPU_VAR(__call_depth__)
	popq	%rax
	ret
END(calldepth_hook)
