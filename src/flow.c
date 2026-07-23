#include "structs.h"
#include "topology.h"
#include "vp_vec.h"
#include "event.h"
#include "config.h"
#include <string.h>
#include <limits.h>

extern struct topology topology;
extern time_t sim_time;
extern struct config config;

void path_attach_flow(struct vp_vec *path, struct flow *f)
{
	struct link *l;
	vp_for (l, path) {
		vp_vec_append(&l->active_flows, f);
	}
}

void path_detach_flow(struct vp_vec *path, struct flow *f)
{
	struct link *l;
	int ind;
	vp_for (l, path) {
		ind = vp_vec_index(&l->active_flows, f);
		assert(ind > -1);
		vp_vec_remove(&l->active_flows, (size_t) ind);
	}
}

int path_min_bw(const struct vp_vec *path_list)
{
	struct link *aux;
	int link_min = INT_MAX;
	int aux_bw;

	vp_for (aux, path_list) {
		assert(aux->active_flows.length > 0);
		aux_bw = aux->weight / aux->active_flows.length;
		if (aux_bw < link_min) {
			link_min = aux_bw;
		}
	}
	return link_min > 0 ? link_min : 1;
}

time_t flow_recalc_makespan(struct flow *f, time_t cur_time)
{
	f->marked = 0;
	int new_bw = path_min_bw(&f->path);
	int dbg_prev_bw = f->min_bw;
	time_t delta_time = cur_time - f->prev_ts;
	/* Byte counts are widened: min_bw (B/ms) * delta_time overflows a 32-bit
	 * int once a flow lives long enough (~2^31 / 6550 B/ms ≈ 5 min of sim
	 * time on a weight-50 link), and model payloads push nbytes far past
	 * block-sized transfers. remaining_bytes must stay signed — it goes
	 * negative when a flow has already delivered everything it owed. */
	long long transferred_bytes = (long long) f->min_bw * delta_time;
	long long remaining_bytes = (long long) f->nbytes - transferred_bytes;

	time_t remaining_time;
	if (remaining_bytes > 0) {
		remaining_time = (remaining_bytes + new_bw - 1) / new_bw;
		f->min_bw = new_bw;
		f->nbytes = (size_t) remaining_bytes;
	} else {
		remaining_time = 0;
	}
	f->makespan = cur_time - f->start_time + remaining_time;
	f->prev_ts = cur_time;
	dbg("flow %u recalc tB %lld (%d) dt[%ld] rB %lld (%d) rt[%ld]\n",
		f->id, transferred_bytes, dbg_prev_bw/Bpms, delta_time, remaining_bytes, f->min_bw/Bpms, remaining_time);

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
	dbg("%u -> %u\n", orig, dest);
	
	f->nbytes = nbytes;
	f->t = t;
	f->id = flow_id++;
	if (config.routing == ROUTING_DYNAMIC) {
		path_resolve_dynamic(&topology, orig, dest, &f->path);
		if (!topology.directed) {
			path_resolve_dynamic(&topology, dest, orig, &f->path_aux);
		}
	} else if (config.routing == ROUTING_WIDEST) {
		path_resolve_widest(&topology, orig, dest, &f->path);
		if (!topology.directed) {
			path_resolve_widest(&topology, dest, orig, &f->path_aux);
		}
	} else {
		path_resolve(&topology, orig, dest, &f->path);
		if (!topology.directed) {
			path_resolve(&topology, dest, orig, &f->path_aux);
		}
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
	event_enqueue(ev);
}
