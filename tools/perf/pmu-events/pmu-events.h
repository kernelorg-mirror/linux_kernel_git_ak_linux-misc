#ifndef PMU_EVENTS_H
#define PMU_EVENTS_H

/*
 * Describe each PMU event. Each CPU has a table of these
 * events.
 */
struct pmu_event {
	const char *name;
	const char *event;
	const char *desc;
};

/*
 *
 * Map a CPU to its table of PMU events. The CPU Id is encoded in
 * in an arch-specific manner in the file tools/perf/arch/xxx/mapfile.
 * The cpuid is decoded/compared in arch_pmu_events_match_cpu().
 *
 * The  cpuid can contain any character other than the comma. It
 * could, for instance use hyphen to further break up the cpuid into
 * as "vendor-family-model-revision".
 */
struct pmu_events_map {
	const char *cpuid;
	const char *version;
	const char *type;		/* core, uncore etc */
	struct pmu_event *table;
};

/*
 * Global table mapping each known CPU for the architecture to its
 * table of PMU-Events.
 */
extern struct pmu_events_map pmu_events_map[];

#endif
