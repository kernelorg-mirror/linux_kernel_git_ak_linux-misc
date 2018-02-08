/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_X86_NOSPEC_BRANCH_H_
#define _ASM_X86_NOSPEC_BRANCH_H_

#include <asm/alternative.h>
#include <asm/alternative-asm.h>
#include <asm/cpufeatures.h>
#include <asm/percpu.h>

/*
 * 16 for skylake return buffer - 3x slack for __fentry__ and
 * uninstrumented functions. The instrumentation keeps a per
 * CPU counter in the range of 0...CALL_DEPTH_INIT.
 * If it ever over- or underflows the return buffer is filled.
 */
#define CALL_DEPTH_INIT		13

#ifdef __ASSEMBLY__

/*
 * This should be used immediately before a retpoline alternative.  It tells
 * objtool where the retpolines are so that it can make sense of the control
 * flow by just reading the original instruction(s) and ignoring the
 * alternatives.
 */
.macro ANNOTATE_NOSPEC_ALTERNATIVE
	.Lannotate_\@:
	.pushsection .discard.nospec
	.long .Lannotate_\@ - .
	.popsection
.endm

/*
 * These are the bare retpoline primitives for indirect jmp and call.
 * Do not use these directly; they only exist to make the ALTERNATIVE
 * invocation below less ugly.
 */
.macro RETPOLINE_JMP reg:req
	call	.Ldo_rop_\@
.Lspec_trap_\@:
	pause
	lfence
	jmp	.Lspec_trap_\@
.Ldo_rop_\@:
	mov	\reg, (%_ASM_SP)
	ret
.endm

/*
 * This is a wrapper around RETPOLINE_JMP so the called function in reg
 * returns to the instruction after the macro.
 */
.macro RETPOLINE_CALL reg:req
	jmp	.Ldo_call_\@
.Ldo_retpoline_jmp_\@:
	RETPOLINE_JMP \reg
.Ldo_call_\@:
	call	.Ldo_retpoline_jmp_\@
.endm

/*
 * JMP_NOSPEC and CALL_NOSPEC macros can be used instead of a simple
 * indirect jmp/call which may be susceptible to the Spectre variant 2
 * attack.
 */
.macro JMP_NOSPEC reg:req
#ifdef CONFIG_RETPOLINE
	ANNOTATE_NOSPEC_ALTERNATIVE
	ALTERNATIVE_2 __stringify(jmp *\reg),				\
		__stringify(RETPOLINE_JMP \reg), X86_FEATURE_RETPOLINE,	\
		__stringify(lfence; jmp *\reg), X86_FEATURE_RETPOLINE_AMD
#else
	jmp	*\reg
#endif
.endm

.macro CALL_NOSPEC reg:req
#ifdef CONFIG_RETPOLINE
	ANNOTATE_NOSPEC_ALTERNATIVE
	ALTERNATIVE_2 __stringify(call *\reg),				\
		__stringify(RETPOLINE_CALL \reg), X86_FEATURE_RETPOLINE,\
		__stringify(lfence; call *\reg), X86_FEATURE_RETPOLINE_AMD
#else
	call	*\reg
#endif
.endm

.macro STUFF_ONE_RSB
#ifdef CONFIG_RETPOLINE
	call 581f
	pause ; lfence
581:	add  $(BITS_PER_LONG/8), %_ASM_SP
#endif
.endm

/* This clobbers the BX register */
.macro FILL_RETURN_BUFFER nr:req ftr:req
#ifdef CONFIG_RETPOLINE
	ALTERNATIVE "", "call __clear_rsb" , \ftr
	ALTERNATIVE "", "STUFF_ONE_RSB", \ftr
#endif
.endm

	/*
	 * Maintain call-depth and fill return buffer if needed
	 * to guard against Spectre-V2 attacks on the CPU
	 * return buffer.
	 * This version is only used when the function_hook does
	 * something else too and falls through
	 * Otherwise we use the optimized calldepth_hook
	 */
	.macro DEEP_CHAIN_FILL
#ifdef CONFIG_DEEP_CHAIN
	ALTERNATIVE "jmp 662f", "decl %gs:__call_depth__", X86_FEATURE_RSB_UNDERFLOW
	jz	661f	/* optimized for static branch prediction */
	jmp	662f
661:
	pushq	%rbx
	call	__fill_rsb
	STUFF_ONE_RSB
	movl	$CALL_DEPTH_INIT, PER_CPU_VAR(__call_depth__)
	popq	%rbx
662:
#endif
	.endm

	.set call_depth_init, CALL_DEPTH_INIT

/*
 * Reset call chain counter when entering from user space because
 * we cannot guarantee the state of the return buffer.
 *
 * However we will match call/returns at this point (before
 * doing a context switch), so there is no need to fill the RSB
 * at this point.
 *
 * Must be after swapgs
 */
.macro CALL_DEPTH_RESET
#ifdef CONFIG_DEEP_CHAIN
	ALTERNATIVE "", "movl $call_depth_init, %gs:__call_depth__", \
		    X86_FEATURE_RSB_UNDERFLOW
#endif
.endm

#else /* __ASSEMBLY__ */

#define ANNOTATE_NOSPEC_ALTERNATIVE				\
	"999:\n\t"						\
	".pushsection .discard.nospec\n\t"			\
	".long 999b - .\n\t"					\
	".popsection\n\t"

