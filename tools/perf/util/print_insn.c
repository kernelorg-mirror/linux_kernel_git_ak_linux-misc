// SPDX-License-Identifier: GPL-2.0
/*
 * Instruction binary disassembler based on capstone.
 *
 * Author(s): Changbin Du <changbin.du@huawei.com>
 */
#include <inttypes.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include "debug.h"
#include "sample.h"
#include "symbol.h"
#include "machine.h"
#include "thread.h"
#include "print_insn.h"
#include "dump-insn.h"
#include "map.h"
#include "dso.h"
#include "annotate.h"
#include "disasm.h"
#include "debuginfo.h"
#include "annotate-data.h"
#include "map_symbol.h"

size_t sample__fprintf_insn_raw(struct perf_sample *sample, FILE *fp)
{
	int printed = 0;

	for (int i = 0; i < sample->insn_len; i++) {
		printed += fprintf(fp, "%02x", (unsigned char)sample->insn[i]);
		if (sample->insn_len - i > 1)
			printed += fprintf(fp, " ");
	}
	return printed;
}

#ifdef HAVE_LIBCAPSTONE_SUPPORT
#include <capstone/capstone.h>

int capstone_init(struct machine *machine, csh *cs_handle, bool is64, bool disassembler_style);

int capstone_init(struct machine *machine, csh *cs_handle, bool is64, bool disassembler_style)
{
	cs_arch arch;
	cs_mode mode;

	if (machine__is(machine, "x86_64") && is64) {
		arch = CS_ARCH_X86;
		mode = CS_MODE_64;
	} else if (machine__normalized_is(machine, "x86")) {
		arch = CS_ARCH_X86;
		mode = CS_MODE_32;
	} else if (machine__normalized_is(machine, "arm64")) {
		arch = CS_ARCH_ARM64;
		mode = CS_MODE_ARM;
	} else if (machine__normalized_is(machine, "arm")) {
		arch = CS_ARCH_ARM;
		mode = CS_MODE_ARM + CS_MODE_V8;
	} else if (machine__normalized_is(machine, "s390")) {
		arch = CS_ARCH_SYSZ;
		mode = CS_MODE_BIG_ENDIAN;
	} else {
		return -1;
	}

	if (cs_open(arch, mode, cs_handle) != CS_ERR_OK) {
		pr_warning_once("cs_open failed\n");
		return -1;
	}

	if (machine__normalized_is(machine, "x86")) {
		/*
		 * In case of using capstone_init while symbol__disassemble
		 * setting CS_OPT_SYNTAX_ATT depends if disassembler_style opts
		 * is set via annotation args
		 */
		if (disassembler_style)
			cs_option(*cs_handle, CS_OPT_SYNTAX, CS_OPT_SYNTAX_ATT);
		/*
		 * Resolving address operands to symbols is implemented
		 * on x86 by investigating instruction details.
		 */
		cs_option(*cs_handle, CS_OPT_DETAIL, CS_OPT_ON);
	}

	return 0;
}

#define MAX_INSN_LEN 5000

static size_t print_insn_x86(struct thread *thread, u8 cpumode, cs_insn *insn,
			     int print_opts, char *buf)
{
	struct addr_location al;
	size_t printed = 0;

	if (insn->detail && insn->detail->x86.op_count == 1) {
		cs_x86_op *op = &insn->detail->x86.operands[0];

		addr_location__init(&al);
		if (op->type == X86_OP_IMM &&
		    thread__find_symbol(thread, cpumode, op->imm, &al)) {
			printed += snprintf(buf, MAX_INSN_LEN,
					    "%s ", insn[0].mnemonic);
			printed += __symbol__fprintf_symname_offs_buf(al.sym, &al, false, true,
								      buf + strlen(buf),
								      MAX_INSN_LEN - strlen(buf));
			if (print_opts & PRINT_INSN_IMM_HEX)
				printed += snprintf(buf + strlen(buf), MAX_INSN_LEN - strlen(buf),
						    " [%#" PRIx64 "]", op->imm);
			addr_location__exit(&al);
			return printed;
		}
		addr_location__exit(&al);
	}

	printed += snprintf(buf, MAX_INSN_LEN, "%s %s", insn[0].mnemonic, insn[0].op_str);
	return printed;
}

static bool is64bitip(struct machine *machine, struct addr_location *al)
{
	const struct dso *dso = al->map ? map__dso(al->map) : NULL;

	if (dso)
		return dso__is_64_bit(dso);

	return machine__is(machine, "x86_64") ||
		machine__normalized_is(machine, "arm64") ||
		machine__normalized_is(machine, "s390");
}

