#include "topology.h"
#include <stdio.h>
#include <limits.h>
#include <string.h>

#define TOPO_DELIM "-"
#define STR_LEN 512
char line[STR_LEN];

char *links_read(FILE *f, struct topology *t)
{
    id_t orig, dest;
    int weight;
    
    struct link *l;
    struct vp_vec *adj_list;

    int res;
    char *nl;
    nl = fgets(line, STR_LEN, f);
    while (nl && strcmp(line, "-\n")) {
        res = sscanf(line, "%d %d %d\n", &orig, &dest, &weight);
        if (res == 3) {
            l = link_alloc(orig, dest, weight * Bpms);
            adj_list = &t->adj_lists[orig];
            vp_vec_append(adj_list, l);
            if (!t->directed) {
                l = link_alloc(dest, orig, weight * Bpms);
                adj_list = &t->adj_lists[dest];
                vp_vec_append(adj_list, l);
            }
        } else if (strcmp(line, "\n") == 0) {
            break;
        } else {
            printf("res: %d line: '%s'\n", res, line);
            perror("sscanf");
            fclose(f);
            exit(1);
        }
        nl = fgets(line, STR_LEN, f);
    }
    return nl;
}

/*
int topo_multi_read(char *str, struct topology *t)
{
    size_t num_nodes;
    FILE *f = fopen(str, "r");
    struct topology *aux = t;
    char *res;
    int count = 1;

    if (f == NULL) {
        perror("fopen");
        exit(1);
    }

    fscanf(f, "%lu\n", &num_nodes);
    printf("num nodes: %d\n", num_nodes);
    topo_init(aux, num_nodes);

    res = links_read(f, aux);
    while (res != NULL) {
        aux->virt_topo = malloc(sizeof(struct topology));
        aux = aux->virt_topo;
        topo_init(aux, num_nodes);
        res = links_read(f, aux);
        count++;
    }
    return count;
}
*/
int topo_multi_read(char *str, struct topology *t)
{
    FILE *f = fopen(str, "r");
    char *res;
    int ret;
    int n_nodes;
    int orig, dest, weight;
    struct link *l;
    struct vp_vec *al;
    struct topology *aux;
    
    if (f == NULL) {
        perror("fopen");
        exit(1);
    }
    res = fgets(line, STR_LEN, f);
    ret = sscanf(line, "%d\n", &n_nodes);
    //fprintf(stdout, "ret: %d line: %s", ret, line);
    topo_init(t, n_nodes);
    
    aux = t;
    while ((res = fgets(line, STR_LEN, f))) {
        ret = sscanf(line, "%d %d %d\n", &orig, &dest, &weight);
        //fprintf(stdout, "ret: %d line: %s", ret, line);
        if (ret == 3) {
            l = link_alloc(orig, dest, weight * Bpms);
            al = &aux->adj_lists[orig];
            vp_vec_append(al, l);
            if (!aux->directed) {
                l = link_alloc(dest, orig, weight * Bpms);
                al = &aux->adj_lists[dest];
                vp_vec_append(al, l);
            }
        } else if (ret == 0) {
            aux->virt_topo = malloc(sizeof(struct topology));
            aux = aux->virt_topo;
            aux->directed = t->directed;
            topo_init(aux, n_nodes);
        } else {
            // error
            perror("sscanf");
            fclose(f);
            //fprintf(stderr, "err ret: %d line: %s", ret, line);
            exit(1);
        }

    }
    fclose(f);
    return 0;
    //printf("res: %p\n", res);
}

