#include "gen.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void gen_init(unsigned int seed)
{
    srand(seed);
}

void gen_models(struct vp_vec *ml,
    unsigned int n_models,
    unsigned int n_groups,
    unsigned int model_size)
{
    struct model *m;
    id_t group_id = 0;
    int models_per_group = n_models / n_groups;
    if (!models_per_group) {
        models_per_group = 1;
    }

    for (size_t i = 0; i < n_models; i++) {
        m = malloc(sizeof(struct model));
        memset(m, 0, sizeof(struct model));
        m->id = i;
        m->size = model_size;
        m->group = group_id;
        vp_vec_append(ml, m);
        if ((i + 1) % models_per_group == 0) {
            group_id++;
        }
    }
}

/*
 * Assigns `models_per_node` models to each node round-robin (dir_id
 * bookkeeping only). Replicas themselves are *not* created here: a replica
 * carries real backend-produced parameters, and the backend isn't spawned
 * yet at generation time. The caller creates the initial replica for each
 * (node, model) pair returned by this assignment, in the same order, via
 * backend_init().
 */
id_t gen_nodes(struct vp_vec *nl,
    struct vp_vec *ml,
    unsigned int n_nodes,
    unsigned int n_groups,
    int directory,
    int models_per_node)
{
    struct node *n;
    struct model *m;
    id_t mid = 0;
    id_t group_id = 0;
    int nodes_per_group = n_nodes / n_groups;
    id_t dir_central = rand() % n_nodes;
    if (!models_per_node) {
        models_per_node = 1;
    }

    for (size_t i = 0; i < n_nodes; i++) {
        n = malloc(sizeof(struct node));
        memset(n, 0, sizeof(struct node));
        n->id = i;
        n->group = group_id;
        for (int j = 0; j < models_per_node && mid < ml->length; j++) {
            m = vp_vec_get(ml, mid);
            if (directory == DIR_DISTRIB) {
                m->dir_id = i;
            } else if (directory == DIR_RANDOM) {
                m->dir_id = rand() % n_nodes;
            } else if (directory == DIR_CENTRAL) {
                m->dir_id = dir_central;
            } else {
                m->dir_id = directory;
            }
            mid++;
        }
        vp_vec_append(nl, n);
        if ((i + 1) % nodes_per_group == 0) {
            group_id++;
        }
    }
    return mid;
}
