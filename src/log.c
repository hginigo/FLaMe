#include "log.h"
#include "topology.h"
#include <stdarg.h>
#include <string.h>

extern time_t sim_time;

#define ROW_FMT "%9s  %-12s %-5s %-13s %-3s %s"

static const char *task_type_name(enum task_t type)
{
	switch (type) {
	case READ:    return "READ";
	case WRITE:   return "WRITE";
	case TRAIN:   return "TRAIN";
	case BARRIER: return "BARRIER";
	}
	return "?";
}

void log_header(void)
{
	dbg(ROW_FMT "\n", "time(ms)", "event", "node", "task", "st", "detail");
}

void log_line(const char *event, const struct node *n, const struct task *t,
	int stage, const char *fmt, ...)
{
	char time[24], node[16] = "-", task[32] = "-", st[12] = "-";
	char detail[1024], row[1200];
	size_t len;
	va_list ap;

	snprintf(time, sizeof(time), "%ld", (long) sim_time);
	if (n) snprintf(node, sizeof(node), "n%u", n->id);
	if (t) snprintf(task, sizeof(task), "t%u %s", t->id, task_type_name(t->type));
	if (stage >= 0) snprintf(st, sizeof(st), "%d", stage);

	va_start(ap, fmt);
	vsnprintf(detail, sizeof(detail), fmt, ap);
	va_end(ap);

	snprintf(row, sizeof(row), ROW_FMT, time, event, node, task, st, detail);
	len = strlen(row);
	while (len && row[len - 1] == ' ') row[--len] = '\0';
	dbg("%s\n", row);
}

const char *log_path(char *buf, size_t size, const struct vp_vec *path)
{
	struct link *l;
	size_t off = 0;

	buf[0] = '\0';
	vp_for (l, path) {
		if (off == 0) {
			off += (size_t) snprintf(buf, size, "%u", l->orig);
		}
		if (off < size) {
			off += (size_t) snprintf(buf + off, size - off, ">%u", l->dest);
		}
	}
	return buf;
}

const char *log_load(char *buf, size_t size, const struct vp_vec *path)
{
	struct link *l;
	size_t off = 0;

	buf[0] = '\0';
	vp_for (l, path) {
		if (off < size) {
			off += (size_t) snprintf(buf + off, size - off, off ? ",%zu" : "%zu",
				l->active_flows.length);
		}
	}
	return buf;
}
