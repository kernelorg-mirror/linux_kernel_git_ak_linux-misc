/* Check if we can set up all aliases and can read JSON files */
#include <stdlib.h>
#include "tests.h"
#include "pmu.h"
#include "evlist.h"
#include "parse-events.h"
#include "util/debug.h"

static struct perf_evlist *evlist;

static int num_events;
static int failed;

static int test__event(const char *pmu, const char *name)
{
	int ret;

	/* Not supported for now */
	if (strcmp(pmu, "cpu"))
		return 0;

	ret = parse_events(evlist, name);

	if (ret) {
		/*
		 * We only print on failure because common perf setups
		 * have events that cannot be parsed.
		 */
		fprintf(stderr, "invalid or unsupported event: '%s'\n", name);
		ret = 0;
		failed++;
	} else
		num_events++;
	return ret;
}

int test__aliases(void)
{
	int err;

	/* Download JSON files */
	/* XXX assumes perf is installed */
	/* For now user must manually download */
	if (0 && system("perf download > /dev/null") < 0) {
		/* Don't error out for this for now */
		fprintf(stderr, "perf download failed\n");
	}

	evlist = perf_evlist__new();
	if (evlist == NULL)
		return -ENOMEM;

	err = pmu_iterate_events(test__event);
	fprintf(stderr, " Parsed %d events :", num_events);
	if (failed > 0)
		pr_debug(" %d events failed", failed);
	perf_evlist__delete(evlist);
	return err;
}
