/*
 * qjson_sqlite_ext.c — SQLite/SQLCipher extension for QJSON.
 *
 * Scalar functions:
 *   qjson_decimal_cmp(a, b) → INTEGER    exact decimal comparison (libbf)
 *   qjson_cmp(a_lo,a_hi,a_str, b_lo,b_hi,b_str) → INTEGER  3-tier comparison
 *   qjson_reconstruct(vid, prefix) → TEXT   value_id → canonical QJSON text
 *   qjson_round_down(raw) → REAL   largest IEEE double <= exact(raw)  (libbf)
 *   qjson_round_up(raw)   → REAL   smallest IEEE double >= exact(raw) (libbf)
 *
 * Table-valued function:
 *   SELECT result_vid, qjson FROM qjson_select(root_id, path, where_expr, prefix)
 *
 * Build:
 *   cc -shared -fPIC -DQJSON_USE_LIBBF -o qjson_ext.dylib \
 *     native/qjson_sqlite_ext.c native/qjson.c \
 *     native/libbf/libbf.c native/libbf/cutils.c -lm
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "qjson.h"

/* ── Dynamic string buffer ──────────────────────────────── */

typedef struct {
    char *buf;
    int len, cap;
} dstr;

static void dstr_init(dstr *s) { s->buf = NULL; s->len = 0; s->cap = 0; }

static void dstr_ensure(dstr *s, int need) {
    if (s->len + need + 1 > s->cap) {
        int nc = s->cap ? s->cap * 2 : 256;
        while (nc < s->len + need + 1) nc *= 2;
        s->buf = sqlite3_realloc(s->buf, nc);
        s->cap = nc;
    }
}

