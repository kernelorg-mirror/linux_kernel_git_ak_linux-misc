#include "builtin.h"
#include "cache.h"

#include "util/evlist.h"
#include "util/parse-options.h"
#include "util/session.h"
#include "util/tool.h"
#include "util/stat.h"
#include "util/cpumap.h"
#include "util/debug.h"
#include "util/annotate.h"

#include <linux/compiler.h>
#include <linux/kernel.h>
#include <sched.h>

typedef struct {
	int  locks;               /* count of 'lock' transactions */
	int  store;               /* count of all stores in trace */
	int  st_uncache;          /* stores to uncacheable address */
	int  st_noadrs;           /* cacheable store with no address */
	int  st_l1hit;            /* count of stores that hit L1D */
	int  st_l1miss;           /* count of stores that miss L1D */
	int  load;                /* count of all loads in trace */
	int  ld_excl;             /* exclusive loads, rmt/lcl DRAM - snp none/miss */
	int  ld_shared;           /* shared loads, rmt/lcl DRAM - snp hit */
	int  ld_uncache;          /* loads to uncacheable address */
	int  ld_io;               /* loads to io address */
	int  ld_miss;             /* loads miss */
	int  ld_noadrs;           /* cacheable load with no address */
	int  ld_fbhit;            /* count of loads hitting Fill Buffer */
	int  ld_l1hit;            /* count of loads that hit L1D */
	int  ld_l2hit;            /* count of loads that hit L2D */
	int  ld_llchit;           /* count of loads that hit LLC */
	int  lcl_hitm;            /* count of loads with local HITM  */
	int  rmt_hitm;            /* count of loads with remote HITM */
	int  rmt_hit;             /* count of loads with remote hit clean; */
	int  lcl_dram;            /* count of loads miss to local DRAM */
	int  rmt_dram;            /* count of loads miss to remote DRAM */
	int  nomap;               /* count of load/stores with no phys adrs */
	int  noparse;             /* count of unparsable data sources */
} trinfo_t;

struct c2c_stats {
	cpu_set_t		cpuset;
	int			nr_entries;
	u64			total_period;
	trinfo_t		t;
	struct stats		stats;
};

struct perf_c2c {
	struct perf_tool tool;
	bool		 raw_records;
	bool		 call_graph;
	struct hists	 hists;

	/* stats */
	struct c2c_stats	stats;
};

#define DISPLAY_LINE_LIMIT  0.0015
#define CACHE_LINESIZE       64
#define CLINE_OFFSET_MSK     (CACHE_LINESIZE - 1)
#define CLADRS(a)            ((a) & ~(CLINE_OFFSET_MSK))
#define CLOFFSET(a)          (int)((a) &  (CLINE_OFFSET_MSK))
#define MAXTITLE_SZ          400
#define MAXLBL_SZ            256

struct c2c_hit {
	struct rb_node	rb_node;
	struct rb_root  tree;
	struct list_head list;
	u64		cacheline;
	int		color;
	struct c2c_stats	stats;
	pid_t		pid;
	pid_t		tid;
	u64		daddr;
	u64		iaddr;
	struct mem_info	*mi;

	struct callchain_root   callchain[0]; /* must be last member */
};

enum { OP, LVL, SNP, LCK, TLB };

#define RMT_RAM              (PERF_MEM_LVL_REM_RAM1 | PERF_MEM_LVL_REM_RAM2)
#define RMT_LLC              (PERF_MEM_LVL_REM_CCE1 | PERF_MEM_LVL_REM_CCE2)

#define L1CACHE_HIT(a)       (((a) & PERF_MEM_LVL_L1 ) && ((a) & PERF_MEM_LVL_HIT))
#define FILLBUF_HIT(a)       (((a) & PERF_MEM_LVL_LFB) && ((a) & PERF_MEM_LVL_HIT))
#define L2CACHE_HIT(a)       (((a) & PERF_MEM_LVL_L2 ) && ((a) & PERF_MEM_LVL_HIT))
#define L3CACHE_HIT(a)       (((a) & PERF_MEM_LVL_L3 ) && ((a) & PERF_MEM_LVL_HIT))

#define L1CACHE_MISS(a)      (((a) & PERF_MEM_LVL_L1 ) && ((a) & PERF_MEM_LVL_MISS))
#define L3CACHE_MISS(a)      (((a) & PERF_MEM_LVL_L3 ) && ((a) & PERF_MEM_LVL_MISS))

#define LD_UNCACHED(a)       (((a) & PERF_MEM_LVL_UNC) && ((a) & PERF_MEM_LVL_HIT))
#define ST_UNCACHED(a)       (((a) & PERF_MEM_LVL_UNC) && ((a) & PERF_MEM_LVL_HIT))

#define RMT_LLCHIT(a)        (((a) & RMT_LLC) && ((a) & PERF_MEM_LVL_HIT))
#define RMT_HIT(a,b)         (((a) & RMT_LLC) && ((b) & PERF_MEM_SNOOP_HIT))
#define RMT_HITM(a,b)        (((a) & RMT_LLC) && ((b) & PERF_MEM_SNOOP_HITM))
#define RMT_MEM(a)           (((a) & RMT_RAM) && ((a) & PERF_MEM_LVL_HIT))

#define LCL_HIT(a,b)         (L3CACHE_HIT(a) && ((b) & PERF_MEM_SNOOP_HIT))
#define LCL_HITM(a,b)        (L3CACHE_HIT(a) && ((b) & PERF_MEM_SNOOP_HITM))
#define LCL_MEM(a)           (((a) & PERF_MEM_LVL_LOC_RAM) && ((a) & PERF_MEM_LVL_HIT))

enum { LVL0, LVL1, LVL2, LVL3, LVL4, MAX_LVL };
static int cloffset = LVL1;
static int node_info = 0;
static int coalesce_level = LVL1;

static int perf_c2c__scnprintf_data_src(char *bf, size_t size, uint64_t val)
{
#define PREFIX       "["
#define SUFFIX       "]"
#define ELLIPSIS     "..."
	static const struct {
		uint64_t   bit;
		int64_t    field;
		const char *name;
	} decode_bits[] = {
	{ PERF_MEM_OP_LOAD,       OP,  "LOAD"     },
	{ PERF_MEM_OP_STORE,      OP,  "STORE"    },
	{ PERF_MEM_OP_NA,         OP,  "OP_NA"    },
	{ PERF_MEM_LVL_LFB,       LVL, "LFB"      },
	{ PERF_MEM_LVL_L1,        LVL, "L1"       },
	{ PERF_MEM_LVL_L2,        LVL, "L2"       },
	{ PERF_MEM_LVL_L3,        LVL, "LCL_LLC"  },
	{ PERF_MEM_LVL_LOC_RAM,   LVL, "LCL_RAM"  },
	{ PERF_MEM_LVL_REM_RAM1,  LVL, "RMT_RAM"  },
	{ PERF_MEM_LVL_REM_RAM2,  LVL, "RMT_RAM"  },
	{ PERF_MEM_LVL_REM_CCE1,  LVL, "RMT_LLC"  },
	{ PERF_MEM_LVL_REM_CCE2,  LVL, "RMT_LLC"  },
	{ PERF_MEM_LVL_IO,        LVL, "I/O"	  },
	{ PERF_MEM_LVL_UNC,       LVL, "UNCACHED" },
	{ PERF_MEM_LVL_NA,        LVL, "N"        },
	{ PERF_MEM_LVL_HIT,       LVL, "HIT"      },
	{ PERF_MEM_LVL_MISS,      LVL, "MISS"     },
	{ PERF_MEM_SNOOP_NONE,    SNP, "SNP NONE" },
	{ PERF_MEM_SNOOP_HIT,     SNP, "SNP HIT"  },
	{ PERF_MEM_SNOOP_MISS,    SNP, "SNP MISS" },
	{ PERF_MEM_SNOOP_HITM,    SNP, "SNP HITM" },
	{ PERF_MEM_SNOOP_NA,      SNP, "SNP NA"   },
	{ PERF_MEM_LOCK_LOCKED,   LCK, "LOCKED"   },
	{ PERF_MEM_LOCK_NA,       LCK, "LOCK_NA"  },
	};
	union perf_mem_data_src dsrc = { .val = val, };
	int printed = scnprintf(bf, size, PREFIX);
	size_t i;
	bool first_present = true;

	for (i = 0; i < ARRAY_SIZE(decode_bits); i++) {
		int bitval;

		switch (decode_bits[i].field) {
		case OP:  bitval = decode_bits[i].bit & dsrc.mem_op;    break;
		case LVL: bitval = decode_bits[i].bit & dsrc.mem_lvl;   break;
		case SNP: bitval = decode_bits[i].bit & dsrc.mem_snoop; break;
		case LCK: bitval = decode_bits[i].bit & dsrc.mem_lock;  break;
		case TLB: bitval = decode_bits[i].bit & dsrc.mem_dtlb;  break;
		default: bitval = 0;					break;
		}

		if (!bitval)
			continue;

		if (strlen(decode_bits[i].name) + !!i > size - printed - sizeof(SUFFIX)) {
			sprintf(bf + size - sizeof(SUFFIX) - sizeof(ELLIPSIS) + 1, ELLIPSIS);
			printed = size - sizeof(SUFFIX);
			break;
		}

		printed += scnprintf(bf + printed, size - printed, "%s%s",
				     first_present ? "" : ",", decode_bits[i].name);
		first_present = false;
	}

	printed += scnprintf(bf + printed, size - printed, SUFFIX);
	return printed;
}

