#ifndef _TOPOLOGY_H
#define _TOPOLOGY_H
#include "vp_vec.h"
#include "structs.h"

struct link {
    id_t orig;
    id_t dest;
    int weight;         /* capacity in B/ms: .tpl weight (Mbit/s) * Bpms */
    int latency;        /* fixed one-hop propagation delay, in ms */
    struct vp_vec active_flows;
};

struct topology {
    size_t num_nodes;
    struct vp_vec *adj_lists;
    int directed;
    struct topology *virt_topo;
};

struct link *link_alloc(id_t orig,
                        id_t dest,
                        int weight,
                        int latency);
void topo_init(struct topology *t, size_t num_nodes);
void topo_free(struct topology *t);

void dijkstra_init(const struct topology *t);
void path_resolve(const struct topology *t,
    id_t orig,
    id_t dest,
    struct vp_vec *path);
int path_hops(const struct topology *t,
    id_t orig,
    id_t dest);

/*
 * Ad-hoc single-pair path search, run fresh (no cache) at flow-creation
 * time: edge cost is link_route_cost() (1/bw for a newcomer flow) plus a
 * fixed `hop_penalty` per hop, so the search routes around what is contended
 * now instead of always taking the static hop-count-shortest path. Same
 * predecessor-from-dest, walk-forward-from-orig convention as path_resolve.
 * hop_penalty == 0 is pure contention routing (ROUTING_DYNAMIC); a large
 * penalty makes length dominate and degenerates toward static hop-count
 * routing (ROUTING_HYBRID with a tunable knob between the two).
 */
void path_resolve_dynamic(const struct topology *t,
    id_t orig,
    id_t dest,
    struct vp_vec *path,
    int hop_penalty);

/*
 * Widest-path variant of path_resolve_dynamic: instead of minimizing the sum
 * of 1/bw over the path, it maximizes the path's bottleneck bandwidth (the
 * min corresp_bw across hops) -- i.e. the same quantity path_min_bw() will
 * compute once the flow is attached. hop_penalty subtracts a fixed B/ms from
 * the running bottleneck per hop (widest-path analog of the dynamic penalty):
 * 0 is pure widest, a large value degenerates toward shortest-path. Same
 * predecessor-from-dest, walk-forward-from-orig convention as path_resolve.
 */
void path_resolve_widest(const struct topology *t,
    id_t orig,
    id_t dest,
    struct vp_vec *path,
    int hop_penalty);

/*
 * Build path_aux as the exact physical reverse of an already-resolved path
 * (reverse-direction link object of each hop, in reverse order). For
 * undirected topologies only, where a flow contends on both directions of
 * every physical edge it crosses; derived from path so the two stay
 * symmetric, which an independent reverse search does not guarantee.
 */
void path_reverse(const struct topology *t,
    const struct vp_vec *path,
    struct vp_vec *path_aux);

/* Sum of ->latency over an already-resolved path (e.g. flow->path). Use
 * this instead of re-deriving a path from (orig, dest) so latency always
 * matches whichever path was actually chosen, static or dynamic. */
int path_vec_latency(const struct vp_vec *path);

int topo_multi_read(char *str, struct topology *t);

#endif // _TOPOLOGY_H