/*
 * intel_pt.c: Intel Processor Trace support
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

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <linux/kernel.h>

#include "../perf.h"
#include "session.h"
#include "machine.h"
#include "tool.h"
#include "event.h"
#include "evlist.h"
#include "evsel.h"
#include "map.h"
#include "cpumap.h"
#include "types.h"
#include "color.h"
#include "util.h"
#include "thread.h"
#include "symbol.h"
#include "parse-options.h"
#include "parse-events.h"
#include "pmu.h"
#include "dso.h"
#include "debug.h"
#include "itrace.h"
#include "tsc.h"
#include "intel-pt.h"

#include "intel-pt-decoder/intel-pt-log.h"
#include "intel-pt-decoder/intel-pt-decoder.h"
#include "intel-pt-decoder/intel-pt-insn-decoder.h"
#include "intel-pt-decoder/intel-pt-pkt-decoder.h"

#define MAX_TIMESTAMP (~0ULL)

#define KiB(x) ((x) * 1024)
#define MiB(x) ((x) * 1024 * 1024)
#define KiB_MASK(x) (KiB(x) - 1)
#define MiB_MASK(x) (MiB(x) - 1)

#define INTEL_PT_DEFAULT_SAMPLE_SIZE	KiB(4)

#define INTEL_PT_MAX_SAMPLE_SIZE	KiB(60)

#define INTEL_PT_PSB_PERIOD_NEAR	256

struct intel_pt_snapshot_ref {
	void *ref_buf;
	size_t ref_offset;
	bool wrapped;
};

struct intel_pt_recording {
	struct itrace_record		itr;
	struct perf_pmu			*intel_pt_pmu;
	int				have_sched_switch;
	struct perf_evlist		*evlist;
	bool				snapshot_mode;
	bool				snapshot_init_done;
	size_t				snapshot_size;
	size_t				snapshot_ref_buf_size;
	int				snapshot_ref_cnt;
	struct intel_pt_snapshot_ref	*snapshot_refs;
};

struct intel_pt {
	struct itrace itrace;
	struct itrace_queues queues;
	struct itrace_heap heap;
	u32 itrace_type;
	struct perf_session *session;
	struct machine *machine;
	struct perf_evsel *switch_evsel;
	bool timeless_decoding;
	bool sampling_mode;
	bool snapshot_mode;
	bool per_cpu_mmaps;
	bool have_tsc;
	bool data_queued;
	bool est_tsc;
	int have_sched_switch;
	u32 pmu_type;

	struct perf_tsc_conversion tc;
	bool cap_user_time_zero;

	struct itrace_synth_opts synth_opts;

	bool sample_instructions;
	u64 instructions_sample_type;
	u64 instructions_sample_period;
	u64 instructions_id;
	size_t instructions_event_size;

	bool sample_branches;
	u64 branches_sample_type;
	u64 branches_id;
	size_t branches_event_size;

	bool synth_needs_swap;

	u64 tsc_bit;
	u64 noretcomp_bit;
};

#define IBUFSIZE 4096

struct intel_pt_queue {
	struct intel_pt *pt;
	unsigned int queue_nr;
	struct itrace_buffer *buffer;
	void *decoder;
	const struct intel_pt_state *state;
	bool on_heap;
	bool stop;
	bool step_through_buffers;
	bool use_buffer_pid_tid;
	pid_t pid, tid;
	int cpu;
	bool exclude_kernel;
	bool have_sample;
	u64 time;
	u64 timestamp;
	int ibuflen;
	uint64_t ibufip;
	u8 ibuf[IBUFSIZE];
};

static void intel_pt_dump(struct intel_pt *pt __maybe_unused,
			  unsigned char *buf, size_t len)
{
	struct intel_pt_pkt packet;
	size_t pos = 0;
	int ret, pkt_len, i;
	char desc[INTEL_PT_PKT_DESC_MAX];
	const char *color = PERF_COLOR_BLUE;

	color_fprintf(stdout, color,
		      ". ... Intel Processor Trace data: size %zu bytes\n",
		      len);

	while (len) {
		ret = intel_pt_get_packet(buf, len, &packet);
		if (ret > 0)
			pkt_len = ret;
		else
			pkt_len = 1;
		printf(".");
		color_fprintf(stdout, color, "  %08x: ", pos);
		for (i = 0; i < pkt_len; i++)
			color_fprintf(stdout, color, " %02x", buf[i]);
		for (; i < 16; i++)
			color_fprintf(stdout, color, "   ");
		if (ret > 0) {
			ret = intel_pt_pkt_desc(&packet, desc,
						INTEL_PT_PKT_DESC_MAX);
			if (ret > 0)
				color_fprintf(stdout, color, " %s\n", desc);
		} else {
			color_fprintf(stdout, color, " Bad packet!\n");
		}
		pos += pkt_len;
		buf += pkt_len;
		len -= pkt_len;
	}
}

static void intel_pt_dump_event(struct intel_pt *pt, unsigned char *buf,
				size_t len)
{
	printf(".\n");
	intel_pt_dump(pt, buf, len);
}

static void intel_pt_dump_sample(struct perf_session *session,
				 struct perf_sample *sample)
{
	struct intel_pt *pt = container_of(session->itrace, struct intel_pt,
					   itrace);

	intel_pt_dump(pt, sample->itrace_sample.data,
		      sample->itrace_sample.size);
	printf(".\n");
}

static int intel_pt_do_fix_overlap(struct intel_pt *pt, struct itrace_buffer *a,
				   struct itrace_buffer *b)
{
	void *start;

	start = intel_pt_find_overlap(a->data, a->size, b->data, b->size,
				      pt->have_tsc);
	if (!start)
		return -EINVAL;
	b->use_size = b->data + b->size - start;
	b->use_data = start;
	return 0;
}

static int intel_pt_fix_overlap(struct intel_pt *pt, unsigned int queue_nr)
{
	struct itrace_queue *queue = &pt->queues.queue_array[queue_nr];
	struct itrace_buffer *a, *b;

	b = list_entry(queue->head.prev, struct itrace_buffer, list);
	if (b->list.prev == &queue->head)
		return 0;
	a = list_entry(b->list.prev, struct itrace_buffer, list);
	return intel_pt_do_fix_overlap(pt, a, b);
}

/* This function assumes data is processed sequentially only */
static int intel_pt_get_trace(struct intel_pt_buffer *b, void *data)
{
	struct intel_pt_queue *ptq = data;
	struct itrace_buffer *buffer = ptq->buffer, *old_buffer = buffer;
	struct itrace_queue *queue;

	if (ptq->stop) {
		b->len = 0;
		return 0;
	}

	queue = &ptq->pt->queues.queue_array[ptq->queue_nr];

	buffer = itrace_buffer__next(queue, buffer);
	if (!buffer) {
		if (old_buffer)
			itrace_buffer__drop_data(old_buffer);
		b->len = 0;
		return 0;
	}

	ptq->buffer = buffer;

	if (!buffer->data) {
		int fd = perf_data_file__fd(ptq->pt->session->file);

		buffer->data = itrace_buffer__get_data(buffer, fd);
		if (!buffer->data)
			return -ENOMEM;
	}

	if (ptq->pt->snapshot_mode && !buffer->consecutive && old_buffer &&
	    intel_pt_do_fix_overlap(ptq->pt, old_buffer, buffer))
		return -ENOMEM;

	if (old_buffer)
		itrace_buffer__drop_data(old_buffer);

	if (buffer->use_data) {
		b->len = buffer->use_size;
		b->buf = buffer->use_data;
	} else {
		b->len = buffer->size;
		b->buf = buffer->data;
	}
	b->ref_timestamp = buffer->reference;

	if (!old_buffer || ptq->pt->sampling_mode || (ptq->pt->snapshot_mode &&
						      !buffer->consecutive))
		b->consecutive = false;
	else
		b->consecutive = true;

	if (ptq->use_buffer_pid_tid && (ptq->pid != buffer->pid ||
					ptq->tid != buffer->tid)) {
		if (queue->cpu == -1 && buffer->cpu != -1)
			ptq->cpu = buffer->cpu;
		ptq->pid = buffer->pid;
		ptq->tid = buffer->tid;
		intel_pt_log("queue %u cpu %d pid %d tid %d\n",
			     ptq->queue_nr, ptq->cpu, ptq->pid, ptq->tid);
	}

	if (ptq->step_through_buffers)
		ptq->stop = true;

	if (!b->len)
		return intel_pt_get_trace(b, data);

	return 0;
}

static int intel_pt_try_decode(struct intel_pt_insn *intel_pt_insn,
			       struct intel_pt_queue *ptq,
			       uint64_t ip, bool x86_64)
{
	int err = -1, off;

	if (ip >= ptq->ibufip && ip < ptq->ibufip + ptq->ibuflen) {
		off = ip - ptq->ibufip;
		err = intel_pt_get_insn(ptq->ibuf + off,
					ptq->ibuflen - off,
					x86_64,
					intel_pt_insn);
	}
	return err;
}

