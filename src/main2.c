// USE_VP_LIST_QUEUE / USE_VP_LIST are build-wide flags now (see Makefile
// FEATURES) — every .c file is a separate translation unit, so defining
// them here would only be visible within main2.c and desync the layout
// that other files (e.g. event.c) compile against.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include "structs.h"
#include "gen.h"
#include "topology.h"
#include "workloads.h"
#include "config.h"
#include "event.h"
#include "flow.h"
#define VP_VEC_IMPLEMENTATION
#include "vp_vec.h"
#define VP_LIST_IMPLEMENTATION
#include "vp_list.h"

#define ETH_LATENCY 40

struct topology topology = {0};
struct vp_vec nodes;
struct vp_vec blocks;
#ifdef USE_VP_LIST_QUEUE
struct vp_list event_queue = {0};
#else
struct vp_vec event_queue = {0};
#endif
time_t sim_time;
enum policy_t policy; // = M_COPY_UPD;
long long total_hops = 0;


void tasks_enqueue(struct vp_vec *task_list)
{
	const struct task *t;
	struct node *n;
	vp_for (t, task_list) {
		n = t->node;
#ifdef USE_VP_LIST
		vp_list_append(&n->task_queue, t);
#else
		vp_vec_append(&n->task_queue, t);
#endif
	}
}

void block_size_update(struct block *b, int diff_size)
{
	if ((long long) (b->size + diff_size) < 0) {
		b->size = 1;
	} else {
		b->size += diff_size;
	}
}

struct node *min_path(const struct vp_vec *nodes,
					  id_t orig,
					  int *weight,
					  int *hops)
{
	struct node *n_min, *n_iter;
	int w_iter, w_min = INT_MAX;
	int h_iter, h_min;
	
	assert(nodes->length != 0);

	vp_for (n_iter, nodes) {
		w_iter = dijkstra(&topology, orig, n_iter->id, &h_iter);
		if (w_iter < w_min) {
			n_min = n_iter;
			w_min = w_iter;
			h_min = h_iter;
		}
	}
	*weight = w_min;
	*hops = h_min;
	return n_min;
}

struct node *max_path(const struct vp_vec *nodes,
					  id_t orig,
					  int *weight,
					  int *hops)
{
	struct node *n_max, *n_iter;
	int w_iter, w_max = INT_MIN;
	int h_iter, h_max;
	
	assert(nodes->length != 0);
	vp_for (n_iter, nodes) {
		w_iter = dijkstra(&topology, orig, n_iter->id, &h_iter);
		if (w_iter > w_max) {
			n_max = n_iter;
			w_max = w_iter;
			h_max = h_iter;
		}
	}
	*weight = w_max;
	*hops = h_max;
	return n_max;
}

void vp_vec_filter(struct vp_vec *dest, struct vp_vec *orig, int (*comp)(const void *))
{
	void *aux;
	for (size_t i = 0; i < orig->length; i++) {
		aux = vp_vec_get(orig, i);
		if (comp(aux)) {
			vp_vec_remove(orig, i--);
			vp_vec_append(dest, aux);
		}
	}
}

void vp_vec_list_filter(struct vp_vec *dest, struct vp_list *orig, int (*comp)(const void *))
{
	const void *aux;
	struct vp_list_node *next;
	for (struct vp_list_node *i = orig->first; i != NULL && i->status != VP_LIST_NODE_FREE;) {
		aux = i->item;
		next = i->next;
		if (comp(aux)) {
			vp_list_remove_at(orig, i);
			vp_vec_append(dest, aux);
		}
		i = next;
	}
}

int global_directory;


void ev_recalc_active_flows()
{
	static struct vp_vec marked = {0};
	struct event *e;
	time_t old_disp;
	
#ifdef USE_VP_LIST_QUEUE
	vp_vec_list_filter(&marked, &event_queue, is_marked);
#else
	vp_vec_filter(&marked, &event_queue, is_marked);
#endif
	while (marked.length != 0) {
		e = vp_vec_pop(&marked);
		old_disp = e->dispatch_time;
		e->dispatch_time = flow_recalc_makespan(e->data.f, sim_time);
		event_enqueue(e);
	}
}

