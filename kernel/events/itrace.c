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
#include <linux/sched.h>
#include <linux/perf_event.h>
#include <linux/itrace.h>
#include <linux/sizes.h>
#include <linux/elf.h>
#include <linux/coredump.h>
#include <linux/slab.h>

#include "internal.h"

static LIST_HEAD(itrace_pmus);
static DEFINE_MUTEX(itrace_pmus_mutex);
static struct itrace_pmu *itrace_pmu_coredump;

#define CORE_OWNER "ITRACE"

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

static void itrace_event_destroy(struct perf_event *event)
{
	struct task_struct *task = event->hw.itrace_target;
	struct ring_buffer *rb = event->rb[PERF_RB_ITRACE];

	if (task && event->hw.counter_type == PERF_ITRACE_COREDUMP)
		static_key_slow_dec_deferred(&itrace_core_events);

	if (!rb)
		return;

	if (event->hw.counter_type != PERF_ITRACE_USER) {
		atomic_dec(&rb->mmap_count);
		atomic_dec(&event->mmap_count[PERF_RB_ITRACE]);
		ring_buffer_detach(event, rb);
		rcu_assign_pointer(event->rb[PERF_RB_ITRACE], NULL);
		ring_buffer_put(rb); /* should be last */
	}
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
	int ret;

	ret = ipmu->event_init(event);
	if (ret)
		return ret;

	event->destroy = itrace_event_destroy;
	event->hw.counter_type = PERF_ITRACE_USER;

	return 0;
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

static size_t roundup_buffer_size(u64 size)
{
	return 1ul << (__get_order(size) + PAGE_SHIFT);
}

int itrace_inherit_event(struct perf_event *event, struct task_struct *task)
{
	size_t size = event->attr.itrace_sample_size;
	struct perf_event *parent = event->parent;
	struct ring_buffer *rb;
	struct itrace_pmu *ipmu;

	if (!is_itrace_event(event))
		return 0;

	ipmu = to_itrace_pmu(event->pmu);

	if (parent->hw.counter_type == PERF_ITRACE_USER) {
		/*
		 * inherited user's counters should inherit buffers IF
		 * they aren't cpu==-1
		 */
		if (parent->cpu == -1)
			return -EINVAL;

		itrace_set_output(event, parent);
		return 0;
	}

	event->hw.counter_type = parent->hw.counter_type;
	if (event->hw.counter_type == PERF_ITRACE_COREDUMP) {
		static_key_slow_inc(&itrace_core_events.key);
		size = task_rlimit(task, RLIMIT_ITRACE);
	}

	size = roundup_buffer_size(size);
	rb = rb_alloc(event, size >> PAGE_SHIFT, 0, event->cpu, 0,
		      &itrace_rb_ops);
	if (!rb)
		return -ENOMEM;

	ring_buffer_attach(event, rb);
	rcu_assign_pointer(event->rb[PERF_RB_ITRACE], rb);
	atomic_set(&rb->mmap_count, 1);
	atomic_set(&event->mmap_count[PERF_RB_ITRACE], 1);

	return 0;
}

int itrace_kernel_event(struct perf_event *event, struct task_struct *task)
{
	struct itrace_pmu *ipmu;
	struct ring_buffer *rb;
	size_t size;

	if (!is_itrace_event(event))
		return 0;

	ipmu = to_itrace_pmu(event->pmu);

	if (event->attr.itrace_sample_size)
		size = roundup_buffer_size(event->attr.itrace_sample_size);
	else
		size = task_rlimit(task, RLIMIT_ITRACE);

	rb = rb_alloc(event, size >> PAGE_SHIFT, 0, event->cpu, 0,
		      &itrace_rb_ops);
	if (!rb)
		return -ENOMEM;

	ring_buffer_attach(event, rb);
	rcu_assign_pointer(event->rb[PERF_RB_ITRACE], rb);
	atomic_set(&rb->mmap_count, 1);
	atomic_set(&event->mmap_count[PERF_RB_ITRACE], 1);

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

static ssize_t
coredump_show(struct device *dev,
	      struct device_attribute *attr,
	      char *page)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct itrace_pmu *ipmu = to_itrace_pmu(pmu);
	int ret;

	mutex_lock(&itrace_pmus_mutex);
	ret = itrace_pmu_coredump == ipmu;
	mutex_unlock(&itrace_pmus_mutex);

	return snprintf(page, PAGE_SIZE-1, "%d\n", ret);
}

static ssize_t
coredump_store(struct device *dev,
	       struct device_attribute *attr,
	       const char *buf, size_t count)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct itrace_pmu *ipmu = to_itrace_pmu(pmu);

	mutex_lock(&itrace_pmus_mutex);
	if (ipmu->core_size && ipmu->core_output)
		itrace_pmu_coredump = ipmu;
	mutex_unlock(&itrace_pmus_mutex);

	return count;
}
static DEVICE_ATTR_RW(coredump);

