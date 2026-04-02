/* ============================================================
 * qjson_db.c — Reactive QJSON store
 *
 * Push-based: mutating a fact set fires watchers on dependent views.
 * Dependencies extracted mechanically from IN clauses.
 * ============================================================ */

#include <string.h>
#include <stdlib.h>
#include "qjson_db.h"

/* ── Named entries (facts or views) ─────────────────────────── */

#define MAX_ENTRIES 256
#define MAX_WATCHERS 256
#define MAX_DEPS 16

typedef struct {
    const char *name;
    qjson_val  *val;          /* fact set or view definition */
    int         is_view;
    const char *deps[MAX_DEPS]; /* for views: names of fact sets */
    int         dep_count;
} qjson_entry;

typedef struct {
    int              id;
    const char      *view_name;
    qjson_notify_fn  fn;
    void            *userdata;
    int              active;
} qjson_watcher;

struct qjson_db {
    qjson_arena   *arena;
    qjson_entry    entries[MAX_ENTRIES];
    int            entry_count;
    qjson_watcher  watchers[MAX_WATCHERS];
    int            watcher_count;
    int            next_watch_id;
};

/* ── Dependency extraction ──────────────────────────────────── */

static void collect_deps(qjson_val *cond, const char **deps, int *count, int max) {
    if (!cond || *count >= max) return;

    switch (cond->type) {
    case QJSON_MATCH:
        /* source is the dependency */
        if (cond->match.source && cond->match.source->type == QJSON_STRING) {
            /* Deduplicate */
            for (int i = 0; i < *count; i++)
                if (strcmp(deps[i], cond->match.source->str.s) == 0) return;
            deps[(*count)++] = cond->match.source->str.s;
        }
        break;
    case QJSON_BINOP:
        collect_deps(cond->binop.left, deps, count, max);
        collect_deps(cond->binop.right, deps, count, max);
        break;
    case QJSON_NOTOP:
        collect_deps(cond->notop.operand, deps, count, max);
        break;
    default:
        break;
    }
}

int qjson_view_deps(qjson_val *view, const char **deps, int max_deps) {
    if (!view || view->type != QJSON_VIEW) return 0;
    int count = 0;
    collect_deps(view->view.cond, deps, &count, max_deps);
    return count;
}

/* ── Entry lookup ───────────────────────────────────────────── */

static qjson_entry *find_entry(qjson_db *db, const char *name) {
    for (int i = 0; i < db->entry_count; i++)
        if (strcmp(db->entries[i].name, name) == 0)
            return &db->entries[i];
    return NULL;
}

static qjson_entry *get_or_create_entry(qjson_db *db, const char *name) {
    qjson_entry *e = find_entry(db, name);
    if (e) return e;
    if (db->entry_count >= MAX_ENTRIES) return NULL;
    e = &db->entries[db->entry_count++];
    memset(e, 0, sizeof(*e));
    e->name = name;
    return e;
}

/* ── Fire watchers ──────────────────────────────────────────── */