static int c2c_hitm__add_to_list(struct rb_root *root, struct c2c_hit *h)
{
	struct rb_node **p;
	struct rb_node *parent = NULL;
	struct c2c_hit *he;
	int64_t cmp;
	u64 l_hitms, r_hitms;

	p = &root->rb_node;

	while (*p != NULL) {
		parent = *p;
		he = rb_entry(parent, struct c2c_hit, rb_node);

		/* sort on remote hitms first */
		l_hitms = he->stats.t.rmt_hitm;
		r_hitms = h->stats.t.rmt_hitm;
		cmp = r_hitms - l_hitms;

		if (!cmp) {
			/* sort on local hitms */
			l_hitms = he->stats.t.lcl_hitm;
			r_hitms = h->stats.t.lcl_hitm;
			cmp = r_hitms - l_hitms;
		}

		if (cmp > 0)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	rb_link_node(&h->rb_node, parent, p);
	rb_insert_color(&h->rb_node, root);

	return 0;
}

static int perf_c2c__fprintf_header(FILE *fp)
{
	int printed = fprintf(fp, "%c %-16s  %6s  %6s  %4s  %18s  %18s  %18s  %6s  %-10s %-60s %s\n", 
			      'T',
			      "Status",
			      "Pid",
			      "Tid",
			      "CPU",
			      "Inst Adrs",
			      "Virt Data Adrs",
			      "Phys Data Adrs",
			      "Cycles",
			      "Source",
			      "  Decoded Source",
			      "ObJect:Symbol");
	return printed + fprintf(fp, "%-*.*s\n", printed, printed, graph_dotted_line);
}

static int perf_sample__fprintf(struct perf_sample *sample, char tag,
				const char *reason, struct mem_info *mi, FILE *fp)
{
	char data_src[61];
	const char *fmt, *sep;
	struct map *map = mi->iaddr.map;

	perf_c2c__scnprintf_data_src(data_src, sizeof(data_src), sample->data_src);

	if (symbol_conf.field_sep) {
		fmt = "%c%s%s%s%d%s%d%s%d%s%#"PRIx64"%s%#"PRIx64"%s"
		      "%"PRIu64"%s%#"PRIx64"%s%s%s%s:%s\n";
		sep = symbol_conf.field_sep;
	} else {
		fmt = "%c%s%-16s%s%6d%s%6d%s%4d%s%#18"PRIx64"%s%#18"PRIx64"%s"
		      "%6"PRIu64"%s%#10"PRIx64"%s%-60.60s%s%s:%s\n";
		sep = " ";
	}

	return fprintf(fp, fmt,
		       tag,				sep,
		       reason ?: "valid record",	sep,
		       sample->pid,			sep,
		       sample->tid,			sep,
		       sample->cpu,			sep,
		       sample->ip,			sep,
		       sample->addr,			sep,
		       sample->weight,			sep,
		       sample->data_src,		sep,
		       data_src,			sep,
		       map ? (map->dso ? map->dso->long_name : "???") : "???",
		       mi->iaddr.sym ? mi->iaddr.sym->name : "???");
}

static int c2c_decode_stats(struct c2c_stats *stats, struct hist_entry *entry)
{
	union perf_mem_data_src *data_src = &entry->mem_info->data_src;
	u64 daddr = entry->mem_info->daddr.addr;
	u64 weight = entry->stat.weight;
	int err = 0;

	u64 op = data_src->mem_op;
	u64 lvl = data_src->mem_lvl;
	u64 snoop = data_src->mem_snoop;
	u64 lock = data_src->mem_lock;

#define P(a,b) PERF_MEM_##a##_##b

	stats->nr_entries++;
	stats->total_period += entry->stat.period;

	if (lock & P(LOCK,LOCKED)) stats->t.locks++;

	if (op & P(OP,LOAD)) {
		stats->t.load++;

		if (!daddr) {
			stats->t.ld_noadrs++;
			return -1;
		}

		if (lvl & P(LVL,HIT)) {
			if (lvl & P(LVL,UNC)) stats->t.ld_uncache++;
			if (lvl & P(LVL,IO))  stats->t.ld_io++;
			if (lvl & P(LVL,LFB)) stats->t.ld_fbhit++;
			if (lvl & P(LVL,L1 )) stats->t.ld_l1hit++;
			if (lvl & P(LVL,L2 )) stats->t.ld_l2hit++;
			if (lvl & P(LVL,L3 )) {
				if (snoop & P(SNOOP,HITM))
					stats->t.lcl_hitm++;
				else
					stats->t.ld_llchit++;
			}

			if (lvl & P(LVL,LOC_RAM)) {
				stats->t.lcl_dram++;
				if (snoop & P(SNOOP,HIT))
					stats->t.ld_shared++;
				else
					stats->t.ld_excl++;
			}

			if ((lvl & P(LVL,REM_RAM1)) ||
			    (lvl & P(LVL,REM_RAM2))) {
				stats->t.rmt_dram++;
				if (snoop & P(SNOOP,HIT))
					stats->t.ld_shared++;
				else
					stats->t.ld_excl++;
			}
		}

		if ((lvl & P(LVL,REM_CCE1)) ||
		    (lvl & P(LVL,REM_CCE2))) {
			if (snoop & P(SNOOP, HIT))
				stats->t.rmt_hit++;
			else if (snoop & P(SNOOP, HITM)) {
				stats->t.rmt_hitm++;
				update_stats(&stats->stats, weight);
			}
		}

		if ((lvl & P(LVL,MISS)))
			stats->t.ld_miss++;

	} else if (op & P(OP,STORE)) {
		/* store */
		stats->t.store++;

		if (!daddr) {
			stats->t.st_noadrs++;
			return -1;
		}

		if (lvl & P(LVL,HIT)) {
			if (lvl & P(LVL,UNC)) stats->t.st_uncache++;
			if (lvl & P(LVL,L1 )) stats->t.st_l1hit++;
		}
		if (lvl & P(LVL,MISS))
			if (lvl & P(LVL,L1)) stats->t.st_l1miss++;
	} else {
		/* unparsable data_src? */
		stats->t.noparse++;
		return -1;
	}

	if (!entry->mem_info->daddr.map || !entry->mem_info->iaddr.map) {
		stats->t.nomap++;
		return -1;
	}

	return err;
}

static struct c2c_hit *c2c_hit__new(u64 cacheline, struct hist_entry *entry)
{
	size_t callchain_size = symbol_conf.use_callchain ? sizeof(struct callchain_root) : 0;
	struct c2c_hit *h = zalloc(sizeof(struct c2c_hit) + callchain_size);

	if (!h) {
		pr_err("Could not allocate c2c_hit memory\n");
		return NULL;
	}

	CPU_ZERO(&h->stats.cpuset);
	INIT_LIST_HEAD(&h->list);
	init_stats(&h->stats.stats);
	h->tree = RB_ROOT;
	h->cacheline = cacheline;
	h->pid = entry->thread->pid_;
	h->tid = entry->thread->tid;
	if (symbol_conf.use_callchain)
		callchain_init(h->callchain);

	/* use original addresses here, not adjusted al_addr */
	h->iaddr = entry->mem_info->iaddr.addr;
	h->daddr = entry->mem_info->daddr.addr;

	h->mi = entry->mem_info;
	return h;
}

static void c2c_hit__update_strings(struct c2c_hit *h,
				    struct hist_entry *n)
{
	if (h->pid != n->thread->pid_)
		h->pid = -1;

	if (h->tid != n->thread->tid)
		h->tid = -1;

	/* use original addresses here, not adjusted al_addr */
	if (h->iaddr != n->mem_info->iaddr.addr)
		h->iaddr = -1;

	if (CLADRS(h->daddr) != CLADRS(n->mem_info->daddr.addr))
		h->daddr = -1;

	CPU_SET(n->cpu, &h->stats.cpuset);
}

static inline bool matching_coalescing(struct c2c_hit *h,
				       struct hist_entry *e)
{
	bool value = false;
	struct mem_info *mi = e->mem_info;

	if (coalesce_level > MAX_LVL)
		printf("DON: bad coalesce level %d\n", coalesce_level);

	if (e->cpumode != PERF_RECORD_MISC_KERNEL) {

		switch (coalesce_level) {

		case LVL0:
		case LVL1:
			value = ((h->daddr == mi->daddr.addr) &&
				 (h->pid   == e->thread->pid_) &&
				 (h->tid   == e->thread->tid) &&
				 (h->iaddr == mi->iaddr.addr));
			break;

		case LVL2:
			value = ((h->daddr == mi->daddr.addr) &&
				 (h->pid   == e->thread->pid_) &&
				 (h->iaddr == mi->iaddr.addr));
			break;

		case LVL3:
			value = ((h->daddr == mi->daddr.addr) &&
				 (h->iaddr == mi->iaddr.addr));
			break;

		case LVL4:
			value = ((h->daddr == mi->daddr.addr) &&
				 (h->mi->iaddr.sym == mi->iaddr.sym));
			break;

		default:
			break;

		}

	} else {

		switch (coalesce_level) {

		case LVL0:
			value = ((h->daddr == mi->daddr.addr) &&
				 (h->pid   == e->thread->pid_) &&
				 (h->tid   == e->thread->tid) &&
				 (h->iaddr == mi->iaddr.addr));
			break;

		case LVL1:
		case LVL2:
		case LVL3:
			value = ((h->daddr == mi->daddr.addr) &&
				 (h->iaddr == mi->iaddr.addr));
			break;

		case LVL4:
			value = ((h->daddr == mi->daddr.addr) &&
				 (h->mi->iaddr.sym == mi->iaddr.sym));
			break;

		default:
			break;

		}
	}

	return value;
}

static int perf_c2c__process_load_store(struct perf_c2c *c2c,
					struct addr_location *al,
					struct perf_sample *sample,
					struct perf_evsel *evsel)
{
	struct symbol *parent = NULL;
	struct hist_entry *he;
	struct mem_info *mi, *mx;
	uint64_t cost;
	int err;

	mi = sample__resolve_mem(sample, al);
	if (!mi)
		return -ENOMEM;

	if (c2c->raw_records) {
		perf_sample__fprintf(sample, ' ', "raw input", mi, stdout);
		free(mi);
		return 0;
	}

	err = sample__resolve_callchain(sample, &parent, evsel, al, PERF_MAX_STACK_DEPTH);
	if (err)
		return err;

	cost = sample->weight;
	if (!cost)
		cost = 1;

	/*
	 * must pass period=weight in order to get the correct
	 * sorting from hists__collapse_resort() which is solely
	 * based on periods. We want sorting be done on nr_events * weight
	 * and this is indirectly achieved by passing period=weight here
	 * and the he_stat__add_period() function.
	 */
	he = __hists__add_entry(&c2c->hists, al, parent, NULL, mi,
				cost, cost, 0);
	if (!he) {
		err = -ENOMEM;
		goto out_mem;
	}

	err = c2c_decode_stats(&c2c->stats, he);
	if (err < 0) {
		err = 0;
		rb_erase(&he->rb_node_in, c2c->hists.entries_in);
		free(he);
		goto out;
	}

	err = hist_entry__inc_addr_samples(he, evsel->idx, al->addr);
	if (err)
		goto out;

	mx = he->mem_info;
	err = addr_map_symbol__inc_samples(&mx->daddr, evsel->idx);
	if (err)
		goto out;

        c2c->hists.stats.total_period += cost;
        hists__inc_nr_events(&c2c->hists, PERF_RECORD_SAMPLE);
        err = hist_entry__append_callchain(he, sample);
	return err;

out_mem:
	/* implicitly freed by __hists__add_entry */
	free(mi);
out:
	return err;
}

static const struct perf_evsel_str_handler handlers[] = {
	{ "cpu/mem-loads,ldlat=30/pp", perf_c2c__process_load_store, },
	{ "cpu/mem-stores/pp",	       perf_c2c__process_load_store, },
};

typedef int (*sample_handler)(struct perf_c2c *c2c,
			      struct addr_location *al,
			      struct perf_sample *sample,
			      struct perf_evsel *evsel);

static int perf_c2c__process_sample(struct perf_tool *tool,
				    union perf_event *event,
				    struct perf_sample *sample,
				    struct perf_evsel *evsel,
				    struct machine *machine)
{
	struct perf_c2c *c2c = container_of(tool, struct perf_c2c, tool);
	u8 cpumode = event->header.misc & PERF_RECORD_MISC_CPUMODE_MASK;
	struct thread *thread;
	sample_handler f;
	int err = -1;
	struct addr_location al = {
			.machine = machine,
			.cpu = sample->cpu,
			.cpumode = cpumode,
	};

	if (evsel->handler == NULL)
		return 0;

	thread = machine__find_thread(machine, sample->tid);
	if (thread == NULL)
		goto err;

	al.thread = thread;

	f = evsel->handler;
	err = f(c2c, &al, sample, evsel);
	if (err)
		goto err;

	return 0;
err:
	if (err > 0)
		err = 0;
	return err;
}

#define HAS_HITMS(h) (h->stats.t.lcl_hitm || h->stats.t.rmt_hitm)

static void dump_rb_tree(struct rb_root *tree,
			 struct perf_c2c *c2c __maybe_unused)
{
	struct rb_node *next = rb_first(tree);
	struct hist_entry *he;
	u64 cl = 0;
	int idx = 0;

	printf("# Summary: Total entries - %d\n", c2c->stats.nr_entries);
	printf("# HITMs: Local - %d   Remote - %d     Total - %d\n",
		c2c->stats.t.lcl_hitm, c2c->stats.t.rmt_hitm,
		(c2c->stats.t.lcl_hitm + c2c->stats.t.rmt_hitm));

	printf("%6s %3s %3s %3s %8s %16s %6s %16s %16s %16s %32s %8s\n",
		"Idx", "Hit", "Maj", "Min", "Ino", "InoGen", "Pid", 
		"Daddr", "Iaddr", "Data Src", "(string)", "cpumode");
	while (next) {
		char data_src[32];
		u64 val;

		he = rb_entry(next, struct hist_entry, rb_node_in);
		next = rb_next(&he->rb_node_in);

		if ((!he->color) || (cl != CLADRS(he->mem_info->daddr.al_addr))) {
			printf("\n");
			cl = CLADRS(he->mem_info->daddr.al_addr);
		}

		val = he->mem_info->data_src.val;
		perf_c2c__scnprintf_data_src(data_src, sizeof(data_src), val);

		printf("%6d %3s %3x %3x %8lx %16lx %6d %16lx %16lx %16lx %32s %8x\n",
			idx,
			(PERF_MEM_S(SNOOP,HITM) & val) ? " * " : "   ",
			he->mem_info->daddr.map->maj,
			he->mem_info->daddr.map->min,
			he->mem_info->daddr.map->ino,
			he->mem_info->daddr.map->ino_generation,
			he->thread->pid_,
			he->mem_info->daddr.addr,
			he->mem_info->iaddr.addr,
			val,
			data_src,
			he->cpumode);

		idx++;
	}
}

static void c2c_hit__update_stats(struct c2c_stats *new,
				  struct c2c_stats *old)
{
	new->t.load		+= old->t.load;
	new->t.ld_fbhit		+= old->t.ld_fbhit;
	new->t.ld_l1hit		+= old->t.ld_l1hit;
	new->t.ld_l2hit		+= old->t.ld_l2hit;
	new->t.ld_llchit	+= old->t.ld_llchit;
	new->t.locks		+= old->t.locks;
	new->t.lcl_dram		+= old->t.lcl_dram;
	new->t.rmt_dram		+= old->t.rmt_dram;
	new->t.lcl_hitm		+= old->t.lcl_hitm;
	new->t.rmt_hitm		+= old->t.rmt_hitm;
	new->t.rmt_hit		+= old->t.rmt_hit;
	new->t.store		+= old->t.store;
	new->t.st_l1hit		+= old->t.st_l1hit;

	new->total_period	+= old->total_period;
}

LIST_HEAD(ref_tree);
LIST_HEAD(ref_tree_sorted);
struct refs {
	struct list_head	list;
	int			nr;
	const char		*name;
	const char		*long_name;
};

static int update_ref_list(struct hist_entry *entry)
{
	struct refs *p;
	struct dso *dso = entry->mem_info->iaddr.map->dso;
	const char *name = dso->short_name;

	list_for_each_entry(p, &ref_tree, list) {
		if (!strcmp(p->name, name))
			goto found;
	}

	p = zalloc(sizeof(struct refs));
	if (!p)
		return -1;
	p->name = name;
	p->long_name = dso->long_name;
	list_add_tail(&p->list, &ref_tree);

found:
	p->nr++;
	return 0;
}

static void print_symbol_record_count(struct rb_root *tree)
{
	struct rb_node *next = rb_first(tree);
	struct hist_entry *he;
	struct refs *p, *q, *pn;
	char      string[256];
	char      delimit[256];
	int       i;
	int       idx = 0;

	/* gather symbol references */
	while (next) {
		he = rb_entry(next, struct hist_entry, rb_node_in);
		next = rb_next(&he->rb_node_in);

		if (update_ref_list(he)) {
			pr_err("Could not update reference tree\n");
			goto cleanup;
		}
	}

	/* sort on number of references per symbol */
	list_for_each_entry_safe(p, pn, &ref_tree, list) {
		list_del_init(&p->list);
		list_for_each_entry(q, &ref_tree_sorted, list) {
			if (p->nr > q->nr) {
				list_add_tail(&p->list, &q->list);
				break;
			}
		}
		if (list_empty(&p->list))
			list_add_tail(&p->list, &ref_tree_sorted);
	}

	/* print header info */
	sprintf(string, "%5s   %8s   %-32s  %-80s",
		"Index",
		"Records",
		"Object Name",
		"Object Path");

	delimit[0] = '\0';
	for (i = 0; i < (int)strlen(string); i++) strcat(delimit, "=");

	printf("\n\n");
	printf("%s\n", delimit);
	printf("%50s %s\n", " ", "Object Name, Path & Reference Counts");
	printf("\n");
	printf("%s\n", string);
	printf("%s\n", delimit);

	/* print out table */
	list_for_each_entry(p, &ref_tree_sorted, list) {
		printf("%5d   %8d   %-32s  %-80s\n",
			idx, p->nr, p->name, p->long_name);
		idx++;
	}
	printf("\n");

cleanup:
	list_for_each_entry_safe(p, pn, &ref_tree_sorted, list) {
		list_del(&p->list);
		free(p);
	}
}

static void print_c2c_shared_cacheline_report(struct rb_root *hitm_tree,
					      struct c2c_stats *shared_stats __maybe_unused,
					      struct c2c_stats *c2c_stats __maybe_unused)
{
#define   SHM_TITLE  "Shared Data Cache Line Table"

	struct rb_node	*next = rb_first(hitm_tree);
	struct c2c_hit	*h;
	char		header[256];
	char		delimit[256];
	u32		crecords;
	u32		lclmiss;
	u32		ldcnt;
	double		p_hitm;
	double		p_all;
	int		totmiss;
	int		rmt_hitm;
	int		len;
	int		pad;
	int		i;

	sprintf(header,"%28s  %8s  %8s  %8s  %8s  %28s  %18s  %28s  %18s  %8s  %28s",
		" ",
		"Total",
		"%All ",
		" ",
		"Total",
		"---- Core Load Hit ----",
		"-- LLC Load Hit --",
		"----- LLC Load Hitm -----",
		"-- Load Dram --",
		"LLC  ",
		"---- Store Reference ----");

	len = strlen(header);
	delimit[0] = '\0';

	for (i = 0; i < len; i++)
		strcat(delimit, "=");

	printf("\n\n");
	printf("%s\n", delimit);
	printf("\n");
	pad = (strlen(header)/2) - (strlen(SHM_TITLE)/2);
	for (i = 0; i < pad; i++)
		printf(" ");
	printf("%s\n", SHM_TITLE);
	printf("\n");
	printf("%s\n", header);

	sprintf(header, "%8s  %18s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s  %8s",
		"Index",
		"Phys Adrs",
		"Records",
		"Ld Miss",
		"%hitm",
		"Loads",
		"FB",
		"L1D",
		"L2D",
		"Lcl",
		"Rmt",
		"Total",
		"Lcl",
		"Rmt",
		"Lcl",
		"Rmt",
		"Ld Miss",
		"Total",
		"L1Hit",
		"L1Miss");

	printf("%s\n", header);
	printf("%s\n", delimit);

	rmt_hitm    = c2c_stats->t.rmt_hitm;
	totmiss     = c2c_stats->t.lcl_dram +
		      c2c_stats->t.rmt_dram +
		      c2c_stats->t.rmt_hit +
		      c2c_stats->t.rmt_hitm;

	i = 0;
	while (next) {
		h = rb_entry(next, struct c2c_hit, rb_node);
		next = rb_next(&h->rb_node);

		lclmiss  = h->stats.t.lcl_dram +
			   h->stats.t.rmt_dram +
			   h->stats.t.rmt_hitm +
			   h->stats.t.rmt_hit;

		ldcnt    = lclmiss +
			   h->stats.t.ld_fbhit +
			   h->stats.t.ld_l1hit +
			   h->stats.t.ld_l2hit +
			   h->stats.t.ld_llchit +
			   h->stats.t.lcl_hitm;

		crecords = ldcnt +
			   h->stats.t.st_l1hit +
			   h->stats.t.st_l1miss;

		p_hitm = (double)h->stats.t.rmt_hitm / (double)rmt_hitm;
		p_all  = (double)h->stats.t.rmt_hitm / (double)totmiss;

		/* stop when the percentage gets to low */
		if (p_hitm < DISPLAY_LINE_LIMIT)
			break;

		printf("%8d  %#18lx  %8u  %7.2f%%  %7.2f%%  %8u  %8u  %8u  %8u  %8u  %8u  %8u  %8u  %8u  %8u  %8u  %8u  %8u  %8u  %8u\n",
			i,
			h->cacheline,
			crecords,
			100. * p_all,
			100. * p_hitm,
			ldcnt,
			h->stats.t.ld_fbhit,
			h->stats.t.ld_l1hit,
			h->stats.t.ld_l2hit,
			h->stats.t.ld_llchit,
			h->stats.t.rmt_hit,
			h->stats.t.lcl_hitm + h->stats.t.rmt_hitm,
			h->stats.t.lcl_hitm,
			h->stats.t.rmt_hitm,
			h->stats.t.lcl_dram,
			h->stats.t.rmt_dram,
			lclmiss,
			h->stats.t.store,
			h->stats.t.st_l1hit,
			h->stats.t.st_l1miss);

		i++;
	}
}

static void print_hitm_cacheline_header(void)
{
#define SHARING_REPORT_TITLE  "Shared Cache Line Distribution Pareto"
#define PARTICIPANTS1         "Node{cpus %hitms %stores} Node{cpus %hitms %stores} ..."
#define PARTICIPANTS2         "Node{cpu list}; Node{cpu list}; Node{cpu list}; ..."

	int i;
	const  char *docptr;
	static char  delimit[MAXTITLE_SZ];
	static char  title2[MAXTITLE_SZ];
	int       pad;

	docptr = " ";
	if (node_info == 1)
		docptr = PARTICIPANTS1;
	if (node_info  == 2)
		docptr = PARTICIPANTS2;

	sprintf(title2, "%4s %6s %6s %6s %6s %8s %8s %8s %8s %18s %6s %6s %18s %8s %8s %8s %6s %-30s %-20s %s",
			"Num",
			"%dist",
			"%cumm",
			"%dist",
			"%cumm",
			"LLCmiss",
			"LLChit",
			"L1 hit",
			"L1 Miss",
			"Data Address",
			"Pid",
			"Tid",
			"Inst Address",
			"median",
			"mean",
			"CV  ",
			"cnt",
			"Symbol",
			"Object",
			docptr);

	for (i = 0; i < (int)strlen(title2); i++) strcat(delimit, "=");


	printf("\n\n");
	printf("%s\n", delimit);
	printf("\n");

	pad = (strlen(title2)/2) - (strlen(SHARING_REPORT_TITLE)/2);
	for (i = 0; i < pad; i++) printf(" ");
	printf("%s\n", SHARING_REPORT_TITLE);

	printf("\n");
	printf("%4s %13s %13s %17s %8s %8s %18s %6s %6s %18s %26s %6s %30s %20s %s\n",
		" ",
		"---- All ----",
		"-- Shared --",
		"---- HITM ----",
		" ",
		" ",
		" ",
		" ",
		" ",
		" ",
		"Load Inst Execute Latency",
		" ",
		" ",
		" ",
		node_info  ? "Shared Data Participants" : " ");


	printf("%4s %13s %13s %8s %8s %17s %18s %6s %6s %17s %18s\n",
		" ",
		" Data Misses",
		" Data Misses",
		"Remote",
		"Local",
		"-- Store Refs --",
		" ",
		" ",
		" ",
		" ",
		" ");

	printf("%4s %13s %13s %8s %8s %8s %8s %18s %6s %6s %17s %18s %8s %6s\n",
		" ",
		" ",
		" ",
		" ",
		" ",
		" ",
		" ",
		" ",
		" ",
		" ",
		" ",
		"---- cycles ----",
		" ",
		"cpu");

	printf("%s\n", title2);
	printf("%s\n", delimit);
}

static void print_hitm_cacheline(struct c2c_hit *h,
				 int record,
				 double tot_cumm,
				 double ld_cumm,
				 double tot_dist,
				 double ld_dist)
{
	char pidstr[7];
	char addrstr[20];
	static char  summary[MAXLBL_SZ];
	int j;

	if (h->pid > 0)
		sprintf(pidstr, "%6d", h->pid);
	else
		sprintf(pidstr, "***");
	/*
	 * It is possible to have none distinct virtual addresses
	 * pointing to a distinct SYstem V shared memory region.
	 * if there are multple virtual addresses the address
	 * field will be astericks. It would be possible to subsitute
	 * the physical address but this count be confusing as some
	 * times the field is a virtual address while or times it
	 * may be a physical address which may lead to confusion.
	 */
	if (h->daddr != ~0UL)
		sprintf(addrstr, "%#18lx", CLADRS(h->daddr));
	else
		sprintf(addrstr, "****************");


	sprintf(summary, "%4d %5.1f%% %5.1f%% %5.1f%% %5.1f%% %8d %8d %8d %8d %18s %6s\n",
			record,
			tot_dist * 100.,
			tot_cumm * 100.,
			ld_dist * 100.,
			ld_cumm * 100.,
			h->stats.t.rmt_hitm,
			h->stats.t.lcl_hitm,
			h->stats.t.st_l1hit,
			h->stats.t.st_l1miss,
			addrstr,
			pidstr);

	for (j = 0; j < (int)strlen(summary); j++) printf("-");
	printf("\n");
	printf("%s", summary);
	for (j = 0; j < (int)strlen(summary); j++) printf("-");
	printf("\n");
}

static void print_socket_stats_str(struct c2c_hit *clo,
				   struct c2c_stats *node_stats)
{
	int i, j;

	if (!node_stats)
		return;

	for (i = 0; i < max_node_num; i++) {
		struct c2c_stats *stats = &node_stats[i];
		int num = CPU_COUNT(&stats->cpuset);

		if (!num) {
			/* pad align socket info */
			for (j = 0; j < 21; j++)
				printf(" ");
			continue;
		}

		printf("%2d{%2d ", i, num);

		if (clo->stats.t.rmt_hitm > 0)
			printf("%5.1f%% ", 100. * ((double)stats->t.rmt_hitm / (double) clo->stats.t.rmt_hitm));
		else
			printf("%6s ", "n/a");

		if (clo->stats.t.store > 0)
			printf("%5.1f%%} ", 100. * ((double)stats->t.store / (double)clo->stats.t.store));
		else
			printf("%6s} ", "n/a");
	}
}

static void print_socket_shared_str(struct c2c_stats *node_stats)
{
	int i, j;

	if (!node_stats)
		return;

	for (i = 0; i < max_node_num; i++) {
		struct c2c_stats *stats = &node_stats[i];
		int num = CPU_COUNT(&stats->cpuset);
		int start = -1;
		bool first = true;

		if (!num)
			continue;

		printf("%d{", i);

		for (j = 0; j < max_cpu_num; j++) {
			if (!CPU_ISSET(j, &stats->cpuset)) {
				if (start != -1) {
					if ((j-1) - start)
						/* print the range */
						printf("%s%d-%d", (first ? "" : ","), start, j-1);
					else
						/* standalone */
						printf("%s%d", (first ? "" : ",") , start);
					start = -1;
					first = false;
				}
				continue;
			}

			if (start == -1)
				start = j;
		}
		/* last chunk */
		if (start != -1) {
			if ((j-1) - start)
				/* print the range */
				printf("%s%d-%d", (first ? "" : ","), start, j-1);
			else
				/* standalone */
				printf("%s%d", (first ? "" : ",") , start);
		}

		printf("}; ");
	}
}

static void print_hitm_cacheline_offset(struct c2c_hit *clo,
					struct c2c_hit *h,
					struct c2c_stats *node_stats)
{
#define SHORT_STR_LEN	7
#define LONG_STR_LEN	30

	char pidstr[SHORT_STR_LEN];
	char tidstr[SHORT_STR_LEN];
	char addrstr[LONG_STR_LEN];
	char latstr[LONG_STR_LEN];
	char objptr[LONG_STR_LEN];
	char symptr[LONG_STR_LEN];
	struct c2c_stats *stats = &clo->stats;
	struct addr_map_symbol *ams;

	ams = &clo->mi->iaddr;

	if (clo->pid >= 0)
		snprintf(pidstr, SHORT_STR_LEN, "%6d", clo->pid);
	else
		sprintf(pidstr, "***");

	if (clo->tid >= 0)
		snprintf(tidstr, SHORT_STR_LEN, "%6d", clo->tid);
	else
		sprintf(tidstr, "***");

	if (clo->iaddr != ~0UL)
		snprintf(addrstr, LONG_STR_LEN, "%#18lx", clo->iaddr);
	else
		sprintf(addrstr, "****************");
	snprintf(objptr, LONG_STR_LEN, "%-18s", ams->map->dso->short_name);
	snprintf(symptr, LONG_STR_LEN, "%-18s", (ams->sym ? ams->sym->name : "?????"));

	if (stats->t.rmt_hitm > 0) {
		double mean = avg_stats(&stats->stats);
		double std = stddev_stats(&stats->stats);

		sprintf(latstr, "%8.0f %8.0f %7.1f%%",
			-1.0, /* FIXME */
			mean,
			rel_stddev_stats(std, mean));
	} else {
		sprintf(latstr, "%8s %8s %8s",
			"n/a",
			"n/a",
			"n/a");

	}

	/*
	 * implicit assumption that we are not coalescing over IPs
	 */
	printf("%4s %6s %6s %6s %6s %7.1f%% %7.1f%% %7.1f%% %7.1f%% %14s0x%02lx %6s %6s %18s %8s %6d %-30s %-20s",
		" ",
		" ",
		" ",
		" ",
		" ",
		(stats->t.rmt_hitm  > 0) ? (100. * ((double)stats->t.rmt_hitm  / (double)h->stats.t.rmt_hitm))  : 0.0,
		(stats->t.lcl_hitm  > 0) ? (100. * ((double)stats->t.lcl_hitm  / (double)h->stats.t.lcl_hitm))  : 0.0,
		(stats->t.st_l1hit  > 0) ? (100. * ((double)stats->t.st_l1hit  / (double)h->stats.t.st_l1hit))  : 0.0,
		(stats->t.st_l1miss > 0) ? (100. * ((double)stats->t.st_l1miss / (double)h->stats.t.st_l1miss)) : 0.0,
		" ",
		(cloffset == LVL2) ? (clo->daddr & 0xff) : CLOFFSET(clo->daddr),
		pidstr,
		tidstr,
		addrstr,
		latstr,
		CPU_COUNT(&clo->stats.cpuset),
		symptr,
		objptr);

	if (node_info == 0)
		printf("  ");
	else if (node_info  == 1)
		print_socket_stats_str(clo, node_stats);
	else if (node_info  == 2)
		print_socket_shared_str(node_stats);

	printf("\n");

	if (symbol_conf.use_callchain) {
		generic_entry_callchain__fprintf(clo->callchain,
						 h->stats.total_period,
						 clo->stats.total_period,
						 23, stdout);
	}
}
static void print_c2c_hitm_report(struct rb_root *hitm_tree,
				  struct c2c_stats *hitm_stats __maybe_unused,
				  struct c2c_stats *c2c_stats)
{
	struct rb_node	*next = rb_first(hitm_tree);
	struct c2c_hit	*h, *clo = NULL;
	u64		addr;
	double		tot_dist, tot_cumm;
	double		ld_dist, ld_cumm;
	int		llc_misses;
	int		record = 0;
	struct c2c_stats *node_stats = NULL;

	if (node_info) {
		node_stats = zalloc(sizeof(struct c2c_stats) * cpu_map__max_node());
		if (!node_stats) {
			printf("Can not allocate stats for node output\n");
			return;
		}
	}

	print_hitm_cacheline_header();

	llc_misses = c2c_stats->t.lcl_dram +
		     c2c_stats->t.rmt_dram +
		     c2c_stats->t.rmt_hit +
		     c2c_stats->t.rmt_hitm;

	/*
	 * generate distinct cache line report
	 */
	tot_cumm = 0.0;
	ld_cumm  = 0.0;

	while (next) {
		struct hist_entry *entry;

		h = rb_entry(next, struct c2c_hit, rb_node);
		next = rb_next(&h->rb_node);

		tot_dist  = ((double)h->stats.t.rmt_hitm / llc_misses);
		tot_cumm += tot_dist;

		ld_dist  = ((double)h->stats.t.rmt_hitm / c2c_stats->t.rmt_hitm);
		ld_cumm += ld_dist;

		/*
		 * don't display lines with insignificant sharing contribution
		 */
		if (ld_dist < DISPLAY_LINE_LIMIT)
			break;

		print_hitm_cacheline(h, record, tot_cumm, ld_cumm, tot_dist, ld_dist);

		list_for_each_entry(entry, &h->list, pairs.node) {

			if (!clo || !matching_coalescing(clo, entry)) {
				if (clo)
					print_hitm_cacheline_offset(clo, h, node_stats);

				free(clo);
				addr = entry->mem_info->iaddr.al_addr;
				clo = c2c_hit__new(addr, entry);
				if (node_info)
					memset(node_stats, 0, sizeof(struct c2c_stats) * cpu_map__max_node());
			}
			c2c_decode_stats(&clo->stats, entry);
			c2c_hit__update_strings(clo, entry);

			if (node_info) {
				int node = cpu_map__get_node(entry->cpu);
				c2c_decode_stats(&node_stats[node], entry);
				CPU_SET(entry->cpu, &(node_stats[node].cpuset));
			}
			if (symbol_conf.use_callchain) {
				callchain_cursor_reset(&callchain_cursor);
				callchain_merge(&callchain_cursor,
						clo->callchain,
						entry->callchain);
			}

		}
		if (clo) {
			print_hitm_cacheline_offset(clo, h, node_stats);
			free(clo);
			clo = NULL;
		}

		if (node_info)
			memset(node_stats, 0, sizeof(struct c2c_stats) * cpu_map__max_node());

		printf("\n");
		record++;
	}
}

static inline int valid_hitm_or_store(union perf_mem_data_src *dsrc)
{
	return ((dsrc->mem_snoop & P(SNOOP,HITM)) ||
		(dsrc->mem_op & P(OP,STORE)));
}

static void print_shared_cacheline_info(struct c2c_stats *stats, int cline_cnt)
{
	int hitm_cnt = stats->t.lcl_hitm + stats->t.rmt_hitm;

	printf("=================================================\n");
	printf("    Global Shared Cache Line Event Information   \n");
	printf("=================================================\n");
	printf("  Total Shared Cache Lines          : %10d\n", cline_cnt);
	printf("  Load HITs on shared lines         : %10d\n", stats->t.load);
	printf("  Fill Buffer Hits on shared lines  : %10d\n", stats->t.ld_fbhit);
	printf("  L1D hits on shared lines          : %10d\n", stats->t.ld_l1hit);
	printf("  L2D hits on shared lines          : %10d\n", stats->t.ld_l2hit);
	printf("  LLC hits on shared lines          : %10d\n", stats->t.ld_llchit + stats->t.lcl_hitm);
	printf("  Locked Access on shared lines     : %10d\n", stats->t.locks);
	printf("  Store HITs on shared lines        : %10d\n", stats->t.store);
	printf("  Store L1D hits on shared lines    : %10d\n", stats->t.st_l1hit);
	printf("  Total Merged records              : %10d\n", hitm_cnt + stats->t.store);
}

static void c2c_analyze_hitms(struct perf_c2c *c2c)
{

	struct rb_node *next = rb_first(c2c->hists.entries_in);
	struct hist_entry *he;
	struct c2c_hit *h = NULL;
	struct c2c_stats hitm_stats;
	struct rb_root hitm_tree = RB_ROOT;
	int shared_clines = 0;
	u64 cl = 0;

	memset(&hitm_stats, 0, sizeof(struct c2c_stats));

	/* find HITMs */
	while (next) {
		he = rb_entry(next, struct hist_entry, rb_node_in);
		next = rb_next(&he->rb_node_in);

		cl = he->mem_info->daddr.al_addr;

		/* switch cache line objects */
		/* 'color' forces a boundary change based on the original sort */
		if (!h || !he->color || (CLADRS(cl) != h->cacheline)) {
			if (h && HAS_HITMS(h)) {
				c2c_hit__update_stats(&hitm_stats, &h->stats);

				/* sort based on hottest cacheline */
				c2c_hitm__add_to_list(&hitm_tree, h);
				shared_clines++;
			} else {
				/* stores-only are un-interesting */
				free(h);
			}
			h = c2c_hit__new(CLADRS(cl), he);
			if (!h)
				goto cleanup;
		}


		c2c_decode_stats(&h->stats, he);

		/* filter out non-hitms as un-interesting noise */
		if (valid_hitm_or_store(&he->mem_info->data_src)) {
			/* save the entry for later processing */
			list_add_tail(&he->pairs.node, &h->list);

			c2c_hit__update_strings(h, he);
		}
	}

	/* last chunk */
	if (HAS_HITMS(h)) {
		c2c_hit__update_stats(&hitm_stats, &h->stats);
		c2c_hitm__add_to_list(&hitm_tree, h);
		shared_clines++;
	} else
		free(h);

	print_shared_cacheline_info(&hitm_stats, shared_clines);
	print_c2c_shared_cacheline_report(&hitm_tree, &hitm_stats, &c2c->stats);
	print_c2c_hitm_report(&hitm_tree, &hitm_stats, &c2c->stats);

cleanup:
	next = rb_first(&hitm_tree);
	while (next) {
		h = rb_entry(next, struct c2c_hit, rb_node);
		next = rb_next(&h->rb_node);
		rb_erase(&h->rb_node, &hitm_tree);

		free(h);
	}
	return;
}

static void print_c2c_trace_report(struct perf_c2c *c2c)
{
	int llc_misses;
	struct c2c_stats *stats = &c2c->stats;

	llc_misses = stats->t.lcl_dram +
		     stats->t.rmt_dram +
		     stats->t.rmt_hit +
		     stats->t.rmt_hitm;

	printf("=================================================\n");
	printf("            Trace Event Information              \n");
	printf("=================================================\n");
	printf("  Total records                     : %10d\n", c2c->stats.nr_entries);
	printf("  Locked Load/Store Operations      : %10d\n", stats->t.locks);
	printf("  Load Operations                   : %10d\n", stats->t.load);
	printf("  Loads - uncacheable               : %10d\n", stats->t.ld_uncache);
	printf("  Loads - IO                        : %10d\n", stats->t.ld_io);
	printf("  Loads - Miss                      : %10d\n", stats->t.ld_miss);
	printf("  Loads - no mapping                : %10d\n", stats->t.ld_noadrs);
	printf("  Load Fill Buffer Hit              : %10d\n", stats->t.ld_fbhit);
	printf("  Load L1D hit                      : %10d\n", stats->t.ld_l1hit);
	printf("  Load L2D hit                      : %10d\n", stats->t.ld_l2hit);
	printf("  Load LLC hit                      : %10d\n", stats->t.ld_llchit + stats->t.lcl_hitm);
	printf("  Load Local HITM                   : %10d\n", stats->t.lcl_hitm);
	printf("  Load Remote HITM                  : %10d\n", stats->t.rmt_hitm);
	printf("  Load Remote HIT                   : %10d\n", stats->t.rmt_hit);
	printf("  Load Local DRAM                   : %10d\n", stats->t.lcl_dram);
	printf("  Load Remote DRAM                  : %10d\n", stats->t.rmt_dram);
	printf("  Load MESI State Exclusive         : %10d\n", stats->t.ld_excl);
	printf("  Load MESI State Shared            : %10d\n", stats->t.ld_shared);
	printf("  Load LLC Misses                   : %10d\n", llc_misses);
	printf("  LLC Misses to Local DRAM          : %10.1f%%\n", ((double)stats->t.lcl_dram/(double)llc_misses) * 100.);
	printf("  LLC Misses to Remote DRAM         : %10.1f%%\n", ((double)stats->t.rmt_dram/(double)llc_misses) * 100.);
	printf("  LLC Misses to Remote cache (HIT)  : %10.1f%%\n", ((double)stats->t.rmt_hit /(double)llc_misses) * 100.);
	printf("  LLC Misses to Remote cache (HITM) : %10.1f%%\n", ((double)stats->t.rmt_hitm/(double)llc_misses) * 100.);
	printf("  Store Operations                  : %10d\n", stats->t.store);
	printf("  Store - uncacheable               : %10d\n", stats->t.st_uncache);
	printf("  Store - no mapping                : %10d\n", stats->t.st_noadrs);
	printf("  Store L1D Hit                     : %10d\n", stats->t.st_l1hit);
	printf("  Store L1D Miss                    : %10d\n", stats->t.st_l1miss);
	printf("  No Page Map Rejects               : %10d\n", stats->t.nomap);
	printf("  Unable to parse data source       : %10d\n", stats->t.noparse);
}

static int perf_c2c__process_events(struct perf_session *session,
				    struct perf_c2c *c2c)
{
	int err = -1;

	err = perf_session__process_events(session, &c2c->tool);
	if (err) {
		pr_err("Failed to process count events, error %d\n", err);
		goto err;
	}

	if (verbose > 2)
		dump_rb_tree(c2c->hists.entries_in, c2c);
	print_c2c_trace_report(c2c);
	c2c_analyze_hitms(c2c);
	print_symbol_record_count(c2c->hists.entries_in);

err:
	return err;
}

static int perf_c2c__setup_sample_type(struct perf_c2c *c2c,
				       struct perf_session *session)
{
	u64 sample_type = perf_evlist__combined_sample_type(session->evlist);

	if (!(sample_type & PERF_SAMPLE_CALLCHAIN)) {
		if (symbol_conf.use_callchain) {
			printf("Selected -g but no callchain data. Did "
				  "you call 'perf c2c record' without -g?\n");
			return -1;
		}
	} else if (callchain_param.mode != CHAIN_NONE &&
		   !symbol_conf.use_callchain) {
			symbol_conf.use_callchain = true;
			c2c->call_graph = true;
			if (callchain_register_param(&callchain_param) < 0) {
				printf("Can't register callchain params.\n");
				return -EINVAL;
			}
	}

	return 0;
}

static int perf_c2c__read_events(struct perf_c2c *c2c)
{
	int err = -1;
	struct perf_session *session;
	struct perf_data_file file = {
			.path = input_name,
			.mode = PERF_DATA_MODE_READ,
	};
	struct perf_evsel *evsel;

	session = perf_session__new(&file, 0, &c2c->tool);
	if (session == NULL) {
		pr_debug("No memory for session\n");
		goto out;
	}

	if (symbol__init() < 0)
		goto out_delete;

	if (perf_c2c__setup_sample_type(c2c, session) < 0)
		goto out_delete;

	/* setup the evsel handlers for each event type */
	evlist__for_each(session->evlist, evsel) {
		const char *name = perf_evsel__name(evsel);
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(handlers); i++) {
			if (!strcmp(name, handlers[i].name))
				evsel->handler = handlers[i].handler;
		}
	}

	err = perf_c2c__process_events(session, c2c);

out_delete:
	perf_session__delete(session);
out:
	return err;
}

