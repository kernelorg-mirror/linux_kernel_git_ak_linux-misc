/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __NOSPEC_BRANCH_H__
#define __NOSPEC_BRANCH_H__

#include <asm/alternative.h>
#include <asm/alternative-asm.h>
#include <asm/cpufeatures.h>

#ifdef __ASSEMBLY__

/*
 * These are the bare retpoline primitives for indirect jmp and call.
 * Do not use these directly; they only exist to make the ALTERNATIVE
 * invocation below less ugly.
 */
.macro RETPOLINE_JMP reg:req
	call	1112f
1111:	pause
	jmp	1111b
1112:	mov	\reg, (%_ASM_SP)
	ret
.endm

.macro RETPOLINE_CALL reg:req
	jmp	1113f
1110:	RETPOLINE_JMP \reg
1113:	call	1110b
.endm

/*
 * NOSPEC_JMP and NOSPEC_CALL macros can be used instead of a simple
 * indirect jmp/call which may be susceptible to the Spectre variant 2
 * attack.
 */
.macro NOSPEC_JMP reg:req
#ifdef CONFIG_RETPOLINE
	ALTERNATIVE_2 __stringify(jmp *\reg),				\
		__stringify(RETPOLINE_JMP \reg), X86_FEATURE_RETPOLINE,	\
		__stringify(lfence; jmp *\reg), X86_FEATURE_RETPOLINE_AMD
#else
	jmp	*\reg
#endif
.endm

.macro NOSPEC_CALL reg:req
#ifdef CONFIG_RETPOLINE
	ALTERNATIVE_2 __stringify(call *\reg),				\
		__stringify(RETPOLINE_CALL \reg), X86_FEATURE_RETPOLINE,\
		__stringify(lfence; call *\reg), X86_FEATURE_RETPOLINE_AMD
#else
	call	*\reg
#endif
.endm

/*
 * Fill the CPU return branch buffer to prevent
 * indirect branch prediction on underflow.
 *
 * Caller should check for X86_FEATURE_SMEP.
 */
.macro FILL_RETURN_BUFFER
	.rept	32
	call	1221f
	pause	/* stop speculation */
	/* should be marked unreachable */
1221:
	.endr
#ifdef CONFIG_64BIT
	addq	$8*32, %rsp
#else
	addl    $4*32, %esp
#endif
.endm

#else /* __ASSEMBLY__ */

#if defined(CONFIG_X86_64) && defined(RETPOLINE)
/*
 * Since the inline asm uses the %V modifier which is only in newer GCC,
 * the 64-bit one is dependent on RETPOLINE not CONFIG_RETPOLINE.
 */
# define NOSPEC_CALL ALTERNATIVE(				\
	"call *%[thunk_target]\n",				\
	"call __x86.indirect_thunk.%V[thunk_target]\n",		\
	X86_FEATURE_RETPOLINE)
# define THUNK_TARGET(addr) [thunk_target] "r" (addr)
#elif defined(CONFIG_X86_64) && defined(CONFIG_RETPOLINE)
/*
 * For i386 we use the original ret-equivalent retpoline, because
 * otherwise we'll run out of registers. We don't care about CET
 * here, anyway.
 */
# define NOSPEC_CALL ALTERNATIVE(				\
	"call	*%[thunk_target]\n",				\
	"       jmp    1113f; "					\
	"1110:  call   1112f; "					\
	"1111:	pause; "					\
	"       jmp    1111b; "					\
	"1112:	movl   %[thunk_target], (%esp); "		\
	"       ret; "						\
	"1113:  call   1110b;\n",				\
	X86_FEATURE_RETPOLINE)
# define THUNK_TARGET(addr) [thunk_target] "rm" (addr)
#else /* No retpoline */
# define NOSPEC_CALL "call *%[thunk_target]\n"
# define THUNK_TARGET(addr) [thunk_target] "rm" (addr)
#endif

/* Fill the CPU return branch buffer */

static inline void fill_return_buffer(void)
{
	if (boot_cpu_has(X86_BUG_CPU_MELTDOWN) &&
	    !boot_cpu_has(X86_FEATURE_SMEP))
		asm volatile(
			"	.rept 32\n"
			"	call  1221f\n"
			"	pause\n"	/* stop speculation */
			ASM_UNREACHABLE
			"1221:\n"
			"	.endr\n"
#ifdef CONFIG_64BIT
			"	addq $32*8, %%rsp"
#else
			"	addl $32*4, %%esp"
#endif

			::: "memory");
}

#endif /* __ASSEMBLY__ */
#endif /* __NOSPEC_BRANCH_H__ */
