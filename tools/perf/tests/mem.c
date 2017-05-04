#include "util/mem-events.h"
#include "util/symbol.h"
#include "linux/perf_event.h"
#include "util/debug.h"
#include "tests.h"
#include <string.h>

static int check(union perf_mem_data_src data_src,
		  const char *string)
{
	char out[100];
	char failure[100];
	struct mem_info mi = { .data_src = data_src };

	int n;

	n = perf_mem__snp_scnprintf(out, sizeof out, &mi);
	n += perf_mem__lvl_scnprintf(out + n, sizeof out - n, &mi);
	snprintf(failure, sizeof failure, "unexpected %s", out);
	TEST_ASSERT_VAL(failure, !strcmp(string, out));
	return 0;
}

int test__mem(struct test *text __maybe_unused, int subtest __maybe_unused)
{
	int ret = 0;

	ret |= check(((union perf_mem_data_src) {
				.mem_lvl = PERF_MEM_LVL_HIT,
				.mem_lvl_num = 4 }), "N/AL4 hit");

	ret |= check(((union perf_mem_data_src) {
				.mem_lvl = PERF_MEM_LVL_HIT,
				.mem_lvl_num = 4,
				.mem_remote = 1	}), "N/ARemote L4 hit");

	ret |= check(((union perf_mem_data_src) {
				.mem_lvl = PERF_MEM_LVL_MISS,
				.mem_lvl_num = PERF_MEM_LVLNUM_PMEM }), "N/APMEM miss");

	ret |= check(((union perf_mem_data_src) {
				.mem_lvl = PERF_MEM_LVL_MISS,
				.mem_lvl_num = PERF_MEM_LVLNUM_PMEM,
				.mem_remote =1 }), "N/ARemote PMEM miss");

	ret |= check(((union perf_mem_data_src) {
				.mem_snoopx = PERF_MEM_SNOOPX_FWD,
				.mem_lvl = PERF_MEM_LVL_MISS,
				.mem_lvl_num = PERF_MEM_LVLNUM_RAM,
				.mem_remote = 1	}), "FwdRemote RAM miss");

	return ret;
}
