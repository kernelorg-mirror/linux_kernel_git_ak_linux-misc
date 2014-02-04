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

#undef DEBUG

#include <linux/bitops.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/device.h>

#include <asm-generic/sizes.h>
#include <asm/perf_event.h>
#include <asm/insn.h>

#include "perf_event.h"
#include "intel_pt.h"

static DEFINE_PER_CPU(struct pt, pt_ctx);

static struct pt_pmu pt_pmu;

enum cpuid_regs {
	CR_EAX = 0,
	CR_ECX,
	CR_EDX,
	CR_EBX
};

/*
 * Capabilities of Intel PT hardware, such as number of address bits or
 * supported output schemes, are cached and exported to userspace as "caps"
 * attribute group of pt pmu device
 * (/sys/bus/event_source/devices/intel_pt/caps/) so that userspace can store
 * relevant bits together with intel_pt traces.
 *
 * Currently, for debugging purposes, these attributes are also writable; this
 * should be removed in the final version.
 */
#define PT_CAP(_n, _l, _r, _m)						\
	[PT_CAP_ ## _n] = { .name = __stringify(_n), .leaf = _l,	\
			    .reg = _r, .mask = _m }

static struct pt_cap_desc {
	const char	*name;
	u32		leaf;
	u8		reg;
	u32		mask;
} pt_caps[] = {
	PT_CAP(max_subleaf,		0, CR_EAX, 0xffffffff),
	PT_CAP(cr3_filtering,		0, CR_EBX, BIT(0)),
	PT_CAP(topa_output,		0, CR_ECX, BIT(0)),
	PT_CAP(topa_multiple_entries,	0, CR_ECX, BIT(1)),
	PT_CAP(payloads_lip,		0, CR_ECX, BIT(31)),
};

static u32 pt_cap_get(enum pt_capabilities cap)
{
	struct pt_cap_desc *cd = &pt_caps[cap];
	u32 c = pt_pmu.caps[cd->leaf * 4 + cd->reg];
	unsigned int shift = __ffs(cd->mask);

	return (c & cd->mask) >> shift;
}

static void pt_cap_set(enum pt_capabilities cap, u32 val)
{
	struct pt_cap_desc *cd = &pt_caps[cap];
	unsigned int idx = cd->leaf * 4 + cd->reg;
	unsigned int shift = __ffs(cd->mask);

	pt_pmu.caps[idx] = (val << shift) & cd->mask;
}

static ssize_t pt_cap_show(struct device *cdev,
			   struct device_attribute *attr,
			   char *buf)
{
	struct dev_ext_attribute *ea =
		container_of(attr, struct dev_ext_attribute, attr);
	enum pt_capabilities cap = (long)ea->var;

	return snprintf(buf, PAGE_SIZE, "%x\n", pt_cap_get(cap));
}

static ssize_t pt_cap_store(struct device *cdev,
			    struct device_attribute *attr,
			    const char *buf, size_t size)
{
	struct dev_ext_attribute *ea =
		container_of(attr, struct dev_ext_attribute, attr);
	enum pt_capabilities cap = (long)ea->var;
	unsigned long new;
	char *end;

	new = simple_strtoul(buf, &end, 0);
	if (end == buf)
		return -EINVAL;

	pt_cap_set(cap, new);
	return size;
}

static struct attribute_group pt_cap_group = {
	.name	= "caps",
};

PMU_FORMAT_ATTR(tsc,		"itrace_config:10"	);
PMU_FORMAT_ATTR(noretcomp,	"itrace_config:11"	);

static struct attribute *pt_formats_attr[] = {
	&format_attr_tsc.attr,
	&format_attr_noretcomp.attr,
	NULL,
};

static struct attribute_group pt_format_group = {
	.name	= "format",
	.attrs	= pt_formats_attr,
};

static const struct attribute_group *pt_attr_groups[] = {
	&pt_cap_group,
	&pt_format_group,
	NULL,
};

