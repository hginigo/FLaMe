#ifndef _FLOW_H
#define _FLOW_H
#include "structs.h"
#include "vp_vec.h"
#include <time.h>

void path_attach_flow(struct vp_vec *path, struct flow *f);
void path_detach_flow(struct vp_vec *path, struct flow *f);
int path_min_bw(const struct vp_vec *path_list);
time_t flow_recalc_makespan(struct flow *f, time_t cur_time);
void flows_reschedule(struct flow *trigger);
struct flow *flow_alloc(id_t orig,
			   id_t dest,
			   size_t nbytes,
			   struct task *t);
void flow_dealloc(struct flow *f);
void flow_enqueue(id_t orig, id_t dest, size_t nbytes, struct task *t);

#endif // _FLOW_H