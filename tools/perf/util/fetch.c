#include "perf.h"
#include "machine.h"
#include "thread.h"
#include "symbol.h"
#include "map.h"
#include "fetch.h"

int fetch_exe(u64 ip, struct thread *thread, struct machine *machine,
	      char *buf, int len, bool *is64bit)
{
	struct addr_location al;
	u8 cpumode;
	long offset;

	if (machine__kernel_ip(machine, ip))
		cpumode = PERF_RECORD_MISC_KERNEL;
	else
		cpumode = PERF_RECORD_MISC_USER;
	if (!thread__find_map(thread, cpumode, ip, &al) || !al.map->dso)
		return -1;
	if (al.map->dso->data.status == DSO_DATA_STATUS_ERROR)
		return -1;
	map__load(al.map);
	offset = al.map->map_ip(al.map, ip);
	if (is64bit)
		*is64bit = al.map->dso->is_64_bit;
	return dso__data_read_offset(al.map->dso, machine, offset, (u8 *)buf, len);
}