static int __init pt_pmu_hw_init(void)
{
	struct dev_ext_attribute *de_attrs;
	struct attribute **attrs;
	size_t size;
	long i;

	if (test_cpu_cap(&boot_cpu_data, X86_FEATURE_INTEL_PT)) {
		for (i = 0; i < PT_CPUID_LEAVES; i++)
			cpuid_count(20, i,
				    &pt_pmu.caps[CR_EAX + i * 4],
				    &pt_pmu.caps[CR_EBX + i * 4],
				    &pt_pmu.caps[CR_ECX + i * 4],
				    &pt_pmu.caps[CR_EDX + i * 4]);
	} else
		return -ENODEV;

	size = sizeof(struct attribute *) * (ARRAY_SIZE(pt_caps) + 1);
	attrs = kzalloc(size, GFP_KERNEL);
	if (!attrs)
		goto err_attrs;

	size = sizeof(struct dev_ext_attribute) * (ARRAY_SIZE(pt_caps) + 1);
	de_attrs = kzalloc(size, GFP_KERNEL);
	if (!de_attrs)
		goto err_de_attrs;

	for (i = 0; i < ARRAY_SIZE(pt_caps); i++) {
		de_attrs[i].attr.attr.name = pt_caps[i].name;

		sysfs_attr_init(&de_attrs[i].attr.attr);
		de_attrs[i].attr.attr.mode = S_IRUGO | S_IWUSR;
		de_attrs[i].attr.show = pt_cap_show;
		de_attrs[i].attr.store = pt_cap_store;
		de_attrs[i].var = (void *)i;
		attrs[i] = &de_attrs[i].attr.attr;
	}

	pt_cap_group.attrs = attrs;
	return 0;

err_de_attrs:
	kfree(de_attrs);
err_attrs:
	kfree(attrs);

	return -ENOMEM;
}

#define PT_CONFIG_MASK (RTIT_CTL_TSC_EN | RTIT_CTL_DISRETC)

static bool pt_event_valid(struct perf_event *event)
{
	u64 itrace_config = event->attr.itrace_config;

	if ((itrace_config & PT_CONFIG_MASK) != itrace_config)
		return false;

	return true;
}

/*
 * PT configuration helpers
 * These all are cpu affine and operate on a local PT
 */

static int pt_config(struct perf_event *event)
{
	u64 reg;

	reg = RTIT_CTL_TOPA | RTIT_CTL_BRANCH_EN;

	if (!event->attr.exclude_kernel)
		reg |= RTIT_CTL_OS;
	if (!event->attr.exclude_user)
		reg |= RTIT_CTL_USR;

	reg |= (event->attr.itrace_config & PT_CONFIG_MASK);

	if (wrmsr_safe(MSR_IA32_RTIT_CTL, reg, 0) < 0) {
		pr_warn("Failed to enable PT on cpu %d\n", event->cpu);
		return -EINVAL;
	}

	return 0;
}

static void pt_config_start(bool start)
{
	u64 ctl;

	rdmsrl(MSR_IA32_RTIT_CTL, ctl);
	if (start)
		ctl |= RTIT_CTL_TRACEEN;
	else
		ctl &= ~RTIT_CTL_TRACEEN;
	wrmsrl(MSR_IA32_RTIT_CTL, ctl);
}

static void pt_config_buffer(void *buf, unsigned int topa_idx,
			     unsigned int output_off)
{
	u64 reg;

	wrmsrl(MSR_IA32_RTIT_OUTPUT_BASE, virt_to_phys(buf));

	reg = 0x7f | ((u64)topa_idx << 7) | ((u64)output_off << 32);

	wrmsrl(MSR_IA32_RTIT_OUTPUT_MASK, reg);
}

#define TENTS_PER_PAGE (((PAGE_SIZE - 40) / sizeof(struct topa_entry)) - 1)

struct topa {
	struct topa_entry	table[TENTS_PER_PAGE];
	struct list_head	list;
	u64			phys;
	u64			offset;
	size_t			size;
	int			last;
};

/* make negative table index stand for the last table entry */
#define TOPA_ENTRY(t, i) ((i) == -1 ? &(t)->table[(t)->last] : &(t)->table[(i)])

/*
 * allocate page-sized ToPA table
 */