void flow_start(const struct event *ev)
{
	struct flow *f = ev->data.f;
	struct task *t = f->t;

	f->start_time = sim_time;
	f->prev_ts = sim_time;
	path_attach_flow(&f->path, f);
	if (!topology.directed) {
		path_attach_flow(&f->path_aux, f);
	}
	f->min_bw = path_min_bw(&f->path);
	f->makespan = f->nbytes / (f->min_bw);
	dbg("flow %d start rB %d (%d) rt[%d]\n",
		f->id, f->nbytes, f->min_bw/Bpms, f->makespan);

	ev_recalc_active_flows();
	
	t->flow_rc++;
	f->finish_ev = event_alloc(FLOW_FINISH, f->start_time+f->makespan);
	f->finish_ev->data.f = f;
	event_enqueue(f->finish_ev);
}

void flow_finish(const struct event *ev)
{
	struct flow *f = ev->data.f;
	struct task *t = f->t;
	struct event *stage_next;

	path_detach_flow(&f->path, f);
	if (!topology.directed) {
		path_detach_flow(&f->path_aux, f);
	}
	ev_recalc_active_flows();
	
	t->flow_rc--;
	if (t->flow_rc == 0) {
		stage_next = event_alloc(STAGE_NEXT, sim_time);
		stage_next->data.t = t;
		event_enqueue(stage_next);
	}
	flow_dealloc(f);
}

void dispatcher_init(struct vp_vec *node_list)
{
	struct node *n;
	struct event *ev;
	
	vp_for (n, node_list) {
		if (n->task_queue.length > 0) {
			ev = event_alloc(PULL_TASK, 0);
			ev->data.n = n;
			event_enqueue(ev);
		}
	}
}

void flow_dbg(struct task *t)
{
	struct event *ev;
	struct node *orig, *dest;
	if (t->stage == 1) return;
	
	orig = vp_vec_get(&nodes, 0);
	dest = vp_vec_get(&nodes, 1);
	ev = event_alloc(FLOW_START, sim_time);
	ev->data.f = flow_alloc(orig->id, dest->id, 50, t);
	event_enqueue(ev);
}

#define TRAIN_DEF_MS 500
#define BARRIER_DEF_MS 0
void scu_stage(struct task *t)
{
	struct event *ev;
	struct node *orig, *dest;
	switch (t->type) {
	case READ:
		orig = t->node;
		dest = vp_vec_get(&t->block->owners, 0);
		if (orig->id == dest->id) {
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = orig;
			event_enqueue(ev);
			return;
		}
		switch (t->stage) {
		case 0:
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(orig->id, dest->id, 40, t);
			event_enqueue(ev);
		break;
		case 1:
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(dest->id, orig->id, t->block->size, t);
			event_enqueue(ev);
		break;
		case 2:
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = t->node;
			event_enqueue(ev);
		break;
		default:
		break;
		}
	break;

	case WRITE:
		orig = t->node;
		dest = vp_vec_get(&t->block->owners, 0);
		if (orig->id == dest->id) {
			block_size_update(t->block, t->size);
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = orig;
			event_enqueue(ev);
			return;
		}
		switch (t->stage) {
		case 0:
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(orig->id, dest->id, 40, t);
			event_enqueue(ev);
		break;
		case 1:
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(dest->id, orig->id, t->block->size, t);
			event_enqueue(ev);
		break;
		case 2:
			block_size_update(t->block, t->size);
			ev = event_alloc(STAGE_NEXT, sim_time);
			ev->data.t = t;
			event_enqueue(ev);
		break;
		case 3:
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(orig->id, dest->id, t->block->size, t);
			event_enqueue(ev);
		break;
		case 4:
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = orig;
			event_enqueue(ev);
		break;
		}
	break;
	case TRAIN:
		orig = t->node;
		ev = event_alloc(PULL_TASK, sim_time + TRAIN_DEF_MS);
		ev->data.n = orig;
		event_enqueue(ev);
	break;
	case BARRIER:
		ev = event_alloc(ROUND_BARRIER, sim_time + BARRIER_DEF_MS);
		ev->data.t = t;
		event_enqueue(ev);
	break;
	default:
	break;
	}
	t->stage++;
}

