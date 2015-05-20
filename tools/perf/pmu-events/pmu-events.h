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
 * Map a CPU to its table of PMU events. The CPU is identified, in
 * an arch-specific manner, in arch_pmu_events_match_cpu(), by one
 * or more of the following attributes:
 *
 *	vendor, family, model, revision, type
 *
 * TODO: Split vfm into individual fields or leave it to architectures
 *	 to split it with an alternate delimiter like hyphen in the
 *	 mapfile?
 */
struct pmu_events_map {
	const char *vfm;		/* vendor, family, model */
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
