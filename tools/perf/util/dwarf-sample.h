/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DWARF_SAMPLE_H
#define DWARF_SAMPLE_H 1

#include "probe-finder.h"

struct perf_sample;

int dwarf_resolve_sample(struct perf_sample *sample,
			 struct thread *thread,
			 struct variable_list **vls);
void dwarf_free_varlist(struct variable_list *vls, int dret);
int dwarf_varlist_find_reg(struct variable_list *vls, int dret, int r,
			   char **name, char **type);

#endif