static void dstr_cat(dstr *s, const char *t) {
    int n = (int)strlen(t);
    dstr_ensure(s, n);
    memcpy(s->buf + s->len, t, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

static void dstr_catc(dstr *s, char c) {
    dstr_ensure(s, 1);
    s->buf[s->len++] = c;
    s->buf[s->len] = '\0';
}

static void dstr_catf(dstr *s, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        dstr_ensure(s, n);
        vsnprintf(s->buf + s->len, n + 1, fmt, ap2);
        s->len += n;
    }
    va_end(ap2);
}

static void dstr_free(dstr *s) {
    if (s->buf) sqlite3_free(s->buf);
    s->buf = NULL; s->len = 0; s->cap = 0;
}

/* ── Path parser ────────────────────────────────────────── */

enum { STEP_KEY=1, STEP_INDEX, STEP_VAR, STEP_ITER };

typedef struct {
    int type;
    char *val;   /* dynamically allocated */
    int ival;
} path_step;

static char *dstr_extract_range(const char *s, int start, int end) {
    int n = end - start;
    char *r = sqlite3_malloc(n + 1);
    memcpy(r, s + start, n);
    r[n] = '\0';
    return r;
}

static void free_steps(path_step *steps, int n) {
    for (int i = 0; i < n; i++)
        if (steps[i].val) { sqlite3_free(steps[i].val); steps[i].val = NULL; }
}

static int parse_path(const char *expr, path_step *steps, int max_steps) {
    int n = 0, pos = 0, len = (int)strlen(expr);
    while (pos < len && n < max_steps) {
        if (expr[pos] == '.') {
            pos++;
            if (pos < len && expr[pos] == '[') {
                pos++;
                if (pos < len && expr[pos] == ']') {
                    steps[n].type = STEP_ITER;
                    steps[n].val = NULL;
                    pos++; n++;
                } else {
                    int s = pos;
                    while (pos < len && expr[pos] != ']') pos++;
                    steps[n].val = dstr_extract_range(expr, s, pos);
                    if (pos < len) pos++;
                    steps[n].type = STEP_VAR;
                    n++;
                }
            } else {
                int s = pos;
                while (pos < len && (isalnum(expr[pos]) || expr[pos] == '_')) pos++;
                steps[n].val = dstr_extract_range(expr, s, pos);
                steps[n].type = STEP_KEY;
                n++;
            }
        } else if (expr[pos] == '[') {
            pos++;
            if (pos < len && expr[pos] == ']') {
                steps[n].type = STEP_ITER;
                steps[n].val = NULL;
                pos++; n++;
            } else if (pos < len && isdigit(expr[pos])) {
                int s = pos;
                while (pos < len && isdigit(expr[pos])) pos++;
                steps[n].val = dstr_extract_range(expr, s, pos);
                steps[n].ival = atoi(steps[n].val);
                if (pos < len && expr[pos] == ']') pos++;
                steps[n].type = STEP_INDEX;
                n++;
            } else {
                int s = pos;
                while (pos < len && expr[pos] != ']') pos++;
                steps[n].val = dstr_extract_range(expr, s, pos);
                if (pos < len) pos++;
                steps[n].type = STEP_VAR;
                n++;
            }
        } else {
            pos++;
        }
    }
    return n;
}

/* ── SQL builder ────────────────────────────────────────── */

typedef struct {
    char *name;
    char alias[32]; /* alias is always short: "ai_N" */
} var_binding;

typedef struct {
    const char *prefix;
    int alias_num;
    dstr joins;
    int nvar, var_cap;
    var_binding *vars;
} sql_builder;

static void sb_init(sql_builder *sb, const char *prefix) {
    sb->prefix = prefix;
    sb->alias_num = 0;
    dstr_init(&sb->joins);
    sb->nvar = 0;
    sb->var_cap = 0;
    sb->vars = NULL;
}

static void sb_free(sql_builder *sb) {
    dstr_free(&sb->joins);
    for (int i = 0; i < sb->nvar; i++)
        sqlite3_free(sb->vars[i].name);
    sqlite3_free(sb->vars);
}

static const char *sb_find_var(sql_builder *sb, const char *name) {
    for (int i = 0; i < sb->nvar; i++)
        if (strcmp(sb->vars[i].name, name) == 0) return sb->vars[i].alias;
    return NULL;
}

static void sb_add_var(sql_builder *sb, const char *name, const char *alias) {
    if (sb->nvar >= sb->var_cap) {
        sb->var_cap = sb->var_cap ? sb->var_cap * 2 : 8;
        sb->vars = sqlite3_realloc(sb->vars, sb->var_cap * sizeof(var_binding));
    }
    sb->vars[sb->nvar].name = sqlite3_mprintf("%s", name);
    snprintf(sb->vars[sb->nvar].alias, 32, "%s", alias);
    sb->nvar++;
}

/* Resolve a path, appending JOINs. Returns the final value_id expression
   in out (caller-owned dstr, appended to). */
static void sb_resolve(sql_builder *sb, path_step *steps, int nsteps,
                       const char *root_vid, dstr *out_vid)
{
    dstr cur;
    dstr_init(&cur);
    dstr_cat(&cur, root_vid);

    for (int i = 0; i < nsteps; i++) {
        sb->alias_num++;
        if (steps[i].type == STEP_KEY) {
            char o[32], oi[32];
            snprintf(o, sizeof(o), "o_%d", sb->alias_num);
            sb->alias_num++;
            snprintf(oi, sizeof(oi), "oi_%d", sb->alias_num);
            dstr_catf(&sb->joins,
                " JOIN \"%sobject\" %s ON %s.value_id = %s"
                " JOIN \"%sobject_item\" %s ON %s.object_id = %s.id"
                " AND %s.key_id IN (SELECT value_id FROM \"%sstring\" WHERE value = '%s')",
                sb->prefix, o, o, cur.buf,
                sb->prefix, oi, oi, o,
                oi, sb->prefix, steps[i].val);
            cur.len = 0; cur.buf[0] = '\0';
            dstr_catf(&cur, "%s.value_id", oi);

        } else if (steps[i].type == STEP_INDEX) {
            char a[32], ai[32];
            snprintf(a, sizeof(a), "a_%d", sb->alias_num);
            sb->alias_num++;
            snprintf(ai, sizeof(ai), "ai_%d", sb->alias_num);
            dstr_catf(&sb->joins,
                " JOIN \"%sarray\" %s ON %s.value_id = %s"
                " JOIN \"%sarray_item\" %s ON %s.array_id = %s.id AND %s.idx = %d",
                sb->prefix, a, a, cur.buf,
                sb->prefix, ai, ai, a, ai, steps[i].ival);
            cur.len = 0; cur.buf[0] = '\0';
            dstr_catf(&cur, "%s.value_id", ai);

        } else if (steps[i].type == STEP_VAR) {
            const char *existing = sb_find_var(sb, steps[i].val);
            if (existing) {
                cur.len = 0; cur.buf[0] = '\0';
                dstr_catf(&cur, "%s.dst_vid", existing);
            } else {
                char k[32];
                snprintf(k, sizeof(k), "k_%d", sb->alias_num);
                sb->alias_num++;
                dstr_catf(&sb->joins,
                    " JOIN ("
                    "SELECT a.value_id AS src_vid, ai.value_id AS dst_vid"
                    " FROM \"%sarray\" a JOIN \"%sarray_item\" ai ON ai.array_id = a.id"
                    " UNION ALL"
                    " SELECT o.value_id AS src_vid, oi.key_id AS dst_vid"
                    " FROM \"%sobject\" o JOIN \"%sobject_item\" oi ON oi.object_id = o.id"
                    ") %s ON %s.src_vid = %s",
                    sb->prefix, sb->prefix,
                    sb->prefix, sb->prefix,
                    k, k, cur.buf);
                sb_add_var(sb, steps[i].val, k);
                cur.len = 0; cur.buf[0] = '\0';
                dstr_catf(&cur, "%s.dst_vid", k);
            }

        } else if (steps[i].type == STEP_ITER) {
            char a[32], ai[32];
            snprintf(a, sizeof(a), "a_%d", sb->alias_num);
            sb->alias_num++;
            snprintf(ai, sizeof(ai), "ai_%d", sb->alias_num);
            dstr_catf(&sb->joins,
                " JOIN \"%sarray\" %s ON %s.value_id = %s"
                " JOIN \"%sarray_item\" %s ON %s.array_id = %s.id",
                sb->prefix, a, a, cur.buf,
                sb->prefix, ai, ai, a);
            cur.len = 0; cur.buf[0] = '\0';
            dstr_catf(&cur, "%s.value_id", ai);
        }
    }
    /* Copy result to out_vid */
    out_vid->len = 0;
    if (out_vid->buf) out_vid->buf[0] = '\0';
    dstr_cat(out_vid, cur.buf);
    dstr_free(&cur);
}

/* ── WHERE compiler ─────────────────────────────────────── */

/* Compile a WHERE expression by resolving each path and emitting SQL.
   This handles: path op value, AND, OR, NOT, parentheses.
   For simplicity, numeric comparisons use qjson_cmp (already registered). */
static void sb_compile_where(sql_builder *sb, const char *expr, dstr *out) {
    int pos = 0, len = (int)strlen(expr);

    while (pos < len) {
        /* skip whitespace */
        while (pos < len && isspace(expr[pos])) pos++;
        if (pos >= len) break;

        /* parentheses */
        if (expr[pos] == '(' || expr[pos] == ')') {
            dstr_catc(out, ' ');
            dstr_catc(out, expr[pos++]);
            continue;
        }

        /* AND / OR / NOT */
        if (pos + 3 <= len && strncmp(expr + pos, "AND", 3) == 0 &&
            (pos + 3 >= len || !isalpha(expr[pos+3]))) {
            dstr_cat(out, " AND"); pos += 3; continue;
        }
        if (pos + 2 <= len && strncmp(expr + pos, "OR", 2) == 0 &&
            (pos + 2 >= len || !isalpha(expr[pos+2]))) {
            dstr_cat(out, " OR"); pos += 2; continue;
        }
        if (pos + 3 <= len && strncmp(expr + pos, "NOT", 3) == 0 &&
            (pos + 3 >= len || !isalpha(expr[pos+3]))) {
            dstr_cat(out, " NOT"); pos += 3; continue;
        }

        /* path expression (starts with . or [) */
        if (expr[pos] == '.' || expr[pos] == '[') {
            /* collect path */
            int pstart = pos;
            while (pos < len && (expr[pos] == '.' || expr[pos] == '[' ||
                   isalnum(expr[pos]) || expr[pos] == '_' || expr[pos] == ']'))
                pos++;
            char *path_str = dstr_extract_range(expr, pstart, pos);

            /* skip whitespace */
            while (pos < len && isspace(expr[pos])) pos++;

            /* operator (always short: ==, !=, <, >, <=, >=) */
            char op[4] = {0};
            int oi = 0;
            while (pos < len && (expr[pos] == '=' || expr[pos] == '!' ||
                   expr[pos] == '<' || expr[pos] == '>') && oi < 3)
                op[oi++] = expr[pos++];
            op[oi] = '\0';

            /* skip whitespace */
            while (pos < len && isspace(expr[pos])) pos++;

            /* resolve LHS path */
            path_step wsteps[32];
            int nwsteps = parse_path(path_str, wsteps, 32);
            dstr wvid; dstr_init(&wvid);
            sb_resolve(sb, wsteps, nwsteps, "root.id", &wvid);
            free_steps(wsteps, nwsteps);
            sqlite3_free(path_str);

            /* RHS: path or literal? */
            if (expr[pos] == '.' || expr[pos] == '[') {
                /* ── Cross-path comparison: path op path ── */
                int rstart = pos;
                while (pos < len && (expr[pos] == '.' || expr[pos] == '[' ||
                       isalnum(expr[pos]) || expr[pos] == '_' || expr[pos] == ']'))
                    pos++;
                char *rhs_path = dstr_extract_range(expr, rstart, pos);

                path_step rsteps[32];
                int nrsteps = parse_path(rhs_path, rsteps, 32);
                dstr rvid; dstr_init(&rvid);
                sb_resolve(sb, rsteps, nrsteps, "root.id", &rvid);
                free_steps(rsteps, nrsteps);
                sqlite3_free(rhs_path);

                /* JOIN both sides to value + LEFT JOIN number + LEFT JOIN string */
                char lv[32], rv[32], ln[32], rn[32], ls[32], rs[32];
                sb->alias_num++; snprintf(lv, sizeof(lv), "clv_%d", sb->alias_num);
                sb->alias_num++; snprintf(rv, sizeof(rv), "crv_%d", sb->alias_num);
                sb->alias_num++; snprintf(ln, sizeof(ln), "cln_%d", sb->alias_num);
                sb->alias_num++; snprintf(rn, sizeof(rn), "crn_%d", sb->alias_num);
                sb->alias_num++; snprintf(ls, sizeof(ls), "cls_%d", sb->alias_num);
                sb->alias_num++; snprintf(rs, sizeof(rs), "crs_%d", sb->alias_num);

                dstr_catf(&sb->joins,
                    " JOIN \"%svalue\" %s ON %s.id = %s"
                    " JOIN \"%svalue\" %s ON %s.id = %s"
                    " LEFT JOIN \"%snumber\" %s ON %s.value_id = %s"
                    " LEFT JOIN \"%snumber\" %s ON %s.value_id = %s"
                    " LEFT JOIN \"%sstring\" %s ON %s.value_id = %s"
                    " LEFT JOIN \"%sstring\" %s ON %s.value_id = %s",
                    sb->prefix, lv, lv, wvid.buf,
                    sb->prefix, rv, rv, rvid.buf,
                    sb->prefix, ln, ln, wvid.buf,
                    sb->prefix, rn, rn, rvid.buf,
                    sb->prefix, ls, ls, wvid.buf,
                    sb->prefix, rs, rs, rvid.buf);

                /* Emit type-dispatched comparison */
                if (strcmp(op, "==") == 0) {
                    dstr_catf(out,
                        " (%s.type = %s.type AND ("
                        "(%s.type IN ('number','bigint','bigdec','bigfloat','unbound')"
                        " AND qjson_decimal_cmp("
                          "COALESCE(%s.str, CAST(%s.lo AS TEXT)),"
                          "COALESCE(%s.str, CAST(%s.lo AS TEXT))) = 0)"
                        " OR (%s.type = 'string' AND %s.value = %s.value)"
                        " OR (%s.type IN ('null','true','false'))"
                        "))",
                        lv, rv,
                        lv, ln, ln, rn, rn,
                        lv, ls, rs,
                        lv);
                } else if (strcmp(op, "!=") == 0) {
                    dstr_catf(out,
                        " (%s.type != %s.type OR ("
                        "(%s.type IN ('number','bigint','bigdec','bigfloat','unbound')"
                        " AND qjson_decimal_cmp("
                          "COALESCE(%s.str, CAST(%s.lo AS TEXT)),"
                          "COALESCE(%s.str, CAST(%s.lo AS TEXT))) != 0)"
                        " OR (%s.type = 'string' AND %s.value != %s.value)"
                        "))",
                        lv, rv,
                        lv, ln, ln, rn, rn,
                        lv, ls, rs);
                } else {
                    /* Ordering: numeric types only, use qjson_decimal_cmp */
                    const char *cmp_op;
                    if (strcmp(op, ">") == 0)       cmp_op = "> 0";
                    else if (strcmp(op, ">=") == 0)  cmp_op = ">= 0";
                    else if (strcmp(op, "<") == 0)   cmp_op = "< 0";
                    else                            cmp_op = "<= 0";
                    dstr_catf(out,
                        " (%s.type IN ('number','bigint','bigdec','bigfloat','unbound')"
                        " AND %s.type IN ('number','bigint','bigdec','bigfloat','unbound')"
                        " AND qjson_decimal_cmp("
                          "COALESCE(%s.str, CAST(%s.lo AS TEXT)),"
                          "COALESCE(%s.str, CAST(%s.lo AS TEXT))) %s)",
                        lv, rv, ln, ln, rn, rn, cmp_op);
                }

                dstr_free(&rvid);
                dstr_free(&wvid);
                continue;
            }

            /* ── Path vs literal ── */
            dstr val; dstr_init(&val);
            char val_type = 'n';
            if (expr[pos] == '"') {
                pos++;
                while (pos < len && expr[pos] != '"') {
                    if (expr[pos] == '\\') pos++;
                    dstr_catc(&val, expr[pos++]);
                }
                if (pos < len) pos++;
                val_type = 's';
            } else if (pos + 4 <= len && strncmp(expr + pos, "true", 4) == 0 &&
                       (pos + 4 >= len || !isalpha(expr[pos+4]))) {
                dstr_cat(&val, "true"); val_type = 'l'; pos += 4;
            } else if (pos + 5 <= len && strncmp(expr + pos, "false", 5) == 0 &&
                       (pos + 5 >= len || !isalpha(expr[pos+5]))) {
                dstr_cat(&val, "false"); val_type = 'l'; pos += 5;
            } else if (pos + 4 <= len && strncmp(expr + pos, "null", 4) == 0 &&
                       (pos + 4 >= len || !isalpha(expr[pos+4]))) {
                dstr_cat(&val, "null"); val_type = 'l'; pos += 4;
            } else {
                while (pos < len && (isdigit(expr[pos]) || expr[pos] == '.' ||
                       expr[pos] == '-' || expr[pos] == '+' || expr[pos] == 'e' ||
                       expr[pos] == 'E' || expr[pos] == 'N' || expr[pos] == 'M' ||
                       expr[pos] == 'L' || expr[pos] == 'n' || expr[pos] == 'm' ||
                       expr[pos] == 'l'))
                    dstr_catc(&val, expr[pos++]);
                val_type = 'n';
            }

            /* emit comparison */
            if (val_type == 'l') {
                sb->alias_num++;
                char va[32]; snprintf(va, sizeof(va), "wv_%d", sb->alias_num);
                dstr_catf(&sb->joins, " JOIN \"%svalue\" %s ON %s.id = %s",
                          sb->prefix, va, va, wvid.buf);
                if (strcmp(op, "==") == 0)
                    dstr_catf(out, " %s.type = '%s'", va, val.buf);
                else
                    dstr_catf(out, " %s.type != '%s'", va, val.buf);
            } else if (val_type == 's') {
                sb->alias_num++;
                char sva[32]; snprintf(sva, sizeof(sva), "wsv_%d", sb->alias_num);
                dstr_catf(&sb->joins, " JOIN \"%sstring\" %s ON %s.value_id = %s",
                          sb->prefix, sva, sva, wvid.buf);
                if (strcmp(op, "==") == 0)
                    dstr_catf(out, " %s.value = '%s'", sva, val.buf);
                else
                    dstr_catf(out, " %s.value != '%s'", sva, val.buf);
            } else {
                /* numeric: strip suffix, determine query type, project interval */
                dstr raw; dstr_init(&raw);
                dstr_cat(&raw, val.buf);
                int q_type = QJSON_NUM; /* default: plain number */
                if (raw.len > 0) {
                    char last = raw.buf[raw.len - 1];
                    if (last == 'N' || last == 'n') { q_type = QJSON_BIGINT; raw.buf[--raw.len] = '\0'; }
                    else if (last == 'M' || last == 'm') { q_type = QJSON_BIGDEC; raw.buf[--raw.len] = '\0'; }
                    else if (last == 'L' || last == 'l') { q_type = QJSON_BIGFLOAT; raw.buf[--raw.len] = '\0'; }
                }

                double q_lo, q_hi;
                qjson_project(raw.buf, raw.len, &q_lo, &q_hi);
                int exact = (q_lo == q_hi);

                sb->alias_num++;
                char na[32]; snprintf(na, sizeof(na), "wn_%d", sb->alias_num);
                dstr_catf(&sb->joins, " JOIN \"%snumber\" %s ON %s.value_id = %s",
                          sb->prefix, na, na, wvid.buf);

                /* Also join value table for stored type (needed by qjson_cmp) */
                sb->alias_num++;
                char nva[32]; snprintf(nva, sizeof(nva), "wnv_%d", sb->alias_num);
                dstr_catf(&sb->joins, " JOIN \"%svalue\" %s ON %s.id = %s.value_id",
                          sb->prefix, nva, nva, na);

                if (strcmp(op, "==") == 0) {
                    if (exact)
                        dstr_catf(out, " (%s.lo = %.17g AND %s.hi = %.17g AND %s.str IS NULL)",
                                  na, q_lo, na, q_hi, na);
                    else
                        dstr_catf(out, " (%s.lo = %.17g AND %s.hi = %.17g AND %s.str = '%s')",
                                  na, q_lo, na, q_hi, na, raw.buf);
                } else if (strcmp(op, "!=") == 0) {
                    if (exact)
                        dstr_catf(out, " NOT (%s.lo = %.17g AND %s.hi = %.17g AND %s.str IS NULL)",
                                  na, q_lo, na, q_hi, na);
                    else
                        dstr_catf(out, " NOT (%s.lo = %.17g AND %s.hi = %.17g AND %s.str = '%s')",
                                  na, q_lo, na, q_hi, na, raw.buf);
                } else {
                    /* Ordering: bracket pre-filter + qjson_cmp_[op] exact fallback */
                    dstr q_str_sql; dstr_init(&q_str_sql);
                    if (exact) dstr_cat(&q_str_sql, "NULL");
                    else       dstr_catf(&q_str_sql, "'%s'", raw.buf);

                    /* Type expression for stored value */
                    dstr stype; dstr_init(&stype);
                    dstr_catf(&stype, "CASE "
                        "WHEN %s.type='number' THEN 3 "
                        "WHEN %s.type='bigint' THEN 4 "
                        "WHEN %s.type='bigdec' THEN 5 "
                        "WHEN %s.type='bigfloat' THEN 6 "
                        "WHEN %s.type='unbound' THEN 11 "
                        "ELSE 3 END", nva, nva, nva, nva, nva);

                    const char *fn_name;
                    const char *bracket_op;
                    if (strcmp(op, ">") == 0)       { fn_name = "qjson_cmp_gt"; bracket_op = "%s.hi > %.17g"; }
                    else if (strcmp(op, ">=") == 0)  { fn_name = "qjson_cmp_ge"; bracket_op = "%s.hi >= %.17g"; }
                    else if (strcmp(op, "<") == 0)   { fn_name = "qjson_cmp_lt"; bracket_op = "%s.lo < %.17g"; }
                    else                            { fn_name = "qjson_cmp_le"; bracket_op = "%s.lo <= %.17g"; }

                    double bracket_val = (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0) ? q_hi : q_lo;
                    dstr_catf(out, " (");
                    dstr_catf(out, bracket_op, na, bracket_val);
                    dstr_catf(out, " AND %s(%s, %s.lo, %s.str, %s.hi, %d, %.17g, %s, %.17g) = 1)",
                              fn_name, stype.buf, na, na, na, q_type, q_lo, q_str_sql.buf, q_hi);

                    dstr_free(&q_str_sql);
                    dstr_free(&stype);
                }
                dstr_free(&raw);
            }
            dstr_free(&val);
            dstr_free(&wvid);
            continue;
        }

        pos++; /* skip unknown */
    }
}

/* ── qjson_reconstruct ──────────────────────────────────── */

static char *do_reconstruct(sqlite3 *db, sqlite3_int64 vid, const char *prefix) {
    dstr out;
    dstr_init(&out);

    /* Get type */
    char *sql = sqlite3_mprintf("SELECT type FROM \"%svalue\" WHERE id = %lld", prefix, vid);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_free(sql);
        if (stmt) sqlite3_finalize(stmt);
        dstr_cat(&out, "null");
        return out.buf;
    }
    const char *type = (const char *)sqlite3_column_text(stmt, 0);
    char tstr[16]; strncpy(tstr, type ? type : "", 15); tstr[15] = '\0';
    sqlite3_finalize(stmt);
    sqlite3_free(sql);
    sql = NULL;

    if (strcmp(tstr, "null") == 0)  { dstr_cat(&out, "null"); return out.buf; }
    if (strcmp(tstr, "true") == 0)  { dstr_cat(&out, "true"); return out.buf; }
    if (strcmp(tstr, "false") == 0) { dstr_cat(&out, "false"); return out.buf; }

    /* Number types */
    if (strcmp(tstr, "number") == 0 || strcmp(tstr, "bigint") == 0 ||
        strcmp(tstr, "bigdec") == 0 || strcmp(tstr, "bigfloat") == 0) {
        sql = sqlite3_mprintf("SELECT lo, str, hi FROM \"%snumber\" WHERE value_id = %lld", prefix, vid);
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            double lo = sqlite3_column_double(stmt, 0);
            const char *str = (const char *)sqlite3_column_text(stmt, 1);
            if (strcmp(tstr, "number") == 0) {
                char tmp[64]; snprintf(tmp, sizeof(tmp), "%.17g", lo);
                /* Trim trailing zeros after decimal point */
                char *dot = strchr(tmp, '.');
                if (dot) {
                    char *end = tmp + strlen(tmp) - 1;
                    while (end > dot && *end == '0') *end-- = '\0';
                    if (end == dot) *end = '\0';
                }
                dstr_cat(&out, tmp);
            } else {
                const char *raw;
                char tmp[64];
                if (str) {
                    raw = str;
                } else if (strcmp(tstr, "bigint") == 0) {
                    snprintf(tmp, sizeof(tmp), "%lld", (long long)lo);
                    raw = tmp;
                } else {
                    snprintf(tmp, sizeof(tmp), "%.17g", lo);
                    char *dot = strchr(tmp, '.');
                    if (dot) {
                        char *end = tmp + strlen(tmp) - 1;
                        while (end > dot && *end == '0') *end-- = '\0';
                        if (end == dot) *end = '\0';
                    }
                    raw = tmp;
                }
                dstr_cat(&out, raw);
                if (strcmp(tstr, "bigint") == 0) dstr_catc(&out, 'N');
                else if (strcmp(tstr, "bigdec") == 0) dstr_catc(&out, 'M');
                else dstr_catc(&out, 'L');
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_free(sql);
        return out.buf;
    }

    /* Unbound */
    if (strcmp(tstr, "unbound") == 0) {
        sql = sqlite3_mprintf("SELECT str FROM \"%snumber\" WHERE value_id = %lld", prefix, vid);
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(stmt, 0);
            dstr_catc(&out, '?');
            if (name) {
                /* Check if name needs quoting */
                int needs_quote = 0;
                if (name[0] && !((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z') || name[0] == '_'))
                    needs_quote = 1;
                if (!needs_quote) {
                    for (int i = 1; name[i]; i++) {
                        char ci = name[i];
                        if (!((ci >= 'a' && ci <= 'z') || (ci >= 'A' && ci <= 'Z') ||
                              (ci >= '0' && ci <= '9') || ci == '_'))
                            { needs_quote = 1; break; }
                    }
                }
                if (needs_quote) {
                    dstr_catc(&out, '"');
                    for (int i = 0; name[i]; i++) {
                        if (name[i] == '"') dstr_cat(&out, "\\\"");
                        else if (name[i] == '\\') dstr_cat(&out, "\\\\");
                        else dstr_catc(&out, name[i]);
                    }
                    dstr_catc(&out, '"');
                } else {
                    dstr_cat(&out, name);
                }
            } else {
                dstr_catc(&out, '_');
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_free(sql);
        return out.buf;
    }

    /* String */
    if (strcmp(tstr, "string") == 0) {
        sql = sqlite3_mprintf("SELECT value FROM \"%sstring\" WHERE value_id = %lld", prefix, vid);
        dstr_catc(&out, '"');
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            const char *sv = (const char *)sqlite3_column_text(stmt, 0);
            if (sv) for (int i = 0; sv[i]; i++) {
                if (sv[i] == '"') dstr_cat(&out, "\\\"");
                else if (sv[i] == '\\') dstr_cat(&out, "\\\\");
                else if (sv[i] == '\n') dstr_cat(&out, "\\n");
                else if (sv[i] == '\r') dstr_cat(&out, "\\r");
                else if (sv[i] == '\t') dstr_cat(&out, "\\t");
                else if ((unsigned char)sv[i] < 0x20) {
                    char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)sv[i]);
                    dstr_cat(&out, esc);
                } else dstr_catc(&out, sv[i]);
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_free(sql);
        dstr_catc(&out, '"');
        return out.buf;
    }

    /* Array */
    if (strcmp(tstr, "array") == 0) {
        sqlite3_free(sql);
        sql = sqlite3_mprintf("SELECT id FROM \"%sarray\" WHERE value_id = %lld", prefix, vid);
        sqlite3_int64 arr_id = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
            arr_id = sqlite3_column_int64(stmt, 0);
        if (stmt) sqlite3_finalize(stmt);
        if (!arr_id) { sqlite3_free(sql); dstr_cat(&out, "[]"); return out.buf; }

        dstr_catc(&out, '[');
        sqlite3_free(sql);
        sql = sqlite3_mprintf("SELECT value_id FROM \"%sarray_item\" WHERE array_id = %lld ORDER BY idx", prefix, arr_id);
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            int first = 1;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) dstr_catc(&out, ',');
                first = 0;
                sqlite3_int64 cvid = sqlite3_column_int64(stmt, 0);
                char *child = do_reconstruct(db, cvid, prefix);
                dstr_cat(&out, child);
                sqlite3_free(child);
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_free(sql);
        dstr_catc(&out, ']');
        return out.buf;
    }

    /* Object */
    if (strcmp(tstr, "object") == 0) {
        sqlite3_free(sql);
        sql = sqlite3_mprintf("SELECT id FROM \"%sobject\" WHERE value_id = %lld", prefix, vid);
        sqlite3_int64 obj_id = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
            obj_id = sqlite3_column_int64(stmt, 0);
        if (stmt) sqlite3_finalize(stmt);
        if (!obj_id) { sqlite3_free(sql); dstr_cat(&out, "{}"); return out.buf; }

        dstr_catc(&out, '{');
        sqlite3_free(sql);
        sql = sqlite3_mprintf("SELECT key_id, value_id FROM \"%sobject_item\" WHERE object_id = %lld", prefix, obj_id);
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            int first = 1;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) dstr_catc(&out, ',');
                first = 0;
                sqlite3_int64 kvid = sqlite3_column_int64(stmt, 0);
                sqlite3_int64 cvid = sqlite3_column_int64(stmt, 1);
                char *key_str = do_reconstruct(db, kvid, prefix);
                dstr_cat(&out, key_str);
                sqlite3_free(key_str);
                dstr_catc(&out, ':');
                char *child = do_reconstruct(db, cvid, prefix);
                dstr_cat(&out, child);
                sqlite3_free(child);
            }
        }
        if (stmt) sqlite3_finalize(stmt);
        sqlite3_free(sql);
        dstr_catc(&out, '}');
        return out.buf;
    }

    dstr_cat(&out, "null");
    return out.buf;
}