void scm_stage(struct task *t)
{
	struct event *ev;
	struct node *orig, *dest;

	orig = t->node;
	switch (t->type) {
	case READ:
		dest = vp_vec_get(&t->block->owners, 0);
		if (orig->id == dest->id) {
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = orig;
			event_enqueue(ev);
			return;
		}
		switch (t->stage) {
		case 0:
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(orig->id, dest->id, 40, t);
			event_enqueue(ev);
		break;
		case 1:
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(dest->id, orig->id, t->block->size, t);
			event_enqueue(ev);
		break;
		case 2:
			vp_vec_pop(&t->block->owners);
			vp_vec_append(&t->block->owners, orig);
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = t->node;
			event_enqueue(ev);
		break;
		}
	break;
	case WRITE:
		dest = vp_vec_get(&t->block->owners, 0);
		if (orig->id == dest->id) {
			block_size_update(t->block, t->size);
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = orig;
			event_enqueue(ev);
			return;
		}
		switch (t->stage) {
		case 0:
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(orig->id, dest->id, 40, t);
			event_enqueue(ev);
		break;
		case 1:
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(dest->id, orig->id, t->block->size, t);
			event_enqueue(ev);
		break;
		case 2:
			block_size_update(t->block, t->size);
			ev = event_alloc(STAGE_NEXT, sim_time);
			ev->data.t = t;
			event_enqueue(ev);
		break;
		case 3:
			vp_vec_pop(&t->block->owners);
			vp_vec_append(&t->block->owners, orig);
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = t->node;
			event_enqueue(ev);
		break;
		}
	break;
	case TRAIN:
		ev = event_alloc(PULL_TASK, sim_time + TRAIN_DEF_MS);
		ev->data.n = t->node;
		event_enqueue(ev);
	break;
	case BARRIER:
		ev = event_alloc(ROUND_BARRIER, sim_time + BARRIER_DEF_MS);
		ev->data.t = t;
		event_enqueue(ev);
	break;
	default:
	break;
	}
	t->stage++;
}

struct node *shortest_path(const struct vp_vec *nl,
	const struct node *orig,
	const struct topology *t)
{
	struct node *min = NULL;
	int hops, min_hops = INT_MAX;
	struct node *aux;

	vp_for (aux, nl) {
		hops = path_hops(t, orig->id, aux->id);
		//printf("hops: %d\n", hops);
		if (hops < min_hops) {
			min_hops = hops;
			min = aux;
		}
	}
	return min;
}

