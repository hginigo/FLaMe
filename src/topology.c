#include "topology.h"
#include "config.h"
#include "vp_heap.h"
#include <stdio.h>
#include <limits.h>
#include <float.h>
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
 *   - then one line per edge, "orig dest [weight [latency]]". weight is the
 *     link capacity in Mbit/s (SI; decimals and k/M/G suffixes accepted, so
 *     "0.1" == "100k" == 100 Kbit/s), converted to the simulator's bytes-per-ms
 *     unit by the Bpms multiplier (structs.h), defaulting to
 *     config.bandwidth_mbps when absent; latency is the one-hop propagation delay in ms, defaulting
 *     to config.latency_ms when absent. Either may also be written "-" to take
 *     its default explicitly, so "0 1 - 10" is a default-bandwidth, 10 ms link.
 *     An edge of the physical graph with no weight and no bandwidth_mbps set is
 *     an error; overlay weights are never used for routing, so there it is not.
 *   - a line whose first field is not a number (e.g. "-") closes the current
 *     graph and starts a new virtual overlay, chained onto virt_topo. A file is
 *     therefore one physical graph followed by N per-round overlay graphs.
 * On an undirected topology each edge also gets a reverse link object, so the
 * two directions share one capacity (half-duplex).
 *
 * Nodes need not be connected. A node no edge mentions is a disconnected
 * agent: in the physical graph it has no route to anyone (see topo_reachable),
 * in an overlay it has no peers that round. Either way it still trains and
 * aggregates its own model, it just never exchanges one.
 */
/* One numeric .tpl field: 1 and *out set if it is an integer, 0 if it is the
 * "-" placeholder (use the default), -1 if it is anything else. */
static int tpl_field(const char *tok, int *out)
{
    char *end;
    long v;

    if (strcmp(tok, "-") == 0) {
        return 0;
    }
    v = strtol(tok, &end, 10);
    if (end == tok || *end != '\0' || v < INT_MIN || v > INT_MAX) {
        return -1;
    }
    *out = (int) v;
    return 1;
}

/* The weight field: 1 and *mbps set for a valid rate (see config_parse_mbps),
 * 0 for the "-" placeholder, -1 otherwise. */
static int tpl_rate(const char *tok, double *mbps)
{
    if (strcmp(tok, "-") == 0) {
        return 0;
    }
    return config_parse_mbps(tok, mbps) == 0 ? 1 : -1;
}

int topo_multi_read(char *str, struct topology *t)
{
    FILE *f = fopen(str, "r");
    char *res;
    int ret;
    int n_nodes;
    int orig, dest, latency;
    char *tok[4];
    int ntok, lineno = 1;
    int has_w, has_l;
    double mbps;
    struct link *l;
    struct vp_vec *al;
    struct topology *aux;

    if (f == NULL) {
        perror("fopen");
        exit(1);
    }
    res = fgets(line, STR_LEN, f);
    ret = sscanf(line, "%d\n", &n_nodes);
    if (!res || ret != 1 || n_nodes <= 0) {
        fprintf(stderr, "topology: %s:1: expected the node count\n", str);
        fclose(f);
        exit(1);
    }
    topo_init(t, n_nodes);

    aux = t;
    while ((res = fgets(line, STR_LEN, f))) {
        lineno++;
        ntok = 0;
        for (char *p = strtok(line, " \t\r\n"); p && ntok < 4; p = strtok(NULL, " \t\r\n")) {
            tok[ntok++] = p;
        }
        if (ntok > 0 && tpl_field(tok[0], &orig) != 1) {
            /* "-" or any non-numeric line: start the next overlay */
            aux->virt_topo = malloc(sizeof(struct topology));
            aux = aux->virt_topo;
            aux->directed = t->directed;
            topo_init(aux, n_nodes);
            continue;
        }
        if (ntok < 2 || tpl_field(tok[1], &dest) != 1
                || (has_w = ntok > 2 ? tpl_rate(tok[2], &mbps) : 0) < 0
                || (has_l = ntok > 3 ? tpl_field(tok[3], &latency) : 0) < 0) {
            fprintf(stderr, "topology: %s:%d: expected \"orig dest [weight"
                " [latency]]\"\n", str, lineno);
            fclose(f);
            exit(1);
        }
        if (orig < 0 || orig >= n_nodes || dest < 0 || dest >= n_nodes) {
            fprintf(stderr, "topology: %s: edge %d-%d is outside the %d"
                " declared nodes\n", str, orig, dest, n_nodes);
            fclose(f);
            exit(1);
        }
        if (!has_w) {
            if (config.bandwidth_mbps <= 0 && aux == t) {
                fprintf(stderr, "topology: %s:%d: edge %d-%d has no bandwidth"
                    " and bandwidth_mbps is not set\n", str, lineno, orig, dest);
                fclose(f);
                exit(1);
            }
            mbps = config.bandwidth_mbps > 0 ? config.bandwidth_mbps : 1;
        }
        if (!has_l) {
            latency = config.latency_ms;
        }
        l = link_alloc(orig, dest, mbps * Bpms, latency);
        al = &aux->adj_lists[orig];
        vp_vec_append(al, l);
        if (!aux->directed) {
            l = link_alloc(dest, orig, mbps * Bpms, latency);
            al = &aux->adj_lists[dest];
            vp_vec_append(al, l);
        }
    }
    fclose(f);
    return 0;
}

