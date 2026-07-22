#ifndef _CONFIG_H
#define _CONFIG_H

#define MAX_LINE_LEN 256
#define MAX_VAL_LEN  128

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
};

int load_config(const char *path, struct config *cfg);

#endif /* _CONFIG_H*/