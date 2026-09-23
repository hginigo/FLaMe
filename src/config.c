#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "config.h"


/* Parse "on"/"off" strings to int. Returns -1 on unrecognised input. */
static int parse_bool(const char *val) {
    if (strcmp(val, "on")  == 0) return 1;
    if (strcmp(val, "off") == 0) return 0;
    return -1;
}

/* Strict integer: the whole value must be a number, so "MCI" or "3x" is an
 * error rather than atoi's silent 0. */
static int parse_ll(const char *val, long long *out) {
    char *end;
    long long v;

    errno = 0;
    v = strtoll(val, &end, 10);
    if (end == val || *end != '\0' || errno) return -1;
    *out = v;
    return 0;
}

/* Comma-separated list of non-negative ids ("3,7,12"; "" is an empty list).
 * Fills up to `max` entries of ids (may be NULL to only validate) and returns
 * the count, or -1 on malformed input. */
int config_id_list(const char *val, long long *ids, int max) {
    char buf[MAX_VAL_LEN];
    char *tok, *save;
    long long v;
    int n = 0;

    if (val[0] == '\0') return 0;
    if (strlen(val) >= sizeof(buf)) return -1;
    snprintf(buf, sizeof(buf), "%s", val);
    if (buf[0] == ',' || buf[strlen(buf) - 1] == ',' || strstr(buf, ",,")) return -1;
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (parse_ll(tok, &v) < 0 || v < 0 || n >= max) return -1;
        if (ids) ids[n] = v;
        n++;
    }
    return n;
}

#define SET_INT(field) do {                                   \
        long long v_;                                         \
        if (parse_ll(val, &v_) < 0) goto bad_value;           \
        cfg->field = v_;                                      \
    } while (0)

#define SET_STR(field) do {                                   \
        strncpy(cfg->field, val, MAX_VAL_LEN - 1);            \
        cfg->field[MAX_VAL_LEN - 1] = '\0';                   \
    } while (0)

int config_set(struct config *cfg, const char *key, const char *val, const char *where) {
    int b;

    if (strcmp(key, "model_size") == 0) {
        SET_INT(model_size);
    } else if (strcmp(key, "train_time") == 0) {
        SET_INT(train_time);
    } else if (strcmp(key, "aggregate_time") == 0) {
        SET_INT(aggregate_time);
    } else if (strcmp(key, "rounds_file") == 0) {
        SET_STR(rounds_file);
    } else if (strcmp(key, "backend_mode") == 0) {
        if (strcmp(val, "synthetic") == 0) {
            cfg->backend_mode = BACKEND_SYNTHETIC;
        } else if (strcmp(val, "python") == 0) {
            cfg->backend_mode = BACKEND_PYTHON;
        } else if (strcmp(val, "trace") == 0) {
            cfg->backend_mode = BACKEND_TRACE;
        } else {
            goto bad_value;
        }
    } else if (strcmp(key, "policy") == 0) {
        SET_INT(policy);
    } else if (strcmp(key, "topology_directed") == 0) {
        if ((b = parse_bool(val)) < 0) goto bad_value;
        cfg->topology_directed = b;
    } else if (strcmp(key, "output") == 0) {
        SET_STR(output);
    } else if (strcmp(key, "debug") == 0) {
        SET_STR(debug);
    } else if (strcmp(key, "barriers") == 0) {
        if ((b = parse_bool(val)) < 0) goto bad_value;
        cfg->barriers = b;
    } else if (strcmp(key, "backend_cmd") == 0) {
        SET_STR(backend_cmd);
    } else if (strcmp(key, "model_name") == 0) {
        SET_STR(model_name);
    } else if (strcmp(key, "epochs") == 0) {
        SET_INT(epochs);
    } else if (strcmp(key, "nshards") == 0) {
        SET_INT(nshards);
    } else if (strcmp(key, "ms_per_epoch") == 0) {
        SET_INT(ms_per_epoch);
    } else if (strcmp(key, "latency_ms") == 0) {
        SET_INT(latency_ms);
    } else if (strcmp(key, "control_bytes") == 0) {
        SET_INT(control_bytes);
    } else if (strcmp(key, "hop_penalty") == 0) {
        SET_INT(hop_penalty);
    } else if (strcmp(key, "disconnected") == 0) {
        if (config_id_list(val, NULL, MAX_VAL_LEN) < 0) goto bad_value;
        SET_STR(disconnected);
    } else if (strcmp(key, "routing") == 0) {
        if (strcmp(val, "static") == 0 || strcmp(val, "bfs") == 0) {
            cfg->routing = ROUTING_STATIC;   /* static == cached hop-count BFS */
        } else if (strcmp(val, "dijkstra") == 0) {
            cfg->routing = ROUTING_DIJKSTRA;
        } else if (strcmp(val, "dynamic") == 0) {
            cfg->routing = ROUTING_DYNAMIC;
        } else if (strcmp(val, "widest") == 0) {
            cfg->routing = ROUTING_WIDEST;
        } else if (strcmp(val, "hybrid") == 0) {
            cfg->routing = ROUTING_HYBRID;
        } else if (strcmp(val, "widest_hybrid") == 0) {
            cfg->routing = ROUTING_WIDEST_HYBRID;
        } else {
            goto bad_value;
        }
    } else {
        fprintf(stderr, "%s: unknown key '%s'\n", where, key);
        return -1;
    }
    return 0;

bad_value:
    fprintf(stderr, "%s: invalid value '%s' for key '%s'\n", where, val, key);
    return -1;
}

int config_override(struct config *cfg, const char *arg) {
    char buf[MAX_LINE_LEN];
    char *eq;

    snprintf(buf, sizeof(buf), "%s", arg);
    eq = strchr(buf, '=');
    if (!eq || eq == buf) {
        fprintf(stderr, "argument '%s': expected key=value\n", arg);
        return -1;
    }
    *eq = '\0';
    return config_set(cfg, buf, eq + 1, "argument");
}

/* Fill *cfg from the file at path. Returns 0 on success, -1 on error. A bad
 * line is reported and skipped, leaving that setting at its default. */
int load_config(const char *path, struct config *cfg) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "config: cannot open '%s'\n", path);
        return -1;
    }

    /* Safe defaults */
    memset(cfg, 0, sizeof(*cfg));
    cfg->blocks_per_node = 1;
    cfg->epochs = 1;
    cfg->ms_per_epoch = 500;
    cfg->control_bytes = 40;
    cfg->hop_penalty = 1600;
    cfg->backend_mode = BACKEND_SYNTHETIC;
    cfg->model_size = 1048576;   /* 1 MiB */
    cfg->train_time = 400;       /* ms */
    strncpy(cfg->model_name, "stub", MAX_VAL_LEN - 1);

    char line[MAX_LINE_LEN];
    char where[MAX_LINE_LEN];
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
        snprintf(where, sizeof(where), "%s:%d", path, lineno);
        config_set(cfg, line, eq + 1, where);
    }

    fclose(fp);
    return 0;
}