/* Fire all watchers whose view depends on the named fact set */
static void fire_watchers(qjson_db *db, const char *changed_set) {
    for (int w = 0; w < db->watcher_count; w++) {
        if (!db->watchers[w].active) continue;
        /* Find the view this watcher is on */
        qjson_entry *ve = find_entry(db, db->watchers[w].view_name);
        if (!ve || !ve->is_view) continue;
        /* Check if the view depends on the changed set */
        for (int d = 0; d < ve->dep_count; d++) {
            if (strcmp(ve->deps[d], changed_set) == 0) {
                db->watchers[w].fn(db->watchers[w].view_name,
                                   db->watchers[w].userdata);
                break; /* fire once per watcher per mutation */
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────────────── */

qjson_db *qjson_db_open(qjson_arena *arena) {
    qjson_db *db = (qjson_db *)qjson_arena_alloc(arena, sizeof(qjson_db));
    if (!db) return NULL;
    memset(db, 0, sizeof(*db));
    db->arena = arena;
    db->next_watch_id = 1;
    return db;
}

void qjson_db_close(qjson_db *db) {
    /* Arena-allocated — nothing to free */
    (void)db;
}

void qjson_db_set(qjson_db *db, const char *name, qjson_val *val) {
    qjson_entry *e = get_or_create_entry(db, name);
    if (!e) return;
    e->val = val;
    e->is_view = 0;
    /* Fire watchers for any views that depend on this set */
    fire_watchers(db, name);
}

qjson_val *qjson_db_get(qjson_db *db, const char *name) {
    qjson_entry *e = find_entry(db, name);
    return e ? e->val : NULL;
}

void qjson_db_define(qjson_db *db, const char *name, qjson_val *view) {
    qjson_entry *e = get_or_create_entry(db, name);
    if (!e) return;
    e->val = view;
    e->is_view = 1;
    e->dep_count = qjson_view_deps(view, e->deps, MAX_DEPS);
}

/* Build a facts object from all entries (facts + views) for the resolver */
static qjson_val *build_facts(qjson_db *db) {
    int n = db->entry_count;

    qjson_val *obj = (qjson_val *)qjson_arena_alloc(db->arena, sizeof(qjson_val));
    if (!obj) return NULL;
    memset(obj, 0, sizeof(*obj));
    obj->type = QJSON_OBJECT;
    obj->obj.count = n;
    obj->obj.pairs = (qjson_kv *)qjson_arena_alloc(db->arena, n * sizeof(qjson_kv));
    if (!obj->obj.pairs) return NULL;

    int idx = 0;
    for (int i = 0; i < db->entry_count; i++) {
        /* Make string key for the name */
        qjson_val *key = (qjson_val *)qjson_arena_alloc(db->arena, sizeof(qjson_val));
        if (!key) return NULL;
        memset(key, 0, sizeof(*key));
        key->type = QJSON_STRING;
        key->str.s = db->entries[i].name;
        key->str.len = (int)strlen(db->entries[i].name);
        obj->obj.pairs[idx].key = key;
        obj->obj.pairs[idx].val = db->entries[i].val;
        idx++;
    }
    return obj;
}

qjson_val *qjson_db_resolve(qjson_db *db, const char *view_name) {
    qjson_entry *ve = find_entry(db, view_name);
    if (!ve || !ve->is_view) return NULL;
    qjson_val *facts = build_facts(db);
    if (!facts) return NULL;
    return qjson_resolve(db->arena, facts, ve->val);
}

qjson_val *qjson_db_select(qjson_db *db, const char *view_name, qjson_val *query) {
    qjson_entry *ve = find_entry(db, view_name);
    if (!ve || !ve->is_view) return NULL;
    qjson_val *facts = build_facts(db);
    if (!facts) return NULL;
    return qjson_select(db->arena, facts, view_name, query);
}

int qjson_watch(qjson_db *db, const char *view_name,
                qjson_notify_fn fn, void *userdata) {
    if (db->watcher_count >= MAX_WATCHERS) return -1;
    int id = db->next_watch_id++;
    qjson_watcher *w = &db->watchers[db->watcher_count++];
    w->id = id;
    w->view_name = view_name;
    w->fn = fn;
    w->userdata = userdata;
    w->active = 1;
    return id;
}

void qjson_unwatch(qjson_db *db, int watch_id) {
    for (int i = 0; i < db->watcher_count; i++) {
        if (db->watchers[i].id == watch_id) {
            db->watchers[i].active = 0;
            return;
        }
    }
}

/* ── Brain operations ───────────────────────────────────────── */

int qjson_ephemeral(qjson_db *db, const char *set_name, qjson_val *temp_val,
                    qjson_ephemeral_fn fn, void *userdata) {
    /* Save current state */
    qjson_entry *e = find_entry(db, set_name);
    qjson_val *saved = e ? e->val : NULL;
    int was_view = e ? e->is_view : 0;

    /* Assert: merge temp_val into the set (or create it) */
    qjson_db_set(db, set_name, temp_val);  /* watchers fire */

    /* Call the function — brain reacts */
    int result = fn(db, userdata);

    /* Retract: restore original state */
    if (saved) {
        if (was_view)
            qjson_db_define(db, set_name, saved);
        else
            qjson_db_set(db, set_name, saved);  /* watchers fire again */
    } else {
        /* Entry didn't exist before — remove it */
        qjson_entry *e2 = find_entry(db, set_name);
        if (e2) { e2->val = NULL; }
        fire_watchers(db, set_name);
    }

    return result;
}

void qjson_mineralize(qjson_db *db, const char *view_name) {
    qjson_entry *ve = find_entry(db, view_name);
    if (!ve || !ve->is_view) return;

    /* Resolve the view to concrete results */
    qjson_val *results = qjson_db_resolve(db, view_name);
    if (!results) return;

    /* Replace the view with facts */
    ve->is_view = 0;
    ve->dep_count = 0;
    ve->val = results;

    /* Notify: this "set" now has concrete data */
    fire_watchers(db, view_name);
}

void qjson_fossilize(qjson_db *db) {
    /* Collect view names first (mineralizing changes entries) */
    const char *views[MAX_ENTRIES];
    int nviews = 0;
    for (int i = 0; i < db->entry_count; i++) {
        if (db->entries[i].is_view)
            views[nviews++] = db->entries[i].name;
    }
    /* Mineralize each */
    for (int i = 0; i < nviews; i++)
        qjson_mineralize(db, views[i]);
}