static int perf_c2c__init(struct perf_c2c *c2c)
{
	/* setup cpu map */
	if (cpu_map__setup_cpunode_map() < 0) {
		pr_err("can not setup cpu map\n");
		return -1;
	}

	sort__mode = SORT_MODE__MEMORY;
	sort__wants_unique = 1;
	sort_order = "physid";

	if (setup_sorting() < 0) {
		pr_err("can not setup sorting\n");
		return -1;
	}

	hists__init(&c2c->hists);
	CPU_ZERO(&c2c->stats.cpuset);

	return 0;
}
static int perf_c2c__report(struct perf_c2c *c2c)
{
	setup_pager();

	if (perf_c2c__init(c2c))
		return -1;

	if (c2c->raw_records)
		perf_c2c__fprintf_header(stdout);

	return perf_c2c__read_events(c2c);
}

static int perf_c2c__record(int argc, const char **argv)
{
	unsigned int rec_argc, i, j;
	const char **rec_argv;
	const char * const record_args[] = {
		"record",
		"-W",
		"-d",
		"-a",
	};

	rec_argc = ARRAY_SIZE(record_args) + 2 * ARRAY_SIZE(handlers) + argc - 1;
	rec_argv = calloc(rec_argc + 1, sizeof(char *));

	if (rec_argv == NULL)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(record_args); i++)
		rec_argv[i] = strdup(record_args[i]);

	for (j = 0; j < ARRAY_SIZE(handlers); j++) {
		rec_argv[i++] = strdup("-e");
		rec_argv[i++] = strdup(handlers[j].name);
	}

	for (j = 1; j < (unsigned int)argc; j++, i++)
		rec_argv[i] = argv[j];

	BUG_ON(i != rec_argc);

	return cmd_record(i, rec_argv, NULL);
}