static void sql_qjson_reconstruct(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx); return;
    }
    sqlite3_int64 vid = sqlite3_value_int64(argv[0]);
    const char *prefix = argc > 1 && sqlite3_value_type(argv[1]) != SQLITE_NULL
        ? (const char *)sqlite3_value_text(argv[1]) : "qjson_";
    sqlite3 *db = sqlite3_context_db_handle(ctx);
    char *result = do_reconstruct(db, vid, prefix);
    sqlite3_result_text(ctx, result, -1, sqlite3_free);
}

/* ── qjson_select table-valued function ─────────────────── */

typedef struct {
    sqlite3_vtab base;
    sqlite3 *db;
} qs_vtab;

typedef struct {
    sqlite3_vtab_cursor base;
    int row, nrows;
    sqlite3_int64 *vids;
    char **qjsons;
} qs_cursor;

static int qs_connect(sqlite3 *db, void *pAux, int argc, const char *const*argv,
                      sqlite3_vtab **ppVtab, char **pzErr) {
    int rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(result_vid INTEGER, qjson TEXT,"
        " root_id INTEGER HIDDEN, select_path TEXT HIDDEN,"
        " where_expr TEXT HIDDEN, prefix TEXT HIDDEN)");
    if (rc != SQLITE_OK) return rc;
    qs_vtab *v = sqlite3_malloc(sizeof(qs_vtab));
    memset(v, 0, sizeof(*v));
    v->db = db;
    *ppVtab = &v->base;
    return SQLITE_OK;
}