void topo_read(char *str, struct topology *t)
{
    size_t num_nodes;
    id_t orig, dest;
    int weight;
    struct link *d;
    struct vp_vec *node_list, *adj_list;
    FILE *f = fopen(str, "r");

    if (f == NULL) {
        perror("fopen");
        exit(1);
    }

    fscanf(f, "%lu\n", &num_nodes);
    topo_init(t, num_nodes);
    t->directed = 0;
    node_list = t->adj_lists;

    while (fscanf(f, "%d %d %d\n", &orig, &dest, &weight) != EOF) {
        d = link_alloc(orig, dest, weight * Bpms);
        adj_list = t->adj_lists + orig;
        vp_vec_append(adj_list, d);
        if (!t->directed) {
            d = link_alloc(dest, orig, weight * Bpms);
            adj_list = t->adj_lists + dest;
            vp_vec_append(adj_list, d);
        }
    }
    fclose(f);
}

struct link *link_alloc(id_t orig,
                        id_t dest,
                        int weight)
{
    struct link *aux = malloc(sizeof(struct link));
    
    aux->orig = orig;
    aux->dest = dest;
    aux->weight = weight;
    memset(&aux->active_flows, 0, sizeof(struct vp_vec));
    //aux->active_jobs = {0};
    return aux;
}

void dist_free(struct link *d)
{
    free(d);
}

void topo_init(struct topology *t, size_t num_nodes)
{
    assert(t != NULL);
    t->adj_lists = malloc(num_nodes * sizeof(struct vp_vec));
    memset(t->adj_lists, 0, sizeof(struct vp_vec) * num_nodes);
    t->num_nodes = num_nodes;
    t->virt_topo = NULL;
}

void topo_free(struct topology *t)
{
    struct vp_vec *vec;
    struct link *d;
    
    for (size_t i = 0; i < t->num_nodes; i++) {
        vec = t->adj_lists + i;
        for (size_t j = 0; j < vec->length; j++) {
            d = vp_vec_get(vec, j);
            vp_vec_free(&d->active_flows);
            free(d);
        }
        vp_vec_free(vec);
    }
    free(t->adj_lists);
    if (t->virt_topo != NULL) {
        topo_free(t->virt_topo);
    }
}

void vp_vec_insert_by_weight(struct vp_vec *vec, struct link *d)
{
    struct link *aux;
    for (size_t i = 0; i < vec->length; i++) {
        aux = vp_vec_get(vec, i);
        if (d->weight < aux->weight) {
            vp_vec_insert(vec, d, i);
            return;
        }
    }
    vp_vec_append(vec, d);
}

int **dist_cache;
size_t num_nodes;

int dijkstra2(const struct topology *t,
    id_t orig)
    //id_t dest,
    //struct vp_vec *path)
{
    size_t n_nodes = t->num_nodes;
    int *dists = malloc(n_nodes * sizeof(int));
    int *dis = dist_cache[orig];
    //int *hops = calloc(n_nodes, sizeof(int));
    struct vp_vec pq, *adj_list;
    struct link *d, *aux_dist, *d2;
    int result = -1;
    id_t pivot;

    vp_vec_alloc(&pq, n_nodes);
    for (size_t i = 0; i < n_nodes; i++) {
        dists[i] = INT_MAX;
        dis[i] = -1;
    }

    d = link_alloc(0, orig, 0);
    vp_vec_append(&pq, d);
    dists[orig] = 0;
    dis[orig] = orig;

    while (pq.length > 0) {
        d = vp_vec_pop(&pq);
        pivot = d->dest;
        adj_list = &t->adj_lists[pivot];

        for (size_t i = 0; i < adj_list->length; i++) {
            aux_dist = vp_vec_get(adj_list, i);
            if (dists[aux_dist->dest] > (dists[pivot] + 1)) {
                dists[aux_dist->dest] = dists[pivot] + 1;
                dis[aux_dist->dest] = (int) pivot;
                //hops[aux_dist->dest] = hops[pivot] + 1;
                d2 = link_alloc(0, aux_dist->dest, dists[aux_dist->dest]);
                vp_vec_insert_by_weight(&pq, d2);
            }
        }
        dist_free(d);

        //if (pivot == dest) {
        //    break;
        //}
    }

    //result = dists[dest];
    //if (n_hops != NULL) {
    //    *n_hops = hops[dest];
    //}
    //dbg("dijkstra weight %d, hops %d\n", result, hops[dest]);
    free(dists);
    //free(hops);
    //while (pq.length > 0) {
    //    d = vp_vec_pop(&pq);
    //    dist_free(d);
    //}
    vp_vec_free(&pq);
    return 0;
    //return result;
}