void mcm_stage(struct task *t)
{
	struct event *ev;
	struct node *orig = t->node;
	struct node *dest;
	switch (t->type) {
	case READ:
		// local
		if (vp_vec_exists(&t->block->owners, orig)) {
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = orig;
			event_enqueue(ev);
			return;
		}
		dest = vp_vec_get(&t->block->owners, 0);
		// remote
		switch (t->stage) {
		case 0:
			dest = shortest_path(&t->block->owners, orig, &topology);
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(orig->id, dest->id, 40, t);
			event_enqueue(ev);
		break;
		case 1:
			dest = shortest_path(&t->block->owners, orig, &topology);
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(dest->id, orig->id, t->block->size, t);
			event_enqueue(ev);
		break;
		case 2:
			vp_vec_append(&t->block->owners, orig);
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = t->node;
			event_enqueue(ev);
		break;
		}
	break;
	case WRITE:
	// local
		if (vp_vec_exists(&t->block->owners, orig)) {
			switch (t->stage) {
			case 0:
				vp_for (dest, &t->block->owners) {
					if (dest->id == orig->id) {
						continue;
					}
					ev = event_alloc(FLOW_START, sim_time);
					ev->data.f = flow_alloc(orig->id, dest->id, 40, t);
					event_enqueue(ev);
				}
			break;
			case 1:
				block_size_update(t->block, t->size);
				ev = event_alloc(STAGE_NEXT, sim_time);
				ev->data.t = t;
				event_enqueue(ev);
			break;
			case 2:
				t->block->owners.length = 0;
				vp_vec_append(&t->block->owners, orig);
				ev = event_alloc(PULL_TASK, sim_time);
				ev->data.n = t->node;
				event_enqueue(ev);
			break;
			}
		break;
		}
	// remote
		switch (t->stage) {
		case 0:
			dest = shortest_path(&t->block->owners, orig, &topology);
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(orig->id, dest->id, 40, t);
			event_enqueue(ev);
		break;
		case 1:
			dest = shortest_path(&t->block->owners, orig, &topology);
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(dest->id, orig->id, t->block->size, t);
			event_enqueue(ev);
		break;
		case 2:
			vp_for (dest, &t->block->owners) {
				ev = event_alloc(FLOW_START, sim_time);
				ev->data.f = flow_alloc(orig->id, dest->id, 40, t);
				event_enqueue(ev);
			}
		break;
		case 3:
			block_size_update(t->block, t->size);
			ev = event_alloc(STAGE_NEXT, sim_time);
			ev->data.t = t;
			event_enqueue(ev);
		break;
		case 4:
			t->block->owners.length = 0;
			vp_vec_append(&t->block->owners, orig);
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = t->node;
			event_enqueue(ev);
		break;
		}
	break;
	case TRAIN:
		ev = event_alloc(PULL_TASK, sim_time + TRAIN_DEF_MS);
		ev->data.n = t->node;
		event_enqueue(ev);
	break;
	case BARRIER:
		ev = event_alloc(ROUND_BARRIER, sim_time + BARRIER_DEF_MS);
		ev->data.t = t;
		event_enqueue(ev);
	break;
	}
	t->stage++;
}

void mcu_stage(struct task *t)
{
	struct event *ev;
	struct node *orig = t->node;
	struct node *dest;
	switch (t->type) {
	case READ:
	// local
		if (vp_vec_exists(&t->block->owners, orig)) {
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = orig;
			event_enqueue(ev);
			return;
		}
		// remote
		switch (t->stage) {
		case 0:
			dest = shortest_path(&t->block->owners, orig, &topology);
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(orig->id, dest->id, 40, t);
			event_enqueue(ev);
		break;
		case 1:
			dest = shortest_path(&t->block->owners, orig, &topology);
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(dest->id, orig->id, t->block->size, t);
			event_enqueue(ev);
		break;
		case 2:
			vp_vec_append(&t->block->owners, orig);
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = t->node;
			event_enqueue(ev);
		break;
		}
	break;
	case WRITE:
	// local
		if (vp_vec_exists(&t->block->owners, orig)) {
			switch (t->stage) {
			case 0:
				block_size_update(t->block, t->size);
				ev = event_alloc(STAGE_NEXT, sim_time);
				ev->data.t = t;
				event_enqueue(ev);
			break;
			case 1:
				vp_for (dest, &t->block->owners) {
					if (dest->id == orig->id) {
						continue;
					}
					ev = event_alloc(FLOW_START, sim_time);
					ev->data.f = flow_alloc(orig->id, dest->id, t->block->size, t);
					event_enqueue(ev);
				}
			break;
			case 2:
				ev = event_alloc(PULL_TASK, sim_time);
				ev->data.n = t->node;
				event_enqueue(ev);
			break;
			}
		break;
		}
	// remote
		switch (t->stage) {
		case 0:
			dest = shortest_path(&t->block->owners, orig, &topology);
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(orig->id, dest->id, 40, t);
			event_enqueue(ev);
		break;
		case 1:
			dest = shortest_path(&t->block->owners, orig, &topology);
			ev = event_alloc(FLOW_START, sim_time);
			ev->data.f = flow_alloc(dest->id, orig->id, t->block->size, t);
			event_enqueue(ev);
		break;
		case 3:
			block_size_update(t->block, t->size);
			ev = event_alloc(STAGE_NEXT, sim_time);
			ev->data.t = t;
			event_enqueue(ev);
		break;
		case 2:
			vp_for (dest, &t->block->owners) {
				ev = event_alloc(FLOW_START, sim_time);
				ev->data.f = flow_alloc(orig->id, dest->id, t->block->size, t);
				event_enqueue(ev);
			}
		break;
		case 4:
			vp_vec_append(&t->block->owners, orig);
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = t->node;
			event_enqueue(ev);
		break;
		}
	break;
	case TRAIN:
		ev = event_alloc(PULL_TASK, sim_time + TRAIN_DEF_MS);
		ev->data.n = t->node;
		event_enqueue(ev);
	break;
	case BARRIER:
		ev = event_alloc(ROUND_BARRIER, sim_time + BARRIER_DEF_MS);
		ev->data.t = t;
		event_enqueue(ev);
	break;
	}
	t->stage++;
}

