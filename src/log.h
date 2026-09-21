#ifndef _LOG_H
#define _LOG_H
#include "structs.h"

/*
 * Event log, written to the `debug` stream: one aligned row per event,
 *
 *   time(ms) event        node  task          st  detail
 *         40 FLOW_START   n8    t81 READ      1   f1 4->8 1048576B path 4>3>8 ...
 *
 * `node` and `task` are the task the event belongs to (for a flow, the task
 * that issued it); `st` is the stage that did the work -- for a flow, the
 * stage that started it, not the one its task has since moved on to. Columns
 * that do not apply read "-". Every row is written by a single call, so an
 * event is never split across lines.
 */
void log_header(void);
void log_line(const char *event, const struct node *n, const struct task *t,
	int stage, const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/* "8>16>17>13>4": the nodes a path visits, for a flow's detail column. */
const char *log_path(char *buf, size_t size, const struct vp_vec *path);
/* "2,1,1,3": active flows on each link of a path, this flow included. */
const char *log_load(char *buf, size_t size, const struct vp_vec *path);

#endif /* _LOG_H */
