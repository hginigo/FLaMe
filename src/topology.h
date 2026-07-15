#ifndef _TOPOLOGY_H
#define _TOPOLOGY_H
#include "vp_vec.h"
#include "structs.h"

struct link {
    id_t orig;
    id_t dest;
    int weight;
    struct vp_vec active_flows;
};

struct topology {
    size_t num_nodes;
    struct vp_vec *adj_lists;
    int directed;
    struct topology *virt_topo;
};

void topo_read(char *str, struct topology *t);
struct link *link_alloc(id_t orig,
                        id_t dest,
                        int weight);
void link_free(struct link *d);
void topo_init(struct topology *t, size_t num_nodes);
void topo_free(struct topology *t);
int dijkstra(const struct topology *t,
             id_t orig,
             id_t dest,
             int *n_hops);

void dijkstra_init(const struct topology *t);
void dist_cache_print();
void path_resolve(const struct topology *t,
    id_t orig,
    id_t dest,
    struct vp_vec *path);
int path_hops(const struct topology *t,
    id_t orig,
    id_t dest);
int topo_multi_read(char *str, struct topology *t);

#endif // _TOPOLOGY_H