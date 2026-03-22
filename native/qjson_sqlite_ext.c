/*
 * qjson_sqlite_ext.c — SQLite/SQLCipher extension for QJSON.
 *
 * Scalar functions:
 *   qjson_decimal_cmp(a, b) → INTEGER    exact decimal comparison (libbf)
 *   qjson_cmp(a_lo,a_hi,a_str, b_lo,b_hi,b_str) → INTEGER  3-tier comparison
 *   qjson_reconstruct(vid, prefix) → TEXT   value_id → canonical QJSON text
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
                " JOIN \"%sobject_item\" %s ON %s.object_id = %s.id AND %s.key = '%s'",
                sb->prefix, o, o, cur.buf,
                sb->prefix, oi, oi, o, oi, steps[i].val);
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
                dstr_catf(&cur, "%s.value_id", existing);
            } else {
                char a[32], ai[32];
                snprintf(a, sizeof(a), "a_%d", sb->alias_num);
                sb->alias_num++;
                snprintf(ai, sizeof(ai), "ai_%d", sb->alias_num);
                dstr_catf(&sb->joins,
                    " JOIN \"%sarray\" %s ON %s.value_id = %s"
                    " JOIN \"%sarray_item\" %s ON %s.array_id = %s.id",
                    sb->prefix, a, a, cur.buf,
                    sb->prefix, ai, ai, a);
                sb_add_var(sb, steps[i].val, ai);
                cur.len = 0; cur.buf[0] = '\0';
                dstr_catf(&cur, "%s.value_id", ai);
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

            /* value — dynamically collected */
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

            /* resolve path */
            path_step wsteps[32];
            int nwsteps = parse_path(path_str, wsteps, 32);
            dstr wvid; dstr_init(&wvid);
            sb_resolve(sb, wsteps, nwsteps, "root.id", &wvid);
            free_steps(wsteps, nwsteps);
            sqlite3_free(path_str);

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
                /* numeric: strip suffix, project interval */
                dstr raw; dstr_init(&raw);
                dstr_cat(&raw, val.buf);
                if (raw.len > 0) {
                    char last = raw.buf[raw.len - 1];
                    if (last == 'N' || last == 'M' || last == 'L' ||
                        last == 'n' || last == 'm' || last == 'l')
                        raw.buf[--raw.len] = '\0';
                }

                double q_lo, q_hi;
                qjson_project(raw.buf, raw.len, &q_lo, &q_hi);
                int exact = (q_lo == q_hi);

                sb->alias_num++;
                char na[32]; snprintf(na, sizeof(na), "wn_%d", sb->alias_num);
                dstr_catf(&sb->joins, " JOIN \"%snumber\" %s ON %s.value_id = %s",
                          sb->prefix, na, na, wvid.buf);

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
                    /* Ordering: bracket pre-filter + qjson_cmp exact fallback */
                    dstr q_str_sql; dstr_init(&q_str_sql);
                    if (exact) dstr_cat(&q_str_sql, "NULL");
                    else       dstr_catf(&q_str_sql, "'%s'", raw.buf);

                    if (strcmp(op, ">") == 0) {
                        dstr_catf(out, " (%s.hi > %.17g AND qjson_cmp(%s.lo, %s.hi, %s.str, %.17g, %.17g, %s) > 0)",
                                  na, q_lo, na, na, na, q_lo, q_hi, q_str_sql.buf);
                    } else if (strcmp(op, ">=") == 0) {
                        dstr_catf(out, " (%s.hi >= %.17g AND qjson_cmp(%s.lo, %s.hi, %s.str, %.17g, %.17g, %s) >= 0)",
                                  na, q_lo, na, na, na, q_lo, q_hi, q_str_sql.buf);
                    } else if (strcmp(op, "<") == 0) {
                        dstr_catf(out, " (%s.lo < %.17g AND qjson_cmp(%s.lo, %s.hi, %s.str, %.17g, %.17g, %s) < 0)",
                                  na, q_hi, na, na, na, q_lo, q_hi, q_str_sql.buf);
                    } else if (strcmp(op, "<=") == 0) {
                        dstr_catf(out, " (%s.lo <= %.17g AND qjson_cmp(%s.lo, %s.hi, %s.str, %.17g, %.17g, %s) <= 0)",
                                  na, q_hi, na, na, na, q_lo, q_hi, q_str_sql.buf);
                    }
                    dstr_free(&q_str_sql);
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
        sql = sqlite3_mprintf("SELECT key, value_id FROM \"%sobject_item\" WHERE object_id = %lld ORDER BY key", prefix, obj_id);
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            int first = 1;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) dstr_catc(&out, ',');
                first = 0;
                const char *key = (const char *)sqlite3_column_text(stmt, 0);
                sqlite3_int64 cvid = sqlite3_column_int64(stmt, 1);
                dstr_catc(&out, '"');
                for (int i = 0; key && key[i]; i++) {
                    if (key[i] == '"') dstr_cat(&out, "\\\"");
                    else if (key[i] == '\\') dstr_cat(&out, "\\\\");
                    else dstr_catc(&out, key[i]);
                }
                dstr_cat(&out, "\":");
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

static void sql_qjson_cmp(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    double a_lo = sqlite3_value_double(argv[0]);
    double a_hi = sqlite3_value_double(argv[1]);
    const char *a_str = (const char *)sqlite3_value_text(argv[2]);
    int a_len = a_str ? sqlite3_value_bytes(argv[2]) : 0;
    double b_lo = sqlite3_value_double(argv[3]);
    double b_hi = sqlite3_value_double(argv[4]);
    const char *b_str = (const char *)sqlite3_value_text(argv[5]);
    int b_len = b_str ? sqlite3_value_bytes(argv[5]) : 0;
    sqlite3_result_int(ctx, qjson_cmp(a_lo, a_hi, a_str, a_len, b_lo, b_hi, b_str, b_len));
}

/* ── Extension entry point ──────────────────────────────── */

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_qjsonext_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    SQLITE_EXTENSION_INIT2(pApi);

    sqlite3_create_function(db, "qjson_decimal_cmp", 2,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_qjson_decimal_cmp, NULL, NULL);
    sqlite3_create_function(db, "qjson_cmp", 6,
        SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sql_qjson_cmp, NULL, NULL);
    sqlite3_create_function(db, "qjson_reconstruct", -1,
        SQLITE_UTF8, NULL, sql_qjson_reconstruct, NULL, NULL);

    /* Register qjson_select as eponymous table-valued function */
    sqlite3_create_module(db, "qjson_select", &qs_module, NULL);

    return SQLITE_OK;
}
