/* ============================================================
 * qjson_resolve.h — View resolver: evaluate views against facts
 *
 * In-memory pattern matching with unification.
 * Shared variables across patterns produce joins.
 * ============================================================ */

#ifndef QJSON_RESOLVE_H
#define QJSON_RESOLVE_H

#include "qjson.h"

/* A binding: variable name → value */
typedef struct {
    const char *name;
    int         name_len;
    qjson_val  *value;
} qjson_binding;

/* An environment: set of bindings (one possible solution) */
typedef struct {
    qjson_binding *binds;
    int count;
    int cap;
} qjson_env;

/* A set of environments (all solutions) */
typedef struct {
    qjson_env *envs;
    int count;
    int cap;
} qjson_env_set;

/* Resolve a view against a facts object.
   facts: a QJSON object containing named sets (e.g. {parents: {...}})
   view:  a QJSON_VIEW node (pattern WHERE condition)

   Returns an array of result values (substituted patterns).
   All memory is arena-allocated. */
qjson_val *qjson_resolve(qjson_arena *a, qjson_val *facts, qjson_val *view);

/* Resolve with a query pattern (may contain concrete values as filters).
   query: a pattern like {grandparent: ?GP, grandchild: "alice"}
   view_name: key in facts that holds a QJSON_VIEW
   facts: the facts object

   Returns an array of matching results. */
qjson_val *qjson_select(qjson_arena *a, qjson_val *facts,
                         const char *view_name, qjson_val *query);

#endif
