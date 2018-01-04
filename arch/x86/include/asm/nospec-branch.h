/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __NOSPEC_BRANCH_H__
#define __NOSPEC_BRANCH_H__

#include <asm/alternative.h>
#include <asm/alternative-asm.h>
#include <asm/cpufeatures.h>

#ifdef __ASSEMBLY__

.macro NOSPEC_JMP reg:req
#ifdef CONFIG_RETPOLINE
	ALTERNATIVE __stringify(jmp __x86.indirect_thunk.\reg), __stringify(jmp *%\reg), X86_BUG_NO_RETPOLINE
#else
	jmp *%\reg
#endif
.endm

.macro NOSPEC_CALL reg:req
#ifdef CONFIG_RETPOLINE
	ALTERNATIVE __stringify(call __x86.indirect_thunk.\reg), __stringify(call *%\reg), X86_BUG_NO_RETPOLINE
#else
	call *%\reg
#endif
.endm

#else /* __ASSEMBLY__ */

#ifdef CONFIG_RETPOLINE
# ifdef CONFIG_64BIT
#  define NOSPEC_CALL ALTERNATIVE(					\
	"call __x86.indirect_thunk.%V[thunk_target]\n",			\
	"call *%[thunk_target]\n", X86_BUG_NO_RETPOLINE)
#  define THUNK_TARGET(addr) [thunk_target] "r" (addr)
# else /* i386 is going to run out of registers if we do that */
#  define NOSPEC_CALL ALTERNATIVE(				\
	"       jmp 1221f; "					\
	"1222:  push %[thunk_target];"				\
	"       jmp __x86.indirect_thunk;"			\
	"1221:  call 1222b;\n",					\
	"call *%[thunk_target]\n", X86_BUG_NO_RETPOLINE)
#  define THUNK_TARGET(addr) [thunk_target] "rm" (addr)
# endif /* !CONFIG_64BIT */
#else
# define NOSPEC_CALL "call *%[thunk_target]\n"
# define THUNK_TARGET(addr) [thunk_target] "rm" (addr)
#endif

#endif /* __ASSEMBLY__ */
#endif /* __NOSPEC_BRANCH_H__ */