void path_dbg(const struct vp_vec *p)
{
	assert(p->length >= 1);
	struct link *l = vp_vec_get(p, 0);
	dbg("%d -> %d", l->orig, l->dest);
	for (size_t i = 1; i < p->length; i++) {
		l = vp_vec_get(p, i);
		dbg(" -> %d", l->dest);
	}
	dbg("\n");
	vp_for (l, p) {
		dbg("  %2d ", l->active_flows.length);
	}
	dbg("\n");
}

void event_print(const struct event *ev)
{
	char *str;
	struct link *orig, *dest;
	struct flow *f;
	struct task *t;
	switch (ev->type) {
		case PULL_TASK:
		dbg("%10ld [ ev %3d | node %3d | ",
			sim_time, ev->id, ev->data.n->id);
		if (ev->data.n->task_queue.length == 0) {
			dbg("FINISH ]\n");
		} else {
			dbg("NEW TASK | remaining %3d ]\n",
			ev->data.n->task_queue.length);
		}
		
		break;
		case STAGE_NEXT:
			if (ev->data.t->type == 0) {
				str = "R";
			} else if (ev->data.t->type == 1) {
				str = "W";
			} else if (ev->data.t->type == 3) {
				str = "B";
			} else {
				str = "T";
			}
			dbg("%10ld [ ev %3d | task %3d (%s) st %3d | node %3d | block %3d ]\n",
				sim_time, ev->id, ev->data.t->id, str, ev->data.t->stage, ev->data.t->node->id, ev->data.t->block->id);
		break;
		case FLOW_START:
			f = ev->data.f;
			t = f->t;
			orig = vp_vec_get(&f->path, 0);
			dest = vp_vec_get(&f->path, f->path.length-1);
			if (t->type == READ) {
				str = "R";
			} else if (t->type == WRITE) {
				str = "W";
			} else {
				str = "T";
			}
			dbg("%10ld [ ev %3d | task %3d (%s) st %3d | node %3d | block %3d | FLOW %2d STA %2d  -> %2d | %ldB ]\n",
				sim_time, ev->id, t->id, str, t->stage, t->node->id, t->block->id,
				f->id, orig->orig, dest->dest, f->nbytes);	
			path_dbg(&f->path);
		break;
		case FLOW_FINISH:
			f = ev->data.f;
			t = f->t;
			orig = vp_vec_get(&f->path, 0);
			dest = vp_vec_get(&f->path, f->path.length-1);
			if (t->type == READ) {
				str = "R";
			} else if (t->type == WRITE) {
				str = "W";
			} else {
				str = "T";
			}
			dbg("%10ld [ ev %3d | task %3d (%s) st %3d | node %3d | block %3d | FLOW %2d END %2d  -> %2d | %ldB | %ld + %ldms ]\n",
				sim_time, ev->id, t->id, str, t->stage, t->node->id, t->block->id,
				f->id, orig->orig, dest->dest, f->nbytes, f->start_time, f->makespan);
		break;
	}
}

