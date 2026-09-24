#include "structs.h"
#include "topology.h"
#include "vp_vec.h"
#include "event.h"
#include "config.h"
#include "metrics.h"
#include "log.h"
#include <string.h>
#include <limits.h>
#include <float.h>

extern struct topology topology;
extern time_t sim_time;
extern struct config config;

void path_attach_flow(struct vp_vec *path, struct flow *f, int record)
{
	struct link *l;
	vp_for (l, path) {
		vp_vec_append(&l->active_flows, f);
		if (record) {
			metric_observe(&metrics.link_contention, (long long) l->active_flows.length);
		}
	}
}

void path_detach_flow(struct vp_vec *path, struct flow *f, int record)
{
	struct link *l;
	int ind;
	vp_for (l, path) {
		ind = vp_vec_index(&l->active_flows, f);
		assert(ind > -1);
		vp_vec_remove(&l->active_flows, (size_t) ind);
		if (record) {
			metric_observe(&metrics.link_contention, (long long) l->active_flows.length);
		}
	}
}

/* A flow's rate: the smallest equal share across its links, in B/ms. Kept
 * fractional -- a slow link split several ways (12.5 B/ms at 100 Kbit/s)
 * would otherwise floor to a fraction of its real share. */
double path_min_bw(const struct vp_vec *path_list)
{
	struct link *aux;
	double link_min = DBL_MAX;
	double aux_bw;

	vp_for (aux, path_list) {
		assert(aux->active_flows.length > 0);
		aux_bw = aux->weight / (double) aux->active_flows.length;
		if (aux_bw < link_min) {
			link_min = aux_bw;
		}
	}
	assert(link_min > 0);
	return link_min;
}

time_t flow_recalc_makespan(struct flow *f, time_t cur_time)
{
	f->marked = 0;
	double new_bw = path_min_bw(&f->path);
	double prev_bw = f->min_bw;
	long long left_bytes;
	time_t delta_time = cur_time - f->prev_ts;
	/* Byte counts are long long: model payloads push nbytes far past a
	 * 32-bit int. Bytes sent at a fractional rate are floored to whole bytes.
	 * remaining_bytes must stay signed — it goes negative when a flow has
	 * already delivered everything it owed. */
	long long transferred_bytes = (long long) (f->min_bw * (double) delta_time);
	long long remaining_bytes = (long long) f->nbytes - transferred_bytes;

	time_t remaining_time;
	if (remaining_bytes > 0) {
		/* ceil(remaining / new_bw), without libm */
		remaining_time = (time_t) ((double) remaining_bytes / new_bw);
		if ((double) remaining_time * new_bw < (double) remaining_bytes) {
			remaining_time++;
		}
		f->min_bw = new_bw;
		f->nbytes = (size_t) remaining_bytes;
	} else {
		remaining_time = 0;
	}
	f->makespan = cur_time - f->start_time + remaining_time;
	f->prev_ts = cur_time;
	left_bytes = remaining_bytes > 0 ? remaining_bytes : 0;
	log_line("FLOW_RECALC", f->t->node, f->t, f->stage,
		"f%u sent %lldB left %lldB bw %.2f->%.2fB/ms eta %ldms",
		f->id, (long long) f->nbytes_total - left_bytes, left_bytes,
		prev_bw, f->min_bw, (long) remaining_time);

	return cur_time + remaining_time;
}

/*
 * Reschedule every flow that shares a link with the trigger flow's path.
 * The affected flows are read straight off the path's links (no queue scan).
 * Since the heap can't move a queued event, we use lazy invalidation: push a
 * fresh finish event with the recomputed time and flag the old one stale.
 */
static struct vp_vec resched = {0};

static void reschedule_gather(struct vp_vec *path, struct flow *trigger)
{
	struct link *l;
	struct flow *af;
	vp_for (l, path) {
		vp_for (af, &l->active_flows) {
			if (af != trigger && !af->marked) {
				af->marked = 1;
				vp_vec_append(&resched, af);
			}
		}
	}
}

void flows_reschedule(struct flow *trigger)
{
	struct flow *af;
	struct event *ne;
	time_t nt;

	reschedule_gather(&trigger->path, trigger);
	if (!topology.directed) {
		reschedule_gather(&trigger->path_aux, trigger);
	}
	while (resched.length) {
		af = vp_vec_pop(&resched);
		nt = flow_recalc_makespan(af, sim_time);	/* clears af->marked */
		af->finish_ev->stale = 1;					/* supersede old event */
		ne = event_alloc(FLOW_FINISH, nt);
		ne->data.f = af;
		af->finish_ev = ne;
		event_enqueue(ne);
	}
}

struct flow *flow_alloc(id_t orig,
			   id_t dest,
			   size_t nbytes,
			   struct task *t)
{
	static id_t flow_id = 0;
	struct flow *f = malloc(sizeof(struct flow));
	assert(f != NULL);
	memset(f, 0, sizeof(struct flow));

	f->nbytes = nbytes;
	f->nbytes_total = nbytes;
	f->t = t;
	f->stage = t ? t->stage : -1;
	f->control = nbytes == (size_t) config.control_bytes;
	f->id = flow_id++;
	if (config.routing == ROUTING_DYNAMIC) {
		path_resolve_dynamic(&topology, orig, dest, &f->path, 0);
	} else if (config.routing == ROUTING_HYBRID) {
		path_resolve_dynamic(&topology, orig, dest, &f->path, config.hop_penalty);
	} else if (config.routing == ROUTING_WIDEST) {
		path_resolve_widest(&topology, orig, dest, &f->path, 0);
	} else if (config.routing == ROUTING_WIDEST_HYBRID) {
		path_resolve_widest(&topology, orig, dest, &f->path, config.hop_penalty);
	} else {
		path_resolve(&topology, orig, dest, &f->path);
	}
	if (!f->path.length && orig != dest) {
		/* Workload generation drops READs across partitions, so this is
		 * a policy trying to reach a disconnected agent: fail loudly
		 * rather than start a flow that can never deliver. */
		fprintf(stderr, "flow %u->%u: no physical path\n", orig, dest);
		exit(1);
	}
	/*
	 * Undirected edges are shared half-duplex: a flow must also occupy the
	 * reverse-direction link of every physical hop it crosses. Derive that
	 * reverse from f->path rather than searching again, so path_aux is
	 * guaranteed to be its exact mirror (see path_reverse).
	 */
	if (!topology.directed) {
		path_reverse(&topology, &f->path, &f->path_aux);
	}
	return f;
}

void flow_dealloc(struct flow *f)
{
	vp_vec_free(&f->path);
	if (!topology.directed) {
		vp_vec_free(&f->path_aux);
	}
	free(f);
}

/*
 * Allocates a flow and schedules its FLOW_START after the path's fixed
 * propagation latency has elapsed, rather than instantly. Until then the
 * flow holds no slot in any link's active_flows, so it never distorts
 * other flows' bandwidth-sharing math during its own latency window;
 * flow_start/flow_recalc_makespan/flows_reschedule need no changes, they
 * simply run later than sim_time.
 */
void flow_enqueue(id_t orig, id_t dest, size_t nbytes, struct task *t)
{
	struct flow *f = flow_alloc(orig, dest, nbytes, t);
	struct event *ev = event_alloc(FLOW_START,
		sim_time + path_vec_latency(&f->path));
	ev->data.f = f;
	t->flow_rc++;
	event_enqueue(ev);
}
