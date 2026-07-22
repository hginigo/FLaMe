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
#include "backend.h"
#define VP_VEC_IMPLEMENTATION
#include "vp_vec.h"
#define VP_LIST_IMPLEMENTATION
#include "vp_list.h"
#define VP_HEAP_IMPLEMENTATION
#include "vp_heap.h"

struct topology topology = {0};
struct vp_vec nodes;
struct vp_vec models;
struct vp_heap event_queue = {0};
time_t sim_time;
enum policy_t policy;
long long total_hops = 0;
struct config config;


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

	flows_reschedule(f);

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
	flows_reschedule(f);

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
	struct node *orig, *dest;
	if (t->stage == 1) return;

	orig = vp_vec_get(&nodes, 0);
	dest = vp_vec_get(&nodes, 1);
	flow_enqueue(orig->id, dest->id, 50, t);
}

#define BARRIER_DEF_MS 0

/*
 * Locates the replica of `m` nearest (by hop count) to `from`, mirroring
 * what the old owners-list shortest_path search did — now returning the
 * replica itself (not just its holder), since callers need both.
 */
struct replica *nearest_replica(const struct model *m,
	const struct node *from,
	const struct topology *t)
{
	struct replica *r, *min = NULL;
	int hops, min_hops = INT_MAX;

	vp_for (r, &m->replicas) {
		hops = path_hops(t, from->id, r->node->id);
		if (hops < min_hops) {
			min_hops = hops;
			min = r;
		}
	}
	return min;
}

/*
 * The only place a local training step happens. Trains whichever replica
 * of t->model this node currently holds; if none (a single-copy policy
 * relocated it elsewhere), trains the ambient sole copy instead, at no
 * network cost — the original TRAIN task never touched block/owners state
 * at all, so this preserves that location-agnostic, flow-free character
 * while still doing real work through the backend.
 */
void train_stage(struct task *t)
{
	struct node *orig = t->node;
	struct replica *r = model_replica_of(t->model, orig);
	unsigned int new_version;
	long long sim_ms;
	double loss, acc;
	void *new_params;
	size_t new_nbytes;
	struct event *ev;

	if (!r) {
		r = model_replica_first(t->model);
	}
	assert(r != NULL);

	if (backend_train(orig->id, r->version, config.epochs, r->params, r->nbytes,
			&new_version, &sim_ms, &loss, &acc, &new_params, &new_nbytes) < 0) {
		fprintf(stderr, "backend_train failed for node %u\n", orig->id);
		exit(1);
	}
	free(r->params);
	r->params = new_params;
	r->nbytes = new_nbytes;
	r->version = new_version;
	r->stamp = sim_time + sim_ms;
	orig->loss = loss;
	orig->acc = acc;
	orig->trained = 1;

	ev = event_alloc(PULL_TASK, sim_time + sim_ms);
	ev->data.n = orig;
	event_enqueue(ev);
}

void barrier_stage(struct task *t)
{
	struct event *ev = event_alloc(ROUND_BARRIER, sim_time + BARRIER_DEF_MS);
	ev->data.t = t;
	event_enqueue(ev);
}

/*
 * Aggregates `r` (the current authoritative replica for t->model, wherever
 * it physically sits) with every snapshot orig has staged since its last
 * WRITE. Mutates r in place with the backend's result and clears the
 * staged list. Returns the elapsed sim_ms so the caller can advance time.
 */
long long aggregate_into(struct node *orig, struct replica *r)
{
	int count = 1 + (int) orig->staged.length;
	void **blobs = malloc(count * sizeof(void *));
	size_t *nbytes = malloc(count * sizeof(size_t));
	long long *stale = malloc(count * sizeof(long long));
	struct replica *s;
	int i = 1;
	long long sim_ms;
	void *new_params;
	size_t new_nbytes;

	blobs[0] = r->params;
	nbytes[0] = r->nbytes;
	stale[0] = sim_time - r->stamp;
	vp_for (s, &orig->staged) {
		blobs[i] = s->params;
		nbytes[i] = s->nbytes;
		stale[i] = sim_time - s->stamp;
		i++;
	}

	if (backend_aggregate(orig->id, count, stale, blobs, nbytes,
			&sim_ms, &new_params, &new_nbytes) < 0) {
		fprintf(stderr, "backend_aggregate failed for node %u\n", orig->id);
		exit(1);
	}
	free(blobs);
	free(nbytes);
	free(stale);

	vp_for (s, &orig->staged) {
		replica_free(s);
	}
	orig->staged.length = 0;

	free(r->params);
	r->params = new_params;
	r->nbytes = new_nbytes;
	r->version++;
	r->stamp = sim_time + sim_ms;
	return sim_ms;
}

