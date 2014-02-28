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
	struct hists	 hists;

	/* stats */
	struct c2c_stats	stats;
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

static int perf_c2c__process_events(struct perf_session *session,
				    struct perf_c2c *c2c)
{
	int err = -1;

	err = perf_session__process_events(session, &c2c->tool);
	if (err) {
		pr_err("Failed to process count events, error %d\n", err);
		goto err;
	}

err:
	return err;
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

int cmd_c2c(int argc, const char **argv, const char *prefix __maybe_unused)
{
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
	OPT_INCR('v', "verbose", &verbose,
		 "be more verbose (show counter open errors, etc)"),
	OPT_STRING('i', "input", &input_name, "file",
		   "the input file to process"),
	OPT_STRING('x', "field-separator", &symbol_conf.field_sep,
		   "separator",
		   "separator for columns, no spaces will be added"
		   " between columns '.' is reserved."),
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
