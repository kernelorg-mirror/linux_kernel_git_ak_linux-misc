/* SPDX-License-Identifier: GPL-2.0 */
#ifndef OPERAND_H
#define OPERAND_H 1

#include <linux/types.h>

struct operand_print_ops {
	void (*print_reg)(void *ctx, int reg, bool has_val, u64 val);
	void (*print_symbol)(void *ctx, u64 addr, bool has_val, u64 val);
	void (*print_indirect_reg)(void *ctx, int reg, s32 off, bool has_val, u64 val);
	void (*print_unknown)(void *ctx);
};

int arch_resolve_operand(char *insn, int insnlen, bool is64bit, u64 ip,
			 u64 val,
			 struct operand_print_ops *ops,
			 void *ctx);

#endif
