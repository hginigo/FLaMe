#include "policy.h"
#include "topology.h"
#include "event.h"
#include "flow.h"
#include "backend.h"
#include "config.h"
#include "metrics.h"
#include "vp_vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

extern struct topology topology;
extern time_t sim_time;
extern struct config config;
extern enum policy_t policy;

#define BARRIER_DEF_MS 0

/*
 * Locates the replica of `m` nearest (by hop count) to `from`, mirroring
 * what the old owners-list shortest_path search did — now returning the
 * replica itself (not just its holder), since callers need both. Holders
 * with no physical path from `from` are skipped; NULL if none is reachable.
 */
struct replica *nearest_replica(const struct model *m,
    const struct node *from,
    const struct topology *t)
{
    struct replica *r, *min = NULL;
    int hops, min_hops = INT_MAX;

    vp_for (r, &m->replicas) {
        hops = path_hops(t, from->id, r->node->id);
        if (hops >= 0 && hops < min_hops) {
            min_hops = hops;
            min = r;
        }
    }
    return min;
}

/*
 * The only place a local training step happens. Trains whichever replica
 * of t->model this node currently holds; if none (a single-copy policy
 * relocated it elsewhere), trains the ambient sole copy instead, at no
 * network cost — the original TRAIN task never touched block/owners state
 * at all, so this preserves that location-agnostic, flow-free character
 * while still doing real work through the backend.
 */
void train_stage(struct task *t)
{
    struct node *orig = t->node;
    struct replica *r = model_replica_of(t->model, orig);
    unsigned int new_version;
    long long sim_ms;
    double loss, acc;
    void *new_params;
    size_t new_nbytes;

    if (!r) {
        r = model_replica_first(t->model);
    }
    assert(r != NULL);
    orig->round++;

    if (backend_train(orig->id, r->version, config.epochs, r->params, r->nbytes,
            &new_version, &sim_ms, &loss, &acc, &new_params, &new_nbytes) < 0) {
        fprintf(stderr, "backend_train failed for node %u\n", orig->id);
        exit(1);
    }
    free(r->params);
    r->params = new_params;
    r->nbytes = new_nbytes;
    r->version = new_version;
    r->round = orig->round;
    r->stamp = sim_time + sim_ms;
    orig->loss = loss;
    orig->acc = acc;
    orig->trained = 1;

    task_done(t, sim_time + sim_ms);
}

void barrier_stage(struct task *t)
{
    struct event *ev = event_alloc(ROUND_BARRIER, sim_time + BARRIER_DEF_MS);
    ev->data.t = t;
    event_enqueue(ev);
}

/*
 * Newest version of m whose params exist at sim_time, read off the replica
 * that produces m's versions: its owner's (model ids are owner node ids), or
 * the only copy once a single-copy policy has moved it. TRAIN and
 * aggregation bump that replica's version when they start but stamp it with
 * the time they finish, so until then the previous version is the newest.
 * Other replicas can't be used: a reader's copy is stamped when it was
 * captured, so a version fetched early would look already produced.
 */
static unsigned int newest_version(const struct model *m)
{
    struct replica *r, *src = model_replica_first(m);

    vp_for (r, &m->replicas) {
        if (r->node->id == m->id) {
            src = r;
            break;
        }
    }
    return (sim_time >= src->stamp || !src->version) ? src->version : src->version - 1;
}

/* Stages a snapshot of src for orig's next aggregation, remembering the
 * model's newest version right now so aggregate_into can tell a copy that
 * was already stale when obtained from one that went stale while waiting. */
static void stage_copy(struct node *orig, const struct replica *src)
{
    struct replica *snap = replica_dup(src, orig, sim_time);

    snap->capture_newest = newest_version(src->model);
    node_stage(orig, snap);
}

/*
 * Aggregates `r` (the current authoritative replica for t->model, wherever
 * it physically sits) with every snapshot orig has staged since its last
 * WRITE. Mutates r in place with the backend's result and clears the
 * staged list. Returns the elapsed sim_ms so the caller can advance time.
 *
 * Also records how many of the staged copies it folds are incoherent: their
 * version isn't their model's newest at this moment, either older (stale:
 * also recorded with how many versions behind) or ahead of it (handed out
 * before the owner finished producing it). A stale copy that was already
 * older than the newest version when it was staged is also recorded as
 * protocol staleness, as opposed to pipeline lag: obtained current, then
 * the owner moved on before this aggregation. r itself is checked too but
 * only counted if incoherent, which no current policy does.
 *
 * And, per staged copy, its round skew: orig's round minus the round the
 * copy's model was produced in (+k: k rounds behind orig, -k: ahead).
 */
