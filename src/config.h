#ifndef _CONFIG_H
#define _CONFIG_H

#define MAX_LINE_LEN 256
#define MAX_VAL_LEN  128

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
    int   hop_penalty;           /* ROUTING_HYBRID only: fixed per-hop cost added to each link's
                                    1/bw contention cost, in the same fixed-point units as
                                    link_route_cost (an uncontended mid-weight link is ~100-300);
                                    higher biases toward shorter paths, meant to be swept */
};

int load_config(const char *path, struct config *cfg);

#endif /* _CONFIG_H*/