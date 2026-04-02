/* ============================================================
 * qjson_resolve.c — View resolver: evaluate views against facts
 *
 * Pattern matching via unification.  Each MATCH clause
 * iterates a set and extends bindings.  AND = sequential
 * matching (join on shared vars).  OR = union.  NOT = anti-join.
 * ============================================================ */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "qjson_resolve.h"

#ifdef QJSON_USE_LIBBF
#include "libbf.h"
static void *_resolve_bf_realloc(void *opaque, void *ptr, size_t size) {
    (void)opaque;
    if (size == 0) { free(ptr); return NULL; }
    return realloc(ptr, size);
}
#endif

/* ── Environment helpers ────────────────────────────────────── */

static void env_init(qjson_env *e) {
    e->binds = NULL;
    e->count = 0;
    e->cap = 0;
}

static qjson_env env_copy(qjson_arena *a, const qjson_env *src) {
    qjson_env e;
    e.count = src->count;
    e.cap = src->count > 0 ? src->count : 4;
    e.binds = (qjson_binding *)qjson_arena_alloc(a, e.cap * sizeof(qjson_binding));
    if (e.binds && src->count > 0)
        memcpy(e.binds, src->binds, src->count * sizeof(qjson_binding));
    return e;
}

static qjson_val *env_lookup(const qjson_env *e, const char *name, int name_len) {
    for (int i = 0; i < e->count; i++) {
        if (e->binds[i].name_len == name_len &&
            memcmp(e->binds[i].name, name, name_len) == 0)
            return e->binds[i].value;
    }
    return NULL;
}

static int env_bind(qjson_arena *a, qjson_env *e, const char *name, int name_len, qjson_val *val) {
    /* Update existing binding if present (e.g. resolving an UNBOUND → concrete) */
    for (int i = 0; i < e->count; i++) {
        if (e->binds[i].name_len == name_len &&
            memcmp(e->binds[i].name, name, name_len) == 0) {
            e->binds[i].value = val;
            return 1;
        }
    }
    if (e->count >= e->cap) {
        int new_cap = e->cap ? e->cap * 2 : 8;
        qjson_binding *nb = (qjson_binding *)qjson_arena_alloc(a, new_cap * sizeof(qjson_binding));
        if (!nb) return 0;
        if (e->count > 0) memcpy(nb, e->binds, e->count * sizeof(qjson_binding));
        e->binds = nb;
        e->cap = new_cap;
    }
    e->binds[e->count].name = name;
    e->binds[e->count].name_len = name_len;
    e->binds[e->count].value = val;
    e->count++;
    return 1;
}

/* ── Environment set helpers ────────────────────────────────── */

static void envset_init(qjson_env_set *s) {
    s->envs = NULL;
    s->count = 0;
    s->cap = 0;
}

static int envset_add(qjson_arena *a, qjson_env_set *s, qjson_env e) {
    if (s->count >= s->cap) {
        int new_cap = s->cap ? s->cap * 2 : 8;
        qjson_env *ne = (qjson_env *)qjson_arena_alloc(a, new_cap * sizeof(qjson_env));
        if (!ne) return 0;
        if (s->count > 0) memcpy(ne, s->envs, s->count * sizeof(qjson_env));
        s->envs = ne;
        s->cap = new_cap;
    }
    s->envs[s->count++] = e;
    return 1;
}

/* ── Value equality (structural) ────────────────────────────── */

static int val_equal(qjson_val *a, qjson_val *b) {
    if (!a || !b) return a == b;
    if (a->type != b->type) return 0;
    switch (a->type) {
    case QJSON_NULL: return 1;
    case QJSON_TRUE: return 1;
    case QJSON_FALSE: return 1;
    case QJSON_NUMBER: return a->num == b->num;
    case QJSON_STRING:
    case QJSON_BIGINT:
    case QJSON_BIGFLOAT:
    case QJSON_BIGDECIMAL:
        return a->str.len == b->str.len &&
               memcmp(a->str.s, b->str.s, a->str.len) == 0;
    default: return 0; /* containers: deep compare not needed for now */
    }
}

/* ── Unification ────────────────────────────────────────────── */

/* Try to unify pattern (may contain unbounds) with value (concrete).
   Extends env with new bindings.  Returns 1 on success. */
