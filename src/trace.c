#include "trace.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct config config;

#define TRACE_LINE_LEN 4096

/* One parsed line, kept only until the overlays are built (peers) and for the
 * lifetime of the run (times), which is small: one struct per (head, round). */
struct trace_line {
	int head;
	int round;
	int *peers;
	int npeers;
	struct trace_entry e;
};

static struct trace_line *lines;
static int n_lines, cap_lines;
static int n_nodes, n_rounds;
static struct trace_entry *table;	/* n_nodes * n_rounds */
static int *round_of;			/* per-node TRAIN counter */
static int warned_short;		/* "trace ran out of rounds", said once */

/* Locate "key:" and return the first non-blank character after it. Same
 * approach as backend.c's json_field: these traces are flat text, not a
 * format worth a real parser. */
static const char *field(const char *line, const char *key)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "%s:", key);
	p = strstr(line, pat);
	if (!p) return NULL;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t') p++;
	return p;
}

/* First key in `keys` (NULL-terminated) that the line actually carries. */
static const char *field_any(const char *line, const char *const *keys)
{
	const char *p;
	for (; *keys; keys++) {
		p = field(line, *keys);
		if (p) return p;
	}
	return NULL;
}

static const char *const K_HEAD[]  = {"head_ID", "head_id", "head", NULL};
static const char *const K_ROUND[] = {"round ID", "round_ID", "round_id", "round", NULL};
static const char *const K_NBRS[]  = {"neighbours", "neighbors", NULL};
static const char *const K_LOSS[]  = {"loss", NULL};
static const char *const K_ACC[]   = {"acc", "accuracy", NULL};
static const char *const K_TRAIN[] = {"training_time", "train_time", "training time", NULL};
static const char *const K_AGG[]   = {"aggregation_time", "aggregate_time", "agg_time",
                                      "aggregation time", NULL};

/* Seconds -> sim-ms. No libm: every trace time is non-negative, so adding a
 * half before truncating rounds to nearest. */
static long long secs_to_ms(double s)
{
	return (long long) (s * 1000.0 + 0.5);
}

static long long opt_ms(const char *line, const char *const *keys)
{
	const char *p = field_any(line, keys);
	return p ? secs_to_ms(strtod(p, NULL)) : -1;
}

static double opt_double(const char *line, const char *const *keys)
{
	const char *p = field_any(line, keys);
	return p ? strtod(p, NULL) : 0.0;
}

/* Parse "[0, 1, 19]" into a malloc'd int array. Returns -1 if the field is
 * absent or unterminated. */
static int parse_peers(const char *p, int **out, int *n_out)
{
	int cap = 8, n = 0;
	int *v;

	if (!p || *p != '[') return -1;
	p++;
	v = malloc((size_t) cap * sizeof(int));
	while (*p && *p != ']') {
		if (*p == ',' || *p == ' ' || *p == '\t') { p++; continue; }
		if (n == cap) {
			cap *= 2;
			v = realloc(v, (size_t) cap * sizeof(int));
		}
		v[n++] = (int) strtol(p, (char **) &p, 10);
	}
	if (*p != ']') { free(v); return -1; }
	*out = v;
	*n_out = n;
	return 0;
}

static void push_line(struct trace_line tl)
{
	if (n_lines == cap_lines) {
		cap_lines = cap_lines ? cap_lines * 2 : 64;
		lines = realloc(lines, (size_t) cap_lines * sizeof(*lines));
	}
	lines[n_lines++] = tl;
}

int trace_load(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[TRACE_LINE_LEN];
	int lineno = 0;

	if (!f) {
		fprintf(stderr, "trace: cannot open '%s'\n", path);
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		struct trace_line tl;
		const char *p;
		lineno++;
		if (line[strspn(line, " \t\r\n")] == '\0') continue;	/* blank */

		memset(&tl, 0, sizeof(tl));
		p = field_any(line, K_HEAD);
		if (!p) {
			fprintf(stderr, "trace: %s:%d: no head id\n", path, lineno);
			fclose(f);
			return -1;
		}
		tl.head = (int) strtol(p, NULL, 10);

		p = field_any(line, K_ROUND);
		if (!p) {
			fprintf(stderr, "trace: %s:%d: no round id\n", path, lineno);
			fclose(f);
			return -1;
		}
		tl.round = (int) strtol(p, NULL, 10);

		if (tl.head < 0 || tl.round < 0) {
			fprintf(stderr, "trace: %s:%d: negative head or round id\n", path, lineno);
			fclose(f);
			return -1;
		}

		/* Neighbours are optional per line: a trace that only records
		 * timings still drives the clock, it just cannot build overlays. */
		if (parse_peers(field_any(line, K_NBRS), &tl.peers, &tl.npeers) < 0) {
			tl.peers = NULL;
			tl.npeers = 0;
		}

		tl.e.present = 1;
		tl.e.train_ms = opt_ms(line, K_TRAIN);
		tl.e.agg_ms = opt_ms(line, K_AGG);
		tl.e.loss = opt_double(line, K_LOSS);
		tl.e.acc = opt_double(line, K_ACC);

		if (tl.head + 1 > n_nodes) n_nodes = tl.head + 1;
		if (tl.round + 1 > n_rounds) n_rounds = tl.round + 1;
		push_line(tl);
	}
	fclose(f);

	if (!n_lines) {
		fprintf(stderr, "trace: %s holds no trace lines\n", path);
		return -1;
	}

	table = calloc((size_t) n_nodes * (size_t) n_rounds, sizeof(*table));
	round_of = calloc((size_t) n_nodes, sizeof(*round_of));
	for (int i = 0; i < n_lines; i++) {
		struct trace_entry *e = &table[lines[i].head * n_rounds + lines[i].round];
		if (e->present) {
			fprintf(stderr, "trace: head %d has more than one line for round %d\n",
				lines[i].head, lines[i].round);
			return -1;
		}
		*e = lines[i].e;
	}
	for (int i = 0; i < n_nodes * n_rounds; i++) {
		if (!table[i].present) {
			fprintf(stderr, "trace: warning: no line for head %d round %d;"
				" it will fall back to the config times\n",
				i / n_rounds, i % n_rounds);
		}
	}
	return 0;
}

