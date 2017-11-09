/*
 * SPDX-License-Identifier: GPL-2.0
 * Copyright (c) 2017, Intel Corporation.
 * Author: Andi Kleen
 */

/* Resolve variable names from samples using DWARF. */
#include <errno.h>
#include <stdlib.h>
#include "perf.h"
#include "thread.h"
#include <linux/zalloc.h>
#include "event.h"
#include "strbuf.h"
#include "strlist.h"
#include "util.h"
#include "map.h"
#include "dso.h"
#include "symbol.h"
#include "debug.h"
#include "probe-finder.h"
#include "dwarf-sample.h"

/* Resolve dwarf variables at a sample IP */
int dwarf_resolve_sample(struct perf_sample *sample,
			 struct thread *thread,
			 struct variable_list **vls)
{
	struct addr_location al;
	int dret = -1;
	struct perf_probe_event pev;
	struct debuginfo *dinfo = NULL;

	memset(&pev, 0, sizeof(struct perf_probe_event));
	memset(&al, 0, sizeof(al));
	thread__find_map(thread, sample->cpumode, sample->ip, &al);
	if (al.map && al.map->dso->long_name) {
		pev.target = build_id_cache__complement(al.map->dso->long_name);
		if (pev.target) {
			char *t = pev.target;
			pev.target = build_id_cache__origname(pev.target);
			free(t);
		} else {
			pev.target = strdup(al.map->dso->long_name);
		}
		if (!pev.target)
			return -EIO;
		al.sym = map__find_symbol(al.map, al.addr);
		if (al.sym) {
			pev.point.function = al.sym->name;
			pev.point.offset = al.addr - al.sym->start;
		}
		dinfo = debuginfo_cache__open(pev.target, true);
		if (dinfo)
			dret = debuginfo__find_available_vars_at(dinfo, &pev, vls,
								verbose == 0);
	}

	free(pev.target);

	return dret;
}

/* Free resolved variable list */
void dwarf_free_varlist(struct variable_list *vls, int dret)
{
	int i;

	for (i = 0; i < dret; i++) {
		zfree(&vls[i].point.symbol);
		strlist__delete(vls[i].vars);
	}
	free(vls);
}

/* Find given register in resolved variables. */
int dwarf_varlist_find_reg(struct variable_list *vls, int dret, int r, char **name,
			   char **type)
{
	int i;
	const char *regn = perf_reg_name(r);

	*name = NULL;
	for (i = 0; i < dret; i++) {
		if (vls[i].vars) {
			struct str_node *node;

			strlist__for_each_entry(node, vls[i].vars) {
				struct variable_node *vn =
					container_of(node, struct variable_node, snode);
				char *rr, *value;

				for (rr = vn->value; *rr; rr++)
					*rr = toupper(*rr);
				value = vn->value;
				if (*value == '%')
					value++;
				if (strncmp(regn, value, strlen(regn)))
					continue;
				*name = vn->name;
				if (type) {
					*type = (char *)node->s;
					if ((*type)[0] == ' ' && (*type)[1] == '(')
						(*type) += 2;
				}
				return 0;
			}
		}
	}
	return -EINVAL;
}
