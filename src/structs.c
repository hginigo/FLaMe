#include "structs.h"
#include <stdio.h>

FILE *result;
FILE *debug;

void blocks_read(struct vp_vec *blocks)
{
	size_t n_blocks;
	size_t id, block_size;
	FILE *f = fopen("blocks", "r");
	struct block *b;

	if (f == NULL) {
		perror("fopen");
		exit(1);
	}

	fscanf(f, "%lu\n", &n_blocks);
	dbg("n blocks: %lu\n", n_blocks);
	vp_vec_alloc(blocks, n_blocks);
	for (size_t i = 0; i < n_blocks; i++) {
		fscanf(f, "%lu %lu\n", &id, &block_size);
		dbg("b id: %lu, b size: %lu\n", id, block_size);
		b = malloc(sizeof(struct block));
		if (b == NULL) {
			perror("malloc");
			exit(1);
		}
		b->id = id;
		b->size = block_size;
		//b->state = EXCLUSIVE;

		vp_vec_set(blocks, id, b);
	}
	fclose(f);
	for (size_t i = 0; i < blocks->length; i++) {
		b = vp_vec_get(blocks, i);
		dbg("b id: %ld b sz: %ld\n", b->id, b->size);
	}
}

void blocks_free(struct vp_vec *blocks)
{
	struct block *b;
	for (size_t i = 0; i < blocks->length; i++) {
		b = vp_vec_get(blocks, i);
		if (b->owners.data != NULL) {
			vp_vec_free(&b->owners);
		}
		free(b);
	}
	vp_vec_free(blocks);
}

void nodes_read(struct vp_vec *nodes, const struct vp_vec *blocks)
{
	size_t n_nodes;
	size_t id, n_blocks;
	size_t block_id;
	const struct vp_vec zero = {0};

	struct node *n;
	struct block *cur_block;
	FILE *f = fopen("nodes", "r");

	if (f == NULL) {
		perror("fopen");
		exit(1);
	}

	fscanf(f, "%lu\n", &n_nodes);
	dbg("n nodes: %lu\n", n_nodes);
	vp_vec_alloc(nodes, n_nodes);
	for (size_t i = 0; i < n_nodes; i++) {
		fscanf(f, "%lu %lu", &id, &n_blocks);
		dbg("node id: %lu, n blocks: %lu\n", id, n_blocks);

		n = malloc(sizeof(struct node));
		n->id = id;
        //n->state = IDLE;
		n->task_queue = zero;
		// n->blocks = malloc(sizeof(struct vp_vec));
		//vp_vec_alloc(&n->blocks, n_blocks);

		for (size_t j = 0; j < n_blocks; j++) {
			fscanf(f, " %lu", &block_id);
			cur_block = vp_vec_get(blocks, block_id);
			cur_block->dir_id = id;
			//vp_vec_append(&n->blocks, cur_block);
			vp_vec_append(&cur_block->owners, n);
			dbg(" %lu", block_id);
		}
		dbg("\n");
		fscanf(f, "\n");
		vp_vec_set(nodes, id, n);
	}
	nodes->length = n_nodes;
	fclose(f);
}

void nodes_free(struct vp_vec *nodes)
{
	struct node *n;
	for (size_t i = 0; i < nodes->length; i++) {
		n = vp_vec_get(nodes, i);
		//vp_vec_free(&n->blocks);
		vp_vec_free(&n->task_queue);
		free(n);
	}
	vp_vec_free(nodes);
}

void operations_read(struct vp_vec *events,
					 const struct vp_vec *blocks,
					 const struct vp_vec *nodes)
{
	size_t n_op;
	char c;
	size_t node_id, block_id;
	struct task *op;
	struct node *n;
	struct block *b;
	FILE *f = fopen("events", "r");

	if (f == NULL) {
		perror("fopen");
		exit(1);
	}

	fscanf(f, "%lu\n", &n_op);
	dbg("n ops: %lu\n", n_op);
	vp_vec_alloc(events, n_op);
	for (size_t i = 0; i < n_op; i++) {
		fscanf(f, "%c %lu %lu\n", &c, &node_id, &block_id);
		printf("'%c' %lu %lu\n", c, node_id, block_id);
		op = malloc(sizeof(struct task));
		switch (c) {
		case 'r':
			op->type = READ;
			break;
		case 'w':
			op->type = WRITE;
			break;
		}
		n = vp_vec_get(nodes, node_id);
		b = vp_vec_get(blocks, block_id);
		// op->time = 0;
		op->node = n;
		op->block = b;
		vp_vec_append(events, op);
		printf("event: %c\n", c);
		printf("  n id: %u\n", n->id);
		printf("  b id: %u (dir %u)\n", b->id, b->dir_id);
	}
	fclose(f);
}

void operations_free(struct vp_vec *events)
{
	struct event *op;
	for (size_t i = 0; i < events->length; i++) {
		op = vp_vec_get(events, i);
		free(op);
	}
	vp_vec_free(events);
}
/*
void topology_read(struct topology *t)
{
	size_t num_nodes;
	int *graph;
	int aux;
	FILE *f = fopen("topology", "r");
	assert(t != NULL && "Topology is NULL");

	if (f == NULL) {
		perror("fopen");
		exit(1);
	}

	fscanf(f, "%lu\n", &num_nodes);
	graph = malloc(sizeof(int) * num_nodes * num_nodes);
	for (size_t i = 0; i < num_nodes; i++) {
		for (size_t j = 0; j < num_nodes; j++) {
			fscanf(f, " %d", &aux);
			graph[i + j*num_nodes] = aux;
		}
		fscanf(f, "\n");
	}
	t->num_nodes = num_nodes;
	t->graph = graph;
}

void topology_free(struct topology *t)
{
	free(t->graph);
}

int topology_bfs(struct topology *t)
{
	int cost = 0;
	struct vp_vec queue;
	vp_vec_alloc(&queue, t->num_nodes * t->num_nodes);

	for (size_t i = 0; i < t->num_nodes; i++) {
		for (size_t j = 0; j < t->num_nodes; j++) {
			t->graph[i * t->num_nodes + j];
		}
	}
	vp_vec_free(&queue);
}
	*/

struct event *event_new()
{
	struct event *result = malloc(sizeof(struct event));

	return result;
}