static struct topa *topa_alloc(int cpu, gfp_t gfp)
{
	int node = cpu_to_node(cpu);
	struct topa *topa;
	struct page *p;

	p = alloc_pages_node(node, gfp | __GFP_ZERO, 0);
	if (!p)
		return NULL;

	topa = page_address(p);
	topa->last = 0;
	topa->phys = page_to_phys(p);

	/*
	 * In case of singe-entry ToPA, always put the self-referencing END
	 * link as the 2nd entry in the table
	 */
	if (!pt_cap_get(PT_CAP_topa_multiple_entries)) {
		TOPA_ENTRY(topa, 1)->base = topa->phys >> TOPA_SHIFT;
		TOPA_ENTRY(topa, 1)->end = 1;
	}

	return topa;
}

static void topa_free(struct topa *topa)
{
	free_page((unsigned long)topa);
}

static void topa_free_pages(struct pt_buffer *buf, struct topa *topa, int idx)
{
	size_t size = sizes(TOPA_ENTRY(topa, idx)->size);
	void *base = phys_to_virt(TOPA_ENTRY(topa, idx)->base << TOPA_SHIFT);
	unsigned long pn;

	for (pn = 0; pn < size; pn += PAGE_SIZE) {
		struct page *page = virt_to_page(base + pn);

		page->mapping = NULL;
		__free_page(page);
	}
}

/**
 * topa_insert_table - insert a ToPA table into a buffer
 * @buf - pt buffer that's being extended
 * @topa - new topa table to be inserted
 *
 * If it's the first table in this buffer, set up buffer's pointers
 * accordingly; otherwise, add a END=1 link entry to @topa to the current
 * "last" table and adjust the last table pointer to @topa.
 */
static void topa_insert_table(struct pt_buffer *buf, struct topa *topa)
{
	struct topa *last = buf->last;

	list_add_tail(&topa->list, &buf->tables);

	if (!buf->first) {
		buf->first = buf->last = buf->cur = topa;
		return;
	}

	topa->offset = last->offset + last->size;
	buf->last = topa;

	if (!pt_cap_get(PT_CAP_topa_multiple_entries))
		return;

	BUG_ON(last->last != TENTS_PER_PAGE - 1);

	TOPA_ENTRY(last, -1)->base = topa->phys >> TOPA_SHIFT;
	TOPA_ENTRY(last, -1)->end = 1;
}

static bool topa_table_full(struct topa *topa)
{
	/* single-entry ToPA is a special case */
	if (!pt_cap_get(PT_CAP_topa_multiple_entries))
		return !!topa->last;

	return topa->last == TENTS_PER_PAGE - 1;
}

static bool pt_buffer_needs_watermark(struct pt_buffer *buf, unsigned long offset)
{
	if (buf->snapshot)
		return false;

	return !(offset % (buf->watermark << PAGE_SHIFT));
}

static int topa_insert_pages(struct pt_buffer *buf, gfp_t gfp,
			     enum topa_sz sz)
{
	struct topa *topa = buf->last;
	int node = cpu_to_node(buf->cpu);
	int order = get_order(sizes(sz));
	struct page *p;
	unsigned long pn;

	p = alloc_pages_node(node, gfp | GFP_USER | __GFP_ZERO | __GFP_NOWARN | __GFP_NORETRY, order);
	if (!p)
		return -ENOMEM;

	split_page(p, order);

	if (topa_table_full(topa)) {
		topa = topa_alloc(buf->cpu, gfp);

		if (!topa) {
			free_pages((unsigned long)page_address(p), order);
			return -ENOMEM;
		}

		topa_insert_table(buf, topa);
	}

	TOPA_ENTRY(topa, -1)->base = page_to_phys(p) >> TOPA_SHIFT;
	TOPA_ENTRY(topa, -1)->size = sz;
	if (!buf->snapshot && !pt_cap_get(PT_CAP_topa_multiple_entries)) {
		TOPA_ENTRY(topa, -1)->intr = 1;
		TOPA_ENTRY(topa, -1)->stop = 1;
	}
	if (pt_buffer_needs_watermark(buf, buf->size))
		TOPA_ENTRY(topa, -1)->intr = 1;