static int unify(qjson_arena *a, qjson_val *pattern, qjson_val *value, qjson_env *env) {
    if (!pattern || !value) return pattern == value;

    /* Unbound variable: bind or check consistency */
    if (pattern->type == QJSON_UNBOUND) {
        /* Anonymous ? matches anything */
        if (pattern->str.len == 0) return 1;
        qjson_val *existing = env_lookup(env, pattern->str.s, pattern->str.len);
        if (existing) return val_equal(existing, value);
        return env_bind(a, env, pattern->str.s, pattern->str.len, value);
    }

    /* Same type, structural match */
    if (pattern->type != value->type) return 0;

    switch (pattern->type) {
    case QJSON_NULL: case QJSON_TRUE: case QJSON_FALSE:
        return 1;
    case QJSON_NUMBER:
        return pattern->num == value->num;
    case QJSON_STRING:
    case QJSON_BIGINT:
    case QJSON_BIGFLOAT:
    case QJSON_BIGDECIMAL:
        return pattern->str.len == value->str.len &&
               memcmp(pattern->str.s, value->str.s, pattern->str.len) == 0;
    case QJSON_ARRAY:
        if (pattern->arr.count != value->arr.count) return 0;
        for (int i = 0; i < pattern->arr.count; i++)
            if (!unify(a, pattern->arr.items[i], value->arr.items[i], env)) return 0;
        return 1;
    case QJSON_OBJECT:
        if (pattern->obj.count != value->obj.count) return 0;
        for (int i = 0; i < pattern->obj.count; i++) {
            /* Match by key position (same order) */
            if (!unify(a, pattern->obj.pairs[i].key, value->obj.pairs[i].key, env)) return 0;
            if (!unify(a, pattern->obj.pairs[i].val, value->obj.pairs[i].val, env)) return 0;
        }
        return 1;
    default:
        return 0;
    }
}

/* ── Expression evaluation ───────────────────────────────────── */

/* Convert a qjson_val to a double for arithmetic.
   Returns 1 if successful, 0 if value can't be converted. */
static int val_to_double(qjson_val *v, double *out) {
    if (!v) return 0;
    switch (v->type) {
    case QJSON_NUMBER: *out = v->num; return 1;
    case QJSON_BIGINT:
    case QJSON_BIGFLOAT:
    case QJSON_BIGDECIMAL:
        *out = atof(v->str.s);
        return 1;
    default: return 0;
    }
}

/* Make a NUMBER val from a double */
static qjson_val *make_number(qjson_arena *a, double d) {
    qjson_val *v = (qjson_val *)qjson_arena_alloc(a, sizeof(qjson_val));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    v->type = QJSON_NUMBER;
    v->num = d;
    return v;
}

/* Evaluate an arithmetic expression with bound variables.
   Returns a concrete value, or NULL if evaluation fails (unbound var). */
static qjson_val *eval_expr(qjson_arena *a, qjson_val *expr, const qjson_env *env) {
    if (!expr) return NULL;

    /* Unbound: look up binding */
    if (expr->type == QJSON_UNBOUND) {
        if (expr->str.len == 0) return NULL; /* anonymous */
        qjson_val *bound = env_lookup(env, expr->str.s, expr->str.len);
        if (!bound || bound->type == QJSON_UNBOUND) return NULL; /* still unbound */
        return bound;
    }

    /* Concrete value: return as-is */
    if (expr->type == QJSON_NUMBER || expr->type == QJSON_BIGINT ||
        expr->type == QJSON_BIGFLOAT || expr->type == QJSON_BIGDECIMAL)
        return expr;

    /* Arithmetic: evaluate both sides, compute */
    if (expr->type == QJSON_ARITH) {
        qjson_val *lv = eval_expr(a, expr->binop.left, env);
        qjson_val *rv = eval_expr(a, expr->binop.right, env);
        if (!lv || !rv) return NULL;
        double l, r;
        if (!val_to_double(lv, &l) || !val_to_double(rv, &r)) return NULL;
        double result;
        switch (expr->binop.op[0]) {
        case '+': result = l + r; break;
        case '-': result = l - r; break;
        case '*': result = l * r; break;
        case '/': result = (r != 0) ? l / r : 0; break;
        case '^': result = pow(l, r); break;
        default: return NULL;
        }
        return make_number(a, result);
    }

    return expr;
}

/* Check if an equation holds for given bindings.
   If one side has an unbound variable and the other evaluates to a concrete
   value, bind the variable (simple algebraic solving). */