/*
 * READ under SCU/SCM (single physical copy). `relocate` selects the
 * coherence effect on completion: 0 = leave the copy where it is (SCU,
 * "update"), 1 = the copy migrates to the reader (SCM, "move"). Either
 * way, a snapshot is staged on the reader for its own next aggregation.
 */
void single_copy_read_stage(struct task *t, int relocate)
{
	struct event *ev;
	struct node *orig = t->node, *dest;
	struct replica *src = model_replica_first(t->model);

	dest = src->node;
	if (orig->id == dest->id) {
		ev = event_alloc(PULL_TASK, sim_time);
		ev->data.n = orig;
		event_enqueue(ev);
		return;
	}
	switch (t->stage) {
	case 0:
		flow_enqueue(orig->id, dest->id, 40, t);
	break;
	case 1:
		flow_enqueue(dest->id, orig->id, src->nbytes, t);
	break;
	case 2:
		vp_vec_append(&orig->staged, replica_dup(src, orig, sim_time));
		if (relocate) {
			model_replica_relocate(src, orig);
		}
		ev = event_alloc(PULL_TASK, sim_time);
		ev->data.n = orig;
		event_enqueue(ev);
	break;
	}
	t->stage++;
}

/*
 * READ under MCM/MCU (multi-copy): fetch from the nearest holder and join
 * the replica set, plus stage a snapshot for the reader's own aggregation.
 * Byte-identical between the two policies — only their WRITE differs.
 */
void multi_copy_read_stage(struct task *t)
{
	struct event *ev;
	struct node *orig = t->node;
	struct node *dest;
	struct replica *src;

	if (model_replica_of(t->model, orig)) {
		ev = event_alloc(PULL_TASK, sim_time);
		ev->data.n = orig;
		event_enqueue(ev);
		return;
	}
	switch (t->stage) {
	case 0:
		dest = nearest_replica(t->model, orig, &topology)->node;
		flow_enqueue(orig->id, dest->id, 40, t);
	break;
	case 1:
		src = nearest_replica(t->model, orig, &topology);
		flow_enqueue(src->node->id, orig->id, src->nbytes, t);
	break;
	case 2:
		src = nearest_replica(t->model, orig, &topology);
		vp_vec_append(&t->model->replicas, replica_dup(src, orig, sim_time));
		vp_vec_append(&orig->staged, replica_dup(src, orig, sim_time));
		ev = event_alloc(PULL_TASK, sim_time);
		ev->data.n = orig;
		event_enqueue(ev);
	break;
	}
	t->stage++;
}

