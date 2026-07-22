#ifndef _TOPOLOGY_H
#define _TOPOLOGY_H
#include "vp_vec.h"
#include "structs.h"

struct link {
    id_t orig;
    id_t dest;
    int weight;
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
 * time: edge cost is 1 + the link's current active_flows count, so the
 * search routes around whatever is contended right now instead of always
 * taking the static hop-count-shortest path. Same predecessor-from-dest,
 * walk-forward-from-orig convention as path_resolve.
 */
void path_resolve_dynamic(const struct topology *t,
    id_t orig,
    id_t dest,
    struct vp_vec *path);

/* Sum of ->latency over an already-resolved path (e.g. flow->path). Use
 * this instead of re-deriving a path from (orig, dest) so latency always
 * matches whichever path was actually chosen, static or dynamic. */
int path_vec_latency(const struct vp_vec *path);

int topo_multi_read(char *str, struct topology *t);

#endif // _TOPOLOGY_H