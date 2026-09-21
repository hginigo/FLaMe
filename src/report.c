#include "report.h"
#include "structs.h"
#include "metrics.h"
#include "config.h"
#include "policy.h"
#include "topology.h"
#include <string.h>

extern struct config config;
extern struct topology topology;
extern struct vp_vec nodes;
extern time_t sim_time;
extern enum policy_t policy;

static const char *routing_name(enum routing_mode r)
{
	switch (r) {
	case ROUTING_STATIC:        return "static";
	case ROUTING_DYNAMIC:       return "dynamic";
	case ROUTING_WIDEST:        return "widest";
	case ROUTING_HYBRID:        return "hybrid";
	case ROUTING_WIDEST_HYBRID: return "widest_hybrid";
	case ROUTING_DIJKSTRA:      return "dijkstra";
	}
	return "?";
}

static const char *backend_name(enum backend_mode b)
{
	switch (b) {
	case BACKEND_SYNTHETIC: return "synthetic";
	case BACKEND_PYTHON:    return "python";
	case BACKEND_TRACE:     return "trace";
	}
	return "?";
}

static const char *bytes_human(char *buf, size_t size, long long b)
{
	const char *unit[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	double v = (double) b;
	int u = 0;

	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	if (u == 0) snprintf(buf, size, "%lld B", b);
	else snprintf(buf, size, "%.2f %s", v, unit[u]);
	return buf;
}

/* Basename with any whitespace replaced, so it survives as a key=value token. */
static const char *token(char *buf, size_t size, const char *path)
{
	const char *base = strrchr(path, '/');
	size_t i;

	snprintf(buf, size, "%s", base ? base + 1 : path);
	for (i = 0; buf[i]; i++) {
		if (buf[i] == ' ' || buf[i] == '\t') buf[i] = '_';
	}
	return buf;
}

static size_t physical_links(void)
{
	size_t n = 0;
	for (size_t i = 0; i < topology.num_nodes; i++) {
		n += topology.adj_lists[i].length;
	}
	return topology.directed ? n : n / 2;
}

static int rounds(void)
{
	int r = 0;
	for (const struct topology *v = topology.virt_topo; v; v = v->virt_topo) {
		r++;
	}
	return r;
}

static double pct(long long part, long long whole)
{
	return whole > 0 ? 100.0 * (double) part / (double) whole : 0.0;
}

static void row(const struct metric *m)
{
	if (m->count == 0) return;
	out("  %-22s %9lld %9lld %11.2f %9lld\n",
		m->name, m->count, m->min, metric_avg(m), m->max);
}

void report_print(const char *topology_file, long wall_ms, long cpu_ms)
{
	const struct run_metrics *M = &metrics;
	const char *pol = policy_name(policy) ? policy_name(policy) : "FLOW_DEBUG";
	long long node_time = (long long) nodes.length * (long long) sim_time;
	long long busy = 0, idle;
	long long nodes_done = M->node_finish_ms.count;
	char topo[256], a[32], b[32];

	for (int i = 0; i < N_TASK_TYPES; i++) {
		busy += M->task_ms[i].sum;
	}
	idle = node_time - busy;

	out("\n");
	out("run        %s (%d)  routing %s  backend %s  barriers %s\n",
		pol, (int) policy, routing_name(config.routing),
		backend_name(config.backend_mode), config.barriers ? "on" : "off");
	out("topology   %s  %zu nodes  %zu links  %d rounds",
		token(topo, sizeof(topo), topology_file), (size_t) nodes.length,
		physical_links(), rounds());
	if (config.rounds_file[0]) {
		out(" from %s", config.rounds_file);
	}
	out("\n");
	out("time       makespan %ld ms  wall %ld ms  cpu %ld ms\n",
		(long) sim_time, wall_ms, cpu_ms);
	out("tasks      %lld/%lld done", M->tasks_done, M->tasks_total);
	out("  nodes finished %lld/%zu\n", nodes_done, (size_t) nodes.length);
	if (M->node_finish_ms.count) {
		out("stragglers node finish min %lld  avg %.1f  max %lld ms\n",
			M->node_finish_ms.min, metric_avg(&M->node_finish_ms),
			M->node_finish_ms.max);
	}
	out("node time  train %.1f%%  read %.1f%%  write %.1f%%  barrier %.1f%%  idle %.1f%%\n",
		pct(M->task_ms[TRAIN].sum, node_time), pct(M->task_ms[READ].sum, node_time),
		pct(M->task_ms[WRITE].sum, node_time), pct(M->task_ms[BARRIER].sum, node_time),
		pct(idle, node_time));
	out("traffic    data %s in %lld flows  control %s in %lld flows\n",
		bytes_human(a, sizeof(a), M->data_bytes.sum), M->data_bytes.count,
		bytes_human(b, sizeof(b), M->ctrl_bytes.sum), M->ctrl_bytes.count);

	if (M->tasks_done < M->tasks_total) {
		out("WARNING    %lld of %lld tasks never completed and %lld of %zu nodes never"
			" finished; the makespan covers only the work that ran\n",
			M->tasks_total - M->tasks_done, M->tasks_total,
			(long long) nodes.length - nodes_done, (size_t) nodes.length);
		fprintf(stderr, "WARNING: %lld of %lld tasks never completed\n",
			M->tasks_total - M->tasks_done, M->tasks_total);
	}

	out("\n  %-22s %9s %9s %11s %9s\n", "", "count", "min", "avg", "max");
	row(&M->task_ms[TRAIN]);
	row(&M->task_ms[READ]);
	row(&M->task_ms[WRITE]);
	row(&M->task_ms[BARRIER]);
	row(&M->data_bytes);
	row(&M->data_xfer_ms);
	row(&M->data_bw);
	row(&M->data_hops);
	row(&M->ctrl_bytes);
	row(&M->ctrl_hops);
	row(&M->link_contention);
	out("\n");

	out("result policy=%s policy_id=%d routing=%s backend=%s barriers=%s"
		" topology=%s nodes=%zu links=%zu rounds=%d model_bytes=%lld"
		" makespan_ms=%ld wall_ms=%ld cpu_ms=%ld"
		" tasks_done=%lld tasks_total=%lld nodes_finished=%lld"
		" node_finish_min_ms=%lld node_finish_avg_ms=%.2f node_finish_max_ms=%lld"
		" train_ms_avg=%.2f read_ms_avg=%.2f write_ms_avg=%.2f barrier_ms_avg=%.2f"
		" train_pct=%.2f read_pct=%.2f write_pct=%.2f barrier_pct=%.2f idle_pct=%.2f"
		" data_flows=%lld data_bytes=%lld data_xfer_ms_avg=%.2f data_bw_avg=%.2f"
		" data_hops_avg=%.3f ctrl_flows=%lld ctrl_bytes=%lld ctrl_hops_avg=%.3f"
		" link_contention_avg=%.3f\n",
		pol, (int) policy, routing_name(config.routing),
		backend_name(config.backend_mode), config.barriers ? "on" : "off",
		topo, (size_t) nodes.length, physical_links(), rounds(),
		M->data_bytes.count ? M->data_bytes.max : 0LL,
		(long) sim_time, wall_ms, cpu_ms,
		M->tasks_done, M->tasks_total, nodes_done,
		M->node_finish_ms.count ? M->node_finish_ms.min : 0LL,
		metric_avg(&M->node_finish_ms),
		M->node_finish_ms.count ? M->node_finish_ms.max : 0LL,
		metric_avg(&M->task_ms[TRAIN]), metric_avg(&M->task_ms[READ]),
		metric_avg(&M->task_ms[WRITE]), metric_avg(&M->task_ms[BARRIER]),
		pct(M->task_ms[TRAIN].sum, node_time), pct(M->task_ms[READ].sum, node_time),
		pct(M->task_ms[WRITE].sum, node_time), pct(M->task_ms[BARRIER].sum, node_time),
		pct(idle, node_time),
		M->data_bytes.count, M->data_bytes.sum, metric_avg(&M->data_xfer_ms),
		metric_avg(&M->data_bw), metric_avg(&M->data_hops),
		M->ctrl_bytes.count, M->ctrl_bytes.sum, metric_avg(&M->ctrl_hops),
		metric_avg(&M->link_contention));
}