void trace_free(void)
{
	for (int i = 0; i < n_lines; i++) free(lines[i].peers);
	free(lines);
	free(table);
	free(round_of);
	lines = NULL;
	table = NULL;
	round_of = NULL;
	n_lines = cap_lines = n_nodes = n_rounds = 0;
}

int trace_nodes(void) { return n_nodes; }
int trace_rounds(void) { return n_rounds; }

const struct trace_entry *trace_lookup(id_t node, int round)
{
	if ((int) node >= n_nodes || round < 0 || round >= n_rounds) return NULL;
	return &table[(int) node * n_rounds + round];
}

int trace_take_round(id_t node)
{
	int r;
	if ((int) node >= n_nodes) return 0;
	r = round_of[node]++;
	if (r >= n_rounds && !warned_short) {
		fprintf(stderr, "trace: more TRAINs than the trace has rounds (%d);"
			" the config times take over from here\n", n_rounds);
		warned_short = 1;
	}
	return r;
}

int trace_cur_round(id_t node)
{
	int r;
	if ((int) node >= n_nodes) return 0;
	r = round_of[node] - 1;		/* the round whose TRAIN already ran */
	return r < 0 ? 0 : r;
}

/* Is there already a link orig->dest in this topology? Degrees here are small
 * and this runs once at load, so a scan beats carrying an index. */
static int has_link(const struct topology *t, int orig, int dest)
{
	struct link *l;
	vp_for (l, &t->adj_lists[orig]) {
		if ((int) l->dest == dest) return 1;
	}
	return 0;
}

static void add_edge(struct topology *v, int a, int b, int weight)
{
	if (!has_link(v, a, b)) {
		vp_vec_append(&v->adj_lists[a], link_alloc((id_t) a, (id_t) b,
			weight, config.latency_ms));
	}
	if (!v->directed && !has_link(v, b, a)) {
		vp_vec_append(&v->adj_lists[b], link_alloc((id_t) b, (id_t) a,
			weight, config.latency_ms));
	}
}

void trace_build_rounds(struct topology *t, int weight)
{
	struct topology *chain = NULL, *tail = NULL, *v, *next;

	/* Drop whatever overlays the .tpl carried: with a rounds_file the trace
	 * is the authority on rounds, and the .tpl supplies only the physical
	 * graph. topo_free recurses through virt_topo but never frees the structs
	 * themselves, so unhook each one before freeing it. */
	v = t->virt_topo;
	while (v) {
		next = v->virt_topo;
		v->virt_topo = NULL;
		topo_free(v);
		free(v);
		v = next;
	}
	t->virt_topo = NULL;

	for (int r = 0; r < n_rounds; r++) {
		v = malloc(sizeof(*v));
		topo_init(v, t->num_nodes);
		v->directed = t->directed;

		for (int i = 0; i < n_lines; i++) {
			if (lines[i].round != r) continue;
			if (lines[i].head >= (int) t->num_nodes) {
				fprintf(stderr, "trace: head %d is outside the topology's"
					" %zu nodes\n", lines[i].head, t->num_nodes);
				exit(1);
			}
			for (int j = 0; j < lines[i].npeers; j++) {
				int p = lines[i].peers[j];
				if (p == lines[i].head) continue;	/* lists include self */
				if (p < 0 || p >= (int) t->num_nodes) {
					fprintf(stderr, "trace: head %d round %d lists peer %d,"
						" outside the topology's %zu nodes\n",
						lines[i].head, r, p, t->num_nodes);
					exit(1);
				}
				add_edge(v, lines[i].head, p, weight);
			}
		}

		if (!chain) chain = v;
		else tail->virt_topo = v;
		tail = v;
	}
	t->virt_topo = chain;
}
