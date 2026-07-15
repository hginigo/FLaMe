#include "gen.h"
#include "vp_vec.h"
#include "topology.h"
#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void *vp_vec_rand_pick(const struct vp_vec *vec)
{
    assert(vec->length > 0);
    int ind = rand() % vec->length;
    return vp_vec_get(vec, ind);
}

struct task *ev_gen_rand(struct task *ev,
                          struct vp_vec *nl,
                          struct vp_vec *bl)
{
    //struct event *ev = malloc(sizeof(struct event));
    int bid, nid, type;
    size_t bsize;
    
    //bid = rand() % bl->length;
    //nid = rand() % nl->length;
    //ev->block = vp_vec_get(bl, bid);
    //ev->node = vp_vec_get(nl, nid);
    //ev->type = type;
    ev->type = rand() % 2 ? READ : WRITE;
    ev->block = vp_vec_rand_pick(bl);
    ev->node = vp_vec_rand_pick(nl);
    //ev->time = 0;
    bsize = ev->block->size;
    if (ev->type == WRITE) {
        ev->size = ((rand() % bsize) - (bsize / 2)) * .75;
    } else {
        ev->size = 0;
    }
    return ev;
}

/* O(n^2) -> liadita liadita */
void vp_vec_insert_by_id(const struct vp_vec *bl,
                         struct vp_vec *bl_aux,
                         const struct node *owner)
{
    struct block *b;
    vp_for (b, bl) {
        if (vp_vec_exists(&b->owners, owner)) {
            vp_vec_append(bl_aux, b);
        }
    }
}

void vp_vec_insert_exists(const struct vp_vec *bl,
                          struct vp_vec *bl_aux,
                          const struct vp_vec *nl)
{
    struct block *b;
    struct node *n;
    vp_for (n, nl) {
        vp_for (b, bl) {
            if (vp_vec_exists(&b->owners, n)) {
                vp_vec_append(bl_aux, b);
            }
        }
    }
}

struct task *ev_gen_neigh(struct task *ev,
                           struct topology *t,
                           struct vp_vec *nl,
                           struct vp_vec *bl)
{
    //struct event *ev = malloc(sizeof(struct event));
    int nid, neigh_id;
    struct node *n, *neigh;
    struct vp_vec *adj_list, bl_aux;
    size_t bsize;

    dbg("bl len %d\n", bl->length);
    vp_vec_alloc(&bl_aux, bl->length);

    nid = rand() % nl->length;
    adj_list = &t->adj_lists[nid];
    n = vp_vec_get(nl, nid);

    // neigh_id = rand() % adj_list->length;
    // neigh = vp_vec_get(nl, neigh_id);
    vp_vec_append(adj_list, n);
    vp_vec_insert_exists(bl, &bl_aux, adj_list);
    vp_vec_pop(adj_list);
    
    ev->node = n;
    ev->type = rand() % 2 ? READ : WRITE;
    //ev->time = 0;
    if (bl_aux.length == 0) {
        vp_vec_free(&bl_aux);
        return NULL;
    }
    ev->block = vp_vec_rand_pick(&bl_aux);
    // vp_vec_insert_by_id(bl, &bl_aux, neigh);
    // ev->node = vp_vec_get(nl, nid);
    // if (bl_aux.length == 0) {
    //     vp_vec_insert_by_id(bl, &bl_aux, ev->node);
    // }
    // if (bl_aux.length == 0) {
    //     ev->block = vp_vec_rand_pick(bl);
    // } else {
    //     ev->block = vp_vec_rand_pick(&bl_aux);
    // }
    bsize = ev->block->size;
    if (ev->type == WRITE) {
        ev->size = ((rand() % bsize) - (bsize / 2)) * .75;
    } else {
        ev->size = 0;
    }
    vp_vec_free(&bl_aux);
    return ev;
}

struct task *ev_gen_group(struct task *ev,
                           struct topology *t,
                           struct vp_vec *nl,
                           struct vp_vec *bl)
{
    //struct event *ev = malloc(sizeof(struct event));
    struct vp_vec bl_aux, nl_aux;
    struct node *n, *aux, *iter;
    struct block *b;
    size_t bsize;
    size_t ev_size;
    
