#ifndef _GEN_H
#define _GEN_H
#include "structs.h"
#include "vp_vec.h"
#include "topology.h"

void gen_init(unsigned int seed);
void gen_blocks(struct vp_vec *bl,
    unsigned int n_blocks,
    unsigned int n_groups,
    unsigned int block_size);
id_t gen_nodes(struct vp_vec *nl,
    const struct vp_vec *bl,
    unsigned int n_nodes,
    unsigned int n_groups,
    int directory,
    int blocks_per_node);
void gen_tasks(struct vp_vec *tl,
    const struct vp_vec *bl,
    const struct vp_vec *nl,
    unsigned int n_tasks);
void gen_tasks_groups(struct vp_vec *tl,
    const struct vp_vec *bl,
    const struct vp_vec *nl,
    unsigned int n_tasks);
/*
void gen_all(struct vp_vec *bl,
    struct vp_vec *nl,
    struct vp_vec *tl,
    unsigned int n_blocks,
    unsigned int n_nodes,
    unsigned int n_tasks,
    unsigned int seed);
    */
struct task *ev_gen_rand(struct task *ev,
                          struct vp_vec *nl,
                          struct vp_vec *bl);
struct task *ev_gen_neigh(struct task *ev,
                           struct topology *t,
                           struct vp_vec *nl,
                           struct vp_vec *bl);
struct task *ev_gen_group(struct task *ev,
                           struct topology *t,
                           struct vp_vec *nl,
                           struct vp_vec *bl);

// void gen_rand_topol(struct topology *t, unsigned int n_nodes);

#endif /* _GEN_H */