static int qs_disconnect(sqlite3_vtab *pVtab) {
    sqlite3_free(pVtab); return SQLITE_OK;
}

static int qs_bestindex(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo) {
    /* Map hidden columns 2-5 (root_id, select_path, where_expr, prefix)
       to sequential argv indices. Store column mapping in idxNum as bitmask. */
    int argv_idx = 1;
    int idxNum = 0;
    /* First pass: find which hidden columns have EQ constraints */
    int constraint_for_col[6] = {-1,-1,-1,-1,-1,-1};
    for (int i = 0; i < pInfo->nConstraint; i++) {
        if (!pInfo->aConstraint[i].usable) continue;
        if (pInfo->aConstraint[i].op != SQLITE_INDEX_CONSTRAINT_EQ) continue;
        int col = pInfo->aConstraint[i].iColumn;
        if (col >= 2 && col <= 5)
            constraint_for_col[col] = i;
    }
    /* Assign sequential argvIndex for cols 2,3,4,5 in order */
    for (int col = 2; col <= 5; col++) {
        int ci = constraint_for_col[col];
        if (ci >= 0) {
            pInfo->aConstraintUsage[ci].argvIndex = argv_idx++;
            pInfo->aConstraintUsage[ci].omit = 1;
            idxNum |= (1 << col);
        }
    }
    pInfo->idxNum = idxNum;
    pInfo->estimatedCost = 1000.0;
    return SQLITE_OK;
}

static int qs_open(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCursor) {
    qs_cursor *c = sqlite3_malloc(sizeof(qs_cursor));
    memset(c, 0, sizeof(*c));
    *ppCursor = &c->base;
    return SQLITE_OK;
}

static int qs_close(sqlite3_vtab_cursor *cur) {
    qs_cursor *c = (qs_cursor *)cur;
    for (int i = 0; i < c->nrows; i++)
        if (c->qjsons && c->qjsons[i]) sqlite3_free(c->qjsons[i]);
    if (c->vids) sqlite3_free(c->vids);
    if (c->qjsons) sqlite3_free(c->qjsons);
    sqlite3_free(c);
    return SQLITE_OK;
}