long long aggregate_into(struct node *orig, struct replica *r)
{
    int count = 1 + (int) orig->staged.length;
    void **blobs = malloc(count * sizeof(void *));
    size_t *nbytes = malloc(count * sizeof(size_t));
    long long *stale = malloc(count * sizeof(long long));
    struct replica *s;
    int i = 1;
    int copies = (int) orig->staged.length, incoherent = 0;
    unsigned int newest = newest_version(r->model);
    long long sim_ms;
    void *new_params;
    size_t new_nbytes;

    if (r->version != newest) {
        copies++;
        incoherent++;
        if (r->version < newest) {
            metric_observe(&metrics.agg_stale, (long long) (newest - r->version));
        }
    }
    blobs[0] = r->params;
    nbytes[0] = r->nbytes;
    stale[0] = sim_time - r->stamp;
    vp_for (s, &orig->staged) {
        blobs[i] = s->params;
        nbytes[i] = s->nbytes;
        stale[i] = sim_time - s->stamp;
        newest = newest_version(s->model);
        incoherent += s->version != newest;
        metric_observe(&metrics.agg_round_skew, (long long) (orig->round - s->round));
        if (s->version < newest) {
            metric_observe(&metrics.agg_stale, (long long) (newest - s->version));
            if (s->version < s->capture_newest) {
                metric_observe(&metrics.agg_stale_protocol,
                    (long long) (newest - s->version));
            }
        }
        i++;
    }
    metric_observe(&metrics.agg_copies, copies);
    metric_observe(&metrics.agg_incoherent, incoherent);

    if (backend_aggregate(orig->id, count, stale, blobs, nbytes,
            &sim_ms, &new_params, &new_nbytes) < 0) {
        fprintf(stderr, "backend_aggregate failed for node %u\n", orig->id);
        exit(1);
    }
    free(blobs);
    free(nbytes);
    free(stale);

    node_stage_clear(orig);

    free(r->params);
    r->params = new_params;
    r->nbytes = new_nbytes;
    r->version++;
    r->round = orig->round;
    r->stamp = sim_time + sim_ms;
    return sim_ms;
}

/*
 * READ under SCU/SCM (single physical copy). `relocate` selects the
 * coherence effect on completion: 0 = leave the copy where it is (SCU,
 * "update"), 1 = the copy migrates to the reader (SCM, "move"). Either
 * way, a snapshot is staged on the reader for its own next aggregation,
 * also when the reader already holds the copy and nothing needs fetching.
 */
static void single_copy_read_stage(struct task *t, int relocate)
{
    struct node *orig = t->node, *dest;
    struct replica *src = model_replica_first(t->model);

    dest = src->node;
    if (orig->id == dest->id) {
        stage_copy(orig, src);
        task_done(t, sim_time);
        return;
    }
    switch (t->stage) {
    case 0:
        flow_enqueue(orig->id, dest->id, (size_t) config.control_bytes, t);
    break;
    case 1:
        flow_enqueue(dest->id, orig->id, src->nbytes, t);
    break;
    case 2:
        stage_copy(orig, src);
        if (relocate) {
            model_replica_relocate(src, orig);
        }
        task_done(t, sim_time);
    break;
    }
    t->stage++;
}

static void scu_read_stage(struct task *t) { single_copy_read_stage(t, 0); }
static void scm_read_stage(struct task *t) { single_copy_read_stage(t, 1); }

/*
 * READ under MCM/MCU (multi-copy): fetch from the nearest holder and join
 * the replica set, plus stage a snapshot for the reader's own aggregation.
 * A reader already in the replica set skips the fetch and stages the copy it
 * holds instead, so that neighbour still takes part in its aggregation.
 * Byte-identical between the two policies — only their WRITE differs.
 */
