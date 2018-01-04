/* SPDX-License-Identifier: GPL-2.0 */
#ifndef JUMP_ASM_H
#define JUMP_ASM_H 1

#ifdef __ASSEMBLY__

#ifdef CONFIG_RETPOLINE

/*
 * Jump to an indirect pointer without speculation.
 *
 * The out of line __x86.indirect_thunk has special code sequences
 * to stop speculation.
 */

.macro NOSPEC_JMP target
	push	\target
	jmp	__x86.indirect_thunk
.endm

.macro NOSPEC_JMP_INLINE target
	push	\target
	call	2221f
2222:
	lfence
	jmp	2222b
2221:
#ifdef CONFIG_64BIT
	addq	$8, %rsp
#else
	addl	$4, %esp
#endif
	ret
.endm

/*
 * Call an indirect pointer without speculation.
 */

.macro NOSPEC_CALL target
	jmp     1221f
1222:
	push	\target
	jmp	__x86.indirect_thunk
1221:
	call	1222b
.endm

#else /* CONFIG_RETPOLINE */

.macro NOSPEC_JMP target
	jmp *\target
.endm

.macro NOSPEC_JMP_INLINE target
	jmp *\target
.endm

.macro NOSPEC_CALL target
	call *\target
.endm

#endif /* !CONFIG_RETPOLINE */

#else /* __ASSEMBLY__ */

#ifdef CONFIG_RETPOLINE

#define NOSPEC_JMP(t) \
	"push " t "; "				\
	"jmp __x86.indirect_thunk; "

#define NOSPEC_CALL(t) \
	"	jmp 1221f; "			\
	"1222:	push " t ";"			\
	"	jmp __x86.indirect_thunk;"	\
	"1221:	call 1222b;"

#else /* CONFIG_RETPOLINE */

#define NOSPEC_JMP(t) "jmp *" t "; "
#define NOSPEC_CALL(t) "call *" t "; "

#endif /* !CONFIG_RETPOLINE */

#endif /* !__ASSEMBLY */

#endif
