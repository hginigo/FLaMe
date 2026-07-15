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
#define Bpms (1024 * 1024 / 8 / 1000)
typedef unsigned int id_t;

/*
enum block_state {
	INVALID,
	EXCLUSIVE,
	SHARED,
	DIRTY,
};
*/

/*
enum node_state {
    IDLE,
    BUSY,
};
*/
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

struct block {
	id_t id;
	id_t dir_id;
	size_t size;
	id_t group;
	//enum block_state state;
	//struct vp_vec copies; // node id's
	struct vp_vec owners;
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
    //enum node_state state;
	id_t id;
	id_t group;
	long long op_time[N_OPS];
};

enum task_t {
	//REQUEST,
	//REPLY,
	READ,
	WRITE,
	TRAIN,
	BARRIER
};

/* Associated to workloads */
struct task {
	struct node *node;
	struct block *block;
	//time_t time;
	id_t id;
	int size;
	int flow_rc;
	int stage;
	enum task_t type;
};

enum event_t {
	PULL_TASK, // n
	STAGE_NEXT, // t
	FLOW_START, // f
	FLOW_FINISH, // f
	ROUND_BARRIER
};

/* Associated to the  */
struct event {
	enum event_t type;
	time_t dispatch_time;
	id_t id;
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
	size_t nbytes;
	id_t id;
	int marked;
	int min_bw;
	struct vp_vec path, path_aux;
	struct task *t;
	struct event *finish_ev;
};

void blocks_read(struct vp_vec *blocks);
void blocks_free(struct vp_vec *blocks);
void nodes_read(struct vp_vec *nodes, const struct vp_vec *blocks);
void nodes_free(struct vp_vec *nodes);
void operations_read(struct vp_vec *operations,
					 const struct vp_vec *blocks,
					 const struct vp_vec *nodes);
void operations_free(struct vp_vec *operations);
struct event *event_new();

#endif // _STRUCTS_H
