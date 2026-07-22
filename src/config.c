#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"


/* Parse "on"/"off" strings to int. Returns -1 on unrecognised input. */
static int parse_bool(const char *val) {
    if (strcmp(val, "on")  == 0) return 1;
    if (strcmp(val, "off") == 0) return 0;
    return -1;
}

/* Fill *cfg from the file at path. Returns 0 on success, -1 on error. */
int load_config(const char *path, struct config *cfg) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    /* Safe defaults */
    memset(cfg, 0, sizeof(*cfg));
    cfg->blocks_per_node = 1;
    cfg->epochs = 1;
    cfg->ms_per_epoch = 500;
    strncpy(cfg->model_name, "stub", MAX_VAL_LEN - 1);

    char line[MAX_LINE_LEN];
    int  lineno = 0;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;

        /* Strip trailing newline / carriage-return */
        line[strcspn(line, "\r\n")] = '\0';

        /* Skip blank lines and comments */
        if (line[0] == '\0' || line[0] == '#') continue;

        /* Split on '=' */
        char *eq = strchr(line, '=');
        if (!eq) {
            fprintf(stderr, "Warning: line %d ignored (no '='): %s\n",
                    lineno, line);
            continue;
        }

        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "block_size") == 0) {
            cfg->block_size = atoi(val);
        } else if (strcmp(key, "policy") == 0) {
            cfg->policy = atoi(val);
        } else if (strcmp(key, "topology_directed") == 0) {
            int b = parse_bool(val);
            if (b < 0) fprintf(stderr, "Warning: unknown bool '%s' for key '%s'\n", val, key);
            else cfg->topology_directed = b;
        } else if (strcmp(key, "output") == 0) {
            strncpy(cfg->output, val, MAX_VAL_LEN - 1);
            cfg->output[MAX_VAL_LEN - 1] = '\0';
        } else if (strcmp(key, "debug") == 0) {
            strncpy(cfg->debug, val, MAX_VAL_LEN - 1);
            cfg->debug[MAX_VAL_LEN - 1] = '\0';
        } else if (strcmp(key, "barriers") == 0) {
            int b = parse_bool(val);
            if (b < 0) fprintf(stderr, "Warning: unknown bool '%s' for key '%s'\n", val, key);
            else cfg->barriers = b;
        } else if (strcmp(key, "backend_cmd") == 0) {
            strncpy(cfg->backend_cmd, val, MAX_VAL_LEN - 1);
            cfg->backend_cmd[MAX_VAL_LEN - 1] = '\0';
        } else if (strcmp(key, "model_name") == 0) {
            strncpy(cfg->model_name, val, MAX_VAL_LEN - 1);
            cfg->model_name[MAX_VAL_LEN - 1] = '\0';
        } else if (strcmp(key, "epochs") == 0) {
            cfg->epochs = atoi(val);
        } else if (strcmp(key, "nshards") == 0) {
            cfg->nshards = atoi(val);
        } else if (strcmp(key, "ms_per_epoch") == 0) {
            cfg->ms_per_epoch = atoll(val);
        } else if (strcmp(key, "latency_ms") == 0) {
            cfg->latency_ms = atoi(val);
        } else {
            fprintf(stderr, "Warning: unknown key '%s' on line %d\n", key, lineno);
        }
    }

    fclose(fp);
    return 0;
}