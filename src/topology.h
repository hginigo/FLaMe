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
int path_latency(const struct topology *t,
    id_t orig,
    id_t dest);
int topo_multi_read(char *str, struct topology *t);

#endif // _TOPOLOGY_H