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
};

int load_config(const char *path, struct config *cfg);
void print_config(const struct config *cfg);

#endif /* _CONFIG_H*/