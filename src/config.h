#ifndef _CONFIG_H
#define _CONFIG_H

#define MAX_LINE_LEN 256
#define MAX_VAL_LEN  128

enum backend_mode {
    BACKEND_SYNTHETIC = 0, /* in-process: no training math, fixed per-node train_time,
                              model transfers sized by model_size. No external process. */
    BACKEND_PYTHON = 1,    /* spawn backend_cmd and talk protocol.txt over a pipe;
                              real params/loss/sim_ms, model size decided by the backend. */
    BACKEND_TRACE = 2,     /* replay a real run: like synthetic (no external process, no
                              params buffer, transfers sized by model_size) except TRAIN
                              and aggregation take the times recorded in rounds_file for
                              that (node, round), falling back to train_time/aggregate_time
                              wherever the trace is silent. */
};

enum routing_mode {
    ROUTING_STATIC = 0,   /* precomputed once at startup, hop-count shortest path (== BFS) */
    ROUTING_DYNAMIC = 1,  /* resolved per-flow from current link contention, sum of 1/bw */
    ROUTING_WIDEST = 2,   /* resolved per-flow, maximizes the path's bottleneck bandwidth */
    ROUTING_HYBRID = 3,   /* like DYNAMIC but each hop also carries a fixed hop_penalty cost,
                             so a detour is taken only when the bandwidth it buys outweighs the
                             cost of the extra hop (penalty 0 == DYNAMIC, penalty inf == STATIC) */
    ROUTING_WIDEST_HYBRID = 4, /* like WIDEST but hop_penalty B/ms is subtracted from the running
                             bottleneck per hop, so longer paths are discounted (penalty 0 ==
                             WIDEST, large penalty degenerates toward shortest-path) */
    ROUTING_DIJKSTRA = 5, /* cached shortest path by 1/bandwidth over each link's max cap, no
                             contention; identical to STATIC (== hop-count BFS) when all links
                             share one bandwidth cap. `routing=bfs` is an alias for STATIC. */
};

struct config {
    long long model_size;      /* model wire size in bytes; sizes model-transfer flows in the
                                  synthetic backend (was the unused `block_size`, now live) */
    int   topology_directed;   /* 1 = on, 0 = off */
    int   policy;
    char  output[MAX_VAL_LEN];
    char  debug[MAX_VAL_LEN];
    int   barriers;            /* 1 = on, 0 = off */
    int   blocks_per_node;
    int   seed;
    enum backend_mode backend_mode; /* BACKEND_SYNTHETIC (default), _PYTHON or _TRACE */
    long long train_time;      /* synthetic backend: sim-ms every node's TRAIN takes; in
                                  trace mode, the fallback where a trace line has no time */
    long long aggregate_time;  /* sim-ms an aggregation takes. Fallback in trace mode, and
                                  the flat cost in synthetic mode (was hardcoded 0) */
    char  rounds_file[MAX_VAL_LEN];  /* DFL execution trace: supplies one virtual overlay
                                        per round, so the .tpl need only carry the physical
                                        graph, plus the per-(node, round) work times */
    char  backend_cmd[MAX_VAL_LEN];  /* python mode only: shell command that speaks protocol.txt */
    char  model_name[MAX_VAL_LEN];   /* opaque tag passed through to the backend */
    int   epochs;               /* per TRAIN call */
    int   nshards;               /* dataset partitions; defaults to node count */
    long long ms_per_epoch;      /* advisory hint passed to the backend at init */
    int   latency_ms;            /* default per-link propagation delay; .tpl lines may override */
    enum routing_mode routing;  /* ROUTING_STATIC (default): hop-count path precomputed once
                                    at startup. ROUTING_DYNAMIC: each flow resolves its path
                                    ad-hoc from current link contention. */
    int   control_bytes;         /* size (in bytes) of request/ack control flows, as opposed
                                    to actual block/model transfers */
    int   hop_penalty;           /* ROUTING_HYBRID only: fixed per-hop cost added to each link's
                                    1/bw contention cost, in the same fixed-point units as
                                    link_route_cost (an uncontended mid-weight link is ~100-300);
                                    higher biases toward shorter paths, meant to be swept */
};

int load_config(const char *path, struct config *cfg);
/* Apply one setting; `where` names its source in error messages. Returns -1
 * on an unknown key or invalid value, leaving the setting unchanged. */
int config_set(struct config *cfg, const char *key, const char *val, const char *where);
/* Apply one "key=value" command-line override, same keys as the file. */
int config_override(struct config *cfg, const char *arg);

#endif /* _CONFIG_H*/