	topa->last++;
	topa->size += sizes(sz);
	for (pn = 0; pn < sizes(sz); pn += PAGE_SIZE, buf->size += PAGE_SIZE)
		buf->data_pages[buf->size >> PAGE_SHIFT] = page_address(p) + pn;

	return 0;
}

static void pt_topa_dump(struct pt_buffer *buf)
{
	struct topa *topa;

	list_for_each_entry(topa, &buf->tables, list) {
		int i;

		pr_debug("# table @%p (%p), off %llx size %lx\n", topa->table,
			 (void *)topa->phys, topa->offset, topa->size);
		for (i = 0; i < TENTS_PER_PAGE; i++) {
			pr_debug("# entry @%p (%lx sz %u %c%c%c) raw=%16llx\n",
				 &topa->table[i],
				 (unsigned long)topa->table[i].base << TOPA_SHIFT,
				 sizes(topa->table[i].size),
				 topa->table[i].end ?  'E' : ' ',
				 topa->table[i].intr ? 'I' : ' ',
				 topa->table[i].stop ? 'S' : ' ',
				 *(u64 *)&topa->table[i]);
			if ((pt_cap_get(PT_CAP_topa_multiple_entries) && topa->table[i].stop)
			    || topa->table[i].end)
				break;
		}
	}
}

/* advance to the next output region */
static void pt_buffer_advance(struct pt_buffer *buf)
{
	buf->output_off = 0;
	buf->cur_idx++;

	if (buf->cur_idx == buf->cur->last) {
		if (buf->cur == buf->last)
			buf->cur = buf->first;
		else
			buf->cur = list_entry(buf->cur->list.next, struct topa, list);
		buf->cur_idx = 0;
	}
}

static void pt_update_head(struct pt_buffer *buf)
{
	u64 topa_idx, base;

	/* offset of the first region in this table from the beginning of buf */
	base = buf->cur->offset + buf->output_off;

	/* offset of the current output region within this table */
	for (topa_idx = 0; topa_idx < buf->cur_idx; topa_idx++)
		base += sizes(buf->cur->table[topa_idx].size);

	/* data_head always increases when buffer pointer wraps */
	base += buf->size * buf->round;

	local64_set(&buf->head, base);
	if (!buf->user_page)
		return;

	buf->user_page->data_head = base;
	smp_wmb();
}

static void *pt_buffer_region(struct pt_buffer *buf)
{
	return phys_to_virt(buf->cur->table[buf->cur_idx].base << TOPA_SHIFT);
}

static size_t pt_buffer_region_size(struct pt_buffer *buf)
{
	return sizes(buf->cur->table[buf->cur_idx].size);
}

/**
 * pt_handle_status - take care of possible status conditions
 * @event: currently active PT event
 */
static void pt_handle_status(struct perf_event *event)
{
	struct pt_buffer *buf = itrace_priv(event);
	int advance = 0;
	u64 status;

	rdmsrl(MSR_IA32_RTIT_STATUS, status);

	if (status & RTIT_STATUS_ERROR) {
		pr_err("ToPA ERROR encountered, trying to recover\n");
		pt_topa_dump(buf);
		status &= ~RTIT_STATUS_ERROR;
		wrmsrl(MSR_IA32_RTIT_STATUS, status);
	}

	if (status & RTIT_STATUS_STOPPED) {
		status &= ~RTIT_STATUS_STOPPED;
		wrmsrl(MSR_IA32_RTIT_STATUS, status);

		/*
		 * On systems that only do single-entry ToPA, hitting STOP
		 * means we are already losing data; need to let the decoder
		 * know.
		 */
		if (!pt_cap_get(PT_CAP_topa_multiple_entries) ||
		    buf->output_off == sizes(TOPA_ENTRY(buf->cur, buf->cur_idx)->size)) {
			pt_update_head(buf);
			itrace_lost_data(event, local64_read(&buf->head));
			advance++;
		}
	}

	/*
	 * Also on single-entry ToPA implementations, interrupt will come
	 * before the output reaches its output region's boundary.
	 */
	if (!pt_cap_get(PT_CAP_topa_multiple_entries) && !buf->snapshot &&
	    pt_buffer_region_size(buf) - buf->output_off <= TOPA_PMI_MARGIN) {
		void *head = pt_buffer_region(buf);

		/* everything within this margin needs to be zeroed out */
		memset(head + buf->output_off, 0,
		       pt_buffer_region_size(buf) -
		       buf->output_off);
		advance++;
	}

	if (advance) {
		/* check if the pointer has wrapped */
		if (!buf->snapshot &&
		    buf->cur == buf->last &&
		    buf->cur_idx == buf->cur->last - 1)
			buf->round++;
		pt_buffer_advance(buf);
	}
}

