/* SPDX-License-Identifier: GPL-2.0 */

#include <linux/stringify.h>
#include <linux/linkage.h>
#include <asm/dwarf2.h>
#include <asm/cpufeatures.h>
#include <asm/alternative-asm.h>
#include <asm/export.h>
#include <asm/nospec-branch.h>

.macro THUNK reg
	.section .text.__x86.indirect_thunk.\reg

ENTRY(__x86.indirect_thunk.\reg)
	CFI_STARTPROC
	NOSPEC_JMP %\reg
	CFI_ENDPROC
ENDPROC(__x86.indirect_thunk.\reg)
EXPORT_SYMBOL(__x86.indirect_thunk.\reg)
.endm

#ifdef CONFIG_64BIT
.irp reg rax rbx rcx rdx rsi rdi rbp r8 r9 r10 r11 r12 r13 r14 r15
	THUNK \reg
.endr
#else
.irp reg eax ebx ecx edx esi edi ebp
	THUNK \reg
.endr
#endif
