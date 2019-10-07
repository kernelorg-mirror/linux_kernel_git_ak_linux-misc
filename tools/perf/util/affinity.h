// SPDX-License-Identifier: GPL-2.0
#ifndef AFFINITY_H
#define AFFINITY_H 1

struct affinity {
	unsigned long *orig_cpus;
	unsigned long *sched_cpus;
	bool changed;
};

void affinity__cleanup(struct affinity *a);
void affinity__set(struct affinity *a, int cpu);
int affinity__setup(struct affinity *a);

#endif
