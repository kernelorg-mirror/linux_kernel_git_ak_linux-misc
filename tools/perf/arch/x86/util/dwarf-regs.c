// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dwarf-regs.c : Mapping of DWARF debug register numbers into register names.
 * Extracted from probe-finder.c
 *
 * Written by Masami Hiramatsu <mhiramat@redhat.com>
 */

#include <stddef.h>
#include <errno.h> /* for EINVAL */
#include <string.h> /* for strcmp */
#include <linux/ptrace.h> /* for struct pt_regs */
#include <linux/kernel.h> /* for offsetof */
#include <dwarf-regs.h>

/*
 * See arch/x86/kernel/ptrace.c.
 * Different from it:
 *
 *  - Since struct pt_regs is defined differently for user and kernel,
 *    but we want to use 'ax, bx' instead of 'rax, rbx' (which is struct
 *    field name of user's pt_regs), we make REG_OFFSET_NAME to accept
 *    both string name and reg field name.
 *
 *  - Since accessing x86_32's pt_regs from x86_64 building is difficult
 *    and vise versa, we simply fill offset with -1, so
 *    get_arch_regstr() still works but regs_query_register_offset()
 *    returns error.
 *    The only inconvenience caused by it now is that we are not allowed
 *    to generate BPF prologue for a x86_64 kernel if perf is built for
 *    x86_32. This is really a rare usecase.
 *
 *  - Order is different from kernel's ptrace.c for get_arch_regstr(). Use
 *    the order defined by dwarf.
 */

struct pt_regs_offset {
	const char *name;
	int offset;
};

#define REG_OFFSET_END {.name = NULL, .offset = 0}

#ifdef __x86_64__
# define REG_OFFSET_NAME_64(n, r) {.name = n, .offset = offsetof(struct pt_regs, r)}
# define REG_OFFSET_NAME_32(n, r) {.name = n, .offset = -1}
#else
# define REG_OFFSET_NAME_64(n, r) {.name = n, .offset = -1}
# define REG_OFFSET_NAME_32(n, r) {.name = n, .offset = offsetof(struct pt_regs, r)}
#endif
#define REG_NO_OFFSET(n) { .name = n, .offset = -1 }

/* TODO: switching by dwarf address size */
#ifndef __x86_64__
static const struct pt_regs_offset x86_32_regoffset_table[] = {
	REG_OFFSET_NAME_32("%ax",	eax),
	REG_OFFSET_NAME_32("%cx",	ecx),
	REG_OFFSET_NAME_32("%dx",	edx),
	REG_OFFSET_NAME_32("%bx",	ebx),
	REG_OFFSET_NAME_32("$stack",	esp),	/* Stack address instead of %sp */
	REG_OFFSET_NAME_32("%bp",	ebp),
	REG_OFFSET_NAME_32("%si",	esi),
	REG_OFFSET_NAME_32("%di",	edi),
	REG_OFFSET_END,
};

