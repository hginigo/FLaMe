#include "topology.h"
#include "config.h"
#include "vp_heap.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>

extern struct config config;

#define STR_LEN 512
char line[STR_LEN];

int topo_multi_read(char *str, struct topology *t)
{
    FILE *f = fopen(str, "r");
    char *res;
    int ret;
    int n_nodes;
    int orig, dest, weight, latency;
    struct link *l;
    struct vp_vec *al;
    struct topology *aux;

    if (f == NULL) {
        perror("fopen");
        exit(1);
    }
    res = fgets(line, STR_LEN, f);
    ret = sscanf(line, "%d\n", &n_nodes);
    topo_init(t, n_nodes);

    aux = t;
    while ((res = fgets(line, STR_LEN, f))) {
        ret = sscanf(line, "%d %d %d %d\n", &orig, &dest, &weight, &latency);
        if (ret == 3 || ret == 4) {
            if (ret == 3) {
                latency = config.latency_ms;
            }
            l = link_alloc(orig, dest, weight * Bpms, latency);
            al = &aux->adj_lists[orig];
            vp_vec_append(al, l);
            if (!aux->directed) {
                l = link_alloc(dest, orig, weight * Bpms, latency);
                al = &aux->adj_lists[dest];
                vp_vec_append(al, l);
            }
        } else if (ret == 0) {
            aux->virt_topo = malloc(sizeof(struct topology));
            aux = aux->virt_topo;
            aux->directed = t->directed;
            topo_init(aux, n_nodes);
        } else {
            perror("sscanf");
            fclose(f);
            exit(1);
        }

    }
    fclose(f);
    return 0;
}

struct link *link_alloc(id_t orig,
                        id_t dest,
                        int weight,
                        int latency)
{
    struct link *aux = malloc(sizeof(struct link));

    aux->orig = orig;
    aux->dest = dest;
    aux->weight = weight;
    aux->latency = latency;
    memset(&aux->active_flows, 0, sizeof(struct vp_vec));
    return aux;
}

void dist_free(struct link *d)
{
    free(d);
}

void topo_init(struct topology *t, size_t num_nodes)
{
    assert(t != NULL);
    t->adj_lists = malloc(num_nodes * sizeof(struct vp_vec));
    memset(t->adj_lists, 0, sizeof(struct vp_vec) * num_nodes);
    t->num_nodes = num_nodes;
    t->virt_topo = NULL;
}

void topo_free(struct topology *t)
{
    struct vp_vec *vec;
    struct link *d;
    
    for (size_t i = 0; i < t->num_nodes; i++) {
        vec = t->adj_lists + i;
        for (size_t j = 0; j < vec->length; j++) {
            d = vp_vec_get(vec, j);
            vp_vec_free(&d->active_flows);
            free(d);
        }
        vp_vec_free(vec);
    }
    free(t->adj_lists);
    if (t->virt_topo != NULL) {
        topo_free(t->virt_topo);
    }
}

/* Orders the dijkstra2 priority queue by hop-count (struct link reused as
 * a plain (dest, weight) pair, exactly as link_alloc(0, dest, weight, 0)
 * builds it below — these are queue entries, not real topology links). */
static int pq_cmp(const void *a, const void *b)
{
    const struct link *la = a;
    const struct link *lb = b;
    if (la->weight < lb->weight) return -1;
    if (la->weight > lb->weight) return 1;
    return 0;
}

int **dist_cache;
size_t num_nodes;

int dijkstra2(const struct topology *t,
    id_t orig)
{
    size_t n_nodes = t->num_nodes;
    int *dists = malloc(n_nodes * sizeof(int));
    char *done = calloc(n_nodes, sizeof(char));
    int *dis = dist_cache[orig];
    struct vp_heap pq;
    struct vp_vec *adj_list;
    struct link *d, *aux_dist, *d2;
    id_t pivot;

    vp_heap_alloc(&pq, n_nodes, pq_cmp);
    for (size_t i = 0; i < n_nodes; i++) {
        dists[i] = INT_MAX;
        dis[i] = -1;
    }

    d = link_alloc(0, orig, 0, 0);
    vp_heap_push(&pq, d);
    dists[orig] = 0;
    dis[orig] = orig;

    while (pq.length > 0) {
        d = vp_heap_pop(&pq);
        pivot = d->dest;
        dist_free(d);
        if (done[pivot]) {
            continue;	/* stale duplicate: already finalized with a <= distance */
        }
        done[pivot] = 1;
        adj_list = &t->adj_lists[pivot];

        for (size_t i = 0; i < adj_list->length; i++) {
            aux_dist = vp_vec_get(adj_list, i);
            if (dists[aux_dist->dest] > (dists[pivot] + 1)) {
                dists[aux_dist->dest] = dists[pivot] + 1;
                dis[aux_dist->dest] = (int) pivot;
                d2 = link_alloc(0, aux_dist->dest, dists[aux_dist->dest], 0);
                vp_heap_push(&pq, d2);
            }
        }
    }

    free(dists);
    free(done);
    vp_heap_free(&pq);
    return 0;
}