static void pt_read_offset(struct pt_buffer *buf)
{
	u64 offset, base_topa;

	rdmsrl(MSR_IA32_RTIT_OUTPUT_BASE, base_topa);
	buf->cur = phys_to_virt(base_topa);

	rdmsrl(MSR_IA32_RTIT_OUTPUT_MASK, offset);
	/* offset within current output region */
	buf->output_off = offset >> 32;
	/* index of current output region within this table */
	buf->cur_idx = (offset & 0xffffff80) >> 7;
}

/**
 * pt_buffer_fini_topa() - deallocate ToPA structure of a buffer
 * @buf: pt buffer
 */
static void pt_buffer_fini_topa(struct pt_buffer *buf)
{
	struct topa *topa, *iter;

	list_for_each_entry_safe(topa, iter, &buf->tables, list) {
		int i;

		for (i = 0; i < topa->last; i++)
			topa_free_pages(buf, topa, i);

		list_del(&topa->list);
		topa_free(topa);
	}
}

/**
 * pt_get_topa_region_size - calculate one output region's size
 * @snapshot: if the counter is a snapshot counter
 * @size: overall requested allocation size
 * returns topa region size or error
 */
static int pt_get_topa_region_size(bool snapshot, size_t size)
{
	unsigned int factor = snapshot ? 1 : 2;

	if (pt_cap_get(PT_CAP_topa_multiple_entries))
		return TOPA_4K;

	if (size < SZ_4K * factor)
		return -EINVAL;

	if (!is_power_of_2(size))
		return -EINVAL;

	if (size >= SZ_128M)
		return TOPA_128MB;

	return get_order(size / factor);
}

/**
 * pt_buffer_init_topa() - initialize ToPA table for pt buffer
 * @buf: pt buffer
 * @size: total size of all regions within this ToPA
 * @gfp: allocation flags
 */
static int pt_buffer_init_topa(struct pt_buffer *buf, size_t size, gfp_t gfp)
{
	struct topa *topa;
	int err, region_size;

	topa = topa_alloc(buf->cpu, gfp);
	if (!topa)
		return -ENOMEM;

	topa_insert_table(buf, topa);

	region_size = pt_get_topa_region_size(buf->snapshot, size);
	if (region_size < 0) {
		pt_buffer_fini_topa(buf);
		return region_size;
	}

	while (region_size && get_order(sizes(region_size)) > MAX_ORDER)
		region_size--;

	/* fixup watermark in case of higher order allocations */
	if (buf->watermark < (sizes(region_size) >> PAGE_SHIFT))
		buf->watermark = sizes(region_size) >> PAGE_SHIFT;

	while (buf->size < size) {
		err = topa_insert_pages(buf, gfp, region_size);
		if (err) {
			if (region_size) {
				region_size--;
				continue;
			}
			pt_buffer_fini_topa(buf);
			return -ENOMEM;
		}
	}

	/* link last table to the first one, unless we're double buffering */
	if (pt_cap_get(PT_CAP_topa_multiple_entries)) {
		TOPA_ENTRY(buf->last, -1)->base = buf->first->phys >> TOPA_SHIFT;
		TOPA_ENTRY(buf->last, -1)->end = 1;
	}

	pt_topa_dump(buf);
	return 0;
}

/**
 * pt_buffer_alloc() - make a buffer for pt data
 * @cpu: cpu on which to allocate, -1 means current
 * @size: desired buffer size, should be multiple of pages
 * @watermark: place interrupt flags every @watermark pages, 0 == disable
 * @snapshot: if this is a snapshot counter
 * @gfp: allocation flags
 */
