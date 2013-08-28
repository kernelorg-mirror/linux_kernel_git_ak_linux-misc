/*
 * itrace.h: Instruction Tracing support
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

#ifndef __PERF_ITRACE_H
#define __PERF_ITRACE_H

#include <sys/types.h>
#include <stdbool.h>

#include <linux/perf_event.h>

#include "../perf.h"
#include "types.h"

struct perf_evlist;

/**
 * struct itrace_mmap - records an mmap of the itrace buffer file descriptor.
 * @base: address of mapped area
 * @mask: %0 if @len is not a power of two, otherwise (@len - %1)
 * @len: size of mapped area excluding perf_event_mmap_page
 * @mmap_len: size of mapped area including perf_event_mmap_page
 * @prev: previous data_head
 * @idx: index of this mmap
 * @tid: tid for a per-thread mmap (also set if there is only 1 tid on a per-cpu
 *       mmap) otherwise %0
 * @cpu: cpu number for a per-cpu mmap otherwise %-1
 * @fd: itrace buffer file descriptor
 */
struct itrace_mmap {
	void		*base;
	size_t		mask;
	size_t		len;
	size_t		mmap_len;
	u64		prev;
	int		idx;
	pid_t		tid;
	int		cpu;
	int		fd;
};

/**
 * struct itrace_mmap_params - parameters to set up struct itrace_mmap.
 * @mask: %0 if @len is not a power of two, otherwise (@len - %1)
 * @len: size of mapped area excluding perf_event_mmap_page
 * @mmap_len: size of mapped area including perf_event_mmap_page
 * @prot: mmap memory protection
 * @idx: index of this mmap
 * @tid: tid for a per-thread mmap (also set if there is only 1 tid on a per-cpu
 *       mmap) otherwise %0
 * @cpu: cpu number for a per-cpu mmap otherwise %-1
 */
struct itrace_mmap_params {
	size_t		mask;
	size_t		len;
	size_t		mmap_len;
	int		prot;
	int		idx;
	pid_t		tid;
	int		cpu;
};

static inline u64 itrace_mmap__read_head(struct itrace_mmap *mm)
{
	struct perf_event_mmap_page *pc = mm->base;
#if BITS_PER_LONG == 64 || !defined(HAVE_SYNC_COMPARE_AND_SWAP_SUPPORT)
	u64 head = ACCESS_ONCE(pc->data_head);
#else
	u64 head = __sync_val_compare_and_swap(&pc->data_head, 0, 0);
#endif

	/* Ensure all reads are done after we read the head */
	rmb();
	return head;
}

static inline void itrace_mmap__write_tail(struct itrace_mmap *mm, u64 tail)
{
	struct perf_event_mmap_page *pc = mm->base;
#if BITS_PER_LONG != 64 && defined(HAVE_SYNC_COMPARE_AND_SWAP_SUPPORT)
	u64 old_tail;
#endif

	/* Ensure all reads are done before we write the tail out */
	mb();
#if BITS_PER_LONG == 64 || !defined(HAVE_SYNC_COMPARE_AND_SWAP_SUPPORT)
	pc->data_tail = tail;
#else
	do {
		old_tail = __sync_val_compare_and_swap(&pc->data_tail, 0, 0);
	} while (!__sync_bool_compare_and_swap(&pc->data_tail, old_tail, tail));
#endif
}

int itrace_mmap__mmap(struct itrace_mmap *mm,
		      struct itrace_mmap_params *mp, int fd);
void itrace_mmap__munmap(struct itrace_mmap *mm);
void itrace_mmap_params__init(struct itrace_mmap_params *mp,
			      unsigned int itrace_pages, bool itrace_overwrite);
void itrace_mmap_params__set_idx(struct itrace_mmap_params *mp,
				 struct perf_evlist *evlist, int idx,
				 bool per_cpu);

#endif