static int append_snprintf(char *buf, int bufl, const char *fmt, ...)
{
	int len = strlen(buf);
	int ret;
	va_list ap;

	if (len >= bufl)
		return -1;
	va_start(ap, fmt);
	ret = vsnprintf(buf + len, bufl - len, fmt, ap);
	va_end(ap);
	return ret;
}

static void add_data_type(char *buf, struct thread *thread,
			  u8 cpumode, struct arch *arch, uint64_t ip,
			  int insn_len)
{
	struct annotate_args args = {
		.line = buf,
		.arch = arch,
	};
	struct disasm_line *dl;
	int type_offset;
	struct annotated_item_stat istat;
	struct annotated_data_type *mem_type;
	char buf2[4096];
	struct addr_location al;
	char *name = NULL;

	addr_location__init(&al);
	thread__find_map(thread, cpumode, ip, &al);
	if (!al.map)
		goto out;
	al.sym = map__find_symbol(al.map, al.addr);
	if (!al.sym)
		goto out;
	args.ms.map = al.map;
	args.ms.sym = al.sym;
	args.offset = al.addr - al.sym->start;
	if (map__dso(al.map) != di_cache.dso || !di_cache.dbg) {
		dso__put(di_cache.dso);
		di_cache.dso = dso__get(map__dso(al.map));

		debuginfo__delete(di_cache.dbg);
		di_cache.dbg = debuginfo__new(dso__long_name(di_cache.dso));
		if (!di_cache.dbg)
			goto out;
	}
       	dl = disasm_line__new(&args);
	if (!dl)
		goto out;
	mem_type = annotate__get_data_type(&args.ms, arch, di_cache.dbg, dl,
					   &type_offset, thread,
					   cpumode, &istat, insn_len, &name);
	if (mem_type == NULL || mem_type == NO_TYPE)
		goto out_line;
	append_snprintf(buf, MAX_INSN_LEN, " { ");
	/* No need to handle fusing here. */
	if (name)
		append_snprintf(buf, MAX_INSN_LEN, "%s", name);
	append_snprintf(buf, MAX_INSN_LEN, " , %s", mem_type->self.type_name);
	if (annotated_data_type__get_member_name(mem_type, buf2, sizeof(buf2),
						 type_offset)) {
		append_snprintf(buf, MAX_INSN_LEN, " ->%s [%#x]", buf2, type_offset);
	}
	append_snprintf(buf, MAX_INSN_LEN, " }");
out_line:
	zfree(&name);
	disasm_line__free(dl);
out:
	addr_location__exit(&al);
}

ssize_t fprintf_insn_asm(struct machine *machine, struct thread *thread, u8 cpumode,
			 bool is64bit, const uint8_t *code, size_t code_size,
			 uint64_t ip, int *lenp, int print_opts, FILE *fp,
			 bool data_type, struct arch *arch)
{
	size_t printed;
	cs_insn *insn;
	csh cs_handle;
	size_t count;
	int ret;
	char buf[MAX_INSN_LEN];

	/* TODO: Try to initiate capstone only once but need a proper place. */
	ret = capstone_init(machine, &cs_handle, is64bit, true);
	if (ret < 0)
		return ret;

	count = cs_disasm(cs_handle, code, code_size, ip, 1, &insn);
	if (count > 0) {
		if (machine__normalized_is(machine, "x86"))
			print_insn_x86(thread, cpumode, &insn[0], print_opts, buf);
		else
			snprintf(buf, MAX_INSN_LEN, "%s %s", insn[0].mnemonic,
					   insn[0].op_str);
		if (lenp)
			*lenp = insn->size;
		if (data_type)
			add_data_type(buf, thread, cpumode, arch, ip, insn->size);
		printed = fputs(buf, fp);
		cs_free(insn, count);
	} else {
		printed = -1;
	}

	cs_close(&cs_handle);
	return printed;
}

size_t sample__fprintf_insn_asm(struct perf_sample *sample, struct thread *thread,
				struct machine *machine, FILE *fp,
				struct addr_location *al,
				bool data_type,
				struct arch *arch)
{
	bool is64bit = is64bitip(machine, al);
	ssize_t printed;

	printed = fprintf_insn_asm(machine, thread, sample->cpumode, is64bit,
				   (uint8_t *)sample->insn, sample->insn_len,
				   sample->ip, NULL, 0, fp, data_type, arch);
	if (printed < 0)
		return sample__fprintf_insn_raw(sample, fp);

	return printed;
}
#else
size_t sample__fprintf_insn_asm(struct perf_sample *sample __maybe_unused,
				struct thread *thread __maybe_unused,
				struct machine *machine __maybe_unused,
				FILE *fp __maybe_unused,
				struct addr_location *al __maybe_unused,
				bool data_type __maybe_unused,
				struct arch *arch)
{
	return 0;
}
#endif