    vp_vec_alloc(&bl_aux, bl->length);
    vp_vec_alloc(&nl_aux, nl->length);

    n = vp_vec_rand_pick(nl);
    vp_for (iter, nl) {
        if (n->group == iter->group) {
            vp_vec_append(&nl_aux, iter);
        }
    }
    vp_vec_append(&nl_aux, n);
    aux = vp_vec_rand_pick(&nl_aux);
    vp_vec_insert_by_id(bl, &bl_aux, aux);
    
    ev->node = n;
    ev->type = rand() % 2 ? READ : WRITE;
    if (bl_aux.length == 0) {
        vp_vec_free(&bl_aux);
        vp_vec_free(&nl_aux);
        return NULL;
    }
    ev->block = vp_vec_rand_pick(&bl_aux);
    //ev->time = 0;
    bsize = ev->block->size;
    ev_size = ((rand() % bsize) - (bsize / 2)) * .75;
    if (ev->type == WRITE) {
        ev->size = ev_size;
        //ev->size = ((rand() % bsize) - (bsize / 2)) * .75;
    } else {
        ev->size = 0;
    }

    vp_vec_free(&bl_aux);
    vp_vec_free(&nl_aux);

    return ev;
}

void gen_init(unsigned int seed)
{
    srand(seed);
}

//#define MAX_BSIZE 19
void gen_blocks(struct vp_vec *bl,
    unsigned int n_blocks,
    unsigned int n_groups,
    unsigned int block_size)
{
    struct block *b;
    id_t group_id = 0;
    int blocks_per_group = n_blocks / n_groups;
    if (!blocks_per_group) {
        blocks_per_group = 1;
    }

    for (size_t i = 0; i < n_blocks; i++) {
        b = malloc(sizeof(struct block));
        memset(b, 0, sizeof(struct block));
        b->id = i;
        b->size = block_size;
        b->group = group_id;
        //b->size = (1 + rand() % MAX_BSIZE) * 5;
        //b->state = 0;
        vp_vec_append(bl, b);
        if ((i + 1) % blocks_per_group == 0) {
            group_id++;
        }
    }
}

//#define central_dir -2
#define N_GROUPS 3
id_t gen_nodes(struct vp_vec *nl,
    const struct vp_vec *bl,
    unsigned int n_nodes,
    unsigned int n_groups,
    int directory,
    int blocks_per_node)
{
    struct node *n;
    struct block *b;
    id_t bid = 0;
    id_t group_id = 0;
    int nodes_per_group = n_nodes / n_groups;
    id_t dir_central = rand() % n_nodes;
    if (!blocks_per_node) {
        blocks_per_node = 1;
    }

    for (size_t i = 0; i < n_nodes; i++) {
        n = malloc(sizeof(struct node));
        memset(n, 0, sizeof(struct node));
        n->id = i;
        //n->group = rand() % N_GROUPS;
        n->group = group_id;
        //n->state = IDLE;
        for (int j = 0; j < blocks_per_node && bid < bl->length; j++) {
            b = vp_vec_get(bl, bid);
            if (directory == DIR_DISTRIB) {
                b->dir_id = i;
            } else if (directory == DIR_RANDOM) {
                b->dir_id = rand() % n_nodes;
            } else if (directory == DIR_CENTRAL) {
                b->dir_id = dir_central;
            } else {
                b->dir_id = directory;
            }
            //b->state = EXCLUSIVE;
            vp_vec_append(&b->owners, n);
            //vp_vec_append(&n->blocks, b);
            bid++;
        }
        vp_vec_append(nl, n);
        if ((i + 1) % nodes_per_group == 0) {
            group_id++;
        }
    }
    return bid;
}

void gen_tasks(struct vp_vec *el,
    const struct vp_vec *bl,
    const struct vp_vec *nl,
    unsigned int n_tasks)
{
    struct task *ev;
    for (size_t i = 0; i < n_tasks; i++) {
        ev = malloc(sizeof(struct task));
        ev->node = vp_vec_rand_pick(nl);
        vp_vec_append(el, ev);
    }
}

