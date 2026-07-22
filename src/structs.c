#include "structs.h"
#include <stdio.h>
#include <string.h>

FILE *result;
FILE *debug;

struct replica *replica_alloc(struct model *m, struct node *n,
	void *params, size_t nbytes, unsigned int version, time_t stamp)
{
	struct replica *r = malloc(sizeof(struct replica));
	r->model = m;
	r->node = n;
	r->params = params;
	r->nbytes = nbytes;
	r->version = version;
	r->stamp = stamp;
	return r;
}

/* Deep-copies params: used whenever a *second* physical copy comes into
 * existence (multi-copy replication, or a staged snapshot pulled from a
 * peer) rather than a single copy just changing hands. */
struct replica *replica_dup(const struct replica *src, struct node *n, time_t stamp)
{
	void *params = NULL;
	if (src->nbytes > 0) {
		params = malloc(src->nbytes);
		memcpy(params, src->params, src->nbytes);
	}
	return replica_alloc(src->model, n, params, src->nbytes, src->version, stamp);
}

void replica_free(struct replica *r)
{
	if (!r) return;
	free(r->params);
	free(r);
}

struct replica *model_replica_of(const struct model *m, const struct node *n)
{
	struct replica *r;
	vp_for (r, &m->replicas) {
		if (r->node == n) {
			return r;
		}
	}
	return NULL;
}

struct replica *model_replica_first(const struct model *m)
{
	if (m->replicas.length == 0) return NULL;
	return vp_vec_get(&m->replicas, 0);
}

/* Frees every replica currently attached to the model (they are owned
 * heap objects, unlike the borrowed node pointers `owners` used to hold). */
void model_replica_clear(struct model *m)
{
	struct replica *r;
	vp_for (r, &m->replicas) {
		replica_free(r);
	}
	m->replicas.length = 0;
}

/* Same physical replica, new holder: no bytes move, just reassign. */
void model_replica_relocate(struct replica *r, struct node *n)
{
	r->node = n;
}

/* Frees every replica in m->replicas except `keep`, then leaves replicas
 * holding just [keep]. Used by the write-invalidate policies collapsing a
 * multi-copy set back down to one. */
void model_replica_keep_only(struct model *m, struct replica *keep)
{
	struct replica *r;
	vp_for (r, &m->replicas) {
		if (r != keep) {
			replica_free(r);
		}
	}
	m->replicas.length = 0;
	vp_vec_append(&m->replicas, keep);
}

void models_free(struct vp_vec *models)
{
	struct model *m;
	for (size_t i = 0; i < models->length; i++) {
		m = vp_vec_get(models, i);
		model_replica_clear(m);
		if (m->replicas.data != NULL) {
			vp_vec_free(&m->replicas);
		}
		free(m);
	}
	vp_vec_free(models);
}

void nodes_free(struct vp_vec *nodes)
{
	struct node *n;
	struct replica *r;
	for (size_t i = 0; i < nodes->length; i++) {
		n = vp_vec_get(nodes, i);
		vp_vec_free(&n->task_queue);
		vp_for (r, &n->staged) {
			replica_free(r);
		}
		if (n->staged.data != NULL) {
			vp_vec_free(&n->staged);
		}
		free(n);
	}
	vp_vec_free(nodes);
}