void round_barrier(struct event *ev)
{
	struct node *n;
	struct task *t;
	struct event *new;
	static int node_count = 0;
	
	node_count++;
	if (node_count >= nodes.length) {
		node_count = 0;
		vp_for (n, &nodes) {
			if (!n->task_queue.length) {
				continue;
			}
#ifdef USE_VP_LIST
			t = vp_list_remove_at(&n->task_queue, n->task_queue.first);
#else
			t = vp_vec_remove(&n->task_queue, 0);
#endif
			n->current_task = t;

			new = event_alloc(STAGE_NEXT, sim_time);
			new->data.t = t;
			event_enqueue(new);
		}
	}
}

void event_process(struct event *ev)
{
	struct node *n;
	struct task *t;
	struct flow *f;

	struct event *new;
	event_print(ev);
	
	switch (ev->type) {
	case ROUND_BARRIER:
		round_barrier(ev);
	break;
	case PULL_TASK:
		n = ev->data.n;
		if (!n->task_queue.length) {
			break;
		}
#ifdef USE_VP_LIST
		//t = vp_list_remove_at(&n->task_queue, n->task_queue.first);
		t = vp_list_remove_at(&n->task_queue, n->task_queue.first);
#else
		t = vp_vec_remove(&n->task_queue, 0);
#endif
		n->current_task = t;
		
		new = event_alloc(STAGE_NEXT, sim_time);
		new->data.t = t;
		event_enqueue(new);
	break;
	case STAGE_NEXT:
		switch (policy)
		{
		case S_COPY_UPD:
			scu_stage(ev->data.t);
		break;
		case S_COPY_MOV:
			scm_stage(ev->data.t);
		break;
		case M_COPY_MOV:
			mcm_stage(ev->data.t);
		break;
		case M_COPY_UPD:
			mcu_stage(ev->data.t);
		break;
		case FLOW_DEBUG:
			flow_dbg(ev->data.t);
		break;
		
		default:
			break;
		}
	break;
	case FLOW_START:
		flow_start(ev);
	break;
	case FLOW_FINISH:
		flow_finish(ev);
	break;
	}
}

void event_loop(struct vp_vec *node_list,
#ifdef USE_VP_LIST_QUEUE
				struct vp_list *ev_queue)
#else
				struct vp_vec *ev_queue)
#endif
{
	struct event *ev;
	
	dispatcher_init(node_list);
	while (ev_queue->length > 0) {
#ifdef USE_VP_LIST_QUEUE
		ev = vp_list_remove_at(ev_queue, ev_queue->first);
#else
		ev = vp_vec_remove(ev_queue, 0);
#endif
		sim_time = ev->dispatch_time;
		
		event_process(ev);
		event_free(ev);
	}
}

void flow_debug(struct config *cfg, struct vp_vec *tasks)
{
	struct task *t;
	
	///////////////
	t = vp_vec_get(tasks, 0);
	t->node = vp_vec_get(&nodes, 0);
	t->block = vp_vec_get(&blocks, 3);
	t->type = READ;
	
	///////////////
	t = vp_vec_get(tasks, 1);
	t->node = vp_vec_get(&nodes, 1);
	t->block = vp_vec_get(&blocks, 0);
	t->type = TRAIN;

	t = vp_vec_get(tasks, 2);
	t->node = vp_vec_get(&nodes, 1);
	t->block = vp_vec_get(&blocks, 4);
	t->type = READ;

	///////////////
	t = vp_vec_get(tasks, 3);
	t->node = vp_vec_get(&nodes, 2);
	t->block = vp_vec_get(&blocks, 0);
	t->type = TRAIN;

	t = vp_vec_get(tasks, 4);
	t->node = vp_vec_get(&nodes, 2);
	t->block = vp_vec_get(&blocks, 0);
	t->type = TRAIN;
	
	t = vp_vec_get(tasks, 5);
	t->node = vp_vec_get(&nodes, 2);
	t->block = vp_vec_get(&blocks, 5);
	t->type = READ;
}