static int qs_filter(sqlite3_vtab_cursor *cur, int idxNum,
                     const char *idxStr, int argc, sqlite3_value **argv) {
    qs_cursor *c = (qs_cursor *)cur;
    qs_vtab *v = (qs_vtab *)cur->pVtab;
    c->row = 0; c->nrows = 0;

    /* Extract arguments in order matching xBestIndex: col 2,3,4,5 */
    sqlite3_int64 root_id = 0;
    const char *select_path = NULL, *where_expr = NULL, *prefix = "qjson_";
    int ai = 0;
    if (idxNum & (1<<2)) root_id = sqlite3_value_int64(argv[ai++]);
    if (idxNum & (1<<3)) select_path = (const char *)sqlite3_value_text(argv[ai++]);
    if (idxNum & (1<<4)) where_expr = (const char *)sqlite3_value_text(argv[ai++]);
    if (idxNum & (1<<5)) { const char *p = (const char *)sqlite3_value_text(argv[ai++]); if (p) prefix = p; }
    if (!select_path || !root_id) return SQLITE_OK;
    if (!prefix) prefix = "qjson_";

    /* Build SQL */
    sql_builder sb;
    sb_init(&sb, prefix);

    path_step steps[32];
    int nsteps = parse_path(select_path, steps, 32);
    dstr select_vid; dstr_init(&select_vid);
    sb_resolve(&sb, steps, nsteps, "root.id", &select_vid);
    free_steps(steps, nsteps);

    dstr where_sql;
    dstr_init(&where_sql);
    if (where_expr && strlen(where_expr) > 0)
        sb_compile_where(&sb, where_expr, &where_sql);

    dstr query;
    dstr_init(&query);
    dstr_catf(&query, "SELECT %s AS result_vid FROM \"%svalue\" root %s WHERE root.id = %lld",
              select_vid.buf, prefix, sb.joins.buf ? sb.joins.buf : "", root_id);
    if (where_sql.len > 0)
        dstr_catf(&query, " AND (%s)", where_sql.buf);

    /* Execute */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(v->db, query.buf, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        /* Collect results */
        int cap = 64;
        c->vids = sqlite3_malloc(cap * sizeof(sqlite3_int64));
        c->qjsons = sqlite3_malloc(cap * sizeof(char *));
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (c->nrows >= cap) {
                cap *= 2;
                c->vids = sqlite3_realloc(c->vids, cap * sizeof(sqlite3_int64));
                c->qjsons = sqlite3_realloc(c->qjsons, cap * sizeof(char *));
            }
            c->vids[c->nrows] = sqlite3_column_int64(stmt, 0);
            c->qjsons[c->nrows] = do_reconstruct(v->db, c->vids[c->nrows], prefix);
            c->nrows++;
        }
        sqlite3_finalize(stmt);
    }

    dstr_free(&query);
    dstr_free(&where_sql);
    dstr_free(&select_vid);
    sb_free(&sb);
    return SQLITE_OK;
}

static int qs_next(sqlite3_vtab_cursor *cur) {
    ((qs_cursor *)cur)->row++;
    return SQLITE_OK;
}

static int qs_eof(sqlite3_vtab_cursor *cur) {
    qs_cursor *c = (qs_cursor *)cur;
    return c->row >= c->nrows;
}

static int qs_column(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int col) {
    qs_cursor *c = (qs_cursor *)cur;
    if (col == 0) sqlite3_result_int64(ctx, c->vids[c->row]);
    else if (col == 1) sqlite3_result_text(ctx, c->qjsons[c->row], -1, SQLITE_TRANSIENT);
    return SQLITE_OK;
}

static int qs_rowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *pRowid) {
    *pRowid = ((qs_cursor *)cur)->row;
    return SQLITE_OK;
}

static sqlite3_module qs_module = {
    0,              /* iVersion */
    0,              /* xCreate (eponymous-only: NULL) */
    qs_connect,     /* xConnect */
    qs_bestindex,   /* xBestIndex */
    qs_disconnect,  /* xDisconnect */
    0,              /* xDestroy */
    qs_open,        /* xOpen */
    qs_close,       /* xClose */
    qs_filter,      /* xFilter */
    qs_next,        /* xNext */
    qs_eof,         /* xEof */
    qs_column,      /* xColumn */
    qs_rowid,       /* xRowid */
    0,0,0,0,0,0,0,0,0 /* remaining methods: NULL */
};

/* ── Scalar comparison functions ────────────────────────── */

static void sql_qjson_decimal_cmp(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL) {
        sqlite3_result_null(ctx); return;
    }
    const char *a = (const char *)sqlite3_value_text(argv[0]);
    int a_len = sqlite3_value_bytes(argv[0]);
    const char *b = (const char *)sqlite3_value_text(argv[1]);
    int b_len = sqlite3_value_bytes(argv[1]);
    sqlite3_result_int(ctx, qjson_decimal_cmp(a, a_len, b, b_len));
}

/* Generic wrapper: extract 8 args and call a qjson_cmp_[op] function.
   SQL signature: qjson_cmp_XX(a_type, a_lo, a_str, a_hi, b_type, b_lo, b_str, b_hi) → 0/1 */
typedef int (*cmp_op_fn)(QJSON_CMP_ARGS);

static void sql_qjson_cmp_dispatch(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    cmp_op_fn fn = (cmp_op_fn)sqlite3_user_data(ctx);
    int a_type = sqlite3_value_int(argv[0]);
    double a_lo = sqlite3_value_double(argv[1]);
    const char *a_str = (const char *)sqlite3_value_text(argv[2]);
    int a_str_len = a_str ? sqlite3_value_bytes(argv[2]) : 0;
    double a_hi = sqlite3_value_double(argv[3]);
    int b_type = sqlite3_value_int(argv[4]);
    double b_lo = sqlite3_value_double(argv[5]);
    const char *b_str = (const char *)sqlite3_value_text(argv[6]);
    int b_str_len = b_str ? sqlite3_value_bytes(argv[6]) : 0;
    double b_hi = sqlite3_value_double(argv[7]);
    sqlite3_result_int(ctx, fn(a_type, a_lo, a_str, a_str_len, a_hi,
                                b_type, b_lo, b_str, b_str_len, b_hi));
}

/* ── Exact arithmetic via libbf ─────────────────────────── */
/*
 * All arithmetic functions take and return TEXT (decimal strings).
 * Precision is 113 bits (~34 decimal digits) by default — IEEE 754
 * quad precision.  Division and transcendentals accept an optional
 * last argument for custom precision in decimal digits.
 *
 * qjson_add(a, b)          qjson_neg(a)
 * qjson_sub(a, b)          qjson_abs(a)
 * qjson_mul(a, b)          qjson_sqrt(a [, prec])
 * qjson_div(a, b [, prec]) qjson_exp(a [, prec])
 * qjson_pow(a, b [, prec]) qjson_log(a [, prec])
 * qjson_sin(a [, prec])    qjson_cos(a [, prec])
 * qjson_tan(a [, prec])    qjson_atan(a [, prec])
 * qjson_asin(a [, prec])   qjson_acos(a [, prec])
 * qjson_pi([prec])
 */

#ifdef QJSON_USE_LIBBF
#include "libbf.h"

static void *_math_realloc(void *opaque, void *ptr, size_t size) {
    (void)opaque;
    if (size == 0) { sqlite3_free(ptr); return NULL; }
    return sqlite3_realloc(ptr, (int)size);
}

/* Default precision: 113 bits ≈ 34 decimal digits (IEEE 754 quad) */
#define QJSON_DEFAULT_PREC 113

static limb_t _get_prec(sqlite3_value *arg) {
    if (!arg || sqlite3_value_type(arg) == SQLITE_NULL) return QJSON_DEFAULT_PREC;
    int digits = sqlite3_value_int(arg);
    if (digits <= 0) digits = 34;
    /* ~3.32 bits per decimal digit */
    return (limb_t)(digits * 3.32193 + 10);
}

static int _bf_parse(bf_context_t *ctx, bf_t *v, sqlite3_value *arg) {
    if (sqlite3_value_type(arg) == SQLITE_NULL) return -1;
    const char *s = (const char *)sqlite3_value_text(arg);
    if (!s) return -1;
    bf_init(ctx, v);
    bf_atof(v, s, NULL, 10, BF_PREC_INF, BF_RNDN);
    return 0;
}

static void _bf_result_prec(sqlite3_context *ctx, bf_t *v, bf_context_t *bfctx, limb_t prec) {
    size_t len;
    /* Convert binary precision (bits) to decimal digits: bits / log2(10) ≈ bits / 3.32 */
    limb_t digits = (prec > 10) ? (limb_t)(prec / 3.32) + 2 : 36;
    char *s = bf_ftoa(&len, v, 10, digits, BF_RNDN | BF_FTOA_FORMAT_FREE_MIN);
    if (s) {
        sqlite3_result_text(ctx, s, (int)len, SQLITE_TRANSIENT);
        bf_free(bfctx, s);
    } else {
        sqlite3_result_null(ctx);
    }
}

/* Binary operations: add, sub, mul, div, pow */
typedef int (*bf_binop)(bf_t *, const bf_t *, const bf_t *, limb_t, bf_flags_t);

static void _sql_bf_binop(sqlite3_context *ctx, int argc, sqlite3_value **argv, bf_binop op) {
    bf_context_t bfctx;
    bf_context_init(&bfctx, _math_realloc, NULL);
    bf_t a, b, r;
    if (_bf_parse(&bfctx, &a, argv[0]) || _bf_parse(&bfctx, &b, argv[1])) {
        sqlite3_result_null(ctx);
        bf_context_end(&bfctx);
        return;
    }
    bf_init(&bfctx, &r);
    limb_t prec = (argc > 2) ? _get_prec(argv[2]) : QJSON_DEFAULT_PREC;
    op(&r, &a, &b, prec, BF_RNDN);
    _bf_result_prec(ctx, &r, &bfctx, prec);
    bf_delete(&a); bf_delete(&b); bf_delete(&r);
    bf_context_end(&bfctx);
}