static int intel_pt_get_next_insn(struct intel_pt_insn *intel_pt_insn,
				  uint64_t ip, uint64_t cr3 __maybe_unused,
				  void *data, bool x86_64)
{
	struct intel_pt_queue *ptq = data;
	struct machine *machine = ptq->pt->machine;
	struct thread *thread;
	struct addr_location al;
	pid_t pid = ptq->pid;
	uint8_t cpumode;
	int err;

	/*
	 * XXX should read directly from the underlying
	 * dso cache.
	 */

	/* Try to decode the rest first, if it fails get fresh data. */
	err = intel_pt_try_decode(intel_pt_insn, ptq, ip, x86_64);
	if (err == 0)
		return err;

	/* Assume kernel addresses can be identified by "ip < 0" */
	/* AK: I think this is not correct on 32bit kernels */
	if ((int64_t)ip < 0)
		cpumode = PERF_RECORD_MISC_KERNEL;
	else
		cpumode = PERF_RECORD_MISC_USER;

	thread = machine__findnew_thread(machine, pid, pid);
	if (!thread)
		return -1;

	thread__find_addr_map(thread, machine, cpumode, MAP__FUNCTION, ip, &al);
	if (!al.map || !al.map->dso)
		return -1;

	/*
	 * Try to read ahead multiple instructions to amortize the overhead
	 * of the thread/map lookups.
	 */
	ptq->ibuflen = dso__data_read_addr(al.map->dso, al.map, machine, ip,
					   ptq->ibuf, IBUFSIZE);
	if (ptq->ibuflen <= 0) {
		ptq->ibuflen = 0;
		return -1;
	}
	ptq->ibufip = ip;

	return intel_pt_try_decode(intel_pt_insn, ptq, ip, x86_64);
}

static bool intel_pt_exclude_kernel(struct intel_pt *pt)
{
	struct perf_session *session = pt->session;
	struct perf_evlist *evlist = session->evlist;
	struct perf_evsel *evsel;

	list_for_each_entry(evsel, &evlist->entries, node) {
		if ((evsel->attr.type == pt->pmu_type ||
		     (evsel->attr.sample_type & PERF_SAMPLE_ITRACE)) &&
		    !evsel->attr.exclude_kernel)
			return false;
	}
	return true;
}

static bool intel_pt_return_compression(struct intel_pt *pt)
{
	struct perf_session *session = pt->session;
	struct perf_evlist *evlist = session->evlist;
	struct perf_evsel *evsel;

	if (!pt->noretcomp_bit)
		return true;

	list_for_each_entry(evsel, &evlist->entries, node) {
		if (evsel->attr.itrace_config & pt->noretcomp_bit)
				return false;
	}
	return true;
}

static bool intel_pt_timeless_decoding(struct intel_pt *pt)
{
	struct perf_session *session = pt->session;
	struct perf_evlist *evlist = session->evlist;
	struct perf_evsel *evsel;
	bool timeless_decoding = true;

	if (!pt->tsc_bit || !pt->cap_user_time_zero)
		return true;

	list_for_each_entry(evsel, &evlist->entries, node) {
		if (!(evsel->attr.sample_type & PERF_SAMPLE_TIME))
			return true;
		if (evsel->attr.type == pt->pmu_type ||
		    (evsel->attr.sample_type & PERF_SAMPLE_ITRACE)) {
			if (evsel->attr.itrace_config & pt->tsc_bit)
				timeless_decoding = false;
			else
				return true;
		}
	}
	return timeless_decoding;
}

static bool intel_pt_have_tsc(struct intel_pt *pt)
{
	struct perf_session *session = pt->session;
	struct perf_evlist *evlist = session->evlist;
	struct perf_evsel *evsel;
	bool have_tsc = false;

	if (!pt->tsc_bit)
		return false;

	list_for_each_entry(evsel, &evlist->entries, node) {
		if (evsel->attr.type == pt->pmu_type ||
		    (evsel->attr.sample_type & PERF_SAMPLE_ITRACE)) {
			if (evsel->attr.itrace_config & pt->tsc_bit)
				have_tsc = true;
			else
				return false;
		}
	}
	return have_tsc;
}

static bool intel_pt_sampling_mode(struct intel_pt *pt)
{
	struct perf_session *session = pt->session;
	struct perf_evlist *evlist = session->evlist;
	struct perf_evsel *evsel;

	list_for_each_entry(evsel, &evlist->entries, node) {
		if (evsel->attr.type == pt->pmu_type)
			return false;
		if (evsel->attr.sample_type & PERF_SAMPLE_ITRACE)
			return true;
	}
	return false;
}

static u64 intel_pt_ns_to_ticks(const struct intel_pt *pt, u64 ns)
{
	u64 quot, rem;

	quot = ns / pt->tc.time_mult;
	rem  = ns % pt->tc.time_mult;
	return (quot << pt->tc.time_shift) + (rem << pt->tc.time_shift) /
		pt->tc.time_mult;
}

static struct intel_pt_queue *intel_pt_alloc_queue(struct intel_pt *pt,
						   unsigned int queue_nr)
{
	struct intel_pt_params params = {0};
	struct intel_pt_queue *ptq;

	ptq = zalloc(sizeof(struct intel_pt_queue));
	if (!ptq)
		return NULL;

	ptq->pt = pt;
	ptq->queue_nr = queue_nr;
	ptq->exclude_kernel = intel_pt_exclude_kernel(pt);
	ptq->pid = -1;
	ptq->tid = -1;
	ptq->cpu = -1;

	params.get_trace = intel_pt_get_trace;
	params.get_insn = intel_pt_get_next_insn;
	params.data = ptq;
	params.return_compression = intel_pt_return_compression(pt);

	if (pt->synth_opts.instructions) {
		if (pt->synth_opts.period) {
			switch (pt->synth_opts.period_type) {
			case PERF_ITRACE_PERIOD_INSTRUCTIONS:
				params.period_type =
						INTEL_PT_PERIOD_INSTRUCTIONS;
				params.period = pt->synth_opts.period;
				break;
			case PERF_ITRACE_PERIOD_TICKS:
				params.period_type = INTEL_PT_PERIOD_TICKS;
				params.period = pt->synth_opts.period;
				break;
			case PERF_ITRACE_PERIOD_NANOSECS:
				params.period_type = INTEL_PT_PERIOD_TICKS;
				params.period = intel_pt_ns_to_ticks(pt,
							pt->synth_opts.period);
				break;
			default:
				break;
			}
		}

		if (!params.period) {
			params.period_type = INTEL_PT_PERIOD_INSTRUCTIONS;
			params.period = 1000;
		}
	}

	ptq->decoder = intel_pt_decoder_new(&params);
	if (!ptq->decoder) {
		free(ptq);
		return NULL;
	}

	return ptq;
}

static void intel_pt_free_queue(void *priv)
{
	struct intel_pt_queue *ptq = priv;

	if (!ptq)
		return;
	intel_pt_decoder_free(ptq->decoder);
	free(ptq);
}

static void intel_pt_set_pid_tid_cpu(struct intel_pt *pt,
				     struct itrace_queue *queue)
{
	struct intel_pt_queue *ptq = queue->priv;

	if (queue->cpu == -1) {
		/* queue per-thread */
		struct thread *thread;

		thread = machine__find_thread(pt->machine, ptq->tid);
		if (thread) {
			ptq->pid = thread->pid_;
			ptq->cpu = thread->cpu;
		} else {
			ptq->cpu = -1;
		}
	} else if (queue->tid != -1 && !pt->have_sched_switch) {
		/* queue per-cpu workload only */
		if (ptq->pid == -1)
			ptq->pid = machine__get_thread_pid(pt->machine,
							   ptq->tid);
	} else {
		/* queue per-cpu */
		ptq->tid = machine__get_current_tid(pt->machine, ptq->cpu);
		ptq->pid = machine__get_thread_pid(pt->machine, ptq->tid);
	}
}

static int intel_pt_setup_queue(struct intel_pt *pt, struct itrace_queue *queue,
				unsigned int queue_nr)
{
	struct intel_pt_queue *ptq = queue->priv;

	if (list_empty(&queue->head))
		return 0;

	if (!ptq) {
		ptq = intel_pt_alloc_queue(pt, queue_nr);
		if (!ptq)
			return -ENOMEM;
		queue->priv = ptq;

		if (queue->cpu != -1)
			ptq->cpu = queue->cpu;
		ptq->tid = queue->tid;

		if (pt->sampling_mode) {
			if (pt->timeless_decoding)
				ptq->step_through_buffers = true;
			if (pt->timeless_decoding || !pt->have_sched_switch)
				ptq->use_buffer_pid_tid = true;
		}
	}

	if (!ptq->on_heap) {
		const struct intel_pt_state *state;
		int ret;

		if (pt->timeless_decoding)
			return 0;

		intel_pt_set_pid_tid_cpu(pt, queue);

		intel_pt_log("queue %u getting timestamp\n", queue_nr);
		intel_pt_log("queue %u decoding cpu %d pid %d tid %d\n",
			     queue_nr, ptq->cpu, ptq->pid, ptq->tid);
		while (1) {
			state = intel_pt_decode(ptq->decoder);
			if (state->err) {
				if (state->err == -ENODATA) {
					intel_pt_log("queue %u has no timestamp\n",
						     queue_nr);
					return 0;
				}
				continue;
			}
			if (state->timestamp)
				break;
		}

		ptq->timestamp = state->timestamp;
		intel_pt_log("queue %u timestamp 0x%" PRIx64 "\n",
			     queue_nr, ptq->timestamp);
		ptq->state = state;
		ptq->have_sample = true;
		ret = itrace_heap__add(&pt->heap, queue_nr, ptq->timestamp);
		if (ret)
			return ret;
		ptq->on_heap = true;
	}

	return 0;
}

