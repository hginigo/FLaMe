#ifndef _GEN_H
#define _GEN_H
#include "structs.h"
#include "vp_vec.h"
#include "topology.h"

void gen_init(unsigned int seed);
void gen_models(struct vp_vec *ml,
    unsigned int n_models,
    unsigned int n_groups,
    unsigned int model_size);
id_t gen_nodes(struct vp_vec *nl,
    struct vp_vec *ml,
    unsigned int n_nodes,
    unsigned int n_groups,
    int directory,
    int models_per_node);

#endif /* _GEN_H */