static void sql_qjson_add(sqlite3_context *c, int n, sqlite3_value **v) { _sql_bf_binop(c, n, v, bf_add); }
static void sql_qjson_sub(sqlite3_context *c, int n, sqlite3_value **v) { _sql_bf_binop(c, n, v, bf_sub); }
static void sql_qjson_mul(sqlite3_context *c, int n, sqlite3_value **v) { _sql_bf_binop(c, n, v, bf_mul); }
static void sql_qjson_div(sqlite3_context *c, int n, sqlite3_value **v) { _sql_bf_binop(c, n, v, bf_div); }
static void sql_qjson_pow(sqlite3_context *c, int n, sqlite3_value **v) { _sql_bf_binop(c, n, v, bf_pow); }

/* Unary operations: neg, abs, sqrt, exp, log, sin, cos, tan, atan, asin, acos */
typedef int (*bf_unop)(bf_t *, const bf_t *, limb_t, bf_flags_t);

static void _sql_bf_unop(sqlite3_context *ctx, int argc, sqlite3_value **argv, bf_unop op) {
    bf_context_t bfctx;
    bf_context_init(&bfctx, _math_realloc, NULL);
    bf_t a, r;
    if (_bf_parse(&bfctx, &a, argv[0])) {
        sqlite3_result_null(ctx);
        bf_context_end(&bfctx);
        return;
    }
    bf_init(&bfctx, &r);
    limb_t prec = (argc > 1) ? _get_prec(argv[1]) : QJSON_DEFAULT_PREC;
    op(&r, &a, prec, BF_RNDN);
    _bf_result_prec(ctx, &r, &bfctx, prec);
    bf_delete(&a); bf_delete(&r);
    bf_context_end(&bfctx);
}

static void sql_qjson_sqrt(sqlite3_context *c, int n, sqlite3_value **v) { _sql_bf_unop(c, n, v, bf_sqrt); }
static void sql_qjson_exp(sqlite3_context *c, int n, sqlite3_value **v)  { _sql_bf_unop(c, n, v, bf_exp); }
static void sql_qjson_log(sqlite3_context *c, int n, sqlite3_value **v)  { _sql_bf_unop(c, n, v, bf_log); }
static void sql_qjson_sin(sqlite3_context *c, int n, sqlite3_value **v)  { _sql_bf_unop(c, n, v, bf_sin); }
static void sql_qjson_cos(sqlite3_context *c, int n, sqlite3_value **v)  { _sql_bf_unop(c, n, v, bf_cos); }
static void sql_qjson_tan(sqlite3_context *c, int n, sqlite3_value **v)  { _sql_bf_unop(c, n, v, bf_tan); }
static void sql_qjson_atan(sqlite3_context *c, int n, sqlite3_value **v) { _sql_bf_unop(c, n, v, bf_atan); }
static void sql_qjson_asin(sqlite3_context *c, int n, sqlite3_value **v) { _sql_bf_unop(c, n, v, bf_asin); }
static void sql_qjson_acos(sqlite3_context *c, int n, sqlite3_value **v) { _sql_bf_unop(c, n, v, bf_acos); }

/* neg and abs don't use the unop template (different bf API) */
static void sql_qjson_neg(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    bf_context_t bfctx;
    bf_context_init(&bfctx, _math_realloc, NULL);
    bf_t a;
    if (_bf_parse(&bfctx, &a, argv[0])) {
        sqlite3_result_null(ctx);
        bf_context_end(&bfctx);
        return;
    }
    bf_neg(&a);
    /* Use enough precision to represent any input exactly */
    limb_t p = (a.len > 0) ? a.len * LIMB_BITS : QJSON_DEFAULT_PREC;
    _bf_result_prec(ctx, &a, &bfctx, p);
    bf_delete(&a);
    bf_context_end(&bfctx);
}

static void sql_qjson_abs(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    bf_context_t bfctx;
    bf_context_init(&bfctx, _math_realloc, NULL);
    bf_t a;
    if (_bf_parse(&bfctx, &a, argv[0])) {
        sqlite3_result_null(ctx);
        bf_context_end(&bfctx);
        return;
    }
    a.sign = 0;
    limb_t p = (a.len > 0) ? a.len * LIMB_BITS : QJSON_DEFAULT_PREC;
    _bf_result_prec(ctx, &a, &bfctx, p);
    bf_delete(&a);
    bf_context_end(&bfctx);
}

/* pi([prec]) */
static void sql_qjson_pi(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    bf_context_t bfctx;
    bf_context_init(&bfctx, _math_realloc, NULL);
    bf_t r;
    bf_init(&bfctx, &r);
    limb_t prec = (argc > 0) ? _get_prec(argv[0]) : QJSON_DEFAULT_PREC;
    bf_const_pi(&r, prec, BF_RNDN);
    _bf_result_prec(ctx, &r, &bfctx, prec);
    bf_delete(&r);
    bf_context_end(&bfctx);
}

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

/* ── Projection: qjson_round_down / qjson_round_up ──────── */
/* Expose libbf directed rounding as SQL scalar functions.
   qjson_round_down(raw_text) → REAL  (largest double <= exact)
   qjson_round_up(raw_text)   → REAL  (smallest double >= exact)  */

static void sql_qjson_round_down(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx); return;
    }
    const char *raw = (const char *)sqlite3_value_text(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    double lo, hi;
    qjson_project(raw, len, &lo, &hi);
    sqlite3_result_double(ctx, lo);
}

static void sql_qjson_round_up(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL) {
        sqlite3_result_null(ctx); return;
    }
    const char *raw = (const char *)sqlite3_value_text(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    double lo, hi;
    qjson_project(raw, len, &lo, &hi);
    sqlite3_result_double(ctx, hi);
}

#endif /* QJSON_USE_LIBBF */

/* ── qjson_closure: transitive closure of binary relation ── */

static void sql_qjson_closure(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    if (argc < 2 || sqlite3_value_type(argv[0]) == SQLITE_NULL ||
        sqlite3_value_type(argv[1]) == SQLITE_NULL) {
        sqlite3_result_null(ctx); return;
    }

    sqlite3_int64 root_id = sqlite3_value_int64(argv[0]);
    const char *set_path = (const char *)sqlite3_value_text(argv[1]);
    const char *prefix = (argc > 2 && sqlite3_value_type(argv[2]) != SQLITE_NULL)
        ? (const char *)sqlite3_value_text(argv[2]) : "qjson_";

    sqlite3 *db = sqlite3_context_db_handle(ctx);

    /* Resolve set_path to find the set's object_id */
    sql_builder sb;
    sb_init(&sb, prefix);
    path_step steps[32];
    int nsteps = parse_path(set_path, steps, 32);
    dstr vid; dstr_init(&vid);
    sb_resolve(&sb, steps, nsteps, "root.id", &vid);
    free_steps(steps, nsteps);

    dstr resolve_sql; dstr_init(&resolve_sql);
    dstr_catf(&resolve_sql,
        "SELECT o.id FROM \"%svalue\" root %s"
        " JOIN \"%sobject\" o ON o.value_id = %s"
        " WHERE root.id = %lld",
        prefix, sb.joins.buf ? sb.joins.buf : "",
        prefix, vid.buf, root_id);

    sqlite3_stmt *stmt = NULL;
    sqlite3_int64 obj_id = 0;
    if (sqlite3_prepare_v2(db, resolve_sql.buf, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        obj_id = sqlite3_column_int64(stmt, 0);
    if (stmt) sqlite3_finalize(stmt);
    dstr_free(&resolve_sql);
    dstr_free(&vid);
    sb_free(&sb);

    if (!obj_id) {
        sqlite3_result_text(ctx, "{}", 2, SQLITE_STATIC);
        return;
    }

    /* WITH RECURSIVE transitive closure */
    char *cte = sqlite3_mprintf(
        "WITH RECURSIVE closure(from_vid, to_vid) AS ("
        " SELECT ai0.value_id, ai1.value_id"
        " FROM \"%sobject_item\" oi"
        " JOIN \"%sarray\" a ON a.value_id = oi.key_id"
        " JOIN \"%sarray_item\" ai0 ON ai0.array_id = a.id AND ai0.idx = 0"
        " JOIN \"%sarray_item\" ai1 ON ai1.array_id = a.id AND ai1.idx = 1"
        " WHERE oi.object_id = %lld"
        " UNION"
        " SELECT c.from_vid, ai1.value_id"
        " FROM closure c"
        " JOIN \"%sobject_item\" oi ON oi.object_id = %lld"
        " JOIN \"%sarray\" a ON a.value_id = oi.key_id"
        " JOIN \"%sarray_item\" ai0 ON ai0.array_id = a.id AND ai0.idx = 0"
        " JOIN \"%sarray_item\" ai1 ON ai1.array_id = a.id AND ai1.idx = 1"
        " WHERE qjson_reconstruct(ai0.value_id, '%s') = qjson_reconstruct(c.to_vid, '%s')"
        ")"
        " SELECT qjson_reconstruct(from_vid, '%s'), qjson_reconstruct(to_vid, '%s')"
        " FROM closure",
        prefix, prefix, prefix, prefix, obj_id,
        prefix, obj_id, prefix, prefix, prefix,
        prefix, prefix, prefix, prefix);

    stmt = NULL;
    dstr result; dstr_init(&result);
    dstr_catc(&result, '{');
    int first = 1;

    if (sqlite3_prepare_v2(db, cte, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *from_s = (const char *)sqlite3_column_text(stmt, 0);
            const char *to_s = (const char *)sqlite3_column_text(stmt, 1);
            if (!first) dstr_catc(&result, ',');
            first = 0;
            dstr_catc(&result, '[');
            dstr_cat(&result, from_s);
            dstr_catc(&result, ',');
            dstr_cat(&result, to_s);
            dstr_catc(&result, ']');
        }
    }
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_free(cte);
    dstr_catc(&result, '}');

    sqlite3_result_text(ctx, result.buf, result.len, sqlite3_free);
}

/* ── Crypto SQL functions (requires LibreSSL) ──────────── */

#ifdef QJSON_USE_CRYPTO
#include "qjson_crypto.h"

static void sql_sha256(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const void *data; int len;
    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) {
        data = sqlite3_value_blob(argv[0]);
        len = sqlite3_value_bytes(argv[0]);
    } else {
        data = sqlite3_value_text(argv[0]);
        len = sqlite3_value_bytes(argv[0]);
    }
    if (!data) { sqlite3_result_null(ctx); return; }
    unsigned char hash[32];
    qjson_sha256(data, len, hash);
    sqlite3_result_blob(ctx, hash, 32, SQLITE_TRANSIENT);
}

static void sql_sha256_hex(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const void *data; int len;
    if (sqlite3_value_type(argv[0]) == SQLITE_BLOB) {
        data = sqlite3_value_blob(argv[0]);
        len = sqlite3_value_bytes(argv[0]);
    } else {
        data = sqlite3_value_text(argv[0]);
        len = sqlite3_value_bytes(argv[0]);
    }
    if (!data) { sqlite3_result_null(ctx); return; }
    unsigned char hash[32];
    qjson_sha256(data, len, hash);
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i*2, "%02x", hash[i]);
    sqlite3_result_text(ctx, hex, 64, SQLITE_TRANSIENT);
}

