#include "structs.h"
#include "topology.h"
#include "vp_vec.h"
#include <string.h>
#include <limits.h>

extern struct topology topology;
extern time_t sim_time;

#define MARK_ATTACH 1
#define MARK_DETACH 2

void path_attach_flow(struct vp_vec *path, struct flow *f)
{
	struct link *l;
	struct flow *aux;
	vp_for (l, path) {
		vp_for (aux, &l->active_flows) {
			aux->marked = MARK_ATTACH;
		}
		vp_vec_append(&l->active_flows, f);
	}
}

void path_detach_flow(struct vp_vec *path, struct flow *f)
{
	struct link *l;
	struct flow *aux;
	int ind;
	vp_for (l, path) {
		vp_for (aux, &l->active_flows) {
			aux->marked = MARK_DETACH;
		}
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
	int transferred_bytes = f->min_bw * delta_time;
	int remaining_bytes = f->nbytes - transferred_bytes;
	
	time_t remaining_time;
	if (remaining_bytes > 0) {
		//remaining_time = remaining_bytes / (new_bw);
		remaining_time = (remaining_bytes + new_bw - 1) / new_bw;
		f->min_bw = new_bw;
		f->nbytes = remaining_bytes;
	} else {
		remaining_time = 0;
	}
	f->makespan = cur_time - f->start_time + remaining_time;
	f->prev_ts = cur_time;
	dbg("flow %d recalc tB %d (%d) dt[%d] rB %d (%d) rt[%d]\n",
		f->id, transferred_bytes, dbg_prev_bw/Bpms, delta_time, remaining_bytes, f->min_bw/Bpms, remaining_time);

	return cur_time + remaining_time;
}

int is_marked(const void *elem)
{
	const struct event *ev = (const struct event *) elem;
	if (ev->type == FLOW_FINISH) {
		return ev->data.f->marked;
	}
	return 0;
}

struct flow *flow_alloc(id_t orig,
			   id_t dest,
			   size_t nbytes,
			   struct task *t)
{
	static id_t flow_id = 0;
	struct flow *f = malloc(sizeof(struct flow));
	assert(f != NULL);
	memset(f, 0, sizeof(struct flow));//calloc(1, sizeof(struct flow));
	dbg("%u -> %u\n", orig, dest);
	
	f->nbytes = nbytes;
	f->t = t;
	f->id = flow_id++;
	path_resolve(&topology, orig, dest, &f->path);
	if (!topology.directed) {
		path_resolve(&topology, dest, orig, &f->path_aux);
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