void filter_by_group(struct vp_vec *aux,
    const struct vp_vec *bl,
    id_t group)
{
    struct block *b;
    vp_for (b, bl) {
        if (b->group == group) {
            vp_vec_append(aux, b);
        }
    }
}

void gen_tasks_groups(struct vp_vec *tl,
    const struct vp_vec *bl,
    const struct vp_vec *nl,
    unsigned int n_tasks)
{
    struct task *t;
    id_t bid, nid;
    static id_t tid = 0;
    enum task_t ev;
    int bsize, ev_size;
    struct vp_vec aux_bl;
    vp_vec_alloc(&aux_bl, bl->length);

    //assert(bl->length != 0 && "block list length is 0");
    //assert(nl->length != 0 && "node list length is 0");
    for (size_t i = 0; i < n_tasks; i++) {
        t = malloc(sizeof(struct task));
        t->id = tid++;
        t->node = vp_vec_rand_pick(nl);
        ev = rand() % 2;// ? READ : WRITE;
        filter_by_group(&aux_bl, bl, t->node->group);
        t->block = vp_vec_rand_pick(&aux_bl);
        //nid = rand() % nl->length;
        //bid = rand() % bl->length;
        //t->block = vp_vec_get(bl, bid);
        //t->node = vp_vec_get(nl, nid);
        t->type = ev;
        //t->time = 0;
        bsize = t->block->size;
        ev_size = ((rand() % bsize) - (bsize / 2)) * .75;
        if (ev == WRITE) {
            t->size = ev_size;
            //t->size = ((rand() % bsize) - (bsize / 2)) * .75;
        } else {
            t->size = 0;
        }
        t->stage = 0;
        t->flow_rc = 0;
        //printf("%c bl size: %d ev size: %d\n", ev == WRITE ? 'W' : 'R', bsize, t->size);
        vp_vec_append(tl, t);
    }
    vp_vec_free(&aux_bl);
}

/*
void gen_tasks(struct vp_vec *tl,
    const struct vp_vec *bl,
    const struct vp_vec *nl,
    unsigned int n_tasks)
{
    struct event *t;
    id_t bid, nid;
    enum event_t ev;
    int bsize;

    assert(bl->length != 0 && "block list length is 0");
    assert(nl->length != 0 && "node list length is 0");
    for (size_t i = 0; i < n_tasks; i++) {
        t = malloc(sizeof(struct event));
        bid = rand() % bl->length;
        nid = rand() % nl->length;
        ev = rand() % 2 ? READ : WRITE;
        t->block = vp_vec_get(bl, bid);
        t->node = vp_vec_get(nl, nid);
        t->type = ev;
        t->time = 0;
        bsize = t->block->size;
        if (ev == WRITE) {
            t->size = ((rand() % bsize) - (bsize / 2)) * .75;
        } else {
            t->size = 0;
        }
        //printf("%c bl size: %d ev size: %d\n", ev == WRITE ? 'W' : 'R', bsize, t->size);
        vp_vec_append(tl, t);
    }
}
    */

/*
#define MAX_RAND_TOPOL 255
void gen_rand_topol(struct topology *t, unsigned int n_nodes)
{
    t->num_nodes = n_nodes;
    t->graph = calloc(n_nodes * n_nodes, sizeof(int));

    for (size_t i = 0; i < n_nodes; i++) {
        for (size_t j = 0; j < n_nodes; j++) {
            if (i == j) {
                continue;
            }
            t->graph[i*n_nodes + j] = rand() % MAX_RAND_TOPOL;
        }
    }
}
    */
/*
void gen_all(struct vp_vec *bl,
             struct vp_vec *nl,
             struct vp_vec *tl,
             unsigned int n_blocks,
             unsigned int n_nodes,
             unsigned int n_tasks,
             unsigned int seed)
{
    gen_init(seed);
    gen_blocks(bl, n_blocks);
    printf("gen_blocks done\n");
    gen_nodes(nl, bl, n_nodes, n_blocks / n_nodes);
    printf("gen_nodes done\n");
    gen_tasks(tl, bl, nl, n_tasks);
    printf("gen_tasks done\n");
}
    */
