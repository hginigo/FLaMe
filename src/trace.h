#ifndef _TRACE_H
#define _TRACE_H
#include "structs.h"
#include "topology.h"

/*
 * Execution traces from a real DFL framework run (config `rounds_file`).
 *
 * One line per (head, round), e.g.
 *   head_ID: 0 round ID: 0 neighbours: [0, 1, 19] loss: 359.09 acc: 0.1557
 *   f1: 0.0885 training_time: 0.4813
 *
 * The trace supplies the two things the simulator otherwise invents:
 *   - WHO TALKS TO WHOM per round. `neighbours` (which includes the head
 *     itself) becomes one virtual overlay topology per round, chained onto
 *     the physical graph exactly as a .tpl's `-` sections would be. The
 *     .tpl then only has to carry the physical substrate.
 *   - HOW LONG WORK TAKES. training_time, and an aggregation time when the
 *     trace records one, replace the flat config train_time/aggregate_time
 *     that the synthetic backend charges every node in every round.
 *
 * Times in the trace are WALL-CLOCK SECONDS and are converted to the
 * simulator's integer sim-ms on load. Fields the trace omits fall back to
 * the corresponding config value, so a trace without aggregation times (as
 * the first ones are) still runs.
 *
 * Field names are matched loosely -- `neighbors`/`neighbours`,
 * `aggregation_time`/`aggregate_time`/`agg_time` -- so a trace whose
 * producer renames a column keeps working. Unknown fields are ignored.
 */

struct trace_entry {
	long long train_ms;	/* -1 if the line carried no training time */
	long long agg_ms;	/* -1 if the line carried no aggregation time */
	double loss, acc;
	int present;		/* 0 if the trace has no line for this pair */
};

/* Parse `path`. Returns 0, or -1 with a message on stderr. */
int trace_load(const char *path);
void trace_free(void);

int trace_nodes(void);
int trace_rounds(void);
const struct trace_entry *trace_lookup(id_t node, int round);

/*
 * Round bookkeeping. The backend protocol never tells a backend which round
 * it is in, but the workload gives every node exactly one TRAIN per round in
 * round order, so counting a node's TRAIN calls recovers it.
 * trace_take_round() consumes one (call it per TRAIN); trace_cur_round()
 * reports the round a node is presently in, for the WRITE that follows.
 */
int trace_take_round(id_t node);
int trace_cur_round(id_t node);

/*
 * Replace t's virtual overlay chain with one topology per trace round, built
 * from the neighbour lists. Links get weight/latency from `weight` and
 * config.latency_ms; neither is ever used for bandwidth, since every flow is
 * routed over the physical graph -- the overlays only say which pairs talk.
 */
void trace_build_rounds(struct topology *t, int weight);

#endif /* _TRACE_H */