#define regoffset_table x86_32_regoffset_table
#else
static const struct pt_regs_offset x86_64_regoffset_table[] = {
	REG_OFFSET_NAME_64("%ax",	rax),
	REG_OFFSET_NAME_64("%dx",	rdx),
	REG_OFFSET_NAME_64("%cx",	rcx),
	REG_OFFSET_NAME_64("%bx",	rbx),
	REG_OFFSET_NAME_64("%si",	rsi),
	REG_OFFSET_NAME_64("%di",	rdi),
	REG_OFFSET_NAME_64("%bp",	rbp),
	REG_OFFSET_NAME_64("%sp",	rsp),
	REG_OFFSET_NAME_64("%r8",	r8),
	REG_OFFSET_NAME_64("%r9",	r9),
	REG_OFFSET_NAME_64("%r10",	r10),
	REG_OFFSET_NAME_64("%r11",	r11),
	REG_OFFSET_NAME_64("%r12",	r12),
	REG_OFFSET_NAME_64("%r13",	r13),
	REG_OFFSET_NAME_64("%r14",	r14),
	REG_OFFSET_NAME_64("%r15",	r15),
	REG_NO_OFFSET("%ra"),
	REG_NO_OFFSET("%xmm0"),
	REG_NO_OFFSET("%xmm1"),
	REG_NO_OFFSET("%xmm2"),
	REG_NO_OFFSET("%xmm3"),
	REG_NO_OFFSET("%xmm4"),
	REG_NO_OFFSET("%xmm5"),
	REG_NO_OFFSET("%xmm6"),
	REG_NO_OFFSET("%xmm7"),
	REG_NO_OFFSET("%xmm8"),
	REG_NO_OFFSET("%xmm9"),
	REG_NO_OFFSET("%xmm10"),
	REG_NO_OFFSET("%xmm11"),
	REG_NO_OFFSET("%xmm12"),
	REG_NO_OFFSET("%xmm13"),
	REG_NO_OFFSET("%xmm14"),
	REG_NO_OFFSET("%xmm15"),
	REG_NO_OFFSET("%st0"),
	REG_NO_OFFSET("%st1"),
	REG_NO_OFFSET("%st2"),
	REG_NO_OFFSET("%st3"),
	REG_NO_OFFSET("%st4"),
	REG_NO_OFFSET("%st5"),
	REG_NO_OFFSET("%st6"),
	REG_NO_OFFSET("%st7"),
	REG_NO_OFFSET("%mm0"),
	REG_NO_OFFSET("%mm1"),
	REG_NO_OFFSET("%mm2"),
	REG_NO_OFFSET("%mm3"),
	REG_NO_OFFSET("%mm4"),
	REG_NO_OFFSET("%mm5"),
	REG_NO_OFFSET("%mm6"),
	REG_NO_OFFSET("%mm7"),
	REG_NO_OFFSET("%rflags"),
	REG_NO_OFFSET("%es"),
	REG_NO_OFFSET("%cs"),
	REG_NO_OFFSET("%ss"),
	REG_NO_OFFSET("%ds"),
	REG_NO_OFFSET("%fs"),
	REG_NO_OFFSET("%gs"),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET("%fs.base"),
	REG_NO_OFFSET("%gs.base"),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET("%tr"),
	REG_NO_OFFSET("%ldtr"),
	REG_NO_OFFSET("%mxcsr"),
	REG_NO_OFFSET("%fcw"),
	REG_NO_OFFSET("%fsw"),
	REG_NO_OFFSET("%xmm16"),
	REG_NO_OFFSET("%xmm17"),
	REG_NO_OFFSET("%xmm18"),
	REG_NO_OFFSET("%xmm19"),
	REG_NO_OFFSET("%xmm20"),
	REG_NO_OFFSET("%xmm21"),
	REG_NO_OFFSET("%xmm22"),
	REG_NO_OFFSET("%xmm23"),
	REG_NO_OFFSET("%xmm24"),
	REG_NO_OFFSET("%xmm25"),
	REG_NO_OFFSET("%xmm26"),
	REG_NO_OFFSET("%xmm27"),
	REG_NO_OFFSET("%xmm28"),
	REG_NO_OFFSET("%xmm29"),
	REG_NO_OFFSET("%xmm30"),
	REG_NO_OFFSET("%xmm31"),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET(NULL),
	REG_NO_OFFSET("%k0"),
	REG_NO_OFFSET("%k1"),
	REG_NO_OFFSET("%k2"),
	REG_NO_OFFSET("%k3"),
	REG_NO_OFFSET("%k4"),
	REG_NO_OFFSET("%k5"),
	REG_NO_OFFSET("%k6"),
	REG_NO_OFFSET("%k7"),
	REG_OFFSET_END,
};

#define regoffset_table x86_64_regoffset_table
#endif

/* Minus 1 for the ending REG_OFFSET_END */
#define ARCH_MAX_REGS ((sizeof(regoffset_table) / sizeof(regoffset_table[0])) - 1)

/* Return architecture dependent register string (for kprobe-tracer) */
const char *get_arch_regstr(unsigned int n)
{
	return (n < ARCH_MAX_REGS) ? regoffset_table[n].name : NULL;
}

/* Reuse code from arch/x86/kernel/ptrace.c */
/**
 * regs_query_register_offset() - query register offset from its name
 * @name:	the name of a register
 *
 * regs_query_register_offset() returns the offset of a register in struct
 * pt_regs from its name. If the name is invalid, this returns -EINVAL;
 */
int regs_query_register_offset(const char *name)
{
	const struct pt_regs_offset *roff;
	for (roff = regoffset_table; roff->name != NULL; roff++)
		if (!strcmp(roff->name, name))
			return roff->offset;
	return -EINVAL;
}
