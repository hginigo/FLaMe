#ifndef _METRICS_H
#define _METRICS_H

/*
 * Generic running-statistics accumulator: every metric tracked by the
 * simulator (flow hop counts, link contention, future ones) is one of
 * these, fed by metric_observe() calls scattered at the relevant call
 * sites. count doubles as "total number of observations" (e.g. total flow
 * count, if one observation is recorded per flow); min/max/avg summarize
 * the distribution of values observed, not just how many there were.
 */
struct metric {
    const char *name;
    long long count;
    long long sum;
    long long min;
    long long max;
};

void metric_init(struct metric *m, const char *name);
void metric_observe(struct metric *m, long long value);
double metric_avg(const struct metric *m);
void metric_print(const struct metric *m);

#endif // _METRICS_H
