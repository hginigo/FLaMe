#include "event.h"
#include "structs.h"
#include "vp_heap.h"

extern struct vp_heap event_queue;

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
	ev->stale = 0;
	return ev;
}

void event_free(struct event *ev)
{
	free(ev);
}

/* Min-heap order: earliest dispatch_time pops first. */
int event_cmp(const void *a, const void *b)
{
	const struct event *ea = a;
	const struct event *eb = b;
	if (ea->dispatch_time < eb->dispatch_time) return -1;
	if (ea->dispatch_time > eb->dispatch_time) return 1;
	return 0;
}

void event_enqueue(struct event *ev)
{
	vp_heap_push(&event_queue, ev);
}
