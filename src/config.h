#ifndef _CONFIG_H
#define _CONFIG_H

#define MAX_LINE_LEN 256
#define MAX_VAL_LEN  128

enum routing_mode {
    ROUTING_STATIC = 0,   /* precomputed once at startup, hop-count shortest path */
    ROUTING_DYNAMIC = 1,  /* resolved per-flow from current link contention */
};

struct config {
    int   block_size;
    int   topology_directed;   /* 1 = on, 0 = off */
    int   policy;
    char  output[MAX_VAL_LEN];
    char  debug[MAX_VAL_LEN];
    int   barriers;            /* 1 = on, 0 = off */
    int   blocks_per_node;
    int   seed;
    char  backend_cmd[MAX_VAL_LEN];  /* shell command that speaks protocol.txt */
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
};

int load_config(const char *path, struct config *cfg);

#endif /* _CONFIG_H*/