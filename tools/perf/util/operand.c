#include <errno.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include "perf.h"
#include "operand.h"

/* Fall back, can be overriden per architecture */
__weak
int arch_resolve_operand(char *insn __maybe_unused,
			 int insnlen __maybe_unused,
			 bool is64bit __maybe_unused,
			 u64 ip __maybe_unused,
			 u64 val __maybe_unused,
			 struct operand_print_ops *ops __maybe_unused,
			 void *ctx __maybe_unused)
{
	return -EINVAL;
}