void dijkstra_init(const struct topology *t)
{
    assert(t != NULL);
    num_nodes = t->num_nodes;
    assert(num_nodes > 0);
    dist_cache = malloc(num_nodes * sizeof(unsigned int *));
    for (size_t i = 0; i < num_nodes; i++) {
        dist_cache[i] = calloc(num_nodes, sizeof(unsigned int));
        dijkstra2(t, i);
    }
}

void dist_cache_print()
{
    for (size_t i = 0; i < num_nodes; i++) {
        dbg("%d: ", i);
        for (size_t j = 0; j < num_nodes; j++) {
            dbg(" %d", dist_cache[i][j]);
        }
        dbg("\n");
    }
}

void path_resolve(const struct topology *t,
    id_t orig,
    id_t dest,
    struct vp_vec *path)
{
    int *d = dist_cache[dest];
    id_t pivot = orig;
    struct vp_vec *adj_list;
    struct link *aux;
    while (pivot != dest) {
        adj_list = &t->adj_lists[pivot];
        vp_for (aux, adj_list) {
            if (aux->dest == d[pivot]) {
                vp_vec_append(path, aux);
                //printf("asdf\n");
            }
        }
        pivot = d[pivot];
    }
}

int path_hops(const struct topology *t,
    id_t orig,
    id_t dest)
{
    int *d = dist_cache[dest];
    id_t pivot = orig;
    struct vp_vec *adj_list;
    struct link *aux;
    int hops = 0;
    
    while (pivot != dest) {
        adj_list = &t->adj_lists[pivot];
        vp_for (aux, adj_list) {
            if (aux->dest == d[pivot]) {
                hops++;
            }
        }
        pivot = d[pivot];
    }
    return hops;
}

int dijkstra(const struct topology *t,
             id_t orig,
             id_t dest,
             int *n_hops)
{
    size_t n_nodes = t->num_nodes;
    int *dists = malloc(n_nodes * sizeof(int));
    int *hops = calloc(n_nodes, sizeof(int));
    struct vp_vec pq, *adj_list;
    struct link *d, *aux_dist, *d2;
    int result = -1;
    id_t pivot;

    vp_vec_alloc(&pq, n_nodes);
    for (size_t i = 0; i < n_nodes; i++) {
        dists[i] = INT_MAX;
    }

    d = link_alloc(0, orig, 0);
    vp_vec_append(&pq, d);
    dists[orig] = 0;

    while (pq.length > 0) {
        d = vp_vec_pop(&pq);
        pivot = d->dest;
        adj_list = &t->adj_lists[pivot];
        
        for (size_t i = 0; i < adj_list->length; i++) {
            aux_dist = vp_vec_get(adj_list, i);
            if (dists[aux_dist->dest] > (dists[pivot] + aux_dist->weight)) {
                dists[aux_dist->dest] = dists[pivot] + aux_dist->weight;
                hops[aux_dist->dest] = hops[pivot] + 1;
                d2 = link_alloc(0, aux_dist->dest, dists[aux_dist->dest]);
                vp_vec_insert_by_weight(&pq, d2);
            }
        }
        dist_free(d);

        if (pivot == dest) {
            break;
        }
    }

    result = dists[dest];
    if (n_hops != NULL) {
        *n_hops = hops[dest];
    }
    dbg("dijkstra weight %d, hops %d\n", result, hops[dest]);
    free(dists);
    free(hops);
    while (pq.length > 0) {
        d = vp_vec_pop(&pq);
        dist_free(d);
    }
    vp_vec_free(&pq);
    return result;
}