static int check_equation(qjson_arena *a, qjson_val *eqn, qjson_env *env) {
    if (!eqn || eqn->type != QJSON_EQUATION) return 0;

    /* Try to evaluate both sides */
    qjson_val *lv = eval_expr(a, eqn->equation.left, env);
    qjson_val *rv = eval_expr(a, eqn->equation.right, env);

    if (lv && rv) {
        /* Both evaluated: check equality */
        double l, r;
        if (val_to_double(lv, &l) && val_to_double(rv, &r))
            return fabs(l - r) < 1e-10; /* tolerance for floating point */
        return val_equal(lv, rv);
    }

    /* One side is unbound: try to solve.
       Find the unbound variable name — it might be directly an UNBOUND node,
       or bound to another UNBOUND (from query pattern unification). */
    if (!lv && rv) {
        qjson_val *left = eqn->equation.left;
        const char *name = NULL; int name_len = 0;
        if (left->type == QJSON_UNBOUND && left->str.len > 0) {
            name = left->str.s; name_len = left->str.len;
        }
        if (name) {
            env_bind(a, env, name, name_len, rv);
            return 1;
        }
    }
    if (lv && !rv) {
        qjson_val *right = eqn->equation.right;
        const char *name = NULL; int name_len = 0;
        if (right->type == QJSON_UNBOUND && right->str.len > 0) {
            name = right->str.s; name_len = right->str.len;
        }
        if (name) {
            env_bind(a, env, name, name_len, lv);
            return 1;
        }
    }

    return 0; /* can't resolve */
}

/* ── Substitute bindings into a pattern ─────────────────────── */

static qjson_val *substitute(qjson_arena *a, qjson_val *pattern, const qjson_env *env) {
    if (!pattern) return NULL;

    if (pattern->type == QJSON_UNBOUND) {
        if (pattern->str.len == 0) return pattern; /* anonymous stays */
        qjson_val *bound = env_lookup(env, pattern->str.s, pattern->str.len);
        return bound ? bound : pattern;
    }

    if (pattern->type == QJSON_ARRAY) {
        qjson_val *v = (qjson_val *)qjson_arena_alloc(a, sizeof(qjson_val));
        if (!v) return NULL;
        v->type = QJSON_ARRAY;
        v->arr.count = pattern->arr.count;
        v->arr.items = (qjson_val **)qjson_arena_alloc(a, v->arr.count * sizeof(qjson_val *));
        if (!v->arr.items) return NULL;
        for (int i = 0; i < v->arr.count; i++)
            v->arr.items[i] = substitute(a, pattern->arr.items[i], env);
        return v;
    }

    if (pattern->type == QJSON_OBJECT) {
        qjson_val *v = (qjson_val *)qjson_arena_alloc(a, sizeof(qjson_val));
        if (!v) return NULL;
        v->type = QJSON_OBJECT;
        v->obj.count = pattern->obj.count;
        v->obj.pairs = (qjson_kv *)qjson_arena_alloc(a, v->obj.count * sizeof(qjson_kv));
        if (!v->obj.pairs) return NULL;
        for (int i = 0; i < v->obj.count; i++) {
            v->obj.pairs[i].key = substitute(a, pattern->obj.pairs[i].key, env);
            v->obj.pairs[i].val = substitute(a, pattern->obj.pairs[i].val, env);
        }
        return v;
    }

    return pattern; /* atoms pass through */
}

/* ── Resolve a MATCH clause against facts ───────────────────── */

/* Find a named set in the facts object */
static qjson_val *find_set(qjson_val *facts, qjson_val *source) {
    if (!facts || facts->type != QJSON_OBJECT || !source) return NULL;
    /* source is a bare identifier (string) — look up in facts */
    if (source->type != QJSON_STRING) return NULL;
    return qjson_obj_get(facts, source->str.s);
}

/* Iterate elements of a set (object keys for sets, array items for arrays) */
static int set_count(qjson_val *set) {
    if (!set) return 0;
    if (set->type == QJSON_OBJECT) return set->obj.count;
    if (set->type == QJSON_ARRAY) return set->arr.count;
    return 0;
}

static qjson_val *set_element(qjson_val *set, int i) {
    if (!set) return NULL;
    if (set->type == QJSON_OBJECT) return set->obj.pairs[i].key; /* set = keys */
    if (set->type == QJSON_ARRAY) return set->arr.items[i];
    return NULL;
}

/* Resolve a condition against facts, given a set of input environments.
   Returns a new environment set with extended/filtered bindings. */
static qjson_env_set resolve_cond(qjson_arena *a, qjson_val *cond,
                                   qjson_val *facts, qjson_env_set *input);

static qjson_env_set resolve_match(qjson_arena *a, qjson_val *match,
                                    qjson_val *facts, qjson_env_set *input) {
    qjson_env_set result;
    envset_init(&result);

    qjson_val *set = find_set(facts, match->match.source);
    if (!set) return result;

    int n = set_count(set);
    for (int ei = 0; ei < input->count; ei++) {
        for (int si = 0; si < n; si++) {
            qjson_env trial = env_copy(a, &input->envs[ei]);
            if (unify(a, match->match.pattern, set_element(set, si), &trial))
                envset_add(a, &result, trial);
        }
    }
    return result;
}

