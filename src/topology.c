#include "topology.h"
#include "config.h"
#include "vp_heap.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>

extern struct config config;

#define STR_LEN 512
char line[STR_LEN];

/* Fixed-point numerator for the 1/bandwidth costs used by the weighted
 * (ROUTING_DIJKSTRA) cache and by link_route_cost(): link weights are big
 * enough that plain integer 1/weight would floor to 0, so costs are
 * SCALE/weight instead. */
#define ROUTE_COST_SCALE 1000000

/*
 * Reads a .tpl topology file:
 *   - line 1: the node count.
 *   - then one line per edge, "orig dest weight [latency]". weight is the link
 *     capacity in Mbit/s, converted to the simulator's bytes-per-ms unit by the
 *     Bpms multiplier (structs.h); latency is the one-hop propagation delay in
 *     ms, defaulting to config.latency_ms when the 4th field is absent.
 *   - a line parsing to 0 fields (e.g. "-") closes the current graph and starts
 *     a new virtual overlay, chained onto virt_topo. A file is therefore one
 *     physical graph followed by N per-round overlay graphs.
 * On an undirected topology each edge also gets a reverse link object, so the
 * two directions share one capacity (half-duplex).
 */
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

/* Orders the dijkstra2 priority queue by accumulated path cost (struct link
 * reused as a plain (dest, cost) pair, exactly as link_alloc(0, dest, cost, 0)
 * builds it below — these are queue entries, not real topology links). */
static int pq_cmp(const void *a, const void *b)
{
    const struct link *la = a;
    const struct link *lb = b;
    if (la->weight < lb->weight) return -1;
    if (la->weight > lb->weight) return 1;
    return 0;
}

/* Reverse of pq_cmp: vp_heap is a min-heap by cmp, so this makes it pop the
 * largest ->weight first -- used by path_resolve_widest, where ->weight
 * holds a running bottleneck bandwidth and the search must always finalize
 * whichever frontier node currently has the widest guaranteed path. */
static int pq_cmp_max(const void *a, const void *b)
{
    return pq_cmp(b, a);
}

int **dist_cache;
size_t num_nodes;

/*
 * Per-edge cost for the precomputed dist_cache. weighted==0 (ROUTING_BFS /
 * ROUTING_STATIC) charges a flat 1 per hop, so the cache holds hop-count
 * shortest paths. weighted==1 (ROUTING_DIJKSTRA) charges SCALE/weight, i.e.
 * 1/bandwidth, so the cache holds paths that minimise total transmission
 * time over each link's max bandwidth cap (contention is ignored — active
 * flows never enter this cost). When every link shares one bandwidth cap the
 * weighted cost is a constant per hop, so both modes minimise the same
 * quantity and produce identical caches.
 */
static int link_cache_cost(const struct link *l, int weighted)
{
    long long c;

    if (!weighted) {
        return 1;
    }
    if (l->weight <= 0) {
        return INT_MAX;
    }
    c = (long long) ROUTE_COST_SCALE / l->weight;
    return c > 0 ? (int) c : 1;
}

