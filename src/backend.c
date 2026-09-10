#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include "structs.h"
#include "backend.h"
#include "config.h"
#include "trace.h"

extern struct config config;

struct backend backend = {0};

/*
 * Synthetic backend: no external process, no training math, NO params buffer.
 * A model is represented only by its size: model-transfer flows are sized by
 * config.model_size and every TRAIN takes config.train_time sim-ms. Replicas
 * carry params == NULL with nbytes == model_size; the copy sites that would
 * otherwise duplicate a params blob (replica_dup / params_dup) treat NULL as
 * "nothing to copy", so nothing O(model_size) is ever allocated or moved --
 * that is the whole point of avoiding the Python pipe.
 *
 * The trace backend is the same machinery with recorded timings: identical
 * zero-copy model handling (so model_size still governs transfer sizes, which
 * the Python backend cannot honour since it decides its own blob size), but
 * TRAIN and aggregation cost what the trace recorded for that (node, round)
 * rather than one flat figure. Both are "local" modes -- everything below
 * that is not BACKEND_PYTHON answers without touching the pipe.
 */
static size_t synth_size(void)
{
	return (size_t) (config.model_size > 0 ? config.model_size : 0);
}

static int full_write(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	size_t left = len;
	ssize_t n;
	while (left > 0) {
		n = write(fd, p, left);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += n;
		left -= (size_t) n;
	}
	return 0;
}

static int full_read(int fd, void *buf, size_t len)
{
	char *p = buf;
	size_t left = len;
	ssize_t n;
	while (left > 0) {
		n = read(fd, p, left);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (n == 0) return -1;	/* EOF: child died mid-response */
		p += n;
		left -= (size_t) n;
	}
	return 0;
}

/* Read one '\n'-terminated line (newline stripped). Returns -1 on EOF/error. */
static int read_line(int fd, char *buf, size_t bufsz)
{
	size_t i = 0;
	char c;
	ssize_t n;
	while (i + 1 < bufsz) {
		n = read(fd, &c, 1);
		if (n < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		if (n == 0) return -1;
		if (c == '\n') break;
		buf[i++] = c;
	}
	buf[i] = '\0';
	return 0;
}

/* Fixed-shape JSON only: scan for "key": and parse a number/string right
 * after it. The backend is a trusted local child process speaking a
 * protocol we define, so a full parser buys nothing here. */
static const char *json_field(const char *line, const char *key)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(line, pat);
	if (!p) return NULL;
	p += strlen(pat);
	while (*p == ' ') p++;
	return p;
}

static int json_get_ll(const char *line, const char *key, long long *out)
{
	const char *p = json_field(line, key);
	if (!p) return -1;
	*out = strtoll(p, NULL, 10);
	return 0;
}

static int json_get_double(const char *line, const char *key, double *out)
{
	const char *p = json_field(line, key);
	if (!p) return -1;
	*out = strtod(p, NULL);
	return 0;
}

int backend_spawn(const char *cmd)
{
	int in_pipe[2];	/* C -> child */
	int out_pipe[2];	/* child -> C */
	pid_t pid;

	if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
		perror("pipe");
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		dup2(in_pipe[0], 0);
		dup2(out_pipe[1], 1);
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		execl("/bin/sh", "sh", "-c", cmd, (char *) NULL);
		perror("execl");
		_exit(127);
	}
	close(in_pipe[0]);
	close(out_pipe[1]);
	backend.pid = pid;
	backend.to_child = in_pipe[1];
	backend.from_child = out_pipe[0];
	return 0;
}

void backend_shutdown(void)
{
	char line[64];
	if (backend.pid <= 0) return;
	snprintf(line, sizeof(line), "{\"op\":\"shutdown\"}\n");
	full_write(backend.to_child, line, strlen(line));
	close(backend.to_child);
	close(backend.from_child);
	waitpid(backend.pid, NULL, 0);
	backend.pid = 0;
}

/* Send `header` (no trailing newline expected in it), then `payload`. */
static int send_msg(const char *header, const void *payload, size_t nbytes)
{
	char line[512];
	int len = snprintf(line, sizeof(line), "%s\n", header);
	if (len < 0 || (size_t) len >= sizeof(line)) return -1;
	if (full_write(backend.to_child, line, (size_t) len) < 0) return -1;
	if (nbytes > 0 && full_write(backend.to_child, payload, nbytes) < 0) return -1;
	return 0;
}

/* Read a header line, then its trailing `nbytes` payload (from the header's
 * own "nbytes" field). *out_params is malloc'd; caller frees. */
static int recv_msg(char *line, size_t linesz, void **out_params, size_t *out_nbytes)
{
	long long nb;
	if (read_line(backend.from_child, line, linesz) < 0) return -1;
	if (json_get_ll(line, "nbytes", &nb) < 0 || nb < 0) return -1;
	*out_nbytes = (size_t) nb;
	*out_params = NULL;
	if (nb > 0) {
		*out_params = malloc((size_t) nb);
		if (!*out_params) return -1;
		if (full_read(backend.from_child, *out_params, (size_t) nb) < 0) {
			free(*out_params);
			*out_params = NULL;
			return -1;
		}
	}
	return 0;
}