#if defined(CONFIG_X86_64) && defined(RETPOLINE)

/*
 * Since the inline asm uses the %V modifier which is only in newer GCC,
 * the 64-bit one is dependent on RETPOLINE not CONFIG_RETPOLINE.
 */
# define CALL_NOSPEC						\
	ANNOTATE_NOSPEC_ALTERNATIVE				\
	ALTERNATIVE(						\
	"call *%[thunk_target]\n",				\
	"call __x86_indirect_thunk_%V[thunk_target]\n",		\
	X86_FEATURE_RETPOLINE)
# define THUNK_TARGET(addr) [thunk_target] "r" (addr)

#elif defined(CONFIG_X86_32) && defined(CONFIG_RETPOLINE)
/*
 * For i386 we use the original ret-equivalent retpoline, because
 * otherwise we'll run out of registers. We don't care about CET
 * here, anyway.
 */
# define CALL_NOSPEC ALTERNATIVE("call *%[thunk_target]\n",	\
	"       jmp    904f;\n"					\
	"       .align 16\n"					\
	"901:	call   903f;\n"					\
	"902:	pause;\n"					\
	"    	lfence;\n"					\
	"       jmp    902b;\n"					\
	"       .align 16\n"					\
	"903:	addl   $4, %%esp;\n"				\
	"       pushl  %[thunk_target];\n"			\
	"       ret;\n"						\
	"       .align 16\n"					\
	"904:	call   901b;\n",				\
	X86_FEATURE_RETPOLINE)

# define THUNK_TARGET(addr) [thunk_target] "rm" (addr)
#else /* No retpoline for C / inline asm */
# define CALL_NOSPEC "call *%[thunk_target]\n"
# define THUNK_TARGET(addr) [thunk_target] "rm" (addr)
#endif

#ifdef CONFIG_X86_64
#define STUFF_ONE_RSB	\
	"	call 881f\n"					\
	"	pause;lfence\n"					\
	"881:\n"						\
	"	addq $8,%%rsp\n"
#else
#define STUFF_ONE_RSB	\
	"	call 881f\n"					\
	"	pause;lfence\n"					\
	"881:\n"						\
	"	addl $4,%%esp\n"
#endif

/* The Spectre V2 mitigation variants */
enum spectre_v2_mitigation {
	SPECTRE_V2_NONE,
	SPECTRE_V2_RETPOLINE_MINIMAL,
	SPECTRE_V2_RETPOLINE_MINIMAL_AMD,
	SPECTRE_V2_RETPOLINE_GENERIC,
	SPECTRE_V2_RETPOLINE_AMD,
	SPECTRE_V2_IBRS,
};

extern char __indirect_thunk_start[];
extern char __indirect_thunk_end[];

/*
 * On VMEXIT we must ensure that no RSB predictions learned in the guest
 * can be followed in the host, by overwriting the RSB completely. Both
 * retpoline and IBRS mitigations for Spectre v2 need this; only on future
 * CPUs with IBRS_ALL *might* it be avoided.
 */
static inline void vmexit_fill_RSB(void)
{
#ifdef CONFIG_RETPOLINE
	alternative_input("",
			  "call __fill_rsb;" STUFF_ONE_RSB,
			  X86_FEATURE_RETPOLINE,
			  ASM_NO_INPUT_CLOBBER(_ASM_BX, "memory"));
#endif
}

#define alternative_msr_write(_msr, _val, _feature)		\
	asm volatile(ALTERNATIVE("",				\
				 "movl %[msr], %%ecx\n\t"	\
				 "movl %[val], %%eax\n\t"	\
				 "movl $0, %%edx\n\t"		\
				 "wrmsr",			\
				 _feature)			\
		     : : [msr] "i" (_msr), [val] "i" (_val)	\
		     : "eax", "ecx", "edx", "memory")

static inline void indirect_branch_prediction_barrier(void)
{
	alternative_msr_write(MSR_IA32_PRED_CMD, PRED_CMD_IBPB,
			      X86_FEATURE_USE_IBPB);
}

/*
 * With retpoline, we must use IBRS to restrict branch prediction
 * before calling into firmware.
 */
static inline void firmware_restrict_branch_speculation_start(void)
{
	alternative_msr_write(MSR_IA32_SPEC_CTRL, SPEC_CTRL_IBRS,
			      X86_FEATURE_USE_IBRS_FW);
}

static inline void firmware_restrict_branch_speculation_end(void)
{
	alternative_msr_write(MSR_IA32_SPEC_CTRL, 0,
			      X86_FEATURE_USE_IBRS_FW);
}

/*
 * Fill the return buffer to avoid return buffer overflow.
 *
 * This is different from the one above because it is controlled
 * by a different feature bit. It should be used for any fixes
 * for call chains deeper than 16, or the context switch.
 */
static inline void fill_return_buffer(void)
{
#ifdef CONFIG_RETPOLINE
	alternative_input("",
			  "call __fill_rsb; " STUFF_ONE_RSB,
			  X86_FEATURE_RETPOLINE,
			  ASM_NO_INPUT_CLOBBER(_ASM_BX, "memory"));
#endif
}

#endif /* __ASSEMBLY__ */
#endif /* _ASM_X86_NOSPEC_BRANCH_H_ */
