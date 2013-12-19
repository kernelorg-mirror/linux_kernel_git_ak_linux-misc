/*
 * itrace.c: instruction tracing support
 * Copyright (c) 2013-2014, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <string.h>

#include "../../util/header.h"
#include "../../util/pmu.h"
#include "../../util/itrace.h"
#include "../../util/intel-pt.h"
#include "../../util/intel-bts.h"

static int find_in_args(int argc, const char **argv, const char *str)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp("--", argv[i]))
			break;
		if (strstr(argv[i], str))
			return 1;
	}
	return 0;
}

static int use_bts(char *itrace_type, int argc, const char **argv)
{
	if (!perf_pmu__find(INTEL_BTS_PMU_NAME))
		return 0;

	if (!perf_pmu__find(INTEL_PT_PMU_NAME))
		return 1;

	if (find_in_args(argc, argv, INTEL_PT_PMU_NAME))
		return 0;

	if (find_in_args(argc, argv, INTEL_BTS_PMU_NAME))
		return 1;

	if (itrace_type && !strcmp(itrace_type, INTEL_BTS_PMU_NAME))
		return 1;

	return 0;
}

struct itrace_record *itrace_record__init(char *itrace_type, int argc,
					  const char **argv, int *err)
{
	char buffer[64];
	int ret;

	*err = 0;

	ret = get_cpuid(buffer, sizeof(buffer));
	if (ret) {
		*err = ret;
		return NULL;
	}

	if (!strncmp(buffer, "GenuineIntel,", 13)) {
		if (use_bts(itrace_type, argc, argv))
			return intel_bts_recording_init(err);
		else
			return intel_pt_recording_init(err);
	}

	return NULL;
}
