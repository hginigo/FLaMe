#ifndef _METRICS_H
#define _METRICS_H
#include "structs.h"

/*
 * Generic running-statistics accumulator: every metric tracked by the
 * simulator is one of these, fed by metric_observe() calls at the relevant
 * call sites. count doubles as "total number of observations" (e.g. total flow
 * count, if one observation is recorded per flow); min/max/avg summarize
 * the distribution of values observed, not just how many there were, and sum
 * is itself a figure when the value is a size (total traffic, total time).
 */
struct metric {
    const char *name;
    long long count;
    long long sum;
    long long min;
    long long max;
};

/*
 * Everything one run measures, in one place.
 *
 * Flow metrics are split by kind. Control messages (config.control_bytes)
 * and model transfers differ by four orders of magnitude in size, and a
 * control flow finishes in 0 ms, so pooling them made every average describe
 * neither population. All flow metrics are observed once per flow at
 * flow_finish, so they share one count: flows that completed.
 *
 * Task latency is measured per task type from the moment the node pulls the
 * task to the moment it moves on, so READ and WRITE durations are the cost of
 * each coherence operation, and BARRIER durations are time spent waiting for
 * the slowest node.
 */
struct run_metrics {
    struct metric task_ms[N_TASK_TYPES];    /* duration per task, by enum task_t */
    struct metric data_hops, ctrl_hops;
    struct metric data_bytes, ctrl_bytes;   /* per flow; .sum is the traffic */
    struct metric data_xfer_ms;             /* transmission time, latency excluded */
    struct metric data_bw;                  /* effective B/ms: bytes / transfer time */
    struct metric link_contention;          /* active flows, sampled per attach/detach */
    struct metric node_finish_ms;           /* one per node that drained its queue */
    long long tasks_total, tasks_done;
    long long reads_dropped;    /* overlay READs with no physical path; counted at
                                   workload generation, before metrics_init */
};

extern struct run_metrics metrics;

void metric_init(struct metric *m, const char *name);
void metric_observe(struct metric *m, long long value);
double metric_avg(const struct metric *m);
void metrics_init(void);

#endif // _METRICS_H