int dijkstra2(const struct topology *t,
    id_t orig,
    int weighted)
{
    size_t n_nodes = t->num_nodes;
    int *dists = malloc(n_nodes * sizeof(int));
    char *done = calloc(n_nodes, sizeof(char));
    int *dis = dist_cache[orig];
    struct vp_heap pq;
    struct vp_vec *adj_list;
    struct link *d, *aux_dist, *d2;
    id_t pivot;
    int cost;

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
            cost = link_cache_cost(aux_dist, weighted);
            if (dists[aux_dist->dest] > (dists[pivot] + cost)) {
                dists[aux_dist->dest] = dists[pivot] + cost;
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
    /* Weighted (1/bandwidth) cache only for ROUTING_DIJKSTRA; every other mode
     * uses the hop-count cache -- ROUTING_BFS/STATIC route on it directly, and
     * the ad-hoc modes (dynamic/widest/hybrid) still read it via path_hops for
     * nearest-replica selection. */
    int weighted = (config.routing == ROUTING_DIJKSTRA);

    assert(t != NULL);
    num_nodes = t->num_nodes;
    assert(num_nodes > 0);
    dist_cache = malloc(num_nodes * sizeof(unsigned int *));
    for (size_t i = 0; i < num_nodes; i++) {
        dist_cache[i] = calloc(num_nodes, sizeof(unsigned int));
        dijkstra2(t, i, weighted);
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

/*
 * Build path_aux as the exact physical reverse of an already-resolved path:
 * walk path backwards and, for each hop's link (X -> Y), append the
 * reverse-direction link object (Y -> X) found in adj_lists[Y]. Used for
 * undirected topologies, where a flow must register contention on both
 * directions of every physical edge it crosses. Deriving path_aux this way
 * keeps it perfectly symmetric with path (identical physical edges), which
 * an independent second path search does NOT guarantee once shortest-path
 * ties or contention differences let the two searches pick different routes.
 */
void path_reverse(const struct topology *t,
    const struct vp_vec *path,
    struct vp_vec *path_aux)
{
    struct link *l, *rev;
    struct vp_vec *adj_list;

    for (size_t i = path->length; i-- > 0; ) {
        l = vp_vec_get(path, i);
        adj_list = &t->adj_lists[l->dest];
        vp_for (rev, adj_list) {
            if (rev->dest == l->orig) {
                vp_vec_append(path_aux, rev);
                break;
            }
        }
    }
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
 * dist_cache, and costed by link_route_cost() (1/bandwidth per hop) plus a
 * fixed `hop_penalty` per hop, instead of a flat 1 per hop, so the search
 * steers toward currently-fast, uncontended links. hop_penalty == 0 is pure
 * contention routing; a larger value prices each extra hop and makes the
 * search prefer shorter paths, degenerating toward static hop-count routing
 * as it grows. Early-exits once orig itself is finalized, since this is a
 * single-pair query, not an all-pairs precompute.
 */
void path_resolve_dynamic(const struct topology *t,
    id_t orig,
    id_t dest,
    struct vp_vec *path,
    int hop_penalty)
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
            /* add the fixed per-hop penalty, saturating instead of overflowing */
            cost = cost < INT_MAX - hop_penalty ? cost + hop_penalty : INT_MAX;
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

/*
 * corresp_bw of `l` in raw B/ms: the bandwidth share a newcomer flow would
 * get once it joins (l->weight / (active_flows.length + 1); l->weight is
 * already Bpms-scaled). Larger is better -- it feeds a widest-path
 * (bottleneck-maximizing) search instead of a shortest-path one. Unlike
 * link_route_cost this is NOT ROUTE_COST_SCALE-scaled: weight >> flow count
 * so there is no floor-to-zero risk, and keeping it in real B/ms both avoids
 * the INT_MAX saturation that used to make all uncontended links look equal
 * and puts it on a scale where path_resolve_widest's hop_penalty (also in
 * B/ms of bottleneck given up per hop) is a meaningful knob.
 */
static int link_bandwidth(const struct link *l)
{
    if (l->weight <= 0) {
        return 0;
    }
    return l->weight / (int) (l->active_flows.length + 1);
}

/*
 * Widest-path search: maximize the path's bottleneck bandwidth (min
 * corresp_bw across hops), the same quantity path_min_bw() computes once the
 * flow attaches. hop_penalty subtracts a fixed B/ms from the running
 * bottleneck at every hop, so a longer path's effective width is discounted
 * -- the widest-path analog of the additive penalty path_resolve_dynamic
 * uses. penalty 0 is pure widest; a large penalty makes hop count dominate
 * and degenerates toward shortest-path routing. Because the penalty can push
 * the running value negative, unreached nodes use INT_MIN (not -1) as the
 * "no path yet" sentinel. Same predecessor-from-dest / walk-forward-from-orig
 * convention as the other resolvers.
 */
void path_resolve_widest(const struct topology *t,
    id_t orig,
    id_t dest,
    struct vp_vec *path,
    int hop_penalty)
{
    size_t n_nodes = t->num_nodes;
    int *dists = malloc(n_nodes * sizeof(int));
    int *pred = malloc(n_nodes * sizeof(int));
    char *done = calloc(n_nodes, sizeof(char));
    struct vp_heap pq;
    struct vp_vec *adj_list;
    struct link *d, *aux_link, *d2;
    id_t pivot;
    int bw, cand;

    for (size_t i = 0; i < n_nodes; i++) {
        dists[i] = INT_MIN;
        pred[i] = -1;
    }
    dists[dest] = INT_MAX;
    pred[dest] = (int) dest;

    vp_heap_alloc(&pq, n_nodes, pq_cmp_max);
    d = link_alloc(0, dest, INT_MAX, 0);
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
            bw = link_bandwidth(aux_link);
            /* bottleneck so far, then discount this hop */
            cand = bw < dists[pivot] ? bw : dists[pivot];
            cand -= hop_penalty;
            if (cand > dists[aux_link->dest]) {
                dists[aux_link->dest] = cand;
                pred[aux_link->dest] = (int) pivot;
                d2 = link_alloc(0, aux_link->dest, cand, 0);
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