void dijkstra_init(const struct topology *t)
{
    assert(t != NULL);
    num_nodes = t->num_nodes;
    assert(num_nodes > 0);
    dist_cache = malloc(num_nodes * sizeof(unsigned int *));
    for (size_t i = 0; i < num_nodes; i++) {
        dist_cache[i] = calloc(num_nodes, sizeof(unsigned int));
        dijkstra2(t, i);
    }
}

void path_resolve(const struct topology *t,
    id_t orig,
    id_t dest,
    struct vp_vec *path)
{
    int *d = dist_cache[dest];
    id_t pivot = orig;
    struct vp_vec *adj_list;
    struct link *aux;
    while (pivot != dest) {
        adj_list = &t->adj_lists[pivot];
        vp_for (aux, adj_list) {
            if (aux->dest == d[pivot]) {
                vp_vec_append(path, aux);
            }
        }
        pivot = d[pivot];
    }
}

int path_hops(const struct topology *t,
    id_t orig,
    id_t dest)
{
    int *d = dist_cache[dest];
    id_t pivot = orig;
    struct vp_vec *adj_list;
    struct link *aux;
    int hops = 0;

    while (pivot != dest) {
        adj_list = &t->adj_lists[pivot];
        vp_for (aux, adj_list) {
            if (aux->dest == d[pivot]) {
                hops++;
            }
        }
        pivot = d[pivot];
    }
    return hops;
}

int path_vec_latency(const struct vp_vec *path)
{
    struct link *l;
    int latency = 0;

    vp_for (l, path) {
        latency += l->latency;
    }
    return latency;
}

/*
 * Fixed-point cost of routing one more flow over `l`: 1 / corresp_bw,
 * where corresp_bw = l->weight / (active_flows.length + 1) is the share
 * a newcomer flow would get once it joins (existing flows plus this one).
 * Scaled by ROUTE_COST_SCALE because weight (bandwidth, already Bpms-
 * scaled) is orders of magnitude bigger than active_flows.length+1, so
 * unscaled integer division would floor to 0 for nearly every link.
 */
#define ROUTE_COST_SCALE 1000000

static int link_route_cost(const struct link *l)
{
    long long numer, cost;

    if (l->weight <= 0) {
        return INT_MAX;
    }
    numer = (long long) (l->active_flows.length + 1) * ROUTE_COST_SCALE;
    cost = numer / l->weight;
    return cost > INT_MAX ? INT_MAX : (int) cost;
}

/*
 * Same predecessor-from-dest / walk-forward-from-orig shape as
 * path_resolve, but run fresh against the live graph instead of
 * dist_cache, and costed by link_route_cost() (sum of 1/bandwidth per
 * hop) instead of a flat 1 per hop, so the search steers toward
 * currently-fast, uncontended links instead of always taking the static
 * hop-count-shortest path. Early-exits once orig itself is finalized,
 * since this is a single-pair query, not an all-pairs precompute.
 */
void path_resolve_dynamic(const struct topology *t,
    id_t orig,
    id_t dest,
    struct vp_vec *path)
{
    size_t n_nodes = t->num_nodes;
    int *dists = malloc(n_nodes * sizeof(int));
    int *pred = malloc(n_nodes * sizeof(int));
    char *done = calloc(n_nodes, sizeof(char));
    struct vp_heap pq;
    struct vp_vec *adj_list;
    struct link *d, *aux_link, *d2;
    id_t pivot;
    int cost;

    for (size_t i = 0; i < n_nodes; i++) {
        dists[i] = INT_MAX;
        pred[i] = -1;
    }
    dists[dest] = 0;
    pred[dest] = (int) dest;

    vp_heap_alloc(&pq, n_nodes, pq_cmp);
    d = link_alloc(0, dest, 0, 0);
    vp_heap_push(&pq, d);

    while (pq.length > 0) {
        d = vp_heap_pop(&pq);
        pivot = d->dest;
        dist_free(d);
        if (done[pivot]) {
            continue;
        }
        done[pivot] = 1;
        if (pivot == orig) {
            break;
        }
        adj_list = &t->adj_lists[pivot];
        for (size_t i = 0; i < adj_list->length; i++) {
            aux_link = vp_vec_get(adj_list, i);
            cost = link_route_cost(aux_link);
            if (dists[aux_link->dest] > dists[pivot] + cost) {
                dists[aux_link->dest] = dists[pivot] + cost;
                pred[aux_link->dest] = (int) pivot;
                d2 = link_alloc(0, aux_link->dest, dists[aux_link->dest], 0);
                vp_heap_push(&pq, d2);
            }
        }
    }
    while (pq.length > 0) {
        d = vp_heap_pop(&pq);
        dist_free(d);
    }
    vp_heap_free(&pq);

    pivot = orig;
    while (pivot != dest) {
        adj_list = &t->adj_lists[pivot];
        vp_for (aux_link, adj_list) {
            if (aux_link->dest == (id_t) pred[pivot]) {
                vp_vec_append(path, aux_link);
            }
        }
        pivot = (id_t) pred[pivot];
    }

    free(dists);
    free(pred);
    free(done);
}