static struct pt_buffer *pt_buffer_alloc(int cpu, size_t size,
					 unsigned long watermark,
					 bool snapshot, gfp_t gfp,
					 void **pages)
{
	struct pt_buffer *buf;
	int node;
	int ret;

	if (!size || watermark << PAGE_SHIFT > size)
		return NULL;

	if (cpu == -1)
		cpu = raw_smp_processor_id();
	node = cpu_to_node(cpu);

	buf = kzalloc(sizeof(struct pt_buffer), gfp);
	if (!buf)
		return NULL;

	buf->cpu = cpu;
	buf->data_pages = pages;
	buf->snapshot = snapshot;
	buf->watermark = watermark;
	if (!buf->watermark)
		buf->watermark = (size / 2) >> PAGE_SHIFT;

	INIT_LIST_HEAD(&buf->tables);

	ret = pt_buffer_init_topa(buf, size, gfp);
	if (ret) {
		kfree(buf);
		return NULL;
	}

	return buf;
}

/**
 * pt_buffer_free() - dispose of pt buffer
 * @buf: pt buffer
 */
static void pt_buffer_itrace_free(void *data)
{
	struct pt_buffer *buf = data;

	pt_buffer_fini_topa(buf);
	if (buf->user_page) {
		struct page *up = virt_to_page(buf->user_page);

		up->mapping = NULL;
		__free_page(up);
	}

	kfree(buf);
}

static void *
pt_buffer_itrace_alloc(int cpu, int nr_pages, bool overwrite, void **pages,
		       struct perf_event_mmap_page **user_page)
{
	struct pt_buffer *buf;
	struct page *up = NULL;
	int node;

	if (user_page) {
		*user_page = NULL;
		node = (cpu == -1) ? cpu : cpu_to_node(cpu);
		up = alloc_pages_node(node, GFP_KERNEL | __GFP_ZERO, 0);
		if (!up)
			return NULL;
	}

	buf = pt_buffer_alloc(cpu, nr_pages << PAGE_SHIFT, 0, overwrite,
			      GFP_KERNEL, pages);
	if (user_page && buf) {
		buf->user_page = page_address(up);
		*user_page = page_address(up);
	} else if (up)
		__free_page(up);

	return buf;
}

/**
 * pt_buffer_get_page() - find n'th page in pt buffer
 * @buf: pt buffer
 * @idx: page index in the buffer
 */
static void *pt_buffer_get_page(struct pt_buffer *buf, unsigned long idx)
{
	return buf->data_pages[idx];
}

/**
 * pt_buffer_is_full - check if the buffer is full
 * @event: pt event
 * If the user hasn't read data from the output region that data_head
 * points to, the buffer is considered full: the user needs to read at
 * least this region and update data_tail to point past it.
 */
static bool pt_buffer_is_full(struct pt_buffer *buf)
{
	void *tail, *head;
	unsigned long tailoff, headoff = local64_read(&buf->head);

	if (buf->snapshot)
		return false;

	tailoff = ACCESS_ONCE(buf->user_page->data_tail);
	smp_mb();

	if (headoff < tailoff || headoff - tailoff < buf->size / 2)
		return false;

	tailoff %= buf->size;
	headoff %= buf->size;

	if (headoff > tailoff)
		return false;

	/* check if head and tail are in the same output region */
	tail = pt_buffer_get_page(buf, tailoff >> PAGE_SHIFT);
	head = pt_buffer_region(buf);

	if (tail >= head && tail < head + pt_buffer_region_size(buf))
		return true;

	return false;
}

static void pt_wake_up(struct perf_event *event)
{
	struct pt_buffer *buf = itrace_priv(event);

	if (!buf || buf->snapshot)
		return;
	if (pt_buffer_is_full(buf)) {
		event->pending_disable = 1;
		event->pending_kill = POLL_IN;
		event->pending_wakeup = 1;
		event->hw.state = PERF_HES_STOPPED;
	}

	if (pt_buffer_needs_watermark(buf, local64_read(&buf->head))) {
		event->pending_wakeup = 1;
		event->pending_kill = POLL_IN;
	}

	if (event->pending_disable || event->pending_kill)
		itrace_wake_up(event);
}

