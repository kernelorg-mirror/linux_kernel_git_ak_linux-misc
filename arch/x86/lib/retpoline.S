/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Out of line jump trampoline for calls that disable speculation.
 *
 * This is a special sequence that prevents the CPU speculating
 * for indirect calls.
 *
 * This can be called by gcc generated code, or with the asm macros
 * in asm/jump-asm.h
 */

#include <linux/linkage.h>
#include <linux/compiler.h>
#include <asm/dwarf2.h>
#include <asm/export.h>

	.section	.text.__x86.indirect_thunk,"ax"

ENTRY(__x86.indirect_thunk)
	CFI_STARTPROC
	call	retpoline_call_target
2:
	lfence		/* stop speculation */
	ASM_UNREACHABLE
	jmp	2b
	ASM_UNREACHABLE
retpoline_call_target:
#ifdef CONFIG_64BIT
	lea	8(%rsp), %rsp
#else
	lea	4(%esp), %esp
#endif
	ret
	CFI_ENDPROC
ENDPROC(__x86.indirect_thunk)

	EXPORT_SYMBOL(__x86.indirect_thunk)