static int
opt_callchain_cb(const struct option *opt, const char *arg, int unset)
{
	struct perf_c2c *c2c = (struct perf_c2c *)opt->value;
	char *tok, *tok2;
	char *endptr;

	/*
	 * --no-call-graph
	 */
	if (unset) {
		c2c->call_graph = false;
		return 0;
	}

	symbol_conf.use_callchain = true;
	c2c->call_graph = true;

	if (!arg)
		return 0;

	tok = strtok((char *)arg, ",");
	if (!tok)
		return -1;

	/* get the output mode */
	if (!strncmp(tok, "graph", strlen(arg)))
		callchain_param.mode = CHAIN_GRAPH_ABS;

	else if (!strncmp(tok, "flat", strlen(arg)))
		callchain_param.mode = CHAIN_FLAT;

	else if (!strncmp(tok, "fractal", strlen(arg)))
		callchain_param.mode = CHAIN_GRAPH_REL;

	else if (!strncmp(tok, "none", strlen(arg))) {
		callchain_param.mode = CHAIN_NONE;
		symbol_conf.use_callchain = false;

		return 0;
	}

	else
		return -1;

	/* get the min percentage */
	tok = strtok(NULL, ",");
	if (!tok)
		goto setup;

	callchain_param.min_percent = strtod(tok, &endptr);
	if (tok == endptr)
		return -1;

	/* get the print limit */
	tok2 = strtok(NULL, ",");
	if (!tok2)
		goto setup;

	if (tok2[0] != 'c') {
		callchain_param.print_limit = strtoul(tok2, &endptr, 0);
		tok2 = strtok(NULL, ",");
		if (!tok2)
			goto setup;
	}

	/* get the call chain order */
	if (!strncmp(tok2, "caller", strlen("caller")))
		callchain_param.order = ORDER_CALLER;
	else if (!strncmp(tok2, "callee", strlen("callee")))
		callchain_param.order = ORDER_CALLEE;
	else
		return -1;

	/* Get the sort key */
	tok2 = strtok(NULL, ",");
	if (!tok2)
		goto setup;
	if (!strncmp(tok2, "function", strlen("function")))
		callchain_param.key = CCKEY_FUNCTION;
	else if (!strncmp(tok2, "address", strlen("address")))
		callchain_param.key = CCKEY_ADDRESS;
	else
		return -1;
setup:
	if (callchain_register_param(&callchain_param) < 0) {
		fprintf(stderr, "Can't register callchain params\n");
		return -1;
	}
	return 0;
}