static void multi_copy_read_stage(struct task *t)
{
    struct node *orig = t->node;
    struct replica *src, *held;

    held = model_replica_of(t->model, orig);
    if (held) {
        stage_copy(orig, held);
        task_done(t, sim_time);
        return;
    }
    switch (t->stage) {
    case 0:
        src = nearest_replica(t->model, orig, &topology);
        if (!src) {
            /* every copy sits across a partition: nothing to read */
            task_done(t, sim_time);
            return;
        }
        flow_enqueue(orig->id, src->node->id, (size_t) config.control_bytes, t);
    break;
    case 1:
        src = nearest_replica(t->model, orig, &topology);
        flow_enqueue(src->node->id, orig->id, src->nbytes, t);
    break;
    case 2:
        src = nearest_replica(t->model, orig, &topology);
        model_replica_add(t->model, replica_dup(src, orig, sim_time));
        stage_copy(orig, src);
        task_done(t, sim_time);
    break;
    }
    t->stage++;
}

static void stage_advance_now(struct task *t)
{
    struct event *ev = event_alloc(STAGE_NEXT, sim_time);
    ev->data.t = t;
    event_enqueue(ev);
}

/*
 * WRITE implementations. Each returns 1 if policy_dispatch should advance
 * t->stage afterwards, or 0 if the task was already fully retired (e.g. the
 * local-write fast path scheduled its own task_done directly).
 */

static int scu_write_stage(struct task *t)
{
    struct event *ev;
    struct node *orig, *dest;
    struct replica *r;
    long long sim_ms;

    orig = t->node;
    r = model_replica_first(t->model);
    dest = r->node;
    if (orig->id == dest->id) {
        sim_ms = aggregate_into(orig, r);
        task_done(t, sim_time + sim_ms);
        return 0;
    }
    switch (t->stage) {
    case 0:
        flow_enqueue(orig->id, dest->id, (size_t) config.control_bytes, t);
    break;
    case 1:
        flow_enqueue(dest->id, orig->id, r->nbytes, t);
    break;
    case 2:
        sim_ms = aggregate_into(orig, r);
        ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
        ev->data.t = t;
        event_enqueue(ev);
    break;
    case 3:
        flow_enqueue(orig->id, dest->id, r->nbytes, t);
    break;
    case 4:
        task_done(t, sim_time);
    break;
    }
    return 1;
}

static int scm_write_stage(struct task *t)
{
    struct event *ev;
    struct node *orig, *dest;
    struct replica *r;
    long long sim_ms;

    orig = t->node;
    r = model_replica_first(t->model);
    dest = r->node;
    if (orig->id == dest->id) {
        sim_ms = aggregate_into(orig, r);
        task_done(t, sim_time + sim_ms);
        return 0;
    }
    switch (t->stage) {
    case 0:
        flow_enqueue(orig->id, dest->id, (size_t) config.control_bytes, t);
    break;
    case 1:
        flow_enqueue(dest->id, orig->id, r->nbytes, t);
    break;
    case 2:
        sim_ms = aggregate_into(orig, r);
        ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
        ev->data.t = t;
        event_enqueue(ev);
    break;
    case 3:
        model_replica_relocate(r, orig);
        task_done(t, sim_time);
    break;
    }
    return 1;
}

static int mcm_write_stage(struct task *t)
{
    struct event *ev;
    struct node *orig = t->node;
    struct node *dest;
    struct replica *own, *r2, *mine;
    long long sim_ms;
    int sent;

    own = model_replica_of(t->model, orig);
    if (own) {
        switch (t->stage) {
        case 0:
            sent = 0;
            vp_for (r2, &t->model->replicas) {
                if (r2->node->id == orig->id) {
                    continue;
                }
                flow_enqueue(orig->id, r2->node->id, (size_t) config.control_bytes, t);
                sent++;
            }
            if (!sent) {
                stage_advance_now(t);
            }
        break;
        case 1:
            sim_ms = aggregate_into(orig, own);
            ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
            ev->data.t = t;
            event_enqueue(ev);
        break;
        case 2:
            model_replica_keep_only(t->model, own);
            task_done(t, sim_time);
        break;
        }
        return 1;
    }
    // remote: orig doesn't currently hold a copy
    switch (t->stage) {
    case 0:
        dest = nearest_replica(t->model, orig, &topology)->node;
        flow_enqueue(orig->id, dest->id, (size_t) config.control_bytes, t);
    break;
    case 1:
        dest = nearest_replica(t->model, orig, &topology)->node;
        flow_enqueue(dest->id, orig->id,
            model_replica_of(t->model, dest)->nbytes, t);
    break;
    case 2:
        vp_for (r2, &t->model->replicas) {
            flow_enqueue(orig->id, r2->node->id, (size_t) config.control_bytes, t);
        }
    break;
    case 3:
        mine = replica_dup(nearest_replica(t->model, orig, &topology), orig, sim_time);
        model_replica_add(t->model, mine);
        sim_ms = aggregate_into(orig, mine);
        ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
        ev->data.t = t;
        event_enqueue(ev);
    break;
    case 4:
        own = model_replica_of(t->model, orig);
        model_replica_keep_only(t->model, own);
        task_done(t, sim_time);
    break;
    }
    return 1;
}

