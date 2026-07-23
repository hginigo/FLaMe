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
#include "policy.h"
#include "metrics.h"
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
struct config config;
struct metric metric_flow_hops;
struct metric metric_link_contention;


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
	path_attach_flow(&f->path, f, 1);
	if (!topology.directed) {
		path_attach_flow(&f->path_aux, f, 0);
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

	path_detach_flow(&f->path, f, 1);
	if (!topology.directed) {
		path_detach_flow(&f->path_aux, f, 0);
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
		if (policy == FLOW_DEBUG) {
			flow_dbg(ev->data.t);
		} else {
			policy_dispatch(ev->data.t);
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

	metric_init(&metric_flow_hops, "flow_hops");
	metric_init(&metric_link_contention, "link_contention");

	tasks_enqueue(&tasks);
	dbg("-- SIMULATION START --\n");
	sta = clock();
	event_loop(&nodes, &event_queue);
	end = clock();

	metric_print(&metric_flow_hops);
	metric_print(&metric_link_contention);

	if (policy_name(policy)) {
		out("%s (%d): ", policy_name(policy), (int) policy);
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