static void sql_encrypt(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL)
        { sqlite3_result_null(ctx); return; }
    const void *pt = sqlite3_value_blob(argv[0]);
    int pt_len = sqlite3_value_bytes(argv[0]);
    const void *key = sqlite3_value_blob(argv[1]);
    int key_len = sqlite3_value_bytes(argv[1]);
    if (key_len != 32) { sqlite3_result_error(ctx, "key must be 32 bytes", -1); return; }
    /* IV(16) + ciphertext(padded up to +16) + HMAC(32) = up to +64 */
    void *out = sqlite3_malloc(pt_len + 64);
    int r = qjson_aes_encrypt(pt, pt_len, key, out);
    if (r < 0) { sqlite3_free(out); sqlite3_result_null(ctx); return; }
    sqlite3_result_blob(ctx, out, r, sqlite3_free);
}

static void sql_decrypt(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL)
        { sqlite3_result_null(ctx); return; }
    const void *ct = sqlite3_value_blob(argv[0]);
    int ct_len = sqlite3_value_bytes(argv[0]);
    const void *key = sqlite3_value_blob(argv[1]);
    int key_len = sqlite3_value_bytes(argv[1]);
    if (key_len != 32) { sqlite3_result_error(ctx, "key must be 32 bytes", -1); return; }
    if (ct_len < 28) { sqlite3_result_null(ctx); return; }
    void *out = sqlite3_malloc(ct_len);
    int r = qjson_aes_decrypt(ct, ct_len, key, out);
    if (r < 0) { sqlite3_free(out); sqlite3_result_null(ctx); return; }
    sqlite3_result_blob(ctx, out, r, sqlite3_free);
}

static void sql_random_bytes(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    int n = sqlite3_value_int(argv[0]);
    if (n <= 0 || n > 1024*1024) { sqlite3_result_null(ctx); return; }
    void *buf = sqlite3_malloc(n);
    qjson_random_bytes(buf, n);
    sqlite3_result_blob(ctx, buf, n, sqlite3_free);
}

static void sql_hmac(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    if (sqlite3_value_type(argv[0]) == SQLITE_NULL || sqlite3_value_type(argv[1]) == SQLITE_NULL)
        { sqlite3_result_null(ctx); return; }
    const void *data = sqlite3_value_blob(argv[0]);
    int data_len = sqlite3_value_bytes(argv[0]);
    const void *key = sqlite3_value_blob(argv[1]);
    int key_len = sqlite3_value_bytes(argv[1]);
    unsigned char mac[32];
    qjson_hmac_sha256(data, data_len, key, key_len, mac);
    sqlite3_result_blob(ctx, mac, 32, SQLITE_TRANSIENT);
}

static void sql_hkdf(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const void *ikm = sqlite3_value_blob(argv[0]);
    int ikm_len = sqlite3_value_bytes(argv[0]);
    const void *salt = sqlite3_value_type(argv[1]) != SQLITE_NULL ? sqlite3_value_blob(argv[1]) : NULL;
    int salt_len = sqlite3_value_type(argv[1]) != SQLITE_NULL ? sqlite3_value_bytes(argv[1]) : 0;
    const void *info = sqlite3_value_type(argv[2]) != SQLITE_NULL ? sqlite3_value_blob(argv[2]) : NULL;
    int info_len = sqlite3_value_type(argv[2]) != SQLITE_NULL ? sqlite3_value_bytes(argv[2]) : 0;
    int out_len = sqlite3_value_int(argv[3]);
    if (out_len <= 0 || out_len > 8160) { sqlite3_result_null(ctx); return; }
    void *out = sqlite3_malloc(out_len);
    qjson_hkdf(ikm, ikm_len, salt, salt_len, info, info_len, out, out_len);
    sqlite3_result_blob(ctx, out, out_len, sqlite3_free);
}

static void sql_shamir_split(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *secret = (const char *)sqlite3_value_text(argv[0]);
    int m = sqlite3_value_int(argv[1]);
    int n = sqlite3_value_int(argv[2]);
    if (!secret || m < 1 || n < m || n > 255) {
        sqlite3_result_error(ctx, "shamir_split(secret_hex, M, N)", -1); return;
    }
    char **keys = sqlite3_malloc(sizeof(char*) * n);
    if (qjson_shamir_split(secret, m, n, NULL, keys) != 0) {
        sqlite3_free(keys);
        sqlite3_result_error(ctx, "shamir split failed", -1); return;
    }
    /* Build QJSON array: ["share1","share2",...] */
    dstr out; dstr_init(&out);
    dstr_catc(&out, '[');
    for (int i = 0; i < n; i++) {
        if (i > 0) dstr_catc(&out, ',');
        dstr_catc(&out, '"');
        dstr_cat(&out, keys[i]);
        dstr_catc(&out, '"');
        free(keys[i]);
    }
    dstr_catc(&out, ']');
    sqlite3_free(keys);
    sqlite3_result_text(ctx, out.buf, out.len, sqlite3_free);
}

static void sql_shamir_recover(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    /* qjson_shamir_recover(indices_json, keys_json)
       indices: [1, 3, 5]  keys: ["hex1", "hex2", "hex3"] */
    (void)argc;
    const char *idx_json = (const char *)sqlite3_value_text(argv[0]);
    const char *keys_json = (const char *)sqlite3_value_text(argv[1]);
    if (!idx_json || !keys_json) { sqlite3_result_null(ctx); return; }

    /* Simple JSON array parsing for indices and keys */
    int indices[256]; const char *keys[256]; char *key_bufs[256];
    int count = 0;

    /* Parse indices: [1, 3, 5] */
    const char *p = idx_json;
    while (*p && *p != '[') p++;
    if (*p) p++;
    while (*p && *p != ']' && count < 256) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']') break;
        indices[count] = atoi(p);
        while (*p && *p != ',' && *p != ']') p++;
        count++;
    }

    /* Parse keys: ["hex1", "hex2", ...] */
    int kcount = 0;
    p = keys_json;
    while (*p && *p != '[') p++;
    if (*p) p++;
    while (*p && *p != ']' && kcount < count) {
        while (*p == ' ' || *p == ',' || *p == '"') p++;
        if (*p == ']') break;
        const char *start = p;
        while (*p && *p != '"') p++;
        int len = (int)(p - start);
        key_bufs[kcount] = sqlite3_malloc(len + 1);
        memcpy(key_bufs[kcount], start, len);
        key_bufs[kcount][len] = '\0';
        keys[kcount] = key_bufs[kcount];
        if (*p == '"') p++;
        kcount++;
    }

    if (kcount != count || count == 0) {
        for (int i = 0; i < kcount; i++) sqlite3_free(key_bufs[i]);
        sqlite3_result_error(ctx, "indices and keys must have same length", -1);
        return;
    }

    char out[256];
    int r = qjson_shamir_recover(indices, keys, count, 0, NULL, out, sizeof(out));
    for (int i = 0; i < kcount; i++) sqlite3_free(key_bufs[i]);

    if (r != 0) { sqlite3_result_null(ctx); return; }
    sqlite3_result_text(ctx, out, -1, SQLITE_TRANSIENT);
}