static int intel_pt_setup_queues(struct intel_pt *pt)
{
	unsigned int i;
	int ret;

	for (i = 0; i < pt->queues.nr_queues; i++) {
		ret = intel_pt_setup_queue(pt, &pt->queues.queue_array[i], i);
		if (ret)
			return ret;
	}
	return 0;
}

static int intel_pt_synth_branch_sample(struct intel_pt_queue *ptq,
					struct perf_tool *tool)
{
	int ret;
	struct intel_pt *pt = ptq->pt;
	union perf_event event;
	struct perf_sample sample = {0};

	event.sample.header.type = PERF_RECORD_SAMPLE;
	event.sample.header.misc = PERF_RECORD_MISC_USER;
	event.sample.header.size = sizeof(struct perf_event_header);

	if (!pt->timeless_decoding)
		sample.time = tsc_to_perf_time(ptq->timestamp, &pt->tc);

	sample.ip = ptq->state->from_ip;
	sample.pid = ptq->pid;
	sample.tid = ptq->tid;
	sample.addr = ptq->state->to_ip;
	sample.id = ptq->pt->branches_id;
	sample.stream_id = ptq->pt->branches_id;
	sample.period = 1;
	sample.cpu = ptq->cpu;

	if (pt->synth_opts.inject) {
		event.sample.header.size = pt->branches_event_size;
		ret = perf_event__synthesize_sample(&event,
						    pt->branches_sample_type, 0,
						    0, &sample,
						    pt->synth_needs_swap);
		if (ret)
			return ret;
	}

	ret = perf_session__deliver_synth_event(pt->session, &event, &sample,
						tool);
	if (ret)
		pr_err("Intel Processor Trace: failed to deliver branch event, error %d\n",
		       ret);

	return ret;
}

static int intel_pt_synth_instruction_sample(struct intel_pt_queue *ptq,
					     struct perf_tool *tool)
{
	int ret;
	struct intel_pt *pt = ptq->pt;
	union perf_event event;
	struct perf_sample sample = {0};

	event.sample.header.type = PERF_RECORD_SAMPLE;
	event.sample.header.misc = PERF_RECORD_MISC_USER;
	event.sample.header.size = sizeof(struct perf_event_header);

	if (!pt->timeless_decoding)
		sample.time = tsc_to_perf_time(ptq->timestamp, &pt->tc);

	sample.ip = ptq->state->from_ip;
	sample.pid = ptq->pid;
	sample.tid = ptq->tid;
	sample.addr = ptq->state->to_ip;
	sample.id = ptq->pt->instructions_id;
	sample.stream_id = ptq->pt->instructions_id;
	sample.period = ptq->pt->instructions_sample_period;
	sample.cpu = ptq->cpu;

	if (pt->synth_opts.inject) {
		event.sample.header.size = pt->instructions_event_size;
		ret = perf_event__synthesize_sample(&event,
						pt->instructions_sample_type, 0,
						0, &sample,
						pt->synth_needs_swap);
		if (ret)
			return ret;
	}

	ret = perf_session__deliver_synth_event(pt->session, &event, &sample,
						tool);
	if (ret)
		pr_err("Intel Processor Trace: failed to deliver instruction event, error %d\n",
		       ret);

	return ret;
}

static int intel_pt_synth_error(struct intel_pt *pt, struct perf_tool *tool,
				int code, int cpu, pid_t pid, pid_t tid, u64 ip)
{
	union perf_event event;
	const char *msg;
	int err;

	msg = intel_pt_error_message(code);

	itrace_synth_error(&event.itrace_error, PERF_ITRACE_DECODER_ERROR, code,
			   cpu, pid, tid, ip, msg);

	err = perf_session__deliver_synth_event(pt->session, &event, NULL,
						tool);
	if (err)
		pr_err("Intel Processor Trace: failed to deliver error event, error %d\n",
		       err);

	return err;
}

static int intel_pt_run_decoder(struct intel_pt_queue *ptq, u64 *timestamp,
				struct perf_tool *tool)
{
	const struct intel_pt_state *state = ptq->state;
	struct intel_pt *pt = ptq->pt;
	int err;

	intel_pt_log("queue %u decoding cpu %d pid %d tid %d\n",
		     ptq->queue_nr, ptq->cpu, ptq->pid, ptq->tid);
	while (1) {
		if (ptq->have_sample) {
			ptq->have_sample = false;

			if (pt->sample_instructions &&
			    (state->type & INTEL_PT_INSTRUCTION)) {
				err = intel_pt_synth_instruction_sample(ptq,
									tool);
				if (err)
					return err;
			}

			if (pt->sample_branches &&
			    (state->type & INTEL_PT_BRANCH)) {
				err = intel_pt_synth_branch_sample(ptq, tool);
				if (err)
					return err;
			}
		}

		state = intel_pt_decode(ptq->decoder);
		if (state->err) {
			if (state->err == -ENODATA)
				return 1;
			if (pt->synth_opts.errors) {
				err = intel_pt_synth_error(pt, tool,
						-state->err, ptq->cpu, ptq->pid,
						ptq->tid, state->from_ip);
				if (err)
					return err;
			}
			continue;
		}

		ptq->state = state;
		ptq->have_sample = true;

		/* Use estimated TSC upon return to user space */
		if (pt->est_tsc) {
			if ((s64)state->from_ip < 0 && (s64)state->to_ip > 0)
				ptq->timestamp = state->est_timestamp;
			else if (state->timestamp > ptq->timestamp)
				ptq->timestamp = state->timestamp;
		} else {
			ptq->timestamp = state->timestamp;
		}

		if (!pt->timeless_decoding && ptq->timestamp >= *timestamp) {
			*timestamp = ptq->timestamp;
			return 0;
		}
	}
	return 0;
}

static inline int intel_pt_update_queues(struct intel_pt *pt)
{
	if (pt->queues.new_data) {
		pt->queues.new_data = false;
		return intel_pt_setup_queues(pt);
	}
	return 0;
}

static int intel_pt_process_queues(struct intel_pt *pt, u64 timestamp,
				   struct perf_tool *tool)
{
	unsigned int queue_nr;
	u64 ts;
	int ret;

	while (1) {
		struct itrace_queue *queue;
		struct intel_pt_queue *ptq;

		if (!pt->heap.heap_cnt)
			return 0;

		if (pt->heap.heap_array[0].ordinal >= timestamp)
			return 0;

		queue_nr = pt->heap.heap_array[0].queue_nr;
		queue = &pt->queues.queue_array[queue_nr];
		ptq = queue->priv;

		intel_pt_log("queue %u processing 0x%" PRIx64 " < 0x%" PRIx64 "\n",
			     queue_nr, pt->heap.heap_array[0].ordinal,
			     timestamp);

		itrace_heap__pop(&pt->heap);

		if (pt->heap.heap_cnt) {
			ts = pt->heap.heap_array[0].ordinal + 1;
			if (ts > timestamp)
				ts = timestamp;
		} else {
			ts = timestamp;
		}

		intel_pt_set_pid_tid_cpu(pt, queue);

		ret = intel_pt_run_decoder(ptq, &ts, tool);

		if (ret < 0) {
			itrace_heap__add(&pt->heap, queue_nr, ts);
			return ret;
		}

		if (!ret) {
			ret = itrace_heap__add(&pt->heap, queue_nr, ts);
			if (ret < 0)
				return ret;
		} else {
			ptq->on_heap = false;
		}
	}

	return 0;
}

static int intel_pt_process_sample_queues(struct intel_pt *pt, u64 timestamp,
				union perf_event *event __maybe_unused,
				struct perf_sample *sample __maybe_unused,
				struct perf_tool *tool)
{
	unsigned int queue_nr;
	u64 ts;
	int ret;

