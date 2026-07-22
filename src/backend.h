#ifndef _BACKEND_H
#define _BACKEND_H
#include <sys/types.h>
#include <stddef.h>
#include "structs.h"

/*
 * Transport to the Python training backend (protocol.txt): one child
 * process, spawned once, talked to over its stdin/stdout. Each request is
 * a single '\n'-terminated JSON line, immediately followed (when the op
 * carries a payload) by exactly `nbytes` raw bytes — no delimiter between
 * header and payload, none needed after it either, since nbytes is known.
 *
 * Every call here blocks until the matching reply arrives: the sim is
 * single-threaded and strictly sequential (one event at a time), so this
 * mirrors the request/response shape of the protocol exactly.
 */
struct backend {
	pid_t pid;
	int to_child;	/* C write end   -> child's stdin */
	int from_child;	/* child's stdout -> C read end */
};

extern struct backend backend;

int backend_spawn(const char *cmd);
void backend_shutdown(void);

/* All out_params buffers are malloc'd; caller owns and frees them. */

int backend_init(id_t node, const char *model_name, id_t shard, id_t nshards,
	unsigned int seed, long long ms_per_epoch,
	void **out_params, size_t *out_nbytes);

int backend_train(id_t node, unsigned int version, int epochs,
	const void *params, size_t nbytes,
	unsigned int *out_version, long long *out_sim_ms,
	double *out_loss, double *out_acc,
	void **out_params, size_t *out_nbytes);

/* blobs[i]/blob_nbytes[i] for i in [0,count); staleness_ms[i] is sim_time
 * minus the sim_time each blob was captured at. */
int backend_aggregate(id_t node, int count, const long long *staleness_ms,
	void *const *blobs, const size_t *blob_nbytes,
	long long *out_sim_ms,
	void **out_params, size_t *out_nbytes);

#endif /* _BACKEND_H */
