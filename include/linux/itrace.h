/*
 * Instruction flow trace unit infrastructure
 * Copyright (c) 2013, Intel Corporation.
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

#ifndef _LINUX_ITRACE_H
#define _LINUX_ITRACE_H

#include <linux/perf_event.h>
#include <linux/file.h>

extern struct ring_buffer_ops itrace_rb_ops;

static inline bool is_itrace_vma(struct vm_area_struct *vma)
{
	if (vma->vm_file) {
		struct perf_event *event = vma->vm_file->private_data;
		if (event->hw.itrace_file == vma->vm_file)
			return true;
	}

	return false;
}

void *itrace_priv(struct perf_event *event);

void *itrace_event_get_priv(struct perf_event *event);
void itrace_event_put(struct perf_event *event);

struct itrace_pmu {
	struct pmu		pmu;
	struct list_head	entry;
	/*
	 * Allocate/free ring_buffer backing store
	 */
	void			*(*alloc_buffer)(int cpu, int nr_pages, bool overwrite,
						 void **pages,
						 struct perf_event_mmap_page **user_page);
	void			(*free_buffer)(void *buffer);

	int			(*event_init)(struct perf_event *event);

	/*
	 * Calculate the size of a sample to be written out
	 */
	unsigned long		(*sample_trace)(struct perf_event *event,
						struct perf_sample_data *data);

	/*
	 * Write out a trace sample to the given output handle
	 */
	void			(*sample_output)(struct perf_event *event,
						 struct perf_output_handle *handle,
						 struct perf_sample_data *data);
	char			*name;
};

#define to_itrace_pmu(x) container_of((x), struct itrace_pmu, pmu)

#ifdef CONFIG_PERF_EVENTS

extern int itrace_kernel_event(struct perf_event *event,
			       struct task_struct *task);
extern int itrace_inherit_event(struct perf_event *event,
				struct task_struct *task);
extern void itrace_lost_data(struct perf_event *event, u64 offset);
extern int itrace_pmu_register(struct itrace_pmu *ipmu);

extern int itrace_event_installable(struct perf_event *event,
				    struct perf_event_context *ctx);

extern void itrace_wake_up(struct perf_event *event);

extern bool is_itrace_event(struct perf_event *event);

extern int itrace_sampler_init(struct perf_event *event,
			       struct task_struct *task);
extern void itrace_sampler_fini(struct perf_event *event);
extern unsigned long itrace_sampler_trace(struct perf_event *event,
					  struct perf_sample_data *data);
extern void itrace_sampler_output(struct perf_event *event,
				  struct perf_output_handle *handle,
				  struct perf_sample_data *data);
#else
static int itrace_kernel_event(struct perf_event *event,
			       struct task_struct *task)	{ return 0; }
static int itrace_inherit_event(struct perf_event *event,
				struct task_struct *task)	{ return 0; }
static inline void
itrace_lost_data(struct perf_event *event, u64 offset)		{}
static inline int itrace_pmu_register(struct itrace_pmu *ipmu)	{ return -EINVAL; }

static inline int
itrace_event_installable(struct perf_event *event,
			 struct perf_event_context *ctx)	{ return -EINVAL; }
static inline void itrace_wake_up(struct perf_event *event)	{}
static inline bool is_itrace_event(struct perf_event *event)	{ return false; }

static inline int itrace_sampler_init(struct perf_event *event,
				      struct task_struct *task)	{}
static inline void
itrace_sampler_fini(struct perf_event *event)			{}
static inline unsigned long
itrace_sampler_trace(struct perf_event *event,
		     struct perf_sample_data *data)		{ return 0; }
static inline void
itrace_sampler_output(struct perf_event *event,
		      struct perf_output_handle *handle,
		      struct perf_sample_data *data)		{}
#endif

#endif /* _LINUX_PERF_EVENT_H */
