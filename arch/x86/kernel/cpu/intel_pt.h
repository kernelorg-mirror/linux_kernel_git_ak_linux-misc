/*
 * Intel(R) Processor Trace PMU driver for perf
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
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#ifndef __INTEL_PT_H__
#define __INTEL_PT_H__

#include <linux/radix-tree.h>
#include <linux/itrace.h>

/*
 * Single-entry ToPA: when this close to region boundary, switch
 * buffers to avoid losing data.
 */
#define TOPA_PMI_MARGIN 512

/*
 * Table of Physical Addresses bits
 */
enum topa_sz {
	TOPA_4K	= 0,
	TOPA_8K,
	TOPA_16K,
	TOPA_32K,
	TOPA_64K,
	TOPA_128K,
	TOPA_256K,
	TOPA_512K,
	TOPA_1MB,
	TOPA_2MB,
	TOPA_4MB,
	TOPA_8MB,
	TOPA_16MB,
	TOPA_32MB,
	TOPA_64MB,
	TOPA_128MB,
	TOPA_SZ_END,
};

static inline unsigned int sizes(enum topa_sz tsz)
{
	return 1 << (tsz + 12);
};

struct topa_entry {
	u64	end	: 1;
	u64	rsvd0	: 1;
	u64	intr	: 1;
	u64	rsvd1	: 1;
	u64	stop	: 1;
	u64	rsvd2	: 1;
	u64	size	: 4;
	u64	rsvd3	: 2;
	u64	base	: 36;
	u64	rsvd4	: 16;
};

#define TOPA_SHIFT 12
#define PT_CPUID_LEAVES 2

enum pt_capabilities {
	PT_CAP_max_subleaf = 0,
	PT_CAP_cr3_filtering,
	PT_CAP_topa_output,
	PT_CAP_topa_multiple_entries,
	PT_CAP_payloads_lip,
};

struct pt_pmu {
	struct itrace_pmu	itrace;
	u32			caps[4 * PT_CPUID_LEAVES];
};

/**
 * struct pt_buffer - buffer configuration; one buffer per task_struct or
 * cpu, depending on perf event configuration
 * @tables: list of ToPA tables in this buffer
 * @first, @last: shorthands for first and last topa tables
 * @cur: current topa table
 * @size: total size of all output regions within this buffer
 * @cur_idx: current output region's index within @cur table
 * @output_off: offset within the current output region
 */
struct pt_buffer {
	/* hint for allocation */
	int			cpu;
	/* list of ToPA tables */
	struct list_head	tables;
	/* top-level table */
	struct topa		*first, *last, *cur;
	unsigned long		round;
	unsigned int		cur_idx;
	size_t			output_off;
	unsigned long		size;
	local64_t		head;
	unsigned long		watermark;
	bool			snapshot;
	struct perf_event_mmap_page *user_page;
	void			**data_pages;
};

/**
 * struct pt - per-cpu pt
 */
struct pt {
	raw_spinlock_t		lock;
	struct perf_event	*event;
};

void intel_pt_interrupt(void);

#endif /* __INTEL_PT_H__ */
