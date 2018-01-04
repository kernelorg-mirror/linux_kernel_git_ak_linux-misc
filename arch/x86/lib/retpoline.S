/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/stringify.h>
#include <linux/linkage.h>
#include <asm/dwarf2.h>
#include <asm/cpufeatures.h>
#include <asm/alternative-asm.h>

.macro THUNK sp reg
	.section .text.__x86.indirect_thunk.\reg

ENTRY(__x86.indirect_thunk.\reg)
	CFI_STARTPROC
	ALTERNATIVE "call 2f", __stringify(jmp *%\reg), X86_BUG_NO_RETPOLINE
1:
	lfence
	jmp	1b
2:
	mov	%\reg, (%\sp)
	ret
	CFI_ENDPROC
ENDPROC(__x86.indirect_thunk.\reg)
.endm

#ifdef CONFIG_64BIT
.irp reg rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15
	THUNK rsp \reg
.endr
#else
.irp reg eax ebx ecx edx esi edi ebp
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
	jmp	1b
2:
	lea	4(%esp), %esp
	ret
	CFI_ENDPROC
ENDPROC(__x86.indirect_thunk)

#endif