static ssize_t
coredump_config_show(struct device *dev,
		     struct device_attribute *attr,
		     char *page)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct itrace_pmu *ipmu = to_itrace_pmu(pmu);

	return snprintf(page, PAGE_SIZE-1, "%016llx\n", ipmu->coredump_config);
}

static ssize_t
coredump_config_store(struct device *dev,
		      struct device_attribute *attr,
		      const char *buf, size_t count)
{
	struct pmu *pmu = dev_get_drvdata(dev);
	struct itrace_pmu *ipmu = to_itrace_pmu(pmu);
	u64 config;
	int ret;

	ret = kstrtou64(buf, 0, &config);
	if (ret)
		return ret;

	ipmu->coredump_config = config;

	return count;
}
static DEVICE_ATTR_RW(coredump_config);

static struct attribute *itrace_attrs[] = {
	&dev_attr_coredump.attr,
	&dev_attr_coredump_config.attr,
	NULL,
};

struct attribute_group itrace_group = {
	.attrs	= itrace_attrs,
};

static const struct attribute_group **
itrace_get_attr_groups(const struct attribute_group **pgroups)
{
	const struct attribute_group **groups;
	int i, ngroups;
	size_t size;

	for (i = 0, ngroups = 2; pgroups[i]; i++, ngroups++)
		;

	size = sizeof(struct attribute_group *) * ngroups;
	groups = kzalloc(size, GFP_KERNEL);
	if (!groups)
		goto out;

	for (i = 0; pgroups[i]; i++)
		groups[i] = pgroups[i];

	groups[i] = &itrace_group;

out:
	return groups;
}

int itrace_pmu_register(struct itrace_pmu *ipmu)
{
	int ret;

	if (!ipmu->alloc_buffer || !ipmu->free_buffer)
		return -EINVAL;

	ipmu->event_init = ipmu->pmu.event_init;
	ipmu->pmu.event_init = itrace_event_init;
	ipmu->pmu.attr_groups = itrace_get_attr_groups(ipmu->pmu.attr_groups);

	ret = perf_pmu_register(&ipmu->pmu, ipmu->name, -1);
	if (ret)
		return ret;

	mutex_lock(&itrace_pmus_mutex);
	list_add_tail_rcu(&ipmu->entry, &itrace_pmus);
	if (ipmu->core_size && ipmu->core_output)
		itrace_pmu_coredump = ipmu;
	mutex_unlock(&itrace_pmus_mutex);

	return ret;
}

/*
 * Trace sample annotation
 * For events that have attr.sample_type & PERF_SAMPLE_ITRACE, perf calls here
 * to configure and obtain itrace samples.
 */

int itrace_sampler_init(struct perf_event *event, struct task_struct *task)
{
	struct perf_event_attr attr;
	struct perf_event *tevt;
	struct itrace_pmu *ipmu;

	ipmu = itrace_pmu_find(event->attr.itrace_sample_type);
	if (!ipmu || !ipmu->sample_trace || !ipmu->sample_output)
		return -ENOTSUPP;

	memset(&attr, 0, sizeof(attr));
	attr.type = ipmu->pmu.type;
	attr.config = 0;
	attr.sample_type = 0;
	attr.exclude_user = event->attr.exclude_user;
	attr.exclude_kernel = event->attr.exclude_kernel;
	attr.itrace_sample_size = event->attr.itrace_sample_size;
	attr.itrace_config = event->attr.itrace_config;

	tevt = perf_event_create_kernel_counter(&attr, event->cpu, task, NULL, NULL);
	if (IS_ERR(tevt))
		return PTR_ERR(tevt);

	if (!itrace_priv(tevt)) {
		perf_event_release_kernel(tevt);
		return -EINVAL;
	}

	event->trace_event = tevt;
	tevt->hw.counter_type = PERF_ITRACE_SAMPLING;
	if (event->state != PERF_EVENT_STATE_OFF)
		perf_event_enable(event->trace_event);

	return 0;
}

void itrace_sampler_fini(struct perf_event *event)
{
	struct perf_event *tevt = event->trace_event;

	perf_event_release_kernel(tevt);
	event->trace_event = NULL;
}

unsigned long itrace_sampler_trace(struct perf_event *event,
				   struct perf_sample_data *data)
{
	struct perf_event *tevt = event->trace_event;
	struct itrace_pmu *ipmu;

	if (!tevt)
		return 0;

	ipmu = to_itrace_pmu(tevt->pmu);
	return ipmu->sample_trace(tevt, data);
}

void itrace_sampler_output(struct perf_event *event,
			   struct perf_output_handle *handle,
			   struct perf_sample_data *data)
{
	struct perf_event *tevt = event->trace_event;
	struct itrace_pmu *ipmu;

	if (!tevt || !data->trace.size)
		return;

	ipmu = to_itrace_pmu(tevt->pmu);
	ipmu->sample_output(tevt, handle, data);
}