	while (1) {
		struct itrace_queue *queue;
		struct intel_pt_queue *ptq;

		if (!pt->heap.heap_cnt)
			return 0;

		if (pt->heap.heap_array[0].ordinal >= timestamp)
			return 0;

		queue_nr = pt->heap.heap_array[0].queue_nr;
		queue = &pt->queues.queue_array[queue_nr];
		ptq = queue->priv;

		intel_pt_log("queue %u processing 0x%" PRIx64 " < 0x%" PRIx64 "\n",
			     queue_nr, pt->heap.heap_array[0].ordinal,
			     timestamp);

		itrace_heap__pop(&pt->heap);

		if (pt->heap.heap_cnt) {
			ts = pt->heap.heap_array[0].ordinal + 1;
			if (ts > timestamp)
				ts = timestamp;
		} else {
			ts = timestamp;
		}

		if (!ptq->use_buffer_pid_tid)
			intel_pt_set_pid_tid_cpu(pt, queue);

		ret = intel_pt_run_decoder(ptq, &ts, tool);
		if (ret < 0) {
			itrace_heap__add(&pt->heap, queue_nr, ts);
			return ret;
		}

		if (ret) {
			ptq->on_heap = false;
		} else {
			ret = itrace_heap__add(&pt->heap, queue_nr, ts);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

static int intel_pt_process_timeless_queues(struct intel_pt *pt, pid_t tid,
					    u64 time, struct perf_tool *tool)
{
	struct itrace_queues *queues = &pt->queues;
	unsigned int i;
	u64 ts = 0;

	for (i = 0; i < queues->nr_queues; i++) {
		struct itrace_queue *queue = &pt->queues.queue_array[i];
		struct intel_pt_queue *ptq = queue->priv;

		if (ptq && (tid == -1 || ptq->tid == tid)) {
			ptq->time = time;

			if (ptq->pid == -1 && ptq->tid != -1)
				ptq->pid = machine__get_thread_pid(pt->machine,
								   ptq->tid);

			intel_pt_run_decoder(ptq, &ts, tool);
		}
	}
	return 0;
}

static int intel_pt_process_timeless_sample(struct intel_pt *pt,
					    struct perf_sample *sample,
					    struct perf_tool *tool)
{
	struct itrace_queue *queue = itrace_queues__sample_queue(&pt->queues,
								 sample,
								 pt->session);
	struct intel_pt_queue *ptq = queue->priv;
	u64 ts = 0;

	ptq->stop = false;
	ptq->time = sample->time;
	intel_pt_run_decoder(ptq, &ts, tool);
	return 0;
}

static int intel_pt_lost(struct intel_pt *pt, struct perf_sample *sample,
			 struct perf_tool *tool)
{
	union perf_event event;
	int err;

	itrace_synth_error(&event.itrace_error, PERF_ITRACE_DECODER_ERROR,
			   ENOSPC, sample->cpu, sample->pid, sample->tid, 0,
			   "Lost trace data");

	err = perf_session__deliver_synth_event(pt->session, &event, NULL,
						tool);
	if (err)
		pr_err("Intel Processor Trace: failed to deliver error event, error %d\n",
		       err);

	return err;
}

static int intel_pt_process_switch(struct intel_pt *pt,
				   struct perf_sample *sample)
{
	struct perf_evsel *evsel;
	pid_t tid;
	int cpu;

	evsel = perf_evlist__id2evsel(pt->session->evlist, sample->id);
	if (evsel != pt->switch_evsel)
		return 0;

	tid = perf_evsel__intval(evsel, sample, "next_pid");
	cpu = sample->cpu;

	return machine__set_current_tid(pt->machine, cpu, 0, tid);
}

static int intel_pt_process_event(struct perf_session *session,
				  union perf_event *event,
				  struct perf_sample *sample,
				  struct perf_tool *tool)
{
	struct intel_pt *pt = container_of(session->itrace, struct intel_pt,
					   itrace);
	u64 timestamp;
	int err = 0;

	if (dump_trace)
		return 0;

	if (!tool->ordered_samples) {
		pr_err("Intel Processor Trace requires ordered samples\n");
		return -EINVAL;
	}

	if (sample->time)
		timestamp = perf_time_to_tsc(sample->time, &pt->tc);
	else
		timestamp = 0;

	if (timestamp || pt->timeless_decoding) {
		err = intel_pt_update_queues(pt);
		if (err)
			return err;
	}

	if (pt->timeless_decoding) {
		if (pt->sampling_mode) {
			if (sample->itrace_sample.size)
				err = intel_pt_process_timeless_sample(pt,
								sample, tool);
		} else if (event->header.type == PERF_RECORD_EXIT) {
			err = intel_pt_process_timeless_queues(pt,
					event->comm.tid, sample->time, tool);
		}
	} else if (timestamp) {
		if (pt->sampling_mode)
			err = intel_pt_process_sample_queues(pt, timestamp,
							event, sample, tool);
		else
			err = intel_pt_process_queues(pt, timestamp, tool);
	}
	if (err)
		return err;

	if (event->header.type == PERF_RECORD_ITRACE_LOST &&
	    pt->synth_opts.errors)
		err = intel_pt_lost(pt, sample, tool);

	if (pt->switch_evsel && event->header.type == PERF_RECORD_SAMPLE)
		err = intel_pt_process_switch(pt, sample);

	return err;
}

static int intel_pt_flush(struct perf_session *session, struct perf_tool *tool)
{
	struct intel_pt *pt = container_of(session->itrace, struct intel_pt,
					   itrace);
	int ret;

	if (dump_trace)
		return 0;

	if (!tool->ordered_samples)
		return -EINVAL;

	ret = intel_pt_update_queues(pt);
	if (ret < 0)
		return ret;

	if (pt->timeless_decoding)
		return intel_pt_process_timeless_queues(pt, -1,
						MAX_TIMESTAMP - 1, tool);

	return intel_pt_process_queues(pt, MAX_TIMESTAMP, tool);
}

static void intel_pt_free_events(struct perf_session *session)
{
	struct intel_pt *pt = container_of(session->itrace, struct intel_pt,
					   itrace);
	struct itrace_queues *queues = &pt->queues;
	unsigned int i;

	for (i = 0; i < queues->nr_queues; i++) {
		intel_pt_free_queue(queues->queue_array[i].priv);
		queues->queue_array[i].priv = NULL;
	}
	itrace_queues__free(queues);
}

static void intel_pt_free(struct perf_session *session)
{
	struct intel_pt *pt = container_of(session->itrace, struct intel_pt,
					   itrace);

	itrace_heap__free(&pt->heap);
	intel_pt_free_events(session);
	session->itrace = NULL;
	free(pt);
}

static int intel_pt_process_itrace_event(struct perf_session *session,
					 union perf_event *event,
					 struct perf_tool *tool __maybe_unused)
{
	struct intel_pt *pt = container_of(session->itrace, struct intel_pt,
					   itrace);

	if (pt->sampling_mode)
		return 0;

	if (!pt->data_queued) {
		struct itrace_buffer *buffer;
		off_t data_offset;
		int fd = perf_data_file__fd(session->file);
		int err;

		if (perf_data_file__is_pipe(session->file)) {
			data_offset = 0;
		} else {
			data_offset = lseek(fd, 0, SEEK_CUR);
			if (data_offset == -1)
				return -errno;
		}

		err = itrace_queues__add_event(&pt->queues, session, event,
					       data_offset, &buffer);
		if (err)
			return err;

		/* Dump here now we have copied a piped trace out of the pipe */
		if (dump_trace) {
			if (itrace_buffer__get_data(buffer, fd)) {
				intel_pt_dump_event(pt, buffer->data,
						    buffer->size);
				itrace_buffer__put_data(buffer);
			}
		}
	}

	return 0;
}

static int intel_pt_queue_event(struct perf_session *session,
				union perf_event *event __maybe_unused,
				struct perf_sample *sample)
{
	struct intel_pt *pt = container_of(session->itrace, struct intel_pt,
					   itrace);
	unsigned int queue_nr;
	u64 timestamp;
	int err;

	if (!sample->itrace_sample.size)
		return 0;

	if (!pt->sampling_mode)
		return 0;

	if (sample->time)
		timestamp = perf_time_to_tsc(sample->time, &pt->tc);
	else
		timestamp = 0;

	err = itrace_queues__add_sample(&pt->queues, sample, session, &queue_nr,
					timestamp);
	if (err)
		return err;

	return intel_pt_fix_overlap(pt, queue_nr);
}

struct intel_pt_synth {
	struct perf_tool dummy_tool;
	struct perf_tool *tool;
	struct perf_session *session;
};

static int intel_pt_event_synth(struct perf_tool *tool,
				union perf_event *event,
				struct perf_sample *sample __maybe_unused,
				struct machine *machine __maybe_unused)
{
	struct intel_pt_synth *intel_pt_synth =
			container_of(tool, struct intel_pt_synth, dummy_tool);

	return perf_session__deliver_synth_event(intel_pt_synth->session, event,
						 NULL, intel_pt_synth->tool);
}

static int intel_pt_synth_event(struct perf_session *session,
				struct perf_tool *tool,
				struct perf_event_attr *attr, u64 id)
{
	struct intel_pt_synth intel_pt_synth;

	memset(&intel_pt_synth, 0, sizeof(struct intel_pt_synth));
	intel_pt_synth.tool = tool;
	intel_pt_synth.session = session;

	return perf_event__synthesize_attr(&intel_pt_synth.dummy_tool, attr, 1,
					   &id, intel_pt_event_synth);
}

static int intel_pt_synth_events(struct intel_pt *pt,
				 struct perf_session *session,
				 struct perf_tool *tool)
{
	struct perf_evlist *evlist = session->evlist;
	struct perf_evsel *evsel;
	struct perf_event_attr attr;
	bool found = false;
	u64 id;
	int err;

	list_for_each_entry(evsel, &evlist->entries, node) {
		if ((evsel->attr.type == pt->pmu_type ||
		     (evsel->attr.sample_type & PERF_SAMPLE_ITRACE)) &&
		    evsel->ids) {
			found = true;
			break;
		}
	}

	if (!found) {
		pr_err("%s: failed\n", __func__);
		return -EINVAL;
	}

	memset(&attr, 0, sizeof(struct perf_event_attr));
	attr.size = sizeof(struct perf_event_attr);
	attr.type = PERF_TYPE_HARDWARE;
	attr.sample_type = evsel->attr.sample_type & PERF_SAMPLE_MASK;
	attr.sample_type |= PERF_SAMPLE_IP | PERF_SAMPLE_TID |
			    PERF_SAMPLE_PERIOD;
	if (pt->timeless_decoding)
		attr.sample_type &= ~(u64)PERF_SAMPLE_TIME;
	else
		attr.sample_type |= PERF_SAMPLE_TIME;
	if (!pt->per_cpu_mmaps)
		attr.sample_type &= ~(u64)PERF_SAMPLE_CPU;
	attr.exclude_user = evsel->attr.exclude_user;
	attr.exclude_kernel = evsel->attr.exclude_kernel;
	attr.exclude_hv = evsel->attr.exclude_hv;
	attr.exclude_host = evsel->attr.exclude_host;
	attr.exclude_guest = evsel->attr.exclude_guest;
	attr.sample_id_all = evsel->attr.sample_id_all;
	attr.read_format = evsel->attr.read_format;

	id = evsel->id[0] + 1000000000;
	if (!id)
		id = 1;

	if (pt->synth_opts.instructions) {
		attr.config = PERF_COUNT_HW_INSTRUCTIONS;
		if (pt->synth_opts.period_type == PERF_ITRACE_PERIOD_NANOSECS)
			attr.sample_period =
				intel_pt_ns_to_ticks(pt, pt->synth_opts.period);
		else
			attr.sample_period = pt->synth_opts.period;
		pt->instructions_sample_period = attr.sample_period;
		pr_debug("Synthesizing 'instructions' event with id %" PRIu64 " sample type %#" PRIx64 "\n",
			 id, (u64)attr.sample_type);
		err = intel_pt_synth_event(session, tool, &attr, id);
		if (err) {
			pr_err("%s: failed to synthesize 'instructions' event type\n",
			       __func__);
			return err;
		}
		pt->sample_instructions = true;
		pt->instructions_sample_type = attr.sample_type;
		pt->instructions_id = id;
		/*
		 * We only use sample types from PERF_SAMPLE_MASK so we can use
		 * __perf_evsel__sample_size() here.
		 */
		pt->instructions_event_size = sizeof(struct sample_event) +
				__perf_evsel__sample_size(attr.sample_type);
		id += 1;
	}

	if (pt->synth_opts.branches) {
		attr.config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
		attr.sample_period = 1;
		attr.sample_type |= PERF_SAMPLE_ADDR;
		pr_debug("Synthesizing 'branches' event with id %" PRIu64 " sample type %#" PRIx64 "\n",
			 id, (u64)attr.sample_type);
		err = intel_pt_synth_event(session, tool, &attr, id);
		if (err) {
			pr_err("%s: failed to synthesize 'branches' event type\n",
			       __func__);
			return err;
		}
		pt->sample_branches = true;
		pt->branches_sample_type = attr.sample_type;
		pt->branches_id = id;
		/*
		 * We only use sample types from PERF_SAMPLE_MASK so we can use
		 * __perf_evsel__sample_size() here.
		 */
		pt->branches_event_size = sizeof(struct sample_event) +
				__perf_evsel__sample_size(attr.sample_type);
	}

	pt->synth_needs_swap = evsel->needs_swap;

	return 0;
}

static struct perf_evsel *intel_pt_find_sched_switch(struct perf_evlist *evlist)
{
	struct perf_evsel *evsel;

	list_for_each_entry_reverse(evsel, &evlist->entries, node) {
		const char *name = perf_evsel__name(evsel);

		if (!strcmp(name, "sched:sched_switch"))
			return evsel;
	}

	return NULL;
}

enum {
	INTEL_PT_PMU_TYPE,
	INTEL_PT_TIME_SHIFT,
	INTEL_PT_TIME_MULT,
	INTEL_PT_TIME_ZERO,
	INTEL_PT_CAP_USER_TIME_ZERO,
	INTEL_PT_TSC_BIT,
	INTEL_PT_NORETCOMP_BIT,
	INTEL_PT_HAVE_SCHED_SWITCH,
	INTEL_PT_SNAPSHOT_MODE,
	INTEL_PT_PER_CPU_MMAPS,
	INTEL_PT_ITRACE_PRIV_SIZE,
};

u64 intel_pt_itrace_info_priv[INTEL_PT_ITRACE_PRIV_SIZE];

int intel_pt_process_itrace_info(struct perf_tool *tool,
				 union perf_event *event,
				 struct perf_session *session)
{
	struct itrace_info_event *itrace_info = &event->itrace_info;
	size_t min_sz = sizeof(u64) * INTEL_PT_PER_CPU_MMAPS;
	struct intel_pt *pt;
	int err;

	if (itrace_info->header.size < sizeof(struct itrace_info_event) +
					min_sz)
		return -EINVAL;

	pt = zalloc(sizeof(struct intel_pt));
	if (!pt)
		return -ENOMEM;

	err = itrace_queues__init(&pt->queues);
	if (err)
		goto err_free;

	intel_pt_log_set_name(INTEL_PT_PMU_NAME);

	pt->session = session;
	pt->machine = &session->machines.host; /* No kvm support */
	pt->itrace_type = itrace_info->type;
	pt->pmu_type = itrace_info->priv[INTEL_PT_PMU_TYPE];
	pt->tc.time_shift = itrace_info->priv[INTEL_PT_TIME_SHIFT];
	pt->tc.time_mult = itrace_info->priv[INTEL_PT_TIME_MULT];
	pt->tc.time_zero = itrace_info->priv[INTEL_PT_TIME_ZERO];
	pt->cap_user_time_zero = itrace_info->priv[INTEL_PT_CAP_USER_TIME_ZERO];
	pt->tsc_bit = itrace_info->priv[INTEL_PT_TSC_BIT];
	pt->noretcomp_bit = itrace_info->priv[INTEL_PT_NORETCOMP_BIT];
	pt->have_sched_switch = itrace_info->priv[INTEL_PT_HAVE_SCHED_SWITCH];
	pt->snapshot_mode = itrace_info->priv[INTEL_PT_SNAPSHOT_MODE];
	pt->per_cpu_mmaps = itrace_info->priv[INTEL_PT_PER_CPU_MMAPS];

	pt->timeless_decoding = intel_pt_timeless_decoding(pt);
	pt->have_tsc = intel_pt_have_tsc(pt);
	pt->sampling_mode = intel_pt_sampling_mode(pt);
	pt->est_tsc = pt->per_cpu_mmaps && !pt->timeless_decoding;

	pt->itrace.process_event = intel_pt_process_event;
	pt->itrace.queue_event = intel_pt_queue_event;
	pt->itrace.process_itrace_event = intel_pt_process_itrace_event;
	pt->itrace.dump_itrace_sample = intel_pt_dump_sample;
	pt->itrace.flush_events = intel_pt_flush;
	pt->itrace.free_events = intel_pt_free_events;
	pt->itrace.free = intel_pt_free;
	session->itrace = &pt->itrace;

	if (dump_trace)
		return 0;

	if (pt->have_sched_switch == 1) {
		pt->switch_evsel = intel_pt_find_sched_switch(session->evlist);
		if (!pt->switch_evsel) {
			pr_err("%s: missing sched_switch event\n", __func__);
			goto err_free_queues;
		}
	}

	if (session->itrace_synth_opts && session->itrace_synth_opts->set)
		pt->synth_opts = *session->itrace_synth_opts;
	else
		itrace_synth_opts__set_default(&pt->synth_opts);

	err = intel_pt_synth_events(pt, session, tool);
	if (err)
		goto err_free_queues;

	err = itrace_queues__process_index(&pt->queues, session);
	if (err)
		goto err_free_queues;

	if (pt->queues.populated)
		pt->data_queued = true;

	if (pt->timeless_decoding)
		pr_debug2("Intel PT decoding without timestamps\n");

	return 0;

err_free_queues:
	itrace_queues__free(&pt->queues);
	session->itrace = NULL;
err_free:
	free(pt);
	return err;
}

static bool intel_pt_has_topa_multiple_entries(struct perf_pmu *intel_pt_pmu)
{
	unsigned int topa_multiple_entries;

	if (perf_pmu__scan_file(intel_pt_pmu,
				"caps/topa_multiple_entries", "%u",
				&topa_multiple_entries) == 1 &&
	    topa_multiple_entries)
		return true;

	return false;
}

static int intel_pt_parse_terms_with_default(struct list_head *formats,
					     const char *str,
					     u64 *itrace_config)
{
	struct list_head *terms;
	struct perf_event_attr attr = {0};
	int err;

	terms = malloc(sizeof(struct list_head));
	if (!terms)
		return -ENOMEM;

	INIT_LIST_HEAD(terms);

	err = parse_events_terms(terms, str);
	if (err)
		goto out_free;

	attr.itrace_config = *itrace_config;
	err = perf_pmu__config_terms(formats, &attr, terms, true);
	if (err)
		goto out_free;

	*itrace_config = attr.itrace_config;
out_free:
	parse_events__free_terms(terms);
	return err;
}

static int intel_pt_parse_terms(struct list_head *formats, const char *str,
				u64 *itrace_config)
{
	*itrace_config = 0;
	return intel_pt_parse_terms_with_default(formats, str, itrace_config);
}

static size_t intel_pt_psb_period(struct perf_pmu *intel_pt_pmu __maybe_unused,
				  struct perf_evlist *evlist __maybe_unused)
{
	return 256;
}

static u64 intel_pt_default_config(struct perf_pmu *intel_pt_pmu)
{
	u64 itrace_config;

	intel_pt_parse_terms(&intel_pt_pmu->format, "tsc", &itrace_config);
	return itrace_config;
}

static size_t intel_pt_sample_size(const char **str)
{
	char *endptr;
	unsigned long sample_size;

	sample_size = strtoul(*str, &endptr, 0);
	if (sample_size)
		*str = endptr;
	return sample_size;
}

static int intel_pt_parse_sample_options(struct itrace_record *itr,
					 struct record_opts *opts,
					 const char *str)
{
	struct intel_pt_recording *ptr =
			container_of(itr, struct intel_pt_recording, itr);
	struct perf_pmu *intel_pt_pmu = ptr->intel_pt_pmu;
	u64 *itrace_config = &opts->itrace_sample_config;
	int err;

	opts->itrace_sample_size = str ? intel_pt_sample_size(&str) : 0;
	if (opts->itrace_sample_size > INTEL_PT_MAX_SAMPLE_SIZE) {
		pr_err("Intel Processor Trace: sample size too big\n");
		return -1;
	}

	*itrace_config = intel_pt_default_config(intel_pt_pmu);
	opts->itrace_sample_type = intel_pt_pmu->type;
	opts->sample_itrace = true;

	if (!str || !*str)
		return 0;

	err = intel_pt_parse_terms_with_default(&intel_pt_pmu->format, str,
						itrace_config);
	if (err)
		goto bad_options;

	return 0;

bad_options:
	pr_err("Intel Processor Trace: bad sampling options \"%s\"\n", str);
	return -1;
}

static int intel_pt_parse_snapshot_options(struct itrace_record *itr,
					   struct record_opts *opts,
					   const char *str)
{
	struct intel_pt_recording *ptr =
			container_of(itr, struct intel_pt_recording, itr);
	unsigned long long snapshot_size = 0;
	char *endptr;

	if (str) {
		snapshot_size = strtoull(str, &endptr, 0);
		if (*endptr || snapshot_size > SIZE_MAX)
			return -1;
	}

	opts->itrace_snapshot_mode = true;
	opts->itrace_snapshot_size = snapshot_size;

	ptr->snapshot_size = snapshot_size;

	return 0;
}

struct perf_event_attr *
intel_pt_pmu_default_config(struct perf_pmu *intel_pt_pmu)
{
	struct perf_event_attr *attr;

	attr = zalloc(sizeof(struct perf_event_attr));
	if (!attr)
		return NULL;

	attr->itrace_config = intel_pt_default_config(intel_pt_pmu);

	intel_pt_pmu->selectable = true;

	return attr;
}

static size_t intel_pt_info_priv_size(struct itrace_record *itr __maybe_unused)
{
	return sizeof(intel_pt_itrace_info_priv);
}

static int intel_pt_info_fill(struct itrace_record *itr,
			      struct perf_session *session,
			      struct itrace_info_event *itrace_info,
			      size_t priv_size)
{
	struct intel_pt_recording *ptr =
			container_of(itr, struct intel_pt_recording, itr);
	struct perf_pmu *intel_pt_pmu = ptr->intel_pt_pmu;
	struct perf_event_mmap_page *pc;
	struct perf_tsc_conversion tc;
	bool cap_user_time_zero, per_cpu_mmaps;
	u64 tsc_bit, noretcomp_bit;
	int err;

	if (priv_size != sizeof(intel_pt_itrace_info_priv))
		return -EINVAL;

	intel_pt_parse_terms(&intel_pt_pmu->format, "tsc", &tsc_bit);
	intel_pt_parse_terms(&intel_pt_pmu->format, "noretcomp",
			     &noretcomp_bit);

	if (!session->evlist->nr_mmaps)
		return -EINVAL;

	pc = session->evlist->mmap[0].base;
	err = perf_read_tsc_conversion(pc, &tc);
	if (err) {
		if (err != -EOPNOTSUPP)
			return err;
		cap_user_time_zero = false;
	} else {
		cap_user_time_zero = tc.time_mult != 0;
	}

	if (!cap_user_time_zero)
		ui__warning("Intel Processor Trace: TSC not available\n");

	per_cpu_mmaps = !cpu_map__empty(session->evlist->cpus);

	itrace_info->type = PERF_ITRACE_INTEL_PT;
	itrace_info->priv[INTEL_PT_PMU_TYPE] = intel_pt_pmu->type;
	itrace_info->priv[INTEL_PT_TIME_SHIFT] = tc.time_shift;
	itrace_info->priv[INTEL_PT_TIME_MULT] = tc.time_mult;
	itrace_info->priv[INTEL_PT_TIME_ZERO] = tc.time_zero;
	itrace_info->priv[INTEL_PT_CAP_USER_TIME_ZERO] = cap_user_time_zero;
	itrace_info->priv[INTEL_PT_TSC_BIT] = tsc_bit;
	itrace_info->priv[INTEL_PT_NORETCOMP_BIT] = noretcomp_bit;
	itrace_info->priv[INTEL_PT_HAVE_SCHED_SWITCH] = ptr->have_sched_switch;
	itrace_info->priv[INTEL_PT_SNAPSHOT_MODE] = ptr->snapshot_mode;
	itrace_info->priv[INTEL_PT_PER_CPU_MMAPS] = per_cpu_mmaps;

	return 0;
}

static size_t intel_pt_to_power_of_2(size_t x, size_t min)
{
	size_t y = x;
	int i;

	if (!x)
		return min;
	for (i = 0; y != 1; i++)
		y >>= 1;
	y <<= i;
	if (x & (y - 1))
		y <<= 1;
	if (y < min)
		return min;
	return y;
}

static int intel_pt_track_switches(struct perf_evlist *evlist)
{
	const char *sched_switch = "sched:sched_switch";
	struct perf_evsel *evsel;
	int err;

	if (!perf_evlist__can_select_event(evlist, sched_switch))
		return -EPERM;

	err = parse_events(evlist, sched_switch);
	if (err) {
		pr_debug2("%s: failed to parse %s, error %d\n",
			  __func__, sched_switch, err);
		return err;
	}

	evsel = perf_evlist__last(evlist);

	perf_evsel__set_sample_bit(evsel, CPU);

	evsel->system_wide = true;
	evsel->no_aux_samples = true;
	evsel->immediate = true;

	return 0;
}

static int intel_pt_recording_options(struct itrace_record *itr,
				      struct perf_evlist *evlist,
				      struct record_opts *opts)
{
	struct intel_pt_recording *ptr =
			container_of(itr, struct intel_pt_recording, itr);
	struct perf_pmu *intel_pt_pmu = ptr->intel_pt_pmu;
	bool have_timing_info, topa_multiple_entries;
	struct perf_evsel *evsel, *intel_pt_evsel = NULL;
	const struct cpu_map *cpus = evlist->cpus;
	bool privileged = geteuid() == 0 || perf_event_paranoid() < 0;
	u64 tsc_bit;

	ptr->evlist = evlist;
	ptr->snapshot_mode = opts->itrace_snapshot_mode;

	list_for_each_entry(evsel, &evlist->entries, node) {
		if (evsel->attr.type == intel_pt_pmu->type) {
			if (intel_pt_evsel) {
				pr_err("There may be only one " INTEL_PT_PMU_NAME " event\n");
				return -EINVAL;
			}
			evsel->attr.freq = 0;
			evsel->attr.sample_period = 1;
			intel_pt_evsel = evsel;
			opts->full_itrace = true;
		}
	}

	if (opts->itrace_snapshot_mode && !opts->full_itrace) {
		pr_err("Snapshot mode (-S option) requires " INTEL_PT_PMU_NAME " PMU event (-e " INTEL_PT_PMU_NAME ")\n");
		return -EINVAL;
	}

	if (!opts->full_itrace && !opts->sample_itrace)
		return 0;

	if (opts->full_itrace && opts->sample_itrace) {
		pr_err("Full trace (" INTEL_PT_PMU_NAME " PMU) and sample trace (-I option) cannot be used together\n");
		return -EINVAL;
	}

	/* Set default size for sample mode */
	if (opts->sample_itrace) {
		size_t psb_period = intel_pt_psb_period(intel_pt_pmu, evlist);

		if (!opts->itrace_sample_size)
			opts->itrace_sample_size = INTEL_PT_DEFAULT_SAMPLE_SIZE;
		pr_debug2("Intel PT sample size: %zu\n",
			  opts->itrace_sample_size);
		if (psb_period &&
		    opts->itrace_sample_size <= psb_period +
						INTEL_PT_PSB_PERIOD_NEAR)
			ui__warning("Intel PT sample size (%zu) may be too small for PSB period (%zu)\n",
				    opts->itrace_sample_size, psb_period);
	}

	topa_multiple_entries =
			intel_pt_has_topa_multiple_entries(intel_pt_pmu);

	/* Set default sizes for snapshot mode */
	if (opts->itrace_snapshot_mode) {
		size_t psb_period = intel_pt_psb_period(intel_pt_pmu, evlist);

		if (!opts->itrace_snapshot_size && !opts->itrace_mmap_pages) {
			if (privileged) {
				opts->itrace_mmap_pages = MiB(4) / page_size;
			} else {
				opts->itrace_mmap_pages = KiB(128) / page_size;
				if (opts->mmap_pages == UINT_MAX)
					opts->mmap_pages = KiB(256) / page_size;
			}
		} else if (!opts->itrace_mmap_pages && !privileged &&
			   opts->mmap_pages == UINT_MAX) {
			opts->mmap_pages = KiB(256) / page_size;
		}
		if (!opts->itrace_snapshot_size)
			opts->itrace_snapshot_size =
					opts->itrace_mmap_pages * page_size;
		if (!opts->itrace_mmap_pages) {
			size_t sz = opts->itrace_snapshot_size;

			if (topa_multiple_entries)
				sz += page_size - 1;
			else if (sz <= MiB(128))
				sz = intel_pt_to_power_of_2(sz, 4096);
			else
				sz = roundup(sz, MiB(128));
			opts->itrace_mmap_pages = sz / page_size;
		}
		if (opts->itrace_snapshot_size >
					opts->itrace_mmap_pages * page_size) {
			pr_err("Snapshot size %zu must not be greater than instruction tracing mmap size %zu\n",
			       opts->itrace_snapshot_size,
			       opts->itrace_mmap_pages * (size_t)page_size);
			return -EINVAL;
		}
		if (!opts->itrace_snapshot_size || !opts->itrace_mmap_pages) {
			pr_err("Failed to calculate default snapshot size and/or instruction tracing mmap pages\n");
			return -EINVAL;
		}
		pr_debug2("Intel PT snapshot size: %zu\n",
			  opts->itrace_snapshot_size);
		if (psb_period &&
		    opts->itrace_snapshot_size <= psb_period +
						  INTEL_PT_PSB_PERIOD_NEAR)
			ui__warning("Intel PT snapshot size (%zu) may be too small for PSB period (%zu)\n",
				    opts->itrace_sample_size, psb_period);
	}

	/* Set default sizes for full trace mode */
	if (opts->full_itrace && !opts->itrace_mmap_pages) {
		if (privileged) {
			opts->itrace_mmap_pages = MiB(4) / page_size;
		} else {
			opts->itrace_mmap_pages = KiB(128) / page_size;
			if (opts->mmap_pages == UINT_MAX)
				opts->mmap_pages = KiB(256) / page_size;
		}
	}

	/* Validate itrace_mmap_pages */
	if (opts->itrace_mmap_pages && !topa_multiple_entries) {
		size_t sz = opts->itrace_mmap_pages * page_size;
		size_t min_sz;

		if (opts->itrace_snapshot_mode)
			min_sz = KiB(4);
		else
			min_sz = KiB(8);

		if (sz < min_sz ||
		    (!is_power_of_2(sz) && (sz & MiB_MASK(128)))) {
			pr_err("Invalid mmap size for Intel Processor Trace: must be at least %zuKiB and a power of 2 or a multiple of 128MiB\n",
			       min_sz / 1024);
			return -EINVAL;
		}
	}

	intel_pt_parse_terms(&intel_pt_pmu->format, "tsc", &tsc_bit);

	if ((opts->sample_itrace && (opts->itrace_sample_config & tsc_bit)) ||
	    (opts->full_itrace &&
		    (intel_pt_evsel->attr.itrace_config & tsc_bit)))
		have_timing_info = true;
	else
		have_timing_info = false;

	/*
	 * Per-cpu recording needs sched_switch events to distinguish different
	 * threads.
	 */
	if (have_timing_info && !cpu_map__empty(cpus)) {
		int err;

		err = intel_pt_track_switches(evlist);
		if (err == -EPERM)
			pr_debug2("Unable to select sched:sched_switch\n");
		else if (err)
			return err;
		else
			ptr->have_sched_switch = 1;
	}

	if (intel_pt_evsel) {
		/*
		 * To obtain the itrace buffer file descriptor, the itrace event
		 * must come first.
		 */
		perf_evlist__to_front(evlist, intel_pt_evsel);
		/*
		 * In the case of per-cpu mmaps, we need the CPU on the
		 * ITRACE_LOST event.
		 */
		if (!cpu_map__empty(cpus))
			perf_evsel__set_sample_bit(intel_pt_evsel, CPU);
	}

	/* Add dummy event to keep tracking */
	if (opts->full_itrace) {
		struct perf_evsel *tracking_evsel;
		int err;

		err = parse_events(evlist, "dummy:u");
		if (err)
			return err;

		tracking_evsel = perf_evlist__last(evlist);

		err = perf_evlist__set_tracking_event(evlist, tracking_evsel);
		if (err)
			return err;

		tracking_evsel->attr.freq = 0;
		tracking_evsel->attr.sample_period = 1;

		/* In per-cpu case, always need the time of mmap events etc */
		if (!cpu_map__empty(cpus))
			perf_evsel__set_sample_bit(tracking_evsel, TIME);
	}

	/*
	 * Warn the user when we do not have enough information to decode i.e.
	 * per-cpu with no sched_switch (except workload-only).
	 */
	if (!ptr->have_sched_switch && !opts->sample_itrace &&
	    !cpu_map__empty(cpus) && !target__none(&opts->target))
		ui__warning("Intel Processor Trace decoding will not be possible except for kernel tracing!\n");

	return 0;
}

static int intel_pt_snapshot_start(struct itrace_record *itr)
{
	struct intel_pt_recording *ptr =
			container_of(itr, struct intel_pt_recording, itr);
	struct perf_evsel *evsel;

	list_for_each_entry(evsel, &ptr->evlist->entries, node) {
		if (evsel->attr.type == ptr->intel_pt_pmu->type)
			return perf_evlist__disable_event(ptr->evlist, evsel);
	}
	return -EINVAL;
}

static int intel_pt_snapshot_finish(struct itrace_record *itr)
{
	struct intel_pt_recording *ptr =
			container_of(itr, struct intel_pt_recording, itr);
	struct perf_evsel *evsel;

	list_for_each_entry(evsel, &ptr->evlist->entries, node) {
		if (evsel->attr.type == ptr->intel_pt_pmu->type)
			return perf_evlist__enable_event(ptr->evlist, evsel);
	}
	return -EINVAL;
}

static int intel_pt_alloc_snapshot_refs(struct intel_pt_recording *ptr, int idx)
{
	const size_t sz = sizeof(struct intel_pt_snapshot_ref);
	int cnt = ptr->snapshot_ref_cnt, new_cnt = cnt * 2;
	struct intel_pt_snapshot_ref *refs;

	if (!new_cnt)
		new_cnt = 16;

	while (new_cnt <= idx)
		new_cnt *= 2;

	refs = calloc(new_cnt, sz);
	if (!refs)
		return -ENOMEM;

	memcpy(refs, ptr->snapshot_refs, cnt * sz);

	ptr->snapshot_refs = refs;
	ptr->snapshot_ref_cnt = new_cnt;

	return 0;
}

static void intel_pt_free_snapshot_refs(struct intel_pt_recording *ptr)
{
	int i;

	for (i = 0; i < ptr->snapshot_ref_cnt; i++)
		free(ptr->snapshot_refs[i].ref_buf);
	free(ptr->snapshot_refs);
}

static void intel_pt_recording_free(struct itrace_record *itr)
{
	struct intel_pt_recording *ptr =
			container_of(itr, struct intel_pt_recording, itr);

	intel_pt_free_snapshot_refs(ptr);
	free(ptr);
}

static int intel_pt_alloc_snapshot_ref(struct intel_pt_recording *ptr, int idx,
				       size_t snapshot_buf_size)
{
	size_t ref_buf_size = ptr->snapshot_ref_buf_size;
	void *ref_buf;

	ref_buf = zalloc(ref_buf_size);
	if (!ref_buf)
		return -ENOMEM;

	ptr->snapshot_refs[idx].ref_buf = ref_buf;
	ptr->snapshot_refs[idx].ref_offset = snapshot_buf_size - ref_buf_size;

	return 0;
}

static size_t intel_pt_snapshot_ref_buf_size(struct intel_pt_recording *ptr,
					     size_t snapshot_buf_size)
{
	const size_t max_size = 256 * 1024;
	size_t buf_size = 0, psb_period;

	if (ptr->snapshot_size <= 64 * 1024)
		return 0;

	psb_period = intel_pt_psb_period(ptr->intel_pt_pmu, ptr->evlist);
	if (psb_period)
		buf_size = psb_period * 2;

	if (!buf_size || buf_size > max_size)
		buf_size = max_size;

	if (buf_size >= snapshot_buf_size)
		return 0;

	if (buf_size >= ptr->snapshot_size / 2)
		return 0;

	return buf_size;
}

static int intel_pt_snapshot_init(struct intel_pt_recording *ptr,
				  size_t snapshot_buf_size)
{
	if (ptr->snapshot_init_done)
		return 0;

	ptr->snapshot_init_done = true;

	ptr->snapshot_ref_buf_size = intel_pt_snapshot_ref_buf_size(ptr,
							snapshot_buf_size);

	return 0;
}

/**
 * intel_pt_compare_buffers - compare bytes in a buffer to a circular buffer.
 * @buf1: first buffer
 * @compare_size: number of bytes to compare
 * @buf2: second buffer (a circular buffer)
 * @offs2: offset in second buffer
 * @buf2_size: size of second buffer
 *
 * The comparison allows for the possibility that the bytes to compare in the
 * circular buffer are not contiguous.  It is assumed that @compare_size <=
 * @buf2_size.  This function returns %false if the bytes are identical, %true
 * otherwise.
 */
static bool intel_pt_compare_buffers(void *buf1, size_t compare_size,
				     void *buf2, size_t offs2, size_t buf2_size)
{
	size_t end2 = offs2 + compare_size, part_size;

	if (end2 <= buf2_size)
		return memcmp(buf1, buf2 + offs2, compare_size);

	part_size = end2 - buf2_size;
	if (memcmp(buf1, buf2 + offs2, part_size))
		return true;

	compare_size -= part_size;

	return memcmp(buf1 + part_size, buf2, compare_size);
}

static bool intel_pt_compare_ref(void *ref_buf, size_t ref_offset,
				 size_t ref_size, size_t buf_size,
				 void *data, size_t head)
{
	size_t ref_end = ref_offset + ref_size;

	if (ref_end > buf_size) {
		if (head > ref_offset || head < ref_end - buf_size)
			return true;
	} else if (head > ref_offset && head < ref_end) {
		return true;
	}

	return intel_pt_compare_buffers(ref_buf, ref_size, data, ref_offset,
					buf_size);
}

static void intel_pt_copy_ref(void *ref_buf, size_t ref_size, size_t buf_size,
			      void *data, size_t head)
{
	if (head >= ref_size) {
		memcpy(ref_buf, data + head - ref_size, ref_size);
	} else {
		memcpy(ref_buf, data, head);
		ref_size -= head;
		memcpy(ref_buf + head, data + buf_size - ref_size, ref_size);
	}
}

static bool intel_pt_wrapped(struct intel_pt_recording *ptr, int idx,
			     struct itrace_mmap *mm, unsigned char *data,
			     u64 head)
{
	struct intel_pt_snapshot_ref *ref = &ptr->snapshot_refs[idx];
	bool wrapped;

	wrapped = intel_pt_compare_ref(ref->ref_buf, ref->ref_offset,
				       ptr->snapshot_ref_buf_size, mm->len,
				       data, head);

	intel_pt_copy_ref(ref->ref_buf, ptr->snapshot_ref_buf_size, mm->len,
			  data, head);

	return wrapped;
}

static bool intel_pt_first_wrap(u64 *data, size_t buf_size)
{
	int i, a, b;

	b = buf_size >> 3;
	a = b - 512;
	if (a < 0)
		a = 0;

	for (i = a; i < b; i++) {
		if (data[i])
			return true;
	}

	return false;
}

static int intel_pt_find_snapshot(struct itrace_record *itr, int idx,
				  struct itrace_mmap *mm, unsigned char *data,
				  u64 *head, u64 *old)
{
	struct intel_pt_recording *ptr =
			container_of(itr, struct intel_pt_recording, itr);
	bool wrapped;
	int err;

	pr_debug3("%s: mmap index %d old head %zu new head %zu\n",
		  __func__, idx, (size_t)*old, (size_t)*head);

	err = intel_pt_snapshot_init(ptr, mm->len);
	if (err)
		goto out_err;

	if (idx >= ptr->snapshot_ref_cnt) {
		err = intel_pt_alloc_snapshot_refs(ptr, idx);
		if (err)
			goto out_err;
	}

	if (ptr->snapshot_ref_buf_size) {
		if (!ptr->snapshot_refs[idx].ref_buf) {
			err = intel_pt_alloc_snapshot_ref(ptr, idx, mm->len);
			if (err)
				goto out_err;
		}
		wrapped = intel_pt_wrapped(ptr, idx, mm, data, *head);
	} else {
		wrapped = ptr->snapshot_refs[idx].wrapped;
		if (!wrapped && intel_pt_first_wrap((u64 *)data, mm->len)) {
			ptr->snapshot_refs[idx].wrapped = true;
			wrapped = true;
		}
	}

	/*
	 * In full trace mode 'head' continually increases.  However in snapshot
	 * mode 'head' is an offset within the buffer.  Here 'old' and 'head'
	 * are adjusted to match the full trace case which expects that 'old' is
	 * always less than 'head'.
	 */
	if (wrapped) {
		*old = *head;
		*head += mm->len;
	} else {
		if (mm->mask)
			*old &= mm->mask;
		else
			*old %= mm->len;
		if (*old > *head)
			*head += mm->len;
	}

	pr_debug3("%s: wrap-around %sdetected, adjusted old head %zu adjusted new head %zu\n",
		  __func__, wrapped ? "" : "not ", (size_t)*old, (size_t)*head);

	return 0;

out_err:
	pr_err("%s: failed, error %d\n", __func__, err);
	return err;
}

static u64 intel_pt_reference(struct itrace_record *itr __maybe_unused)
{
	return rdtsc();
}

static int intel_pt_read_finish(struct itrace_record *itr, int idx)
{
	struct intel_pt_recording *ptr =
			container_of(itr, struct intel_pt_recording, itr);
	struct perf_evsel *evsel;

	list_for_each_entry(evsel, &ptr->evlist->entries, node) {
		if (evsel->attr.type == ptr->intel_pt_pmu->type)
			return perf_evlist__enable_event_idx(ptr->evlist, evsel,
							     idx);
	}
	return -EINVAL;
}

struct itrace_record *intel_pt_recording_init(int *err)
{
	struct perf_pmu *intel_pt_pmu = perf_pmu__find(INTEL_PT_PMU_NAME);
	struct intel_pt_recording *ptr;

	if (!intel_pt_pmu)
		return NULL;

	ptr = zalloc(sizeof(struct intel_pt_recording));
	if (!ptr) {
		*err = -ENOMEM;
		return NULL;
	}

	ptr->intel_pt_pmu = intel_pt_pmu;
	ptr->itr.parse_sample_options = intel_pt_parse_sample_options;
	ptr->itr.recording_options = intel_pt_recording_options;
	ptr->itr.info_priv_size = intel_pt_info_priv_size;
	ptr->itr.info_fill = intel_pt_info_fill;
	ptr->itr.free = intel_pt_recording_free;
	ptr->itr.snapshot_start = intel_pt_snapshot_start;
	ptr->itr.snapshot_finish = intel_pt_snapshot_finish;
	ptr->itr.find_snapshot = intel_pt_find_snapshot;
	ptr->itr.parse_snapshot_options = intel_pt_parse_snapshot_options;
	ptr->itr.reference = intel_pt_reference;
	ptr->itr.read_finish = intel_pt_read_finish;
	return &ptr->itr;
}