void intel_pt_interrupt(void)
{
	struct pt *pt = this_cpu_ptr(&pt_ctx);
	struct perf_event *event = pt->event;
	struct pt_buffer *buf;

	pt_config_start(false);

	if (!event)
		return;

	buf = itrace_event_get_priv(event);
	if (!buf)
		return;

	pt_read_offset(buf);

	pt_handle_status(event);

	pt_update_head(buf);

	pt_wake_up(event);

	if (!event->hw.state) {
		pt_config(event);
		pt_config_buffer(buf->cur->table, buf->cur_idx,
				 buf->output_off);
		wrmsrl(MSR_IA32_RTIT_STATUS, 0);
		pt_config_start(true);
	}

	itrace_event_put(event);
}

static void pt_event_start(struct perf_event *event, int flags)
{
	struct pt_buffer *buf = itrace_priv(event);

	if (!buf || pt_buffer_is_full(buf) || pt_config(event)) {
		event->hw.state = PERF_HES_STOPPED;
		return;
	}

	event->hw.state = 0;

	pt_config_buffer(buf->cur->table, buf->cur_idx,
			 buf->output_off);
	wrmsrl(MSR_IA32_RTIT_STATUS, 0);
	pt_config_start(true);
}

static void pt_event_stop(struct perf_event *event, int flags)
{
	if (event->hw.state == PERF_HES_STOPPED)
		return;

	event->hw.state = PERF_HES_STOPPED;

	pt_config_start(false);

	if (flags & PERF_EF_UPDATE) {
		struct pt_buffer *buf = itrace_priv(event);

		if (WARN_ONCE(!buf, "no buffer\n"))
			return;

		pt_read_offset(buf);

		pt_handle_status(event);

		pt_update_head(buf);

		pt_wake_up(event);
	}
}

static void pt_event_del(struct perf_event *event, int flags)
{
	struct pt *pt = this_cpu_ptr(&pt_ctx);

	pt_event_stop(event, PERF_EF_UPDATE);

	raw_spin_lock(&pt->lock);
	pt->event = NULL;
	raw_spin_unlock(&pt->lock);

	itrace_event_put(event);
}

static int pt_event_add(struct perf_event *event, int flags)
{
	struct pt_buffer *buf;
	struct pt *pt = this_cpu_ptr(&pt_ctx);
	struct hw_perf_event *hwc = &event->hw;
	int ret = 0;

	ret = pt_config(event);
	if (ret)
		return ret;

	buf = itrace_event_get_priv(event);
	if (!buf) {
		hwc->state = PERF_HES_STOPPED;
		return -EINVAL;
	}

	raw_spin_lock(&pt->lock);
	if (pt->event) {
		raw_spin_unlock(&pt->lock);
		itrace_event_put(event);
		ret = -EBUSY;
		event->hw.state = PERF_HES_STOPPED;
		goto out;
	}

	pt->event = event;
	raw_spin_unlock(&pt->lock);

	hwc->state = !(flags & PERF_EF_START);
	if (!hwc->state) {
		pt_event_start(event, 0);
		if (hwc->state == PERF_HES_STOPPED) {
			pt_event_del(event, 0);
			pt_wake_up(event);
			ret = -EBUSY;
		}
	}

out:
	return ret;
}

static void pt_event_read(struct perf_event *event)
{
}

typedef unsigned int (*pt_copyfn)(void *data, const void *src,
				  unsigned int len);

/**
 * pt_buffer_output - copy part of pt buffer to perf stream
 * @buf: buffer to copy from
 * @from: initial offset
 * @to: final offset
 * @copyfn: function that copies data out (like perf_output_copy())
 * @data: data to be passed on to the copy function (like perf_output_handle)
 */
