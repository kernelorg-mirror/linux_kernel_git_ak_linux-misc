#include "intel-pt-decoder/insn.h"
#include "intel-pt-decoder/inat.h"
#include "insnlen.h"

int arch_insn_len(char *insnbytes, int insnlen, int is64bit)
{
	struct insn insn;

	insn_init(&insn, insnbytes, insnlen, is64bit);
	insn_get_length(&insn);
	return insn.length;
}
