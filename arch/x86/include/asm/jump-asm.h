/* SPDX-License-Identifier: GPL-2.0 */
#ifndef JUMP_ASM_H
#define JUMP_ASM_H 1

#ifdef __ASSEMBLY__

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

#else /* __ASSEMBLY__ */

#define NOSPEC_JUMP(t) \
	"push " t "; "				\
	"jmp __x86.indirect_thunk; "

#define NOSPEC_CALL(t) \
	"	jmp 1221f; "			\
	"1222:	push " t ";"			\
	"	jmp __x86.indirect_thunk;"	\
	"1221:	call 1222b;"

#endif /* !__ASSEMBLY */

#endif