struct link *link_alloc(id_t orig,
                        id_t dest,
                        double weight,
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

/*
 * Cut agent `n` off the network: free every link out of it and every link
 * into it from the other adjacency lists, so topo_reachable() is false
 * between n and anyone else once dijkstra_init runs. Meant for the physical
 * graph, before dijkstra_init; overlays keep their edges and simply see
 * their READs of or by n dropped at workload generation.
 */
void topo_isolate(struct topology *t, id_t n)
{
    struct vp_vec *al;
    struct link *l;

    al = &t->adj_lists[n];
    vp_for (l, al) {
        vp_vec_free(&l->active_flows);
        free(l);
    }
    al->length = 0;
    for (size_t i = 0; i < t->num_nodes; i++) {
        al = &t->adj_lists[i];
        for (size_t j = al->length; j-- > 0; ) {
            l = vp_vec_get(al, j);
            if (l->dest == n) {
                vp_vec_remove(al, j);
                vp_vec_free(&l->active_flows);
                free(l);
            }
        }
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
    double c;

    if (!weighted) {
        return 1;
    }
    if (l->weight <= 0) {
        return INT_MAX;
    }
    c = ROUTE_COST_SCALE / l->weight;
    if (c >= INT_MAX) {
        return INT_MAX;
    }
    return c >= 1 ? (int) c : 1;
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

/*
 * Is there any physical path orig -> dest? Read off the dist_cache (so only
 * valid after dijkstra_init): dijkstra2 leaves -1 as the predecessor of every
 * node it never reached, i.e. every node in another connected component.
 * Reachability is the same for every routing mode, since the ad-hoc resolvers
 * search the same graph and only differ in how they cost its links.
 */
int topo_reachable(id_t orig, id_t dest)
{
    return dist_cache[dest][orig] >= 0;
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
    if (d[orig] < 0) {
        return;     /* unreachable: leave path empty */
    }
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

    if (d[orig] < 0) {
        return -1;  /* unreachable */
    }
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
    double cost;

    if (l->weight <= 0) {
        return INT_MAX;
    }
    cost = (double) (l->active_flows.length + 1) * ROUTE_COST_SCALE / l->weight;
    return cost >= INT_MAX ? INT_MAX : (int) cost;
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
    while (pred[orig] >= 0 && pivot != dest) {   /* unreachable: path stays empty */
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
static double link_bandwidth(const struct link *l)
{
    if (l->weight <= 0) {
        return 0;
    }
    return l->weight / (double) (l->active_flows.length + 1);
}

/*
 * Widest-path search: maximize the path's bottleneck bandwidth (min
 * corresp_bw across hops), the same quantity path_min_bw() computes once the
 * flow attaches. hop_penalty subtracts a fixed B/ms from the running
 * bottleneck at every hop, so a longer path's effective width is discounted
 * -- the widest-path analog of the additive penalty path_resolve_dynamic
 * uses. penalty 0 is pure widest; a large penalty makes hop count dominate
 * and degenerates toward shortest-path routing. Because the penalty can push
 * the running value negative, unreached nodes use -DBL_MAX (not -1) as the
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
    double *dists = malloc(n_nodes * sizeof(double));
    int *pred = malloc(n_nodes * sizeof(int));
    char *done = calloc(n_nodes, sizeof(char));
    struct vp_heap pq;
    struct vp_vec *adj_list;
    struct link *d, *aux_link, *d2;
    id_t pivot;
    double bw, cand;

    for (size_t i = 0; i < n_nodes; i++) {
        dists[i] = -DBL_MAX;
        pred[i] = -1;
    }
    dists[dest] = DBL_MAX;
    pred[dest] = (int) dest;

    vp_heap_alloc(&pq, n_nodes, pq_cmp_max);
    d = link_alloc(0, dest, DBL_MAX, 0);
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
    while (pred[orig] >= 0 && pivot != dest) {   /* unreachable: path stays empty */
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
