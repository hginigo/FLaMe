#ifndef _WORKLOADS_H
#define _WORKLOADS_H

#include "structs.h"
#include "topology.h"

void workload_gen(struct vp_vec *tl,
    const struct vp_vec *nl,
    const struct vp_vec *bl,
    const struct topology *t,
    int n_rounds);
void tasks_free(struct vp_vec *tl);

#endif // _WORKLOADS_H