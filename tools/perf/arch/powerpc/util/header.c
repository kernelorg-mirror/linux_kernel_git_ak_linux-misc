#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../util/header.h"
#include "../../util/util.h"

#define mfspr(rn)       ({unsigned long rval; \
			 asm volatile("mfspr %0," __stringify(rn) \
				      : "=r" (rval)); rval; })

#define SPRN_PVR        0x11F	/* Processor Version Register */
#define PVR_VER(pvr)    (((pvr) >>  16) & 0xFFFF) /* Version field */
#define PVR_REV(pvr)    (((pvr) >>   0) & 0xFFFF) /* Revison field */

int
get_cpuid(char *buffer, size_t sz)
{
	unsigned long pvr;
	int nb;

	pvr = mfspr(SPRN_PVR);

	nb = scnprintf(buffer, sz, "%lu,%lu$", PVR_VER(pvr), PVR_REV(pvr));

	/* look for end marker to ensure the entire data fit */
	if (strchr(buffer, '$')) {
		buffer[nb-1] = '\0';
		return 0;
	}
	return -1;
}

static char *
get_cpu_str(void)
{
        char *bufp;

        if (asprintf(&bufp, "%.8lx", mfspr(SPRN_PVR)) < 0)
                bufp = NULL;

        return bufp;
}

/*
 * Return TRUE if the CPU identified by @vfm, @version, and @type
 * matches the current CPU.  vfm refers to [Vendor, Family, Model],
 *
 * Return FALSE otherwise.
 *
 * For Powerpc, we only compare @version to the processor PVR.
 */
bool arch_pmu_events_match_cpu(const char *vfm __maybe_unused,
				const char *version,
				const char *type __maybe_unused)
{
	char *cpustr;
	bool rc;

	cpustr = get_cpu_str();
	rc = !strcmp(version, cpustr);
	free(cpustr);

	return rc;
}
