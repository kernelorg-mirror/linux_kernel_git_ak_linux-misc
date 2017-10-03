/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2017, Intel Corporation.
 * Author: Andi Kleen
 */

/* Decode instructions to resolve operands. */
#include <stdio.h>
#include "debug.h"
#include "perf.h"
#include "perf_regs.h"
#include "operand.h"
#include "asm/insn.h"
#include "asm/inat.h"

static unsigned char x86_reg_to_perf[16] = {
	[0] = PERF_REG_X86_AX,
	[1] = PERF_REG_X86_CX,
	[2] = PERF_REG_X86_DX,
	[3] = PERF_REG_X86_BX,
	[4] = PERF_REG_X86_SP,
	[5] = PERF_REG_X86_BP,
	[6] = PERF_REG_X86_SI,
	[7] = PERF_REG_X86_DI,
#ifdef HAVE_ARCH_X86_64_SUPPORT
	[8] = PERF_REG_X86_R8,
	[9] = PERF_REG_X86_R9,
	[10] = PERF_REG_X86_R10,
	[11] = PERF_REG_X86_R11,
	[12] = PERF_REG_X86_R12,
	[13] = PERF_REG_X86_R13,
	[14] = PERF_REG_X86_R14,
	[15] = PERF_REG_X86_R15,
#endif
};

/* Decode x86 instruction and print address mode. */
int arch_resolve_operand(char *insnbytes, int insnlen, bool is64bit,
			 u64 ip,
			 u64 val,
			 struct operand_print_ops *ops,
			 void *ctx)
{
	struct insn insn;
	bool has_value;
	int reg;

	insn_init(&insn, insnbytes, insnlen, is64bit);
	insn_get_length(&insn);
	if (!insn_complete(&insn))
		goto unknown;
	/* Cannot handle Y/Zmm */
	if (insn.vex_prefix.nbytes > 0)
		goto unknown;
	if (!insn.modrm.nbytes)
		goto unknown;

	switch (insn.opcode.bytes[0]) {
	case 0x0f:
		/* For PTWRITE use the caller value */
		if (insn.opcode.bytes[1] == 0xae)
			has_value = true;
		break;
	case 0xb0:
	case 0xb8:
	case 0xc6:
	case 0xc7:
		/* For MOV $xxx use the immediate */
		if (insn.immediate.nbytes) {
			has_value = true;
			val = insn.immediate.value;
		}
		break;
	default:
		break;
	}

	/* Could also get known register values from caller */

	/* Should check for SSE instructions to detect XMM* */

	if (insn_rip_relative(&insn)) {
		ops->print_symbol(ctx, ip + insn.length + insn.displacement.value,
				  has_value, val);
		return 0;
	}

	/* Should handle direct memory offset */

	reg = X86_MODRM_RM(insn.modrm.value);
	if (insn.rex_prefix.nbytes && X86_REX_B(insn.rex_prefix.value))
		reg += 8;
	reg = x86_reg_to_perf[reg];

	switch (X86_MODRM_MOD(insn.modrm.value)) {
	case 0: /* [r/m] */
	case 1: /* [r/m + disp8] */
	case 2: /* [r/m + disp32] */
		if (insn.sib.nbytes) {
			/*
			 * Scaling and multiple registers
			 * not supported for now.
			 */
			pr_debug("SIB encoding not supported\n");
			goto unknown;
		}
		ops->print_indirect_reg(ctx, reg, insn.displacement.value,
					has_value, val);
		break;

	case 3: /* register value */
		ops->print_reg(ctx, reg, has_value, val);
		break;

	default:
		goto unknown;
	}
	return 0;

unknown:
	ops->print_unknown(ctx);
	return 0;

}