int cmd_c2c(int argc, const char **argv, const char *prefix __maybe_unused)
{
	char callchain_default_opt[] = "fractal,0.05,callee";
	struct perf_c2c c2c = {
		.tool = {
			.sample		 = perf_c2c__process_sample,
			.mmap2           = perf_event__process_mmap2,
			.mmap            = perf_event__process_mmap,
			.comm		 = perf_event__process_comm,
			.exit		 = perf_event__process_exit,
			.fork		 = perf_event__process_fork,
			.lost		 = perf_event__process_lost,
			.ordered_samples = true,
		},
	};
	const struct option c2c_options[] = {
	OPT_BOOLEAN('r', "raw_records", &c2c.raw_records, "dump raw events"),
	OPT_INCR('N', "node-info", &node_info,
		 "show extra node info in report (repeat for more info)"),
	OPT_INTEGER('c', "coalesce-level", &coalesce_level,
		 "how much coalescing for tid, pid, and ip is done (repeat for more coalescing)"),
	OPT_INCR('v', "verbose", &verbose,
		 "be more verbose (show counter open errors, etc)"),
	OPT_STRING('i', "input", &input_name, "file",
		   "the input file to process"),
	OPT_STRING('x', "field-separator", &symbol_conf.field_sep,
		   "separator",
		   "separator for columns, no spaces will be added"
		   " between columns '.' is reserved."),
	OPT_CALLBACK_DEFAULT('g', "call-graph", &c2c, "output_type,min_percent[,print_limit],call_order",
			     "Display callchains using output_type (graph, flat, fractal, or none) , min percent threshold, optional print limit, callchain order, key (function or address). "
			     "Default: fractal,0.5,callee,function", &opt_callchain_cb, callchain_default_opt),
	OPT_END()
	};
	const char * const c2c_usage[] = {
		"perf c2c {record|report}",
		NULL
	};

	argc = parse_options(argc, argv, c2c_options, c2c_usage,
			     PARSE_OPT_STOP_AT_NON_OPTION);
	if (!argc)
		usage_with_options(c2c_usage, c2c_options);

	if (!strncmp(argv[0], "rec", 3)) {
		return perf_c2c__record(argc, argv);
	} else if (!strncmp(argv[0], "rep", 3)) {
		return perf_c2c__report(&c2c);
	} else {
		usage_with_options(c2c_usage, c2c_options);
	}

	return 0;
}