static qjson_env_set resolve_cond(qjson_arena *a, qjson_val *cond,
                                   qjson_val *facts, qjson_env_set *input) {
    qjson_env_set result;
    envset_init(&result);

    if (!cond) return *input;

    switch (cond->type) {
    case QJSON_MATCH:
        return resolve_match(a, cond, facts, input);

    case QJSON_BINOP:
        if (cond->binop.op_len == 3 && memcmp(cond->binop.op, "and", 3) == 0) {
            /* AND: sequential — left results feed into right */
            qjson_env_set left = resolve_cond(a, cond->binop.left, facts, input);
            return resolve_cond(a, cond->binop.right, facts, &left);
        }
        if (cond->binop.op_len == 2 && memcmp(cond->binop.op, "or", 2) == 0) {
            /* OR: union of both branches */
            qjson_env_set left = resolve_cond(a, cond->binop.left, facts, input);
            qjson_env_set right = resolve_cond(a, cond->binop.right, facts, input);
            for (int i = 0; i < left.count; i++) envset_add(a, &result, left.envs[i]);
            for (int i = 0; i < right.count; i++) envset_add(a, &result, right.envs[i]);
            return result;
        }
        return result;

    case QJSON_NOTOP: {
        /* NOT: keep environments where the sub-condition produces NO results */
        for (int i = 0; i < input->count; i++) {
            qjson_env_set single;
            envset_init(&single);
            envset_add(a, &single, input->envs[i]);
            qjson_env_set sub = resolve_cond(a, cond->notop.operand, facts, &single);
            if (sub.count == 0)
                envset_add(a, &result, input->envs[i]);
        }
        return result;
    }

    case QJSON_EQUATION: {
        /* Equation: check/solve for each input environment */
        for (int i = 0; i < input->count; i++) {
            qjson_env trial = env_copy(a, &input->envs[i]);
            if (check_equation(a, cond, &trial))
                envset_add(a, &result, trial);
        }
        return result;
    }

    default:
        return *input;
    }
}

/* ── Public API ─────────────────────────────────────────────── */

qjson_val *qjson_resolve(qjson_arena *a, qjson_val *facts, qjson_val *view) {
    if (!view || view->type != QJSON_VIEW) return NULL;

    /* Start with one empty environment */
    qjson_env_set envs;
    envset_init(&envs);
    qjson_env empty;
    env_init(&empty);
    envset_add(a, &envs, empty);

    /* Resolve the condition */
    qjson_env_set results = resolve_cond(a, view->view.cond, facts, &envs);

    /* Substitute bindings into the result pattern */
    qjson_val *arr = (qjson_val *)qjson_arena_alloc(a, sizeof(qjson_val));
    if (!arr) return NULL;
    arr->type = QJSON_ARRAY;
    arr->arr.count = results.count;
    arr->arr.items = (qjson_val **)qjson_arena_alloc(a, results.count * sizeof(qjson_val *));
    if (!arr->arr.items) return NULL;
    for (int i = 0; i < results.count; i++)
        arr->arr.items[i] = substitute(a, view->view.pattern, &results.envs[i]);

    return arr;
}

qjson_val *qjson_select(qjson_arena *a, qjson_val *facts,
                         const char *view_name, qjson_val *query) {
    qjson_val *view_def = qjson_obj_get(facts, view_name);
    if (!view_def || view_def->type != QJSON_VIEW) return NULL;

    if (!query) return qjson_resolve(a, facts, view_def);

    /* Seed initial bindings by unifying query with the view's result pattern.
       This extracts known values from the query (e.g. {P: 1000, R: ?R} gives
       P=1000) and passes them into the resolver so equations can evaluate. */
    qjson_env seed;
    env_init(&seed);
    if (!unify(a, view_def->view.pattern, query, &seed))
        return NULL; /* query doesn't match the view shape */

    qjson_env_set envs;
    envset_init(&envs);
    envset_add(a, &envs, seed);

    /* Resolve the condition with seeded bindings */
    qjson_env_set results = resolve_cond(a, view_def->view.cond, facts, &envs);

    /* Substitute bindings into the view pattern → result objects */
    qjson_val *arr = (qjson_val *)qjson_arena_alloc(a, sizeof(qjson_val));
    if (!arr) return NULL;
    arr->type = QJSON_ARRAY;
    arr->arr.count = results.count;
    arr->arr.items = (qjson_val **)qjson_arena_alloc(a, results.count * sizeof(qjson_val *));
    if (!arr->arr.items) return NULL;
    for (int i = 0; i < results.count; i++)
        arr->arr.items[i] = substitute(a, view_def->view.pattern, &results.envs[i]);

    return arr;
}
