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

#undef DEBUG

#include <linux/kernel.h>
#include <linux/perf_event.h>
#include <linux/itrace.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "internal.h"

static LIST_HEAD(itrace_pmus);
static DEFINE_MUTEX(itrace_pmus_mutex);

struct static_key_deferred itrace_core_events __read_mostly;

struct itrace_lost_record {
	struct perf_event_header	header;
	u64				offset;
};

/*
 * In the worst case, perf buffer might be full and we're not able to output
 * this record, so the decoder won't know that the data was lost. However,
 * it will still see inconsistency in the trace IP.
 */
void itrace_lost_data(struct perf_event *event, u64 offset)
{
	struct perf_output_handle handle;
	struct perf_sample_data sample;
	struct itrace_lost_record rec = {
		.header = {
			.type = PERF_RECORD_ITRACE_LOST,
			.misc = 0,
			.size = sizeof(rec),
		},
		.offset = offset
	};
	int ret;

	perf_event_header__init_id(&rec.header, &sample, event);
	ret = perf_output_begin(&handle, event, rec.header.size);

	if (ret)
		return;

	perf_output_put(&handle, rec);
	perf_event__output_id_sample(event, &handle, &sample);
	perf_output_end(&handle);
}

static struct itrace_pmu *itrace_pmu_find(int type)
{
	struct itrace_pmu *ipmu;

	rcu_read_lock();
	list_for_each_entry_rcu(ipmu, &itrace_pmus, entry) {
		if (ipmu->pmu.type == type)
			goto out;
	}

	ipmu = NULL;
out:
	rcu_read_unlock();

	return ipmu;
}

bool is_itrace_event(struct perf_event *event)
{
	return !!itrace_pmu_find(event->attr.type);
}

int itrace_event_installable(struct perf_event *event,
			     struct perf_event_context *ctx)
{
	struct perf_event *iter_event;

	if (!is_itrace_event(event))
		return 0;

	/*
	 * the context is locked and pinned and won't change under us,
	 * also we don't care if it's a cpu or task context at this point
	 */
	list_for_each_entry(iter_event, &ctx->event_list, event_entry) {
		if (is_itrace_event(iter_event) &&
		    (iter_event->cpu == event->cpu ||
		     iter_event->cpu == -1 ||
		     event->cpu == -1))
			return -EEXIST;
	}

	return 0;
}

static int itrace_event_init(struct perf_event *event)
{
	struct itrace_pmu *ipmu = to_itrace_pmu(event->pmu);

	return ipmu->event_init(event);
}

static unsigned long itrace_rb_get_size(int nr_pages)
{
	return sizeof(struct ring_buffer) + sizeof(void *) * nr_pages;
}

static int itrace_alloc_data_pages(struct ring_buffer *rb, int cpu,
				   int nr_pages, int flags)
{
	struct itrace_pmu *ipmu = to_itrace_pmu(rb->event->pmu);
	bool overwrite = !(flags & RING_BUFFER_WRITABLE);

	rb->priv = ipmu->alloc_buffer(cpu, nr_pages, overwrite,
				      rb->data_pages, &rb->user_page);
	if (!rb->priv)
		return -ENOMEM;
	rb->nr_pages = nr_pages;

	return 0;
}

static void itrace_free(struct ring_buffer *rb)
{
	struct itrace_pmu *ipmu = to_itrace_pmu(rb->event->pmu);

	if (rb->priv)
		ipmu->free_buffer(rb->priv);
}

struct page *
itrace_mmap_to_page(struct ring_buffer *rb, unsigned long pgoff)
{
	if (pgoff > rb->nr_pages)
		return NULL;

	if (pgoff == 0)
		return virt_to_page(rb->user_page);

	return virt_to_page(rb->data_pages[pgoff - 1]);
}

struct ring_buffer_ops itrace_rb_ops = {
	.get_size		= itrace_rb_get_size,
	.alloc_data_page	= itrace_alloc_data_pages,
	.free_buffer		= itrace_free,
	.mmap_to_page		= itrace_mmap_to_page,
};

void *itrace_priv(struct perf_event *event)
{
	if (!event->rb[PERF_RB_ITRACE])
		return NULL;

	return event->rb[PERF_RB_ITRACE]->priv;
}

void *itrace_event_get_priv(struct perf_event *event)
{
	struct ring_buffer *rb = ring_buffer_get(event, PERF_RB_ITRACE);

	return rb ? rb->priv : NULL;
}

void itrace_event_put(struct perf_event *event)
{
	struct ring_buffer *rb;

	rcu_read_lock();
	rb = rcu_dereference(event->rb[PERF_RB_ITRACE]);
	if (rb)
		ring_buffer_put(rb);
	rcu_read_unlock();
}

static void itrace_set_output(struct perf_event *event,
			      struct perf_event *output_event)
{
	struct ring_buffer *rb;

	mutex_lock(&event->mmap_mutex);

	if (atomic_read(&event->mmap_count[PERF_RB_ITRACE]) ||
	    event->rb[PERF_RB_ITRACE])
		goto out;

	rb = ring_buffer_get(output_event, PERF_RB_ITRACE);
	if (!rb)
		goto out;

	ring_buffer_attach(event, rb);
	rcu_assign_pointer(event->rb[PERF_RB_ITRACE], rb);

out:
	mutex_unlock(&event->mmap_mutex);
}

int itrace_inherit_event(struct perf_event *event, struct task_struct *task)
{
	struct perf_event *parent = event->parent;
	struct itrace_pmu *ipmu;

	if (!is_itrace_event(event))
		return 0;

	ipmu = to_itrace_pmu(event->pmu);

	/*
	 * inherited user's counters should inherit buffers IF
	 * they aren't cpu==-1
	 */
	if (parent->cpu == -1)
		return -EINVAL;

	itrace_set_output(event, parent);

	return 0;
}

void itrace_wake_up(struct perf_event *event)
{
	struct ring_buffer *rb;

	rcu_read_lock();
	rb = rcu_dereference(event->rb[PERF_RB_ITRACE]);
	if (rb) {
		atomic_set(&rb->poll, POLL_IN);
		irq_work_queue(&event->pending);
	}
	rcu_read_unlock();
}

int itrace_pmu_register(struct itrace_pmu *ipmu)
{
	int ret;

	if (!ipmu->alloc_buffer || !ipmu->free_buffer)
		return -EINVAL;

	ipmu->event_init = ipmu->pmu.event_init;
	ipmu->pmu.event_init = itrace_event_init;

	ret = perf_pmu_register(&ipmu->pmu, ipmu->name, -1);
	if (ret)
		return ret;

	mutex_lock(&itrace_pmus_mutex);
	list_add_tail_rcu(&ipmu->entry, &itrace_pmus);
	mutex_unlock(&itrace_pmus_mutex);

	return ret;
}
