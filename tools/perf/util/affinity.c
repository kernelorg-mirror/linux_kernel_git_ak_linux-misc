// SPDX-License-Identifier: GPL-2.0
/* Manage affinity to optimize IPIs inside the kernel perf API. */
#define _GNU_SOURCE 1
#include <sched.h>
#include <stdlib.h>
#include <linux/zalloc.h>
#include "perf.h"
#include "cpumap.h"
#include "affinity.h"

static int get_cpu_set_size(void)
{
	int sz = (cpu__max_cpu() + 64 - 1) / 64;
	/*
	 * sched_getaffinity doesn't like masks smaller than the kernel.
	 * Hopefully that's big enough.
	 */
	if (sz < 4096/8)
		sz = 4096/8;
	return sz;
}

int affinity__setup(struct affinity *a)
{
	int cpu_set_size = get_cpu_set_size();

	a->orig_cpus = malloc(cpu_set_size);
	if (!a->orig_cpus)
		return -1;
	sched_getaffinity(0, cpu_set_size, (cpu_set_t *)a->orig_cpus);
	a->sched_cpus = zalloc(cpu_set_size);
	if (!a->sched_cpus) {
		free(a->orig_cpus);
		return -1;
	}
	a->changed = false;
	return 0;
}

/*
 * perf_event_open does an IPI internally to the target CPU.
 * It is more efficient to change perf's affinity to the target
 * CPU and then set up all events on that CPU, so we amortize
 * CPU communication.
 */
void affinity__set(struct affinity *a, int cpu)
{
	int cpu_set_size = get_cpu_set_size();

	if (cpu == -1)
		return;
	a->changed = true;
	a->sched_cpus[cpu / 8] |= 1 << (cpu % 8);
	/*
	 * We ignore errors because affinity is just an optimization.
	 * This could happen for example with isolated CPUs or cpusets.
	 * In this case the IPIs inside the kernel's perf API still work.
	 */
	sched_setaffinity(0, cpu_set_size, (cpu_set_t *)a->sched_cpus);
	a->sched_cpus[cpu / 8] ^= 1 << (cpu % 8);
}

void affinity__cleanup(struct affinity *a)
{
	int cpu_set_size = get_cpu_set_size();

	if (a->changed)
		sched_setaffinity(0, cpu_set_size, (cpu_set_t *)a->orig_cpus);
	free(a->sched_cpus);
	free(a->orig_cpus);
}