int backend_init(id_t node, const char *model_name, id_t shard, id_t nshards,
	unsigned int seed, long long ms_per_epoch,
	void **out_params, size_t *out_nbytes)
{
	char header[512];
	char line[512];

	if (config.backend_mode != BACKEND_PYTHON) {
		*out_params = NULL;
		*out_nbytes = synth_size();
		return 0;
	}

	snprintf(header, sizeof(header),
		"{\"op\":\"init\",\"node\":%u,\"model\":\"%s\",\"shard\":%u,"
		"\"nshards\":%u,\"seed\":%u,\"ms_per_epoch\":%lld}",
		node, model_name, shard, nshards, seed, ms_per_epoch);
	if (send_msg(header, NULL, 0) < 0) return -1;
	if (recv_msg(line, sizeof(line), out_params, out_nbytes) < 0) return -1;
	return 0;
}

int backend_train(id_t node, unsigned int version, int epochs,
	const void *params, size_t nbytes,
	unsigned int *out_version, long long *out_sim_ms,
	double *out_loss, double *out_acc,
	void **out_params, size_t *out_nbytes)
{
	char header[512];
	char line[512];
	long long ver;

	if (config.backend_mode != BACKEND_PYTHON) {
		*out_version = version + 1;
		*out_sim_ms = config.train_time;
		*out_loss = 0.0;
		*out_acc = 0.0;
		if (config.backend_mode == BACKEND_TRACE) {
			/* One TRAIN per node per round, in round order, so counting
			 * this node's calls is which round the trace should be read at. */
			const struct trace_entry *e =
				trace_lookup(node, trace_take_round(node));
			if (e && e->present) {
				if (e->train_ms >= 0) *out_sim_ms = e->train_ms;
				*out_loss = e->loss;
				*out_acc = e->acc;
			}
		}
		*out_params = NULL;
		*out_nbytes = synth_size();
		return 0;
	}

	snprintf(header, sizeof(header),
		"{\"op\":\"train\",\"node\":%u,\"version\":%u,\"epochs\":%d,\"nbytes\":%zu}",
		node, version, epochs, nbytes);
	if (send_msg(header, params, nbytes) < 0) return -1;
	if (recv_msg(line, sizeof(line), out_params, out_nbytes) < 0) return -1;
	if (json_get_ll(line, "version", &ver) < 0) return -1;
	*out_version = (unsigned int) ver;
	if (json_get_ll(line, "sim_ms", out_sim_ms) < 0) return -1;
	if (json_get_double(line, "loss", out_loss) < 0) return -1;
	if (json_get_double(line, "acc", out_acc) < 0) return -1;
	return 0;
}

int backend_aggregate(id_t node, int count, const long long *staleness_ms,
	void *const *blobs, const size_t *blob_nbytes,
	long long *out_sim_ms,
	void **out_params, size_t *out_nbytes)
{
	char header[512];
	char line[512];
	char stale_list[256] = {0};
	size_t total = 0;
	int off = 0;

	if (config.backend_mode != BACKEND_PYTHON) {
		(void) staleness_ms; (void) blobs; (void) blob_nbytes;
		*out_sim_ms = config.aggregate_time;
		if (config.backend_mode == BACKEND_TRACE) {
			/* The WRITE that aggregates belongs to the round this node has
			 * most recently trained in. Traces without an aggregation time
			 * (the early ones) leave config.aggregate_time standing. */
			const struct trace_entry *e =
				trace_lookup(node, trace_cur_round(node));
			if (e && e->present && e->agg_ms >= 0) *out_sim_ms = e->agg_ms;
		}
		*out_params = NULL;
		*out_nbytes = synth_size();
		return 0;
	}

	for (int i = 0; i < count; i++) {
		off += snprintf(stale_list + off, sizeof(stale_list) - (size_t) off,
			"%s%lld", i ? "," : "", staleness_ms[i]);
		total += blob_nbytes[i];
	}
	snprintf(header, sizeof(header),
		"{\"op\":\"aggregate\",\"node\":%u,\"count\":%d,\"staleness_ms\":[%s],\"nbytes\":%zu}",
		node, count, stale_list, total);

	int len = snprintf(line, sizeof(line), "%s\n", header);
	if (len < 0 || (size_t) len >= sizeof(line)) return -1;
	if (full_write(backend.to_child, line, (size_t) len) < 0) return -1;
	for (int i = 0; i < count; i++) {
		if (blob_nbytes[i] > 0 &&
			full_write(backend.to_child, blobs[i], blob_nbytes[i]) < 0) {
			return -1;
		}
	}

	if (recv_msg(line, sizeof(line), out_params, out_nbytes) < 0) return -1;
	if (json_get_ll(line, "sim_ms", out_sim_ms) < 0) return -1;
	return 0;
}