static int pt_buffer_output(struct pt_buffer *buf, unsigned long from,
			    unsigned long to, pt_copyfn copyfn, void *data)
{
	unsigned long tocopy;
	unsigned int len = 0, remainder;
	void *page;

	do {
		tocopy = PAGE_SIZE - offset_in_page(from);
		if (to > from)
			tocopy = min(tocopy, to - from);
		if (!tocopy)
			break;

		page = pt_buffer_get_page(buf, from >> PAGE_SHIFT);
		if (WARN_ONCE(!page, "no data page for %lx offset\n", from))
			break;

		page += offset_in_page(from);

		remainder = copyfn(data, page, tocopy);
		if (remainder)
			return -EFAULT;

		len += tocopy;
		from += tocopy;
		if (from == buf->size)
			from = 0;
	} while (to != from);
	return len;
}

static int pt_event_init(struct perf_event *event)
{
	if (event->attr.type != pt_pmu.itrace.pmu.type)
		return -ENOENT;

	/* can't be both */
	if (event->attr.sample_type & PERF_SAMPLE_ITRACE)
		return -ENOENT;

	if (!pt_event_valid(event))
		return -EINVAL;

	return 0;
}

static unsigned long pt_trace_sampler_trace(struct perf_event *event,
					    struct perf_sample_data *data)
{
	struct pt_buffer *buf;

	pt_event_stop(event, 0);

	buf = itrace_event_get_priv(event);
	if (!buf) {
		data->trace.size = 0;
		goto out;
	}

	pt_read_offset(buf);
	pt_update_head(buf);

	data->trace.to = local64_read(&buf->head);

	if (data->trace.to < event->attr.itrace_sample_size)
		data->trace.from = buf->size + data->trace.to -
			event->attr.itrace_sample_size;
	else
		data->trace.from = data->trace.to -
			event->attr.itrace_sample_size;
	data->trace.size = ALIGN(event->attr.itrace_sample_size, sizeof(u64));

	itrace_event_put(event);

out:
	if (!data->trace.size)
		pt_event_start(event, 0);

	return data->trace.size;
}

static void pt_trace_sampler_output(struct perf_event *event,
				    struct perf_output_handle *handle,
				    struct perf_sample_data *data)
{
	unsigned long padding;
	struct pt_buffer *buf;
	int ret;

	buf = itrace_event_get_priv(event);
	if (!buf)
		return;

	ret = pt_buffer_output(buf, data->trace.from, data->trace.to,
			       (pt_copyfn)perf_output_copy, handle);
	itrace_event_put(event);
	if (ret < 0) {
		pr_warn("%s: failed to copy trace data\n", __func__);
		goto out;
	}

	padding = data->trace.size - ret;
	if (padding) {
		u64 u = 0;

		perf_output_copy(handle, &u, padding);
	}

out:
	pt_event_start(event, 0);
}

static __init int pt_init(void)
{
	int ret, cpu;

	BUILD_BUG_ON(sizeof(struct topa) > PAGE_SIZE);
	get_online_cpus();
	for_each_possible_cpu(cpu) {
		raw_spin_lock_init(&per_cpu(pt_ctx, cpu).lock);
	}
	put_online_cpus();

	ret = pt_pmu_hw_init();
	if (ret)
		return ret;

	pt_pmu.itrace.pmu.attr_groups	= pt_attr_groups;
	pt_pmu.itrace.pmu.task_ctx_nr	= perf_hw_context;
	pt_pmu.itrace.pmu.event_init	= pt_event_init;
	pt_pmu.itrace.pmu.add		= pt_event_add;
	pt_pmu.itrace.pmu.del		= pt_event_del;
	pt_pmu.itrace.pmu.start		= pt_event_start;
	pt_pmu.itrace.pmu.stop		= pt_event_stop;
	pt_pmu.itrace.pmu.read		= pt_event_read;
	pt_pmu.itrace.alloc_buffer	= pt_buffer_itrace_alloc;
	pt_pmu.itrace.free_buffer	= pt_buffer_itrace_free;
	pt_pmu.itrace.sample_trace	= pt_trace_sampler_trace;
	pt_pmu.itrace.sample_output	= pt_trace_sampler_output;
	pt_pmu.itrace.name		= "intel_pt";
	ret = itrace_pmu_register(&pt_pmu.itrace);

	return ret;
}

module_init(pt_init);
