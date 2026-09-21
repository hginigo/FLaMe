#include "metrics.h"
#include <limits.h>

struct run_metrics metrics;

void metric_init(struct metric *m, const char *name)
{
    m->name = name;
    m->count = 0;
    m->sum = 0;
    m->min = LLONG_MAX;
    m->max = LLONG_MIN;
}

void metric_observe(struct metric *m, long long value)
{
    m->count++;
    m->sum += value;
    if (value < m->min) {
        m->min = value;
    }
    if (value > m->max) {
        m->max = value;
    }
}

double metric_avg(const struct metric *m)
{
    return m->count > 0 ? (double) m->sum / (double) m->count : 0.0;
}

void metrics_init(void)
{
    metric_init(&metrics.task_ms[READ], "task read (ms)");
    metric_init(&metrics.task_ms[WRITE], "task write (ms)");
    metric_init(&metrics.task_ms[TRAIN], "task train (ms)");
    metric_init(&metrics.task_ms[BARRIER], "task barrier (ms)");
    metric_init(&metrics.data_hops, "data flow hops");
    metric_init(&metrics.data_bytes, "data flow size (B)");
    metric_init(&metrics.data_xfer_ms, "data flow xfer (ms)");
    metric_init(&metrics.data_bw, "data flow bw (B/ms)");
    metric_init(&metrics.ctrl_hops, "ctrl flow hops");
    metric_init(&metrics.ctrl_bytes, "ctrl flow size (B)");
    metric_init(&metrics.link_contention, "link contention");
    metric_init(&metrics.node_finish_ms, "node finish (ms)");
    metrics.tasks_total = 0;
    metrics.tasks_done = 0;
}
