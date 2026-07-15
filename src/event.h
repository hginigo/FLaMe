#ifndef _EVENT_H
#define _EVENT_H
#include "structs.h"

struct event *event_alloc(enum event_t type, time_t disp_time);
void event_free(struct event *ev);
void event_enqueue(struct event *ev);

#endif // _EVENT_H