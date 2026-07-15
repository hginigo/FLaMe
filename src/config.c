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
        } else {
            fprintf(stderr, "Warning: unknown key '%s' on line %d\n", key, lineno);
        }
    }

    fclose(fp);
    return 0;
}

void print_config(const struct config *cfg) {
    printf("block_size        = %d\n",    cfg->block_size);
    printf("topology_directed = %s\n",    cfg->topology_directed ? "on" : "off");
    printf("output       = %s\n",    cfg->output);
    printf("debug       = %s\n",    cfg->debug);
    printf("barriers          = %s\n",    cfg->barriers          ? "on" : "off");
}

/*
int main(int argc, char *argv[]) {
    const char *path = (argc > 1) ? argv[1] : "config.conf";

    struct config cfg;
    if (load_config(path, &cfg) != 0) {
        return EXIT_FAILURE;
    }

    print_config(&cfg);
    return EXIT_SUCCESS;
}
*/