#include "event.h"
#include "structs.h"

#ifdef USE_VP_LIST_QUEUE
extern struct vp_list event_queue;
#else
extern struct vp_vec event_queue;
#endif

struct event *event_alloc(enum event_t type, time_t disp_time)
{
	struct event *ev = malloc(sizeof(struct event));
	if (!ev) {
    	return NULL;
	}
	static id_t id_count = 0;
	ev->type = type;
	ev->dispatch_time = disp_time;
	ev->id = id_count++;
	return ev;
}

void event_free(struct event *ev)
{
	free(ev);
}

void event_enqueue(struct event *ev)
{
	const struct event *aux;
	//printf("event enqueue\n");

#ifdef USE_VP_LIST_QUEUE
	for (struct vp_list_node *i = event_queue.first; i != NULL && i->status != VP_LIST_NODE_FREE; i = i->next) {
		aux = i->item;
#else
	for (size_t i = 0; i < event_queue.length; i++) {
		aux = vp_vec_get(&event_queue, i);
#endif
		if (aux->dispatch_time > ev->dispatch_time) {
			//printf("event insert\n");
#ifdef USE_VP_LIST_QUEUE
			i = vp_list_insert_before(&event_queue, ev, i);
#else
			vp_vec_insert(&event_queue, ev, i);
#endif
			return;
		}
	}
#ifdef USE_VP_LIST_QUEUE
	vp_list_append(&event_queue, ev);
#else
	vp_vec_append(&event_queue, ev);
#endif
}