/* ============================================================
 * qjson_db.h — Reactive QJSON store
 *
 * Named fact sets + named views + push-based notifications.
 * When a fact set changes, watchers on dependent views fire.
 *
 *   qjson_db *db = qjson_db_open(arena);
 *   qjson_db_set(db, "parents", parents_set);
 *   qjson_db_define(db, "gp", view);
 *   int id = qjson_watch(db, "gp", my_callback, my_data);
 *   qjson_db_set(db, "parents", new_set);  // → my_callback fires
 *   qjson_unwatch(db, id);
 *   qjson_db_close(db);
 * ============================================================ */

#ifndef QJSON_DB_H
#define QJSON_DB_H

#include "qjson.h"
#include "qjson_resolve.h"

/* Callback: fired when a watched view becomes stale */
typedef void (*qjson_notify_fn)(const char *view_name, void *userdata);

/* Opaque database handle */
typedef struct qjson_db qjson_db;

/* Create/destroy */
qjson_db *qjson_db_open(qjson_arena *arena);
void      qjson_db_close(qjson_db *db);

/* Assert: store a named fact set.  Fires watchers. */
void qjson_db_set(qjson_db *db, const char *name, qjson_val *val);

/* Retract: remove a named fact set.  Fires watchers. */
void qjson_db_retract(qjson_db *db, const char *name);

/* Get a named value (fact set or view) */
qjson_val *qjson_db_get(qjson_db *db, const char *name);

/* Define a named view.  Automatically extracts dependencies from IN clauses. */
void qjson_db_define(qjson_db *db, const char *name, qjson_val *view);

/* Resolve a view against the current facts */
qjson_val *qjson_db_resolve(qjson_db *db, const char *view_name);

/* Select from a view with a query pattern */
qjson_val *qjson_db_select(qjson_db *db, const char *view_name, qjson_val *query);

/* Watch: register callback for when a view becomes stale.
   Returns watch ID (for unwatch).  -1 on error. */
int qjson_watch(qjson_db *db, const char *view_name,
                qjson_notify_fn fn, void *userdata);

/* Unwatch: remove a callback by ID */
void qjson_unwatch(qjson_db *db, int watch_id);

/* Extract dependency set names from a view's condition tree.
   Returns count of deps written.  deps[] filled with pointers
   into the view's arena-allocated strings. */
int qjson_view_deps(qjson_val *view, const char **deps, int max_deps);

/* ── Brain operations ───────────────────────────────────────── */

/* Signal: assert a transient fact, fire watchers, call fn,
   then retract and fire watchers again.  Never stored.
   Returns fn's return value. */
typedef int (*qjson_signal_fn)(qjson_db *db, void *userdata);
int qjson_signal(qjson_db *db, const char *set_name, qjson_val *temp_val,
                 qjson_signal_fn fn, void *userdata);

/* Freeze a view into concrete facts.  Resolves the view,
   replaces its definition with the results.  Immutable.
   With name: freeze one predicate.  Without: freeze all. */
void qjson_freeze(qjson_db *db, const char *view_name);
void qjson_freeze_all(qjson_db *db);

#endif
