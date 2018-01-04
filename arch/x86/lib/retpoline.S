/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/stringify.h>
#include <linux/linkage.h>
#include <linux/compiler.h>
#include <asm/dwarf2.h>
#include <asm/cpufeatures.h>
#include <asm/alternative-asm.h>
#include <asm/export.h>

#ifdef CONFIG_64BIT
#define IS64(a, b) a
#else
#define IS64(a, b) b
#endif

.macro THUNK sp reg
	.section .text.__x86.indirect_thunk.\reg

/* Compiler uses ax while inline assembler uses rax. So provide both */

ENTRY(__x86.indirect_thunk.\reg)
ENTRY(IS64(__x86.indirect_thunk.r\reg, __x86.indirect_thunk.e\reg))
	CFI_STARTPROC
	ALTERNATIVE "call 2f", IS64(__stringify(jmp *%r\reg), __stringify(jmp *%e\reg)), X86_BUG_NO_RETPOLINE
1:
	lfence
	ASM_UNREACHABLE
	jmp	1b
	ASM_UNREACHABLE
2:
	mov	IS64(%r\reg, %e\reg), (%\sp)
	ret
	CFI_ENDPROC
ENDPROC(__x86.indirect_thunk.\reg)
EXPORT_SYMBOL(__x86.indirect_thunk.\reg)
EXPORT_SYMBOL(IS64(__x86.indirect_thunk.r\reg, __x86.indirect_thunk.e\reg))
.endm

#ifdef CONFIG_64BIT
.irp reg ax bx cx dx si di bp 8 9 10 11 12 13 14 15
	THUNK rsp \reg
.endr
#else
.irp reg ax bx cx dx si di bp
	THUNK esp \reg
.endr

/*
 * Also provide the original ret-equivalent retpoline for i386 because it's
 * so register-starved, and we don't care about CET compatibility here.
 */
ENTRY(__x86.indirect_thunk)
	CFI_STARTPROC
	ALTERNATIVE "call 2f", "ret", X86_BUG_NO_RETPOLINE
1:
	lfence
	ASM_UNREACHABLE
	jmp	1b
	ASM_UNREACHABLE
2:
	lea	4(%esp), %esp
	ret
	CFI_ENDPROC
ENDPROC(__x86.indirect_thunk)
EXPORT_SYMBOL(__x86.indirect_thunk)

#endif
