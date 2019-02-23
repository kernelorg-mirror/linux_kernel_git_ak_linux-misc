// SPDX-License-Identifier: GPL-2.0
#include "perf.h"
#include "archinsn.h"
#include "util/intel-pt-decoder/insn.h"
#include "machine.h"
#include "thread.h"
#include "symbol.h"
#include "map.h"

void arch_fetch_insn(struct perf_sample *sample,
		     struct thread *thread,
		     struct machine *machine)
{
	struct addr_location al;
	u8 cpumode;
	long offset;
	struct insn insn;
	int len;

	if (!sample->ip)
		return;

	if (machine__kernel_ip(machine, sample->ip))
		cpumode = PERF_RECORD_MISC_KERNEL;
	else
		cpumode = PERF_RECORD_MISC_USER;
	if (!thread__find_map(thread, cpumode, sample->ip, &al) || !al.map->dso)
		return;
	if (al.map->dso->data.status == DSO_DATA_STATUS_ERROR)
		return;
	map__load(al.map);
	offset = al.map->map_ip(al.map, sample->ip);
	len = dso__data_read_offset(al.map->dso, machine, offset, (u8 *)sample->insn,
				   sizeof(sample->insn));
	if (len <= 0)
		return;
	insn_init(&insn, sample->insn, len, al.map->dso->is_64_bit);
	insn_get_length(&insn);
	if (insn_complete(&insn) && insn.length <= len)
		sample->insn_len = insn.length;
}
