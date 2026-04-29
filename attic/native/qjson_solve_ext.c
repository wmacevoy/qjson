/*
 * qjson_solve_ext.c — constraint solver SQL functions (ATTIC)
 *
 * Extracted from native/qjson_sqlite_ext.c on 2026-04-29 as part of
 * the bedrock/foundation/living-space refactor. The constraint solver
 * is query-language territory and belongs in the datalog repo, calling
 * qjson's arithmetic primitives (qjson_add/sub/mul/div/pow + libbf).
 *
 * Reference only — not built, not registered, not tested. Kept as a
 * working example of how to:
 *   - decompose an expression AST into 3-term constraints
 *   - propagate via leaf-folding (each constraint fires at most once)
 *   - handle libbf type widening across solve operations
 *   - round-trip values through the [lo, str, hi] projection
 *
 * To compile this standalone you would need to include from the foundation:
 *   - bf_t / bf_context_t / bf_*  (native/libbf/libbf.h)
 *   - qjson_project()             (native/qjson.h)
 *   - path_step, parse_path, free_steps, sql_builder, sb_init, sb_resolve, sb_free,
 *     dstr, dstr_init, dstr_free  (these were file-local statics in
 *                                  qjson_sqlite_ext.c — see git history
 *                                  pre-2026-04-29 for the originals)
 *   - _math_realloc, QJSON_DEFAULT_PREC  (also file-local in the original)
 *
 * SQL functions exposed (when registered):
 *   qjson_solve_add(a, b, c [, prefix])  — constraint a + b = c
 *   qjson_solve_sub/mul/div/pow          — same shape
 *   qjson_solve_sqrt/exp/log/sin/cos/tan/asin/acos/atan(a, b [, prefix])
 *                                         — F(a) = b
 *   qjson_solve(root_id, expr [, prefix]) — top-level expression solver
 *
 * Each takes INTEGER value_ids (lvalues, may be unbound) or TEXT literals
 * (rvalues, always bound) and returns:
 *    0 = inconsistent
 *    1 = solved (one unbound updated)
 *    2 = consistent (all bound, equation holds)
 *    3 = underdetermined (skipped, retry after other constraints fire)
 */

/* ── Constraint-solving arithmetic ───────────────────────── */
/*
 * qjson_solve_add(a, b, c [, prefix])  — constraint: a + b = c
 * qjson_solve_sub(a, b, c [, prefix])  — constraint: a - b = c
 * qjson_solve_mul(a, b, c [, prefix])  — constraint: a * b = c
 * qjson_solve_div(a, b, c [, prefix])  — constraint: a / b = c
 * qjson_solve_pow(a, b, c [, prefix])  — constraint: a ^ b = c
 *
 * Each argument is either:
 *   INTEGER → value_id (lvalue, loaded from DB, can be solved if unbound)
 *   TEXT    → literal decimal string (rvalue, always bound)
 *
 * Returns:
 *   0 = inconsistent (all bound, a OP b ≠ c)
 *   1 = solved (updated one unbound value)
 *   2 = consistent (all bound, a OP b = c)
 *   3 = underdetermined (2+ unbound, skipped)
 */

typedef struct {
    int is_id;          /* 1 if value_id, 0 if literal */
    sqlite3_int64 vid;  /* value_id (if is_id) */
    int is_unbound;
    bf_t val;           /* parsed value (if bound) */
    int has_val;
} solve_arg;

static void _solve_arg_load(solve_arg *sa, sqlite3_value *arg, sqlite3 *db,
                             const char *prefix, bf_context_t *bfctx) {
    memset(sa, 0, sizeof(*sa));
    if (sqlite3_value_type(arg) == SQLITE_INTEGER) {
        sa->is_id = 1;
        sa->vid = sqlite3_value_int64(arg);
        /* Load type and value from DB */
        char *sql = sqlite3_mprintf("SELECT v.type, n.lo, n.str, n.hi "
            "FROM \"%svalue\" v LEFT JOIN \"%snumber\" n ON n.value_id = v.id "
            "WHERE v.id = %lld", prefix, prefix, sa->vid);
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
            sqlite3_step(stmt) == SQLITE_ROW) {
            const char *type = (const char *)sqlite3_column_text(stmt, 0);
            if (type && strcmp(type, "unbound") == 0) {
                sa->is_unbound = 1;
            } else {
                const char *str = (const char *)sqlite3_column_text(stmt, 2);
                double lo = sqlite3_column_double(stmt, 1);
                bf_init(bfctx, &sa->val);
                if (str) {
                    bf_atof(&sa->val, str, NULL, 10, BF_PREC_INF, BF_RNDN);
                } else {
                    bf_set_float64(&sa->val, lo);
                }
                sa->has_val = 1;
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_free(sql);
    } else {
        /* TEXT literal */
        sa->is_id = 0;
        const char *s = (const char *)sqlite3_value_text(arg);
        if (s) {
            bf_init(bfctx, &sa->val);
            bf_atof(&sa->val, s, NULL, 10, BF_PREC_INF, BF_RNDN);
            sa->has_val = 1;
        }
    }
}

static void _solve_arg_free(solve_arg *sa) {
    if (sa->has_val) bf_delete(&sa->val);
}

/* Update an unbound value_id to a solved result */
static void _solve_update(sqlite3 *db, const char *prefix, sqlite3_int64 vid,
                           bf_t *result, bf_context_t *bfctx) {
    /* Format result as decimal string */
    size_t len;
    char *str = bf_ftoa(&len, result, 10, 36, BF_RNDN | BF_FTOA_FORMAT_FREE_MIN);
    if (!str) return;

    /* Project to [lo, str, hi] interval */
    double lo, hi;
    qjson_project(str, (int)len, &lo, &hi);
    const char *proj_str = (lo == hi) ? NULL : str;

    /* Update type from 'unbound' to 'number' */
    char *sql = sqlite3_mprintf("UPDATE \"%svalue\" SET type = 'number' WHERE id = %lld",
                                 prefix, vid);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);

    /* Delete old unbound projection, insert new number projection */
    sql = sqlite3_mprintf("DELETE FROM \"%snumber\" WHERE value_id = %lld", prefix, vid);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);

    sql = sqlite3_mprintf("INSERT INTO \"%snumber\" (value_id, lo, str, hi) VALUES (%lld, %f, %Q, %f)",
                           prefix, vid, lo, proj_str, hi);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);

    bf_free(bfctx, str);
}