static void sql_base64_encode(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const void *data = sqlite3_value_blob(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    if (!data) { sqlite3_result_null(ctx); return; }
    char *out = qjson_base64_encode(data, len);
    sqlite3_result_text(ctx, out, -1, free);
}

static void sql_base64_decode(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *b64 = (const char *)sqlite3_value_text(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    if (!b64) { sqlite3_result_null(ctx); return; }
    size_t out_len;
    void *out = qjson_base64_decode(b64, len, &out_len);
    if (!out) { sqlite3_result_null(ctx); return; }
    sqlite3_result_blob(ctx, out, (int)out_len, free);
}

static void sql_base64url_encode(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const void *data = sqlite3_value_blob(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    if (!data) { sqlite3_result_null(ctx); return; }
    char *out = qjson_base64url_encode(data, len);
    sqlite3_result_text(ctx, out, -1, free);
}

static void sql_base64url_decode(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *b64 = (const char *)sqlite3_value_text(argv[0]);
    int len = sqlite3_value_bytes(argv[0]);
    if (!b64) { sqlite3_result_null(ctx); return; }
    size_t out_len;
    void *out = qjson_base64url_decode(b64, len, &out_len);
    if (!out) { sqlite3_result_null(ctx); return; }
    sqlite3_result_blob(ctx, out, (int)out_len, free);
}

static void sql_jwt_sign(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *payload = (const char *)sqlite3_value_text(argv[0]);
    int pay_len = sqlite3_value_bytes(argv[0]);
    const void *secret = sqlite3_value_blob(argv[1]);
    int sec_len = sqlite3_value_bytes(argv[1]);
    if (!payload || !secret) { sqlite3_result_null(ctx); return; }
    char *jwt = qjson_jwt_sign(payload, pay_len, secret, sec_len);
    if (!jwt) { sqlite3_result_null(ctx); return; }
    sqlite3_result_text(ctx, jwt, -1, free);
}

static void sql_jwt_verify(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *jwt = (const char *)sqlite3_value_text(argv[0]);
    int jwt_len = sqlite3_value_bytes(argv[0]);
    const void *secret = sqlite3_value_blob(argv[1]);
    int sec_len = sqlite3_value_bytes(argv[1]);
    if (!jwt || !secret) { sqlite3_result_null(ctx); return; }
    char *payload = qjson_jwt_verify(jwt, jwt_len, secret, sec_len);
    if (!payload) { sqlite3_result_null(ctx); return; }
    sqlite3_result_text(ctx, payload, -1, free);
}

#endif /* QJSON_USE_CRYPTO */

/* ── Extension entry point ──────────────────────────────── */

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_qjsonext_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    SQLITE_EXTENSION_INIT2(pApi);

    sqlite3_create_function(db, "qjson_decimal_cmp", 2,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_qjson_decimal_cmp, NULL, NULL);

    /* Register all six comparison functions (8 args each, returns 0/1) */
    struct { const char *name; cmp_op_fn fn; } ops[] = {
        {"qjson_cmp_lt", qjson_cmp_lt}, {"qjson_cmp_le", qjson_cmp_le},
        {"qjson_cmp_eq", qjson_cmp_eq}, {"qjson_cmp_ne", qjson_cmp_ne},
        {"qjson_cmp_gt", qjson_cmp_gt}, {"qjson_cmp_ge", qjson_cmp_ge},
    };
    for (int i = 0; i < 6; i++)
        sqlite3_create_function(db, ops[i].name, 8,
            SQLITE_UTF8 | SQLITE_DETERMINISTIC, (void *)ops[i].fn,
            sql_qjson_cmp_dispatch, NULL, NULL);

    sqlite3_create_function(db, "qjson_reconstruct", -1,
        SQLITE_UTF8, NULL, sql_qjson_reconstruct, NULL, NULL);

#ifdef QJSON_USE_LIBBF
    /* Exact arithmetic (all take/return TEXT decimal strings) */
    sqlite3_create_function(db, "qjson_add",  2, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_add, NULL, NULL);
    sqlite3_create_function(db, "qjson_sub",  2, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_sub, NULL, NULL);
    sqlite3_create_function(db, "qjson_mul",  2, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_mul, NULL, NULL);
    sqlite3_create_function(db, "qjson_div", -1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_div, NULL, NULL);
    sqlite3_create_function(db, "qjson_pow", -1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_pow, NULL, NULL);
    sqlite3_create_function(db, "qjson_neg",  1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_neg, NULL, NULL);
    sqlite3_create_function(db, "qjson_abs",  1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_abs, NULL, NULL);
    sqlite3_create_function(db, "qjson_sqrt",-1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_sqrt, NULL, NULL);
    sqlite3_create_function(db, "qjson_exp", -1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_exp, NULL, NULL);
    sqlite3_create_function(db, "qjson_log", -1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_log, NULL, NULL);
    sqlite3_create_function(db, "qjson_sin", -1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_sin, NULL, NULL);
    sqlite3_create_function(db, "qjson_cos", -1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_cos, NULL, NULL);
    sqlite3_create_function(db, "qjson_tan", -1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_tan, NULL, NULL);
    sqlite3_create_function(db, "qjson_atan",-1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_atan, NULL, NULL);
    sqlite3_create_function(db, "qjson_asin",-1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_asin, NULL, NULL);
    sqlite3_create_function(db, "qjson_acos",-1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_acos, NULL, NULL);
    sqlite3_create_function(db, "qjson_pi",  -1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_pi, NULL, NULL);

    /* Constraint-solving: qjson_solve_XX(a, b, c [, prefix]) → 0/1 */
    sqlite3_create_function(db, "qjson_solve_add", -1, SQLITE_UTF8, NULL, sql_solve_add, NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_sub", -1, SQLITE_UTF8, NULL, sql_solve_sub, NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_mul", -1, SQLITE_UTF8, NULL, sql_solve_mul, NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_div", -1, SQLITE_UTF8, NULL, sql_solve_div, NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_pow", -1, SQLITE_UTF8, NULL, sql_solve_pow, NULL, NULL);
    /* Unary: qjson_solve_F(a, b [, prefix]) means F(a) = b */
    sqlite3_create_function(db, "qjson_solve_sqrt", -1, SQLITE_UTF8, NULL, sql_solve_sqrt, NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_exp",  -1, SQLITE_UTF8, NULL, sql_solve_exp,  NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_log",  -1, SQLITE_UTF8, NULL, sql_solve_log,  NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_sin",  -1, SQLITE_UTF8, NULL, sql_solve_sin,  NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_cos",  -1, SQLITE_UTF8, NULL, sql_solve_cos,  NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_tan",  -1, SQLITE_UTF8, NULL, sql_solve_tan,  NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_asin", -1, SQLITE_UTF8, NULL, sql_solve_asin, NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_acos", -1, SQLITE_UTF8, NULL, sql_solve_acos, NULL, NULL);
    sqlite3_create_function(db, "qjson_solve_atan", -1, SQLITE_UTF8, NULL, sql_solve_atan, NULL, NULL);
    sqlite3_create_function(db, "qjson_solve", -1, SQLITE_UTF8, NULL, sql_qjson_solve, NULL, NULL);

    /* Projection: directed rounding via libbf */
    sqlite3_create_function(db, "qjson_round_down", 1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_round_down, NULL, NULL);
    sqlite3_create_function(db, "qjson_round_up",   1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_qjson_round_up,   NULL, NULL);
#endif

    sqlite3_create_function(db, "qjson_closure", -1,
        SQLITE_UTF8, NULL, sql_qjson_closure, NULL, NULL);

#ifdef QJSON_USE_CRYPTO
    sqlite3_create_function(db, "qjson_sha256",     1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_sha256, NULL, NULL);
    sqlite3_create_function(db, "qjson_sha256_hex", 1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_sha256_hex, NULL, NULL);
    sqlite3_create_function(db, "qjson_encrypt",    2, SQLITE_UTF8, NULL, sql_encrypt, NULL, NULL);
    sqlite3_create_function(db, "qjson_decrypt",    2, SQLITE_UTF8, NULL, sql_decrypt, NULL, NULL);
    sqlite3_create_function(db, "qjson_random",     1, SQLITE_UTF8, NULL, sql_random_bytes, NULL, NULL);
    sqlite3_create_function(db, "qjson_hmac",       2, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_hmac, NULL, NULL);
    sqlite3_create_function(db, "qjson_hkdf",       4, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_hkdf, NULL, NULL);
    sqlite3_create_function(db, "qjson_shamir_split",   3, SQLITE_UTF8, NULL, sql_shamir_split, NULL, NULL);
    sqlite3_create_function(db, "qjson_shamir_recover", 2, SQLITE_UTF8, NULL, sql_shamir_recover, NULL, NULL);
    sqlite3_create_function(db, "qjson_base64_encode",    1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_base64_encode, NULL, NULL);
    sqlite3_create_function(db, "qjson_base64_decode",    1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_base64_decode, NULL, NULL);
    sqlite3_create_function(db, "qjson_base64url_encode", 1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_base64url_encode, NULL, NULL);
    sqlite3_create_function(db, "qjson_base64url_decode", 1, SQLITE_UTF8|SQLITE_DETERMINISTIC, NULL, sql_base64url_decode, NULL, NULL);
    sqlite3_create_function(db, "qjson_jwt_sign",   2, SQLITE_UTF8, NULL, sql_jwt_sign, NULL, NULL);
    sqlite3_create_function(db, "qjson_jwt_verify", 2, SQLITE_UTF8, NULL, sql_jwt_verify, NULL, NULL);
#endif

    /* Register qjson_select as eponymous table-valued function */
    sqlite3_create_module(db, "qjson_select", &qs_module, NULL);

    return SQLITE_OK;
}
