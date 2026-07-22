#include <stdio.h>
#include "structs.h"
#include "topology.h"
#include "vp_vec.h"
#include "config.h"
extern struct config config;

struct task *task_alloc(
    struct node *n,
    struct model *m,
    enum task_t type)
{
    static id_t task_id = 0;
    struct task *t = calloc(1, sizeof(struct task));

    t->id = task_id++;
    t->type = type;
    t->node = n;
    t->model = m;
    t->size = 0;

    return t;
}

void single_node(struct vp_vec *tl,
    struct node *n,
    const struct topology *t,
    const struct vp_vec *ml,
    int max_neigh_cap)
{
    struct task *aux;
    struct model *aux_model;
    struct link *l;
    int count = 0;
    struct vp_vec *adj_list = &t->adj_lists[n->id];

    aux_model = vp_vec_get(ml, n->id);
    aux = task_alloc(n, aux_model, TRAIN);
    vp_vec_append(tl, aux);
    vp_for (l, adj_list) {
        count++;
        if (count > max_neigh_cap) {
            break;
        }
        aux_model = vp_vec_get(ml, l->dest);
        aux = task_alloc(n, aux_model, READ);
        vp_vec_append(tl, aux);
    }
    aux_model = vp_vec_get(ml, n->id);
    aux = task_alloc(n, aux_model, WRITE);
    aux->size = 0;
    vp_vec_append(tl, aux);
    if (config.barriers) {
        aux = task_alloc(n, NULL, BARRIER);
        aux->size = 0;
        vp_vec_append(tl, aux);
    }
}

void single_round(struct vp_vec *tl,
    const struct vp_vec *nl,
    const struct vp_vec *ml,
    const struct topology *t)
{
    struct node *n;
    vp_for (n, nl) {
        single_node(tl, n, t, ml, 100000);
    }
}

void workload_gen(struct vp_vec *tl,
    const struct vp_vec *nl,
    const struct vp_vec *ml,
    const struct topology *t,
    int n_rounds)
{
    for (int i = 0; i < n_rounds; i++) {
        single_round(tl, nl, ml, t);
    }
}

void tasks_free(struct vp_vec *tl)
{
    struct task *t;
    vp_for (t, tl) {
        free(t);
    }
    vp_vec_free(tl);
}
