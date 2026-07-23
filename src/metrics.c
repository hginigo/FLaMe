#include "metrics.h"
#include "structs.h"
#include <limits.h>

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

void metric_print(const struct metric *m)
{
    if (m->count == 0) {
        out("metric %-20s count=0 (no samples)\n", m->name);
        return;
    }
    out("metric %-20s count=%-8lld min=%-8lld max=%-8lld avg=%.3f\n",
        m->name, m->count, m->min, m->max, metric_avg(m));
}
