#include "perf.h"
#include "insnlen.h"

/* Fallback for architectures not supporting this */
__weak int arch_insn_len(char *buf __maybe_unused,
			 int len __maybe_unused,
			 int is64bit __maybe_unused)
{
	return 0;
}
