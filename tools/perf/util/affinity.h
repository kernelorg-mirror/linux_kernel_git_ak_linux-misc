// SPDX-License-Identifier: GPL-2.0
#ifndef AFFINITY_H
#define AFFINITY_H 1

struct affinity {
	unsigned char *orig_cpus;
	unsigned char *sched_cpus;
	bool changed;
};

void affinity__cleanup(struct affinity *a);
void affinity__set(struct affinity *a, int cpu);
int affinity__setup(struct affinity *a);

#endif