/*
 * Core dump bits
 *
 * Various parts of the kernel will call here:
 *   + do_prlimit(): to tell us that the user is trying to set RLIMIT_ITRACE
 *   + various places in bitfmt_elf.c: to write out itrace notes
 *   + do_exit(): to destroy the first core dump counter
 *   + the rest (copy_process()/do_exit()) is taken care of by perf for us
 */

static struct perf_event *
itrace_find_task_event(struct task_struct *task, unsigned type)
{
	struct perf_event_context *ctx;
	struct perf_event *event = NULL;

	rcu_read_lock();
	ctx = rcu_dereference(task->perf_event_ctxp[perf_hw_context]);
	if (!ctx)
		goto out;

	list_for_each_entry_rcu(event, &ctx->event_list, event_entry) {
		if (is_itrace_event(event) &&
		    event->cpu == -1 &&
		    !!(event->hw.counter_type & type))
			goto out;
	}

	event = NULL;
out:
	rcu_read_unlock();

	return event;
}

int update_itrace_rlimit(struct task_struct *task, unsigned long rlim)
{
	struct perf_event_attr attr;
	struct perf_event *event;

	event = itrace_find_task_event(task, PERF_ITRACE_ANY);
	if (event) {
		if (event->hw.counter_type != PERF_ITRACE_COREDUMP)
			return -EINVAL;

		perf_event_release_kernel(event);
		static_key_slow_dec_deferred(&itrace_core_events);
	}

	if (!rlim)
		return 0;

	memset(&attr, 0, sizeof(attr));

	mutex_lock(&itrace_pmus_mutex);
	if (!itrace_pmu_coredump) {
		mutex_unlock(&itrace_pmus_mutex);
		return -ENOTSUPP;
	}

	attr.type = itrace_pmu_coredump->pmu.type;
	attr.config = 0;
	attr.sample_type = 0;
	attr.exclude_kernel = 1;
	attr.inherit = 1;
	attr.itrace_config = itrace_pmu_coredump->coredump_config;

	event = perf_event_create_kernel_counter(&attr, -1, task, NULL, NULL);
	mutex_unlock(&itrace_pmus_mutex);

	if (IS_ERR(event))
		return PTR_ERR(event);

	static_key_slow_inc(&itrace_core_events.key);

	event->hw.counter_type = PERF_ITRACE_COREDUMP;
	perf_event_enable(event);

	return 0;
}

static void itrace_pmu_exit_task(struct task_struct *task)
{
	struct perf_event *event;

	event = itrace_find_task_event(task, PERF_ITRACE_COREDUMP);

	/*
	 * here we are only interested in kernel counters created by
	 * update_itrace_rlimit(), inherited ones should be taken care of by
	 * perf_event_exit_task(), sampling ones are taken care of by
	 * itrace_sampler_fini().
	 */
	if (!event)
		return;

	if (!event->parent)
		perf_event_release_kernel(event);
}

void exit_itrace(struct task_struct *task)
{
	if (static_key_false(&itrace_core_events.key))
		itrace_pmu_exit_task(task);
}

size_t itrace_elf_note_size(struct task_struct *task)
{
	struct itrace_pmu *ipmu;
	struct perf_event *event = NULL;
	size_t size = 0;

	event = itrace_find_task_event(task, PERF_ITRACE_COREDUMP);
	if (event) {
		perf_event_disable(event);

		ipmu = to_itrace_pmu(event->pmu);
		size = ipmu->core_size(event);
		size += task_rlimit(task, RLIMIT_ITRACE);
		size = roundup(size + strlen(ipmu->name) + 1, 4);
		size += sizeof(struct itrace_note) + sizeof(struct elf_note);
		size += roundup(sizeof(CORE_OWNER), 4);
	}

	return size;
}

void itrace_elf_note_write(struct coredump_params *cprm,
			   struct task_struct *task)
{
	struct perf_event *event;
	struct itrace_note note;
	struct itrace_pmu *ipmu;
	struct elf_note en;
	unsigned long rlim;
	size_t pmu_len;

	event = itrace_find_task_event(task, PERF_ITRACE_COREDUMP);
	if (!event)
		return;

	ipmu = to_itrace_pmu(event->pmu);
	pmu_len = strlen(ipmu->name) + 1;

	rlim = task_rlimit(task, RLIMIT_ITRACE);

	/* Elf note with name */
	en.n_namesz = strlen(CORE_OWNER);
	en.n_descsz = roundup(ipmu->core_size(event) + rlim + sizeof(note) +
			      pmu_len, 4);
	en.n_type = NT_ITRACE;
	dump_emit(cprm, &en, sizeof(en));
	dump_align(cprm, 4);
	dump_emit(cprm, CORE_OWNER, sizeof(CORE_OWNER));
	dump_align(cprm, 4);

	/* ITRACE header */
	note.itrace_config = event->attr.itrace_config;
	dump_emit(cprm, &note, sizeof(note));
	dump_emit(cprm, ipmu->name, pmu_len);

	/* ITRACE PMU header + payload */
	ipmu->core_output(cprm, event, rlim);
	dump_align(cprm, 4);
}
