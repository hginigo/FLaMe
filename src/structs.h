#ifndef _STRUCTS_H
#define _STRUCTS_H
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include "vp_vec.h"
#if defined(USE_VP_LIST) || defined(USE_VP_LIST_QUEUE)
#include "vp_list.h"
#endif

//#define debug stderr
//#define result stdout
extern FILE *debug;
extern FILE *result;

#define dbg(s, ...) fprintf(debug, s, ##__VA_ARGS__)
#define out(s, ...) fprintf(result, s, ##__VA_ARGS__)
/*
 * Bandwidth unit. A .tpl link weight is given in Mbit/s; multiplying it by
 * Bpms converts it to the bytes-per-ms capacity every rate computation in the
 * flow model works in (topo_multi_read does this once, at load). SI units:
 * 1 Mbit/s = 10^6 bit/s = 125 B/ms, so 100 Kbit/s (0.1 or "100k") = 12.5 B/ms.
 * (It used to be 1024*1024/8/1000 = 131, i.e. a 2^20-bit "Mbit", truncated.)
 */
#define Bpms (1000.0 * 1000.0 / 8.0 / 1000.0)
typedef unsigned int id_t;

enum policy_t {
	S_COPY_UPD,
	S_COPY_MOV,
	M_COPY_MOV,
	M_COPY_UPD,
	FLOW_DEBUG
};


#define DIR_GLOBAL -1
#define DIR_CENTRAL -2
#define DIR_DISTRIB -3
#define DIR_RANDOM -4

/*
 * A model is the *descriptor* only: identity and wire size. It carries no
 * parameters of its own, because in decentralised FL every holder's copy
 * diverges. The parameters live in the per-holder replicas below.
 */
struct model {
	id_t id;
	id_t dir_id;
	size_t size;		/* payload size on the wire, in bytes */
	id_t group;
	struct vp_vec replicas;	/* struct replica* — one per holding node */
};

/*
 * One node's copy of one model. `params` is an opaque blob: C never looks
 * inside it, it only stores it, sizes network flows by it, and hands it to
 * the Python backend, which alone knows what the bytes mean. That is what
 * keeps the simulator model-agnostic.
 *
 * Two replicas of the same model routinely differ — that is the point.
 * Replicas reachable from model->replicas are "owned" in the coherence
 * sense; node->staged holds detached snapshots pulled from peers and not
 * yet folded into the holder's own model.
 */
struct replica {
	struct model *model;
	struct node *node;	/* holder */
	void *params;		/* opaque; owned here, freed with the replica */
	size_t nbytes;
	unsigned int version;
	time_t stamp;		/* sim_time these params were produced/captured */
};

#define N_OPS 5
#define READ_DIR_OP 0
#define READ_BLOCK_OP 1
#define WRITE_DIR_OP 2
#define WRITE_BLOCK_OP 3
#define NOP_OP 4
struct node {
#ifdef USE_VP_LIST
	struct vp_list task_queue;
#else
	struct vp_vec task_queue;
#endif
	struct task *current_task;
	id_t id;
	id_t group;
	long long op_time[N_OPS];
	struct vp_vec staged;	/* struct replica* pulled from peers, unaggregated */
	double loss, acc;	/* last values reported by the backend */
	int trained;		/* has the backend ever trained this node? */
	int n_reads;		/* READs the workload gave it; 0 = disconnected agent */
};

enum task_t {
	READ,
	WRITE,
	TRAIN,
	BARRIER
};
#define N_TASK_TYPES 4

/* Associated to workloads */
struct task {
	struct node *node;
	struct model *model;
	id_t id;
	int size;
	int flow_rc;
	int stage;
	enum task_t type;
	time_t start_time;	/* when its node pulled it, for the latency metric */
};

enum event_t {
	PULL_TASK, // n
	STAGE_NEXT, // t
	FLOW_START, // f
	FLOW_FINISH, // f
	ROUND_BARRIER
};

/* Associated to the event queue */
struct event {
	enum event_t type;
	time_t dispatch_time;
	id_t id;
	int stale;		/* lazy invalidation: superseded finish event */
	union {
		struct node *n;
		struct task *t;
		struct flow *f;
	} data;
};

/* Associated to the network */
struct flow {
	time_t start_time, prev_ts;
	time_t makespan;
	size_t nbytes;		/* bytes still to deliver; decremented by recalc */
	size_t nbytes_total;	/* original transfer size, kept for the effective-bw metric */
	id_t id;
	int stage;		/* stage of t that started this flow, for the log */
	int control;		/* a control message (config.control_bytes), not a model */
	int marked;
	double min_bw;		/* B/ms, fractional below ~1 Mbit/s or under contention */
	struct vp_vec path, path_aux;
	struct task *t;
	struct event *finish_ev;
};

void models_free(struct vp_vec *models);
void nodes_free(struct vp_vec *nodes);

void *params_dup(const void *params, size_t nbytes);
struct replica *replica_alloc(struct model *m, struct node *n,
	void *params, size_t nbytes, unsigned int version, time_t stamp);
struct replica *replica_dup(const struct replica *src, struct node *n, time_t stamp);
void replica_free(struct replica *r);
struct replica *model_replica_of(const struct model *m, const struct node *n);
struct replica *model_replica_first(const struct model *m);
void model_replica_clear(struct model *m);
void model_replica_relocate(struct replica *r, struct node *n);
void model_replica_keep_only(struct model *m, struct replica *keep);
void model_replica_add(struct model *m, struct replica *r);
void node_stage(struct node *n, struct replica *r);
void node_stage_clear(struct node *n);

#endif // _STRUCTS_H
