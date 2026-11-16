#ifndef _EVENT_H
#define _EVENT_H
#include "structs.h"

struct event *event_alloc(enum event_t type, time_t disp_time);
void event_free(struct event *ev);
void event_enqueue(struct event *ev);
int event_cmp(const void *a, const void *b);
/* Schedule the end of task t at `at`; its node then dispatches what it can. */
void task_done(struct task *t, time_t at);

#endif // _EVENT_H