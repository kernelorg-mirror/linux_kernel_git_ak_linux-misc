/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <linux/types.h>
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
	 * stuffing, so an alternative doesn't make sense here. But we
	 * have to use it anyways just to shut up objfool.
	 */
	FILL_RETURN_BUFFER %rax, RSB_FILL_LOOPS, X86_FEATURE_RSB_UNDERFLOW
	movl	$CALL_DEPTH_INIT, PER_CPU_VAR(__call_depth__)
	popq	%rax
	ret
ENDPROC(calldepth_hook)

/*
 * Return hook to maintain call depth count.
 *
 * When we context switch and return 16 times
 * we would underflow. So check for underflow too
 * and fill then.
 *
 * This is only done at the first underflow to avoid
 * always filling on the first return.
 */
ENTRY(__return__)
	incl	PER_CPU_VAR(__call_depth__)
	cmpl	$CALL_DEPTH_INIT+1, PER_CPU_VAR(__call_depth__)
	je	1f
	ret
1:
	pushq	%rax
	/*
	 * This function is only patched in on demand when we need RSB
	 * stuffing, so an alternative doesn't make sense here. But we
	 * have to use it anyways just to shut up objfool.
	 */
	FILL_RETURN_BUFFER %rax, RSB_FILL_LOOPS, X86_FEATURE_RSB_UNDERFLOW
	/*
	 * Set to half so that the next return will not underflow
	 * again. Drawback is that the next deep nesting will
	 * overflow earlier.
	 */
	movl	$CALL_DEPTH_INIT/2, PER_CPU_VAR(__call_depth__)
	popq	%rax
	ret
ENDPROC(__return__)