/* Inverse operations for solving */
/* add: a+b=c → a=c-b, b=c-a */
/* sub: a-b=c → a=c+b, b=a-c */
/* mul: a*b=c → a=c/b, b=c/a */
/* div: a/b=c → a=c*b, b=a/c */
/* pow: a^b=c → a=c^(1/b), b=log(c)/log(a) */

enum { SOLVE_ADD, SOLVE_SUB, SOLVE_MUL, SOLVE_DIV, SOLVE_POW };

static void _sql_solve(sqlite3_context *ctx, int argc, sqlite3_value **argv, int op) {
    sqlite3 *db = sqlite3_context_db_handle(ctx);
    const char *prefix = (argc > 3 && sqlite3_value_type(argv[3]) == SQLITE_TEXT)
        ? (const char *)sqlite3_value_text(argv[3]) : "qjson_";

    bf_context_t bfctx;
    bf_context_init(&bfctx, _math_realloc, NULL);

    solve_arg args[3];
    _solve_arg_load(&args[0], argv[0], db, prefix, &bfctx);
    _solve_arg_load(&args[1], argv[1], db, prefix, &bfctx);
    _solve_arg_load(&args[2], argv[2], db, prefix, &bfctx);

    int n_unbound = args[0].is_unbound + args[1].is_unbound + args[2].is_unbound;

    if (n_unbound >= 2) {
        /* Underdetermined — skip */
        sqlite3_result_int(ctx, 3);
    } else if (n_unbound == 1) {
        /* Solve for the one unbound */
        int target = args[0].is_unbound ? 0 : args[1].is_unbound ? 1 : 2;
        bf_t result;
        bf_init(&bfctx, &result);

        if (op == SOLVE_ADD) {
            if (target == 0) bf_sub(&result, &args[2].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
            else if (target == 1) bf_sub(&result, &args[2].val, &args[0].val, QJSON_DEFAULT_PREC, BF_RNDN);
            else bf_add(&result, &args[0].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
        } else if (op == SOLVE_SUB) {
            if (target == 0) bf_add(&result, &args[2].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
            else if (target == 1) bf_sub(&result, &args[0].val, &args[2].val, QJSON_DEFAULT_PREC, BF_RNDN);
            else bf_sub(&result, &args[0].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
        } else if (op == SOLVE_MUL) {
            if (target == 0) bf_div(&result, &args[2].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
            else if (target == 1) bf_div(&result, &args[2].val, &args[0].val, QJSON_DEFAULT_PREC, BF_RNDN);
            else bf_mul(&result, &args[0].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
        } else if (op == SOLVE_DIV) {
            if (target == 0) bf_mul(&result, &args[2].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
            else if (target == 1) bf_div(&result, &args[0].val, &args[2].val, QJSON_DEFAULT_PREC, BF_RNDN);
            else bf_div(&result, &args[0].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
        } else if (op == SOLVE_POW) {
            if (target == 2) {
                bf_pow(&result, &args[0].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
            } else if (target == 0) {
                /* a = c^(1/b) */
                bf_t inv; bf_init(&bfctx, &inv);
                bf_t one; bf_init(&bfctx, &one); bf_set_ui(&one, 1);
                bf_div(&inv, &one, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
                bf_pow(&result, &args[2].val, &inv, QJSON_DEFAULT_PREC, BF_RNDN);
                bf_delete(&inv); bf_delete(&one);
            } else {
                /* b = log(c)/log(a) */
                bf_t lc, la; bf_init(&bfctx, &lc); bf_init(&bfctx, &la);
                bf_log(&lc, &args[2].val, QJSON_DEFAULT_PREC, BF_RNDN);
                bf_log(&la, &args[0].val, QJSON_DEFAULT_PREC, BF_RNDN);
                bf_div(&result, &lc, &la, QJSON_DEFAULT_PREC, BF_RNDN);
                bf_delete(&lc); bf_delete(&la);
            }
        }

        _solve_update(db, prefix, args[target].vid, &result, &bfctx);
        bf_delete(&result);
        sqlite3_result_int(ctx, 1);
    } else {
        /* All bound — consistency check */
        bf_t expected;
        bf_init(&bfctx, &expected);
        if (op == SOLVE_ADD) bf_add(&expected, &args[0].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
        else if (op == SOLVE_SUB) bf_sub(&expected, &args[0].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
        else if (op == SOLVE_MUL) bf_mul(&expected, &args[0].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
        else if (op == SOLVE_DIV) bf_div(&expected, &args[0].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
        else if (op == SOLVE_POW) bf_pow(&expected, &args[0].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);

        int eq = (bf_cmp(&expected, &args[2].val) == 0) ? 2 : 0;
        bf_delete(&expected);
        sqlite3_result_int(ctx, eq);  /* 2=consistent, 0=inconsistent */
    }

    _solve_arg_free(&args[0]);
    _solve_arg_free(&args[1]);
    _solve_arg_free(&args[2]);
    bf_context_end(&bfctx);
}

static void sql_solve_add(sqlite3_context *c, int n, sqlite3_value **v) { _sql_solve(c, n, v, SOLVE_ADD); }
static void sql_solve_sub(sqlite3_context *c, int n, sqlite3_value **v) { _sql_solve(c, n, v, SOLVE_SUB); }
static void sql_solve_mul(sqlite3_context *c, int n, sqlite3_value **v) { _sql_solve(c, n, v, SOLVE_MUL); }
static void sql_solve_div(sqlite3_context *c, int n, sqlite3_value **v) { _sql_solve(c, n, v, SOLVE_DIV); }
static void sql_solve_pow(sqlite3_context *c, int n, sqlite3_value **v) { _sql_solve(c, n, v, SOLVE_POW); }

/* ── Unary function solvers: qjson_solve_F(a, b) means F(a) = b ─── */
/* a unknown → a = F_inv(b).  b unknown → b = F(a).  Both known → check. */

enum {
    USOLVE_SQRT, USOLVE_EXP, USOLVE_LOG,
    USOLVE_SIN, USOLVE_COS, USOLVE_TAN,
    USOLVE_ASIN, USOLVE_ACOS, USOLVE_ATAN
};

static void _bf_sqrt(bf_t *r, const bf_t *a, limb_t prec) {
    bf_sqrt(r, a, prec, BF_RNDN);
}

static void _bf_exp(bf_t *r, const bf_t *a, limb_t prec) {
    bf_exp(r, a, prec, BF_RNDN);
}

static void _bf_log(bf_t *r, const bf_t *a, limb_t prec) {
    bf_log(r, a, prec, BF_RNDN);
}

static void _bf_sin(bf_t *r, const bf_t *a, limb_t prec) {
    bf_sin(r, a, prec, BF_RNDN);
}

static void _bf_cos(bf_t *r, const bf_t *a, limb_t prec) {
    bf_cos(r, a, prec, BF_RNDN);
}

static void _bf_tan(bf_t *r, const bf_t *a, limb_t prec) {
    bf_tan(r, a, prec, BF_RNDN);
}

static void _bf_asin(bf_t *r, const bf_t *a, limb_t prec) {
    bf_asin(r, a, prec, BF_RNDN);
}

static void _bf_acos(bf_t *r, const bf_t *a, limb_t prec) {
    bf_acos(r, a, prec, BF_RNDN);
}

static void _bf_atan(bf_t *r, const bf_t *a, limb_t prec) {
    bf_atan(r, a, prec, BF_RNDN);
}

typedef void (*bf_unary_fn)(bf_t *r, const bf_t *a, limb_t prec);

/* Forward/inverse pairs for each function */
static const struct {
    bf_unary_fn forward;
    bf_unary_fn inverse;
} _unary_pairs[] = {
    [USOLVE_SQRT] = { _bf_sqrt, NULL },    /* inv: a = b^2, handled specially */
    [USOLVE_EXP]  = { _bf_exp,  _bf_log },
    [USOLVE_LOG]  = { _bf_log,  _bf_exp },
    [USOLVE_SIN]  = { _bf_sin,  _bf_asin },
    [USOLVE_COS]  = { _bf_cos,  _bf_acos },
    [USOLVE_TAN]  = { _bf_tan,  _bf_atan },
    [USOLVE_ASIN] = { _bf_asin, _bf_sin },
    [USOLVE_ACOS] = { _bf_acos, _bf_cos },
    [USOLVE_ATAN] = { _bf_atan, _bf_tan },
};

static void _sql_solve_unary(sqlite3_context *ctx, int argc, sqlite3_value **argv, int op) {
    /* qjson_solve_F(a, b [, prefix]) means F(a) = b */
    sqlite3 *db = sqlite3_context_db_handle(ctx);
    const char *prefix = (argc > 2 && sqlite3_value_type(argv[2]) == SQLITE_TEXT)
        ? (const char *)sqlite3_value_text(argv[2]) : "qjson_";

    bf_context_t bfctx;
    bf_context_init(&bfctx, _math_realloc, NULL);

    solve_arg args[2];
    _solve_arg_load(&args[0], argv[0], db, prefix, &bfctx);
    _solve_arg_load(&args[1], argv[1], db, prefix, &bfctx);

    int n_unbound = args[0].is_unbound + args[1].is_unbound;

    if (n_unbound >= 2) {
        sqlite3_result_int(ctx, 3); /* underdetermined */
    } else if (n_unbound == 1) {
        bf_t result;
        bf_init(&bfctx, &result);

        if (args[1].is_unbound) {
            /* b unknown: b = F(a) */
            _unary_pairs[op].forward(&result, &args[0].val, QJSON_DEFAULT_PREC);
        } else {
            /* a unknown: a = F_inv(b) */
            if (op == USOLVE_SQRT) {
                /* sqrt inverse: a = b^2 */
                bf_mul(&result, &args[1].val, &args[1].val, QJSON_DEFAULT_PREC, BF_RNDN);
            } else {
                _unary_pairs[op].inverse(&result, &args[1].val, QJSON_DEFAULT_PREC);
            }
        }

        int target = args[0].is_unbound ? 0 : 1;
        _solve_update(db, prefix, args[target].vid, &result, &bfctx);
        bf_delete(&result);
        sqlite3_result_int(ctx, 1); /* solved */
    } else {
        /* Both known: consistency check */
        bf_t expected;
        bf_init(&bfctx, &expected);
        _unary_pairs[op].forward(&expected, &args[0].val, QJSON_DEFAULT_PREC);
        int eq = (bf_cmp(&expected, &args[1].val) == 0) ? 2 : 0;
        bf_delete(&expected);
        sqlite3_result_int(ctx, eq);
    }

    _solve_arg_free(&args[0]);
    _solve_arg_free(&args[1]);
    bf_context_end(&bfctx);
}

static void sql_solve_sqrt(sqlite3_context *c, int n, sqlite3_value **v) { _sql_solve_unary(c, n, v, USOLVE_SQRT); }
static void sql_solve_exp(sqlite3_context *c, int n, sqlite3_value **v)  { _sql_solve_unary(c, n, v, USOLVE_EXP); }
static void sql_solve_log(sqlite3_context *c, int n, sqlite3_value **v)  { _sql_solve_unary(c, n, v, USOLVE_LOG); }
static void sql_solve_sin(sqlite3_context *c, int n, sqlite3_value **v)  { _sql_solve_unary(c, n, v, USOLVE_SIN); }
static void sql_solve_cos(sqlite3_context *c, int n, sqlite3_value **v)  { _sql_solve_unary(c, n, v, USOLVE_COS); }
static void sql_solve_tan(sqlite3_context *c, int n, sqlite3_value **v)  { _sql_solve_unary(c, n, v, USOLVE_TAN); }
static void sql_solve_asin(sqlite3_context *c, int n, sqlite3_value **v) { _sql_solve_unary(c, n, v, USOLVE_ASIN); }
static void sql_solve_acos(sqlite3_context *c, int n, sqlite3_value **v) { _sql_solve_unary(c, n, v, USOLVE_ACOS); }
static void sql_solve_atan(sqlite3_context *c, int n, sqlite3_value **v) { _sql_solve_unary(c, n, v, USOLVE_ATAN); }

/* ── Expression parser + solver ─────────────────────────── */
/*
 * qjson_solve(root_id, expr, [prefix])
 *
 * Parses constraint expression, decomposes AST into 3-term
 * qjson_solve_* calls with anonymous temporaries, runs
 * leaf-folding propagation.
 *
 * Returns: 1=solved, 0=inconsistent/underdetermined
 */

/* AST node types */
enum { AST_LIT, AST_PATH, AST_BINOP, AST_FUNC };
enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW };

typedef struct ast_node {
    int kind;
    union {
        char *lit;           /* AST_LIT: decimal string */
        char *path;          /* AST_PATH: ".field" */
        struct { int op; struct ast_node *left, *right; } bin; /* AST_BINOP */
        struct { char *name; struct ast_node *arg; } func;     /* AST_FUNC (1-arg) */
    };
} ast_node;

static ast_node *_ast_alloc(int kind) {
    ast_node *n = sqlite3_malloc(sizeof(ast_node));
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    return n;
}

static void _ast_free(ast_node *n) {
    if (!n) return;
    if (n->kind == AST_LIT) sqlite3_free(n->lit);
    else if (n->kind == AST_PATH) sqlite3_free(n->path);
    else if (n->kind == AST_BINOP) { _ast_free(n->bin.left); _ast_free(n->bin.right); }
    else if (n->kind == AST_FUNC) { sqlite3_free(n->func.name); _ast_free(n->func.arg); }
    sqlite3_free(n);
}

/* Tokenizer for expressions */
typedef struct { const char *s; int pos, len; } etok;

static void _etok_ws(etok *t) {
    while (t->pos < t->len && isspace(t->s[t->pos])) t->pos++;
}

static ast_node *_parse_expr(etok *t);

static ast_node *_parse_atom(etok *t) {
    _etok_ws(t);
    if (t->pos >= t->len) return NULL;

    /* Function: POWER(...), SQRT(...), LOG(...), etc */
    if (isalpha(t->s[t->pos])) {
        int start = t->pos;
        while (t->pos < t->len && (isalnum(t->s[t->pos]) || t->s[t->pos] == '_')) t->pos++;
        int nlen = t->pos - start;
        char *name = sqlite3_mprintf("%.*s", nlen, t->s + start);
        _etok_ws(t);

        /* Check for AND keyword — not a function */
        if (strcmp(name, "AND") == 0) { sqlite3_free(name); t->pos = start; return NULL; }

        if (t->pos < t->len && t->s[t->pos] == '(') {
            t->pos++; /* skip ( */
            /* POWER has 2 args, PI has 0, others have 1 */
            int is_power = (strcasecmp(name, "POWER") == 0);
            int is_pi = (strcasecmp(name, "PI") == 0);

            if (is_pi) {
                _etok_ws(t);
                if (t->pos < t->len && t->s[t->pos] == ')') t->pos++;
                /* PI → literal via SQL */
                ast_node *n = _ast_alloc(AST_FUNC);
                n->func.name = name;
                n->func.arg = NULL;
                return n;
            }

            ast_node *arg1 = _parse_expr(t);
            if (is_power) {
                _etok_ws(t);
                if (t->pos < t->len && t->s[t->pos] == ',') t->pos++;
                ast_node *arg2 = _parse_expr(t);
                _etok_ws(t);
                if (t->pos < t->len && t->s[t->pos] == ')') t->pos++;
                sqlite3_free(name);
                ast_node *n = _ast_alloc(AST_BINOP);
                n->bin.op = OP_POW; n->bin.left = arg1; n->bin.right = arg2;
                return n;
            }
            _etok_ws(t);
            if (t->pos < t->len && t->s[t->pos] == ')') t->pos++;
            ast_node *n = _ast_alloc(AST_FUNC);
            n->func.name = name;
            n->func.arg = arg1;
            return n;
        }
        sqlite3_free(name);
        t->pos = start; /* backtrack — not a function */
    }

    /* Parenthesized expression */
    if (t->s[t->pos] == '(') {
        t->pos++;
        ast_node *n = _parse_expr(t);
        _etok_ws(t);
        if (t->pos < t->len && t->s[t->pos] == ')') t->pos++;
        return n;
    }

    /* Path: .field, .a.b[K], etc */
    if (t->s[t->pos] == '.' || t->s[t->pos] == '[') {
        int start = t->pos;
        while (t->pos < t->len && (t->s[t->pos] == '.' || t->s[t->pos] == '[' ||
               isalnum(t->s[t->pos]) || t->s[t->pos] == '_' || t->s[t->pos] == ']'))
            t->pos++;
        ast_node *n = _ast_alloc(AST_PATH);
        n->path = sqlite3_mprintf("%.*s", t->pos - start, t->s + start);
        return n;
    }

    /* Number literal */
    if (isdigit(t->s[t->pos]) || (t->s[t->pos] == '-' && t->pos+1 < t->len && isdigit(t->s[t->pos+1]))) {
        int start = t->pos;
        if (t->s[t->pos] == '-') t->pos++;
        while (t->pos < t->len && (isdigit(t->s[t->pos]) || t->s[t->pos] == '.' ||
               t->s[t->pos] == 'e' || t->s[t->pos] == 'E' || t->s[t->pos] == '+' ||
               t->s[t->pos] == '-' || t->s[t->pos] == 'N' || t->s[t->pos] == 'M' ||
               t->s[t->pos] == 'L' || t->s[t->pos] == 'n' || t->s[t->pos] == 'm' ||
               t->s[t->pos] == 'l'))
            t->pos++;
        ast_node *n = _ast_alloc(AST_LIT);
        int nlen = t->pos - start;
        char *raw = sqlite3_mprintf("%.*s", nlen, t->s + start);
        /* Strip suffix */
        int rlen = (int)strlen(raw);
        if (rlen > 0 && (raw[rlen-1] == 'N' || raw[rlen-1] == 'M' || raw[rlen-1] == 'L' ||
                          raw[rlen-1] == 'n' || raw[rlen-1] == 'm' || raw[rlen-1] == 'l'))
            raw[rlen-1] = '\0';
        n->lit = raw;
        return n;
    }

    return NULL;
}

static ast_node *_parse_factor(etok *t) {
    ast_node *left = _parse_atom(t);
    _etok_ws(t);
    if (t->pos+1 < t->len && t->s[t->pos] == '*' && t->s[t->pos+1] == '*') {
        t->pos += 2;
        ast_node *n = _ast_alloc(AST_BINOP);
        n->bin.op = OP_POW; n->bin.left = left; n->bin.right = _parse_atom(t);
        return n;
    }
    return left;
}

static ast_node *_parse_term(etok *t) {
    ast_node *left = _parse_factor(t);
    while (1) {
        _etok_ws(t);
        if (t->pos >= t->len) break;
        if (t->s[t->pos] == '*' && (t->pos+1 >= t->len || t->s[t->pos+1] != '*')) {
            t->pos++;
            ast_node *n = _ast_alloc(AST_BINOP);
            n->bin.op = OP_MUL; n->bin.left = left; n->bin.right = _parse_factor(t);
            left = n;
        } else if (t->s[t->pos] == '/') {
            t->pos++;
            ast_node *n = _ast_alloc(AST_BINOP);
            n->bin.op = OP_DIV; n->bin.left = left; n->bin.right = _parse_factor(t);
            left = n;
        } else break;
    }
    return left;
}

static ast_node *_parse_expr(etok *t) {
    ast_node *left = _parse_term(t);
    while (1) {
        _etok_ws(t);
        if (t->pos >= t->len) break;
        if (t->s[t->pos] == '+') {
            t->pos++;
            ast_node *n = _ast_alloc(AST_BINOP);
            n->bin.op = OP_ADD; n->bin.left = left; n->bin.right = _parse_term(t);
            left = n;
        } else if (t->s[t->pos] == '-') {
            t->pos++;
            ast_node *n = _ast_alloc(AST_BINOP);
            n->bin.op = OP_SUB; n->bin.left = left; n->bin.right = _parse_term(t);
            left = n;
        } else break;
    }
    return left;
}

/* Decompose AST into 3-term constraints, return value_id for each node */
static sqlite3_int64 _ast_to_vid(sqlite3 *db, const char *prefix,
                                  sqlite3_int64 root_id, ast_node *node,
                                  char ***sql_out, sqlite3_int64 **p1, sqlite3_int64 **p2, sqlite3_int64 **p3,
                                  int *nc, int *nc_cap) {
    if (!node) return 0;

    if (node->kind == AST_LIT) {
        /* Store literal as a number value */
        char *sql = sqlite3_mprintf("INSERT INTO \"%svalue\" (type) VALUES ('number')", prefix);
        sqlite3_exec(db, sql, 0, 0, 0); sqlite3_free(sql);
        sqlite3_int64 vid = sqlite3_last_insert_rowid(db);
        double lo, hi;
        qjson_project(node->lit, (int)strlen(node->lit), &lo, &hi);
        const char *str = (lo == hi) ? NULL : node->lit;
        sql = sqlite3_mprintf("INSERT INTO \"%snumber\" (value_id, lo, str, hi) VALUES (%lld, %f, %Q, %f)",
                               prefix, vid, lo, str, hi);
        sqlite3_exec(db, sql, 0, 0, 0); sqlite3_free(sql);
        return vid;
    }

    if (node->kind == AST_PATH) {
        /* Resolve path to value_id */
        path_step steps[32];
        int nsteps = parse_path(node->path, steps, 32);
        sql_builder sb; sb_init(&sb, prefix);
        dstr vid_expr; dstr_init(&vid_expr);
        sb_resolve(&sb, steps, nsteps, "root.id", &vid_expr);
        free_steps(steps, nsteps);

        char *sql = sqlite3_mprintf("SELECT %s FROM \"%svalue\" root %s WHERE root.id = %lld",
                                     vid_expr.buf, prefix, sb.joins.buf ? sb.joins.buf : "", root_id);
        sqlite3_stmt *stmt = NULL;
        sqlite3_int64 vid = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
            vid = sqlite3_column_int64(stmt, 0);
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_free(sql);
        dstr_free(&vid_expr);
        sb_free(&sb);
        return vid;
    }

    if (node->kind == AST_BINOP) {
        sqlite3_int64 left_vid = _ast_to_vid(db, prefix, root_id, node->bin.left, sql_out, p1, p2, p3, nc, nc_cap);
        sqlite3_int64 right_vid = _ast_to_vid(db, prefix, root_id, node->bin.right, sql_out, p1, p2, p3, nc, nc_cap);

        /* Create anonymous temp */
        char *sql = sqlite3_mprintf("INSERT INTO \"%svalue\" (type) VALUES ('unbound')", prefix);
        sqlite3_exec(db, sql, 0, 0, 0); sqlite3_free(sql);
        sqlite3_int64 result_vid = sqlite3_last_insert_rowid(db);
        double neg_inf = -1.0/0.0, pos_inf = 1.0/0.0;
        sql = sqlite3_mprintf("INSERT INTO \"%snumber\" (value_id, lo, str, hi) VALUES (%lld, %f, '?', %f)",
                               prefix, result_vid, neg_inf, pos_inf);
        sqlite3_exec(db, sql, 0, 0, 0); sqlite3_free(sql);

        /* Record constraint */
        if (*nc >= *nc_cap) {
            *nc_cap = (*nc_cap) ? (*nc_cap) * 2 : 32;
            *p1 = sqlite3_realloc(*p1, *nc_cap * sizeof(sqlite3_int64));
            *p2 = sqlite3_realloc(*p2, *nc_cap * sizeof(sqlite3_int64));
            *p3 = sqlite3_realloc(*p3, *nc_cap * sizeof(sqlite3_int64));
            *sql_out = sqlite3_realloc(*sql_out, *nc_cap * sizeof(char*));
        }
        static const char *op_names[] = {"add", "sub", "mul", "div", "pow"};
        (*sql_out)[*nc] = sqlite3_mprintf("qjson_solve_%s", op_names[node->bin.op]);
        (*p1)[*nc] = left_vid;
        (*p2)[*nc] = right_vid;
        (*p3)[*nc] = result_vid;
        (*nc)++;
        return result_vid;
    }

    if (node->kind == AST_FUNC) {
        /* PI() is a constant — evaluate and store as literal */
        if (strcasecmp(node->func.name, "PI") == 0) {
            char *eval_sql = sqlite3_mprintf("SELECT qjson_pi()");
            sqlite3_stmt *stmt = NULL;
            char *result = NULL;
            if (sqlite3_prepare_v2(db, eval_sql, -1, &stmt, NULL) == SQLITE_OK &&
                sqlite3_step(stmt) == SQLITE_ROW) {
                const char *txt = (const char *)sqlite3_column_text(stmt, 0);
                if (txt) result = sqlite3_mprintf("%s", txt);
            }
            if (stmt) sqlite3_finalize(stmt);
            sqlite3_free(eval_sql);
            if (result) {
                char *sql = sqlite3_mprintf("INSERT INTO \"%svalue\" (type) VALUES ('number')", prefix);
                sqlite3_exec(db, sql, 0, 0, 0); sqlite3_free(sql);
                sqlite3_int64 vid = sqlite3_last_insert_rowid(db);
                double lo, hi;
                qjson_project(result, (int)strlen(result), &lo, &hi);
                sql = sqlite3_mprintf("INSERT INTO \"%snumber\" (value_id, lo, str, hi) VALUES (%lld, %f, %Q, %f)",
                                       prefix, vid, lo, (lo == hi) ? NULL : result, hi);
                sqlite3_exec(db, sql, 0, 0, 0); sqlite3_free(sql);
                sqlite3_free(result);
                return vid;
            }
            return 0;
        }

        /* Unary functions: create constraint qjson_solve_F(arg, result) */
        sqlite3_int64 arg_vid = node->func.arg ?
            _ast_to_vid(db, prefix, root_id, node->func.arg, sql_out, p1, p2, p3, nc, nc_cap) : 0;
        if (!arg_vid) return 0;

        /* Map function name to solver name */
        const char *solve_name = NULL;
        if (strcasecmp(node->func.name, "SQRT") == 0) solve_name = "qjson_solve_sqrt";
        else if (strcasecmp(node->func.name, "EXP") == 0) solve_name = "qjson_solve_exp";
        else if (strcasecmp(node->func.name, "LOG") == 0) solve_name = "qjson_solve_log";
        else if (strcasecmp(node->func.name, "SIN") == 0) solve_name = "qjson_solve_sin";
        else if (strcasecmp(node->func.name, "COS") == 0) solve_name = "qjson_solve_cos";
        else if (strcasecmp(node->func.name, "TAN") == 0) solve_name = "qjson_solve_tan";
        else if (strcasecmp(node->func.name, "ATAN") == 0) solve_name = "qjson_solve_atan";
        else if (strcasecmp(node->func.name, "ASIN") == 0) solve_name = "qjson_solve_asin";
        else if (strcasecmp(node->func.name, "ACOS") == 0) solve_name = "qjson_solve_acos";
        if (!solve_name) return 0;

        /* Create anonymous temp for result */
        char *sql = sqlite3_mprintf("INSERT INTO \"%svalue\" (type) VALUES ('unbound')", prefix);
        sqlite3_exec(db, sql, 0, 0, 0); sqlite3_free(sql);
        sqlite3_int64 result_vid = sqlite3_last_insert_rowid(db);
        double neg_inf = -1.0/0.0, pos_inf = 1.0/0.0;
        sql = sqlite3_mprintf("INSERT INTO \"%snumber\" (value_id, lo, str, hi) VALUES (%lld, %f, '?', %f)",
                               prefix, result_vid, neg_inf, pos_inf);
        sqlite3_exec(db, sql, 0, 0, 0); sqlite3_free(sql);

        /* Record 2-arg constraint: solve_F(arg, result) */
        if (*nc >= *nc_cap) {
            *nc_cap = (*nc_cap) ? (*nc_cap) * 2 : 32;
            *p1 = sqlite3_realloc(*p1, *nc_cap * sizeof(sqlite3_int64));
            *p2 = sqlite3_realloc(*p2, *nc_cap * sizeof(sqlite3_int64));
            *p3 = sqlite3_realloc(*p3, *nc_cap * sizeof(sqlite3_int64));
            *sql_out = sqlite3_realloc(*sql_out, *nc_cap * sizeof(char*));
        }
        (*sql_out)[*nc] = sqlite3_mprintf("%s", solve_name);
        (*p1)[*nc] = arg_vid;
        (*p2)[*nc] = result_vid;
        (*p3)[*nc] = 0;  /* unused — 2-arg solver */
        (*nc)++;
        return result_vid;
    }

    return 0;
}

/* qjson_solve(root_id, expr_text, [prefix]) */
static void sql_qjson_solve(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc < 2 || sqlite3_value_type(argv[0]) == SQLITE_NULL ||
        sqlite3_value_type(argv[1]) == SQLITE_NULL) {
        sqlite3_result_int(ctx, 0); return;
    }

    sqlite3 *db = sqlite3_context_db_handle(ctx);
    sqlite3_int64 root_id = sqlite3_value_int64(argv[0]);
    const char *expr_text = (const char *)sqlite3_value_text(argv[1]);
    const char *prefix = (argc > 2 && sqlite3_value_type(argv[2]) != SQLITE_NULL)
        ? (const char *)sqlite3_value_text(argv[2]) : "qjson_";

    /* Split on AND, parse each LHS == RHS */
    char **solve_fns = NULL;
    sqlite3_int64 *c_p1 = NULL, *c_p2 = NULL, *c_p3 = NULL;
    int nc = 0, nc_cap = 0;

    const char *p = expr_text;
    while (*p) {
        /* Skip whitespace */
        while (*p && isspace(*p)) p++;
        if (!*p) break;

        /* Find == */
        const char *eq = strstr(p, "==");
        if (!eq) break;

        /* Parse LHS */
        etok lt = {p, 0, (int)(eq - p)};
        ast_node *lhs = _parse_expr(&lt);

        /* Parse RHS — find end (next AND or end of string) */
        const char *rhs_start = eq + 2;
        const char *and_pos = rhs_start;
        /* Find next AND that's not inside parens */
        int depth = 0;
        while (*and_pos) {
            if (*and_pos == '(') depth++;
            else if (*and_pos == ')') depth--;
            else if (depth == 0 && strncmp(and_pos, "AND", 3) == 0 &&
                     (and_pos == rhs_start || isspace(and_pos[-1])) &&
                     (and_pos[3] == '\0' || isspace(and_pos[3]) || and_pos[3] == '.'))
                break;
            and_pos++;
        }
        int rhs_len = (int)(and_pos - rhs_start);
        etok rt = {rhs_start, 0, rhs_len};
        ast_node *rhs = _parse_expr(&rt);

        /* Decompose both sides */
        sqlite3_int64 lhs_vid = _ast_to_vid(db, prefix, root_id, lhs, &solve_fns, &c_p1, &c_p2, &c_p3, &nc, &nc_cap);
        sqlite3_int64 rhs_vid = _ast_to_vid(db, prefix, root_id, rhs, &solve_fns, &c_p1, &c_p2, &c_p3, &nc, &nc_cap);

        /* Top-level: lhs == rhs → qjson_solve_add(lhs, 0, rhs) */
        /* Store a zero literal */
        char *sql = sqlite3_mprintf("INSERT INTO \"%svalue\" (type) VALUES ('number')", prefix);
        sqlite3_exec(db, sql, 0, 0, 0); sqlite3_free(sql);
        sqlite3_int64 zero_vid = sqlite3_last_insert_rowid(db);
        sql = sqlite3_mprintf("INSERT INTO \"%snumber\" (value_id, lo, str, hi) VALUES (%lld, 0, NULL, 0)", prefix, zero_vid);
        sqlite3_exec(db, sql, 0, 0, 0); sqlite3_free(sql);

        if (nc >= nc_cap) {
            nc_cap = nc_cap ? nc_cap * 2 : 32;
            c_p1 = sqlite3_realloc(c_p1, nc_cap * sizeof(sqlite3_int64));
            c_p2 = sqlite3_realloc(c_p2, nc_cap * sizeof(sqlite3_int64));
            c_p3 = sqlite3_realloc(c_p3, nc_cap * sizeof(sqlite3_int64));
            solve_fns = sqlite3_realloc(solve_fns, nc_cap * sizeof(char*));
        }
        solve_fns[nc] = sqlite3_mprintf("qjson_solve_add");
        c_p1[nc] = lhs_vid; c_p2[nc] = zero_vid; c_p3[nc] = rhs_vid;
        nc++;

        _ast_free(lhs);
        _ast_free(rhs);

        /* Advance past AND */
        p = and_pos;
        if (strncmp(p, "AND", 3) == 0) p += 3;
    }

    /* Leaf-folding propagation */
    int solved = 1;
    int *done = sqlite3_malloc(nc * sizeof(int));
    memset(done, 0, nc * sizeof(int));
    while (1) {
        int progress = 0;
        for (int i = 0; i < nc; i++) {
            if (done[i]) continue;
            char *sql;
            if (c_p3[i] == 0)  /* 2-arg unary solver */
                sql = sqlite3_mprintf("SELECT %s(%lld, %lld)",
                                       solve_fns[i], c_p1[i], c_p2[i]);
            else               /* 3-arg binary solver */
                sql = sqlite3_mprintf("SELECT %s(%lld, %lld, %lld)",
                                       solve_fns[i], c_p1[i], c_p2[i], c_p3[i]);
            sqlite3_stmt *stmt = NULL;
            int r = 3;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
                sqlite3_step(stmt) == SQLITE_ROW)
                r = sqlite3_column_int(stmt, 0);
            if (stmt) sqlite3_finalize(stmt);
            sqlite3_free(sql);

            if (r == 1) { progress = 1; done[i] = 1; }
            else if (r == 2) { done[i] = 1; }
            else if (r == 0) { solved = 0; break; }
            /* r == 3: underdetermined, retry */
        }
        if (!progress || !solved) break;
    }

    /* Check if all done */
    if (solved) {
        for (int i = 0; i < nc; i++)
            if (!done[i]) { solved = 0; break; }
    }

    /* Cleanup */
    for (int i = 0; i < nc; i++) sqlite3_free(solve_fns[i]);
    sqlite3_free(solve_fns);
    sqlite3_free(c_p1); sqlite3_free(c_p2); sqlite3_free(c_p3);
    sqlite3_free(done);

    sqlite3_result_int(ctx, solved ? 1 : 0);
}

/* Original SQL function registrations (in qjson_sqlite_ext.c init):
 *
 *   sqlite3_create_function(db, "qjson_solve_add", -1, SQLITE_UTF8, NULL, sql_solve_add, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_sub", -1, SQLITE_UTF8, NULL, sql_solve_sub, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_mul", -1, SQLITE_UTF8, NULL, sql_solve_mul, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_div", -1, SQLITE_UTF8, NULL, sql_solve_div, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_pow", -1, SQLITE_UTF8, NULL, sql_solve_pow, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_sqrt", -1, SQLITE_UTF8, NULL, sql_solve_sqrt, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_exp",  -1, SQLITE_UTF8, NULL, sql_solve_exp,  NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_log",  -1, SQLITE_UTF8, NULL, sql_solve_log,  NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_sin",  -1, SQLITE_UTF8, NULL, sql_solve_sin,  NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_cos",  -1, SQLITE_UTF8, NULL, sql_solve_cos,  NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_tan",  -1, SQLITE_UTF8, NULL, sql_solve_tan,  NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_asin", -1, SQLITE_UTF8, NULL, sql_solve_asin, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_acos", -1, SQLITE_UTF8, NULL, sql_solve_acos, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve_atan", -1, SQLITE_UTF8, NULL, sql_solve_atan, NULL, NULL);
 *   sqlite3_create_function(db, "qjson_solve", -1, SQLITE_UTF8, NULL, sql_qjson_solve, NULL, NULL);
 */
