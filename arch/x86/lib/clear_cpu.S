/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/linkage.h>
#include <asm/alternative-asm.h>
#include <asm/cpufeatures.h>

/*
 * Clear internal CPU buffers on kernel boundaries.
 *
 * These sequences are somewhat fragile, please don't add
 * or change instructions in the middle of the areas marked with
 * start/end.
 *
 * Interrupts and NMIs we deal with by reclearing. We clear parts
 * of the kernel stack, which has other advantages too.
 *
 * Save all registers to make it easier to use for callers.
 *
 * This sequence is for Nehalem-IvyBridge. For Haswell we jump
 * to hsw_clear_buf.
 *
 * These functions need to be called on a full stack, as they may
 * use upto 1.5k of stack. They should be also called with
 * interrupts disabled. NMIs etc. are handled by letting every
 * NMI do its own clear sequence.
 */
ENTRY(ivb_clear_cpu)
GLOBAL(do_clear_cpu)
	/*
	 * obj[tf]ool complains about unreachable code here,
	 * which appears to be spurious?!?
	 */
	ALTERNATIVE "", "jmp hsw_clear_cpu", X86_BUG_MDS_CLEAR_CPU_HSW
	push %__ASM_REG(si)
	push %__ASM_REG(di)
	push %__ASM_REG(cx)
	mov %_ASM_SP, %__ASM_REG(si)
	sub  $2*16, %_ASM_SP
	and  $-16,%_ASM_SP
	movdqa %xmm0, (%_ASM_SP)
	movdqa %xmm1, 1*16(%_ASM_SP)
	sub  $672, %_ASM_SP
	xorpd %xmm0,%xmm0
	movdqa %xmm0, (%_ASM_SP)
	movdqa %xmm0, 16(%_ASM_SP)
	mov %_ASM_SP, %__ASM_REG(di)
	/* Clear sequence start */
	movdqu %xmm0,(%__ASM_REG(di))
	lfence
	orpd (%__ASM_REG(di)), %xmm0
	orpd (%__ASM_REG(di)), %xmm1
	mfence
	movl $40, %ecx
	add  $32, %__ASM_REG(di)
1:	movntdq %xmm0, (%__ASM_REG(di))
	add  $16, %__ASM_REG(di)
	decl %ecx
	jnz  1b
	mfence
	/* Clear sequence end */
	add  $672, %_ASM_SP
	movdqu (%_ASM_SP), %xmm0
	movdqu 1*16(%_ASM_SP), %xmm1
	mov  %__ASM_REG(si),%_ASM_SP
	pop %__ASM_REG(cx)
	pop %__ASM_REG(di)
	pop %__ASM_REG(si)
	ret
END(ivb_clear_cpu)

/*
 * Version for Haswell/Broadwell.
 */
ENTRY(hsw_clear_cpu)
	push %__ASM_REG(si)
	push %__ASM_REG(di)
	push %__ASM_REG(cx)
	push %__ASM_REG(ax)
	mov  %_ASM_SP, %__ASM_REG(ax)
	sub  $16, %_ASM_SP
	and  $-16,%_ASM_SP
	movdqa %xmm0, (%_ASM_SP)
	/* Clear sequence start */
	xorpd %xmm0,%xmm0
	sub  $1536,%_ASM_SP
	mov  %_ASM_SP, %__ASM_REG(si)
	mov  %__ASM_REG(si), %__ASM_REG(di)
	movl $40,%ecx
1:	movntdq %xmm0, (%__ASM_REG(di))
	add  $16, %__ASM_REG(di)
	decl %ecx
	jnz  1b
	mfence
	mov  %__ASM_REG(si), %__ASM_REG(di)
	mov $1536, %ecx
	rep movsb
	lfence
	/* Clear sequence end */
	add $1536,%_ASM_SP
	movdqa (%_ASM_SP), %xmm0
	mov %__ASM_REG(ax),%_ASM_SP
	pop %__ASM_REG(ax)
	pop %__ASM_REG(cx)
	pop %__ASM_REG(di)
	pop %__ASM_REG(si)
	ret
END(hsw_clear_cpu)
