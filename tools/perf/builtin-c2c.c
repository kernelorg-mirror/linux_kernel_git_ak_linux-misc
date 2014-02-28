#include "builtin.h"
#include "cache.h"

#include "util/evlist.h"
#include "util/parse-options.h"
#include "util/session.h"
#include "util/tool.h"
#include "util/debug.h"
#include "util/annotate.h"

#include <linux/compiler.h>
#include <linux/kernel.h>

struct perf_c2c {
	struct perf_tool tool;
	bool		 raw_records;
	struct hists	 hists;
};

enum { OP, LVL, SNP, LCK, TLB };

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

out:
	return err;
}

static int perf_c2c__init(struct perf_c2c *c2c)
{
	sort__mode = SORT_MODE__MEMORY;
	sort__wants_unique = 1;
	sort_order = "physid";

	if (setup_sorting() < 0) {
		pr_err("can not setup sorting\n");
		return -1;
	}

	hists__init(&c2c->hists);

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