void wl_virt_rounds(struct vp_vec *tl)
{
	struct topology *aux = topology.virt_topo;
	while (aux) {
		workload_gen(tl, &nodes, &blocks, aux, 1);
		aux = aux->virt_topo;
	}
}

struct config config;
int main(int argc, char *argv[])
{
	struct vp_vec tasks;

	long long makespan;
	time_t sta, end;
	
	if (argc < 3) {
		fprintf(stderr, "usage: %s [topol_fname] [config_fname]\n", argv[0]);
	}
	load_config(argv[2], &config);
	/*
	args_parse(&config, argc, argv);
	if (config.dir_mode == DIR_GLOBAL) {
		global_directory = 1;
	} else {
		global_directory = 0;
	}
	policy = (enum policy_t) config.policy;
	*/
	policy = config.policy;
	if (config.debug[0]) {
		debug = fopen(config.debug, "w");
	} else {
		debug = stderr;
	}
	if (config.output[0]) {
		result = fopen(config.output, "a");
	} else {
		result = stdout;
	}

	vp_vec_alloc(&blocks, 512);
	vp_vec_alloc(&tasks, 512);
	vp_vec_alloc(&nodes, 512);
	//vp_list_alloc(&event_queue, 200000);
	
	//topo_read(config.topol_fname, &topology);
	topology.directed = 0;
	int rtopol = topo_multi_read(argv[1], &topology);
	//dbg("topol_read done (%d topologies)\n", rtopol);
	dbg("%s %s\n", argv[0], argv[1]);
	sta = clock();
	dijkstra_init(&topology);
	end = clock();
	//out("dijkstra time: %ldms\n", (end - sta)/(CLOCKS_PER_SEC/1000));
	//dist_cache_print();
	//struct vp_vec path = {0};
	//path_resolve(&topology, 5, 2, &path);
	
	gen_init(config.seed);
	gen_blocks(&blocks, config.blocks_per_node * topology.num_nodes, 1, config.block_size);
	//gen_nodes(&nodes, &blocks, topology.num_nodes, config.n_groups, config.dir_mode, config.blocks_per_node);
	gen_nodes(&nodes, &blocks, topology.num_nodes, 1, 0, 1);
	gen_init(config.seed);
	//gen_tasks_groups(&tasks, &blocks, &nodes, config.n_events);
	//workload_gen(&tasks, &nodes, &blocks, &topology, config.n_events);
	wl_virt_rounds(&tasks);

	// makespan = dispatcher(&nodes, &blocks, &events);
	//flow_debug(&config, &tasks);
	tasks_enqueue(&tasks);
	dbg("-- SIMULATION START --\n");
	sta = clock();
	event_loop(&nodes, &event_queue);
	end = clock();

	//out("exec time: %ld\n", (end - sta)/(CLOCKS_PER_SEC/1000));
	switch (policy) {
	case S_COPY_UPD:
		out("SCU (0): ");
	break;
	case S_COPY_MOV:
		out("SCI (1): ");
	break;
	case M_COPY_MOV:
		out("MCI (2): ");
	break;
	case M_COPY_UPD:
		out("MCU (3): ");
	break;
	}
	out("%ld %ld\n", sim_time, (end - sta)/(CLOCKS_PER_SEC/1000));
	//log_results(&tasks, &nodes, makespan);

	tasks_free(&tasks);
	blocks_free(&blocks);
	nodes_free(&nodes);
	//operations_free(&events);
	topo_free(&topology);
	if (config.output[0]) {
		fclose(result);
	}
	if (config.debug[0]) {
		fclose(debug);
	}
}