static int mcu_write_stage(struct task *t)
{
    struct event *ev;
    struct node *orig = t->node;
    struct node *dest;
    struct replica *own, *r2, *mine;
    long long sim_ms;
    int sent;

    own = model_replica_of(t->model, orig);
    if (own) {
        switch (t->stage) {
        case 0:
            sim_ms = aggregate_into(orig, own);
            ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
            ev->data.t = t;
            event_enqueue(ev);
        break;
        case 1:
            sent = 0;
            vp_for (r2, &t->model->replicas) {
                if (r2->node->id == orig->id) {
                    continue;
                }
                flow_enqueue(orig->id, r2->node->id, own->nbytes, t);
                free(r2->params);
                r2->params = params_dup(own->params, own->nbytes);
                r2->nbytes = own->nbytes;
                r2->version = own->version;
                r2->round = own->round;
                r2->stamp = own->stamp;
                sent++;
            }
            if (!sent) {
                stage_advance_now(t);
            }
        break;
        case 2:
            task_done(t, sim_time);
        break;
        }
        return 1;
    }
    // remote: orig doesn't currently hold a copy
    switch (t->stage) {
    case 0:
        dest = nearest_replica(t->model, orig, &topology)->node;
        flow_enqueue(orig->id, dest->id, (size_t) config.control_bytes, t);
    break;
    case 1:
        dest = nearest_replica(t->model, orig, &topology)->node;
        flow_enqueue(dest->id, orig->id,
            model_replica_of(t->model, dest)->nbytes, t);
    break;
    case 2:
        mine = replica_dup(nearest_replica(t->model, orig, &topology), orig, sim_time);
        model_replica_add(t->model, mine);
        sim_ms = aggregate_into(orig, mine);
        ev = event_alloc(STAGE_NEXT, sim_time + sim_ms);
        ev->data.t = t;
        event_enqueue(ev);
    break;
    case 3:
        mine = model_replica_of(t->model, orig);
        vp_for (r2, &t->model->replicas) {
            if (r2->node->id == orig->id) {
                continue;
            }
            flow_enqueue(orig->id, r2->node->id, mine->nbytes, t);
            free(r2->params);
            r2->params = params_dup(mine->params, mine->nbytes);
            r2->nbytes = mine->nbytes;
            r2->version = mine->version;
            r2->round = mine->round;
            r2->stamp = mine->stamp;
        }
    break;
    case 4:
        task_done(t, sim_time);
    break;
    }
    return 1;
}

/*
 * One row per coherence policy. To add a new one: write its read (or reuse
 * single/multi-copy semantics via a wrapper) and write functions above,
 * then add a row here keyed by its policy_t value. No other file needs to
 * change — event_process and main()'s output both go through policy_name/
 * policy_dispatch.
 */
struct coherence_policy {
    const char *name;
    void (*read)(struct task *t);
    int  (*write)(struct task *t);
};

static const struct coherence_policy policies[] = {
    [S_COPY_UPD] = { "SCU", scu_read_stage, scu_write_stage },
    [S_COPY_MOV] = { "SCI", scm_read_stage, scm_write_stage },
    [M_COPY_MOV] = { "MCI", multi_copy_read_stage, mcm_write_stage },
    [M_COPY_UPD] = { "MCU", multi_copy_read_stage, mcu_write_stage },
};
#define N_POLICIES (sizeof(policies) / sizeof(policies[0]))

const char *policy_name(enum policy_t p)
{
    if ((size_t) p >= N_POLICIES) {
        return NULL;
    }
    return policies[p].name;
}

void policy_dispatch(struct task *t)
{
    assert((size_t) policy < N_POLICIES);
    switch (t->type) {
    case READ:
        policies[policy].read(t);
        return;
    case WRITE:
        if (!policies[policy].write(t)) {
            return;
        }
    break;
    case TRAIN:
        train_stage(t);
        return;
    case BARRIER:
        barrier_stage(t);
    break;
    default:
    break;
    }
    t->stage++;
}