void scu_stage(struct task *t)
{
	struct event *ev;
	struct node *orig, *dest;
	struct replica *r;
	long long sim_ms;

	switch (t->type) {
	case READ:
		single_copy_read_stage(t, 0);
		return;

	case WRITE:
		orig = t->node;
		r = model_replica_first(t->model);
		dest = r->node;
		if (orig->id == dest->id) {
			sim_ms = aggregate_into(orig, r);
			ev = event_alloc(PULL_TASK, sim_time + sim_ms);
			ev->data.n = orig;
			event_enqueue(ev);
			return;
		}
		switch (t->stage) {
		case 0:
			flow_enqueue(orig->id, dest->id, 40, t);
		break;
		case 1:
			flow_enqueue(dest->id, orig->id, r->nbytes, t);
		break;
		case 2:
			sim_ms = aggregate_into(orig, r);
			ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
			ev->data.t = t;
			event_enqueue(ev);
		break;
		case 3:
			flow_enqueue(orig->id, dest->id, r->nbytes, t);
		break;
		case 4:
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = orig;
			event_enqueue(ev);
		break;
		}
	break;
	case TRAIN:
		train_stage(t);
		return;
	case BARRIER:
		barrier_stage(t);
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
	struct replica *r;
	long long sim_ms;

	orig = t->node;
	switch (t->type) {
	case READ:
		single_copy_read_stage(t, 1);
		return;
	case WRITE:
		r = model_replica_first(t->model);
		dest = r->node;
		if (orig->id == dest->id) {
			sim_ms = aggregate_into(orig, r);
			ev = event_alloc(PULL_TASK, sim_time + sim_ms);
			ev->data.n = orig;
			event_enqueue(ev);
			return;
		}
		switch (t->stage) {
		case 0:
			flow_enqueue(orig->id, dest->id, 40, t);
		break;
		case 1:
			flow_enqueue(dest->id, orig->id, r->nbytes, t);
		break;
		case 2:
			sim_ms = aggregate_into(orig, r);
			ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
			ev->data.t = t;
			event_enqueue(ev);
		break;
		case 3:
			model_replica_relocate(r, orig);
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = t->node;
			event_enqueue(ev);
		break;
		}
	break;
	case TRAIN:
		train_stage(t);
		return;
	case BARRIER:
		barrier_stage(t);
	break;
	default:
	break;
	}
	t->stage++;
}

void mcm_stage(struct task *t)
{
	struct event *ev;
	struct node *orig = t->node;
	struct node *dest;
	struct replica *own, *r2, *mine;
	long long sim_ms;
	switch (t->type) {
	case READ:
		multi_copy_read_stage(t);
		return;
	case WRITE:
		own = model_replica_of(t->model, orig);
		if (own) {
			switch (t->stage) {
			case 0:
				vp_for (r2, &t->model->replicas) {
					if (r2->node->id == orig->id) {
						continue;
					}
					flow_enqueue(orig->id, r2->node->id, 40, t);
				}
			break;
			case 1:
				sim_ms = aggregate_into(orig, own);
				ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
				ev->data.t = t;
				event_enqueue(ev);
			break;
			case 2:
				model_replica_keep_only(t->model, own);
				ev = event_alloc(PULL_TASK, sim_time);
				ev->data.n = t->node;
				event_enqueue(ev);
			break;
			}
		break;
		}
		// remote: orig doesn't currently hold a copy
		switch (t->stage) {
		case 0:
			dest = nearest_replica(t->model, orig, &topology)->node;
			flow_enqueue(orig->id, dest->id, 40, t);
		break;
		case 1:
			dest = nearest_replica(t->model, orig, &topology)->node;
			flow_enqueue(dest->id, orig->id,
				model_replica_of(t->model, dest)->nbytes, t);
		break;
		case 2:
			vp_for (r2, &t->model->replicas) {
				flow_enqueue(orig->id, r2->node->id, 40, t);
			}
		break;
		case 3:
			mine = replica_dup(nearest_replica(t->model, orig, &topology), orig, sim_time);
			vp_vec_append(&t->model->replicas, mine);
			sim_ms = aggregate_into(orig, mine);
			ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
			ev->data.t = t;
			event_enqueue(ev);
		break;
		case 4:
			own = model_replica_of(t->model, orig);
			model_replica_keep_only(t->model, own);
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = t->node;
			event_enqueue(ev);
		break;
		}
	break;
	case TRAIN:
		train_stage(t);
		return;
	case BARRIER:
		barrier_stage(t);
	break;
	}
	t->stage++;
}

void mcu_stage(struct task *t)
{
	struct event *ev;
	struct node *orig = t->node;
	struct node *dest;
	struct replica *own, *r2, *mine;
	long long sim_ms;
	switch (t->type) {
	case READ:
		multi_copy_read_stage(t);
		return;
	case WRITE:
		own = model_replica_of(t->model, orig);
		if (own) {
			switch (t->stage) {
			case 0:
				sim_ms = aggregate_into(orig, own);
				ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
				ev->data.t = t;
				event_enqueue(ev);
			break;
			case 1:
				vp_for (r2, &t->model->replicas) {
					if (r2->node->id == orig->id) {
						continue;
					}
					flow_enqueue(orig->id, r2->node->id, own->nbytes, t);
					free(r2->params);
					r2->params = malloc(own->nbytes);
					memcpy(r2->params, own->params, own->nbytes);
					r2->nbytes = own->nbytes;
					r2->version = own->version;
					r2->stamp = own->stamp;
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
		// remote: orig doesn't currently hold a copy
		switch (t->stage) {
		case 0:
			dest = nearest_replica(t->model, orig, &topology)->node;
			flow_enqueue(orig->id, dest->id, 40, t);
		break;
		case 1:
			dest = nearest_replica(t->model, orig, &topology)->node;
			flow_enqueue(dest->id, orig->id,
				model_replica_of(t->model, dest)->nbytes, t);
		break;
		case 2:
			mine = replica_dup(nearest_replica(t->model, orig, &topology), orig, sim_time);
			vp_vec_append(&t->model->replicas, mine);
			sim_ms = aggregate_into(orig, mine);
			ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
			ev->data.t = t;
			event_enqueue(ev);
		break;
		case 3:
			mine = model_replica_of(t->model, orig);
			vp_for (r2, &t->model->replicas) {
				if (r2->node->id == orig->id) {
					continue;
				}
				flow_enqueue(orig->id, r2->node->id, mine->nbytes, t);
				free(r2->params);
				r2->params = malloc(mine->nbytes);
				memcpy(r2->params, mine->params, mine->nbytes);
				r2->nbytes = mine->nbytes;
				r2->version = mine->version;
				r2->stamp = mine->stamp;
			}
		break;
		case 4:
			ev = event_alloc(PULL_TASK, sim_time);
			ev->data.n = t->node;
			event_enqueue(ev);
		break;
		}
	break;
	case TRAIN:
		train_stage(t);
		return;
	case BARRIER:
		barrier_stage(t);
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
			dbg("%10ld [ ev %3d | task %3d (%s) st %3d | node %3d | model %3d ]\n",
				sim_time, ev->id, ev->data.t->id, str, ev->data.t->stage, ev->data.t->node->id, ev->data.t->model->id);
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
			dbg("%10ld [ ev %3d | task %3d (%s) st %3d | node %3d | model %3d | FLOW %2d STA %2d  -> %2d | %ldB ]\n",
				sim_time, ev->id, t->id, str, t->stage, t->node->id, t->model->id,
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
			dbg("%10ld [ ev %3d | task %3d (%s) st %3d | node %3d | model %3d | FLOW %2d END %2d  -> %2d | %ldB | %ld + %ldms ]\n",
				sim_time, ev->id, t->id, str, t->stage, t->node->id, t->model->id,
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

void event_loop(struct vp_vec *node_list, struct vp_heap *ev_queue)
{
	struct event *ev;

	ev_queue->cmp = event_cmp;
	dispatcher_init(node_list);
	while (ev_queue->length > 0) {
		ev = vp_heap_pop(ev_queue);
		if (ev->stale) {			/* lazy invalidation: superseded */
			event_free(ev);
			continue;
		}
		sim_time = ev->dispatch_time;

		event_process(ev);
		event_free(ev);
	}
}

void wl_virt_rounds(struct vp_vec *tl)
{
	struct topology *aux = topology.virt_topo;
	while (aux) {
		workload_gen(tl, &nodes, &models, aux, 1);
		aux = aux->virt_topo;
	}
}

/*
 * Creates the initial replica for every node's own model via the backend's
 * "init" op. Assumes the 1:1 node<->model assignment that gen_nodes leaves
 * behind under main()'s hardcoded one-model-per-node call — if that call
 * ever changes, this loop needs to change with it.
 */
void models_init_replicas(struct vp_vec *nl, struct vp_vec *ml)
{
	struct node *n;
	struct model *m;
	void *params;
	size_t nbytes;
	id_t nshards = config.nshards > 0 ? (id_t) config.nshards : (id_t) nl->length;

	vp_for (n, nl) {
		m = vp_vec_get(ml, n->id);
		if (!m) continue;
		if (backend_init(n->id, config.model_name, n->id, nshards,
				(unsigned int) config.seed, config.ms_per_epoch,
				&params, &nbytes) < 0) {
			fprintf(stderr, "backend_init failed for node %u\n", n->id);
			exit(1);
		}
		vp_vec_append(&m->replicas, replica_alloc(m, n, params, nbytes, 0, sim_time));
	}
}

int main(int argc, char *argv[])
{
	struct vp_vec tasks;
	time_t sta, end;

	if (argc < 3) {
		fprintf(stderr, "usage: %s [topol_fname] [config_fname]\n", argv[0]);
	}
	load_config(argv[2], &config);
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
	if (!config.backend_cmd[0]) {
		fprintf(stderr, "config: backend_cmd is required (path to the training backend)\n");
		return 1;
	}
	if (backend_spawn(config.backend_cmd) < 0) {
		fprintf(stderr, "failed to spawn backend '%s'\n", config.backend_cmd);
		return 1;
	}

	vp_vec_alloc(&models, 512);
	vp_vec_alloc(&tasks, 512);
	vp_vec_alloc(&nodes, 512);

	topology.directed = 0;
	topo_multi_read(argv[1], &topology);
	dbg("%s %s\n", argv[0], argv[1]);
	dijkstra_init(&topology);

	gen_init(config.seed);
	gen_models(&models, config.blocks_per_node * topology.num_nodes, 1, config.block_size);
	gen_nodes(&nodes, &models, topology.num_nodes, 1, 0, 1);
	models_init_replicas(&nodes, &models);
	gen_init(config.seed);
	wl_virt_rounds(&tasks);

	tasks_enqueue(&tasks);
	dbg("-- SIMULATION START --\n");
	sta = clock();
	event_loop(&nodes, &event_queue);
	end = clock();

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
	default:
	break;
	}
	out("%ld %ld\n", sim_time, (end - sta)/(CLOCKS_PER_SEC/1000));

	backend_shutdown();
	tasks_free(&tasks);
	models_free(&models);
	nodes_free(&nodes);
	topo_free(&topology);
	if (config.output[0]) {
		fclose(result);
	}
	if (config.debug[0]) {
		fclose(debug);
	}
}
