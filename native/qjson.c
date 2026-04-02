/* ============================================================
 * qjson.c — QJSON native C API: QJSON + interval projection
 *
 * Arena-allocated recursive descent.  Zero malloc per parse.
 * ============================================================ */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include "qjson.h"

#ifdef QJSON_USE_LIBBF
#include "libbf.h"
/* No global bf_context — allocate per-call for thread safety. */
static void *_qjson_bf_realloc(void *opaque, void *ptr, size_t size) {
    (void)opaque;
    if (size == 0) { free(ptr); return NULL; }
    return realloc(ptr, size);
}
#else
#include <fenv.h>
#pragma STDC FENV_ACCESS ON
#endif

/* ── Arena ───────────────────────────────────────────────── */

void qjson_arena_init(qjson_arena *a, void *buf, size_t cap) {
    a->buf = (char *)buf;
    a->used = 0;
    a->cap = cap;
}

void qjson_arena_reset(qjson_arena *a) { a->used = 0; }

void *qjson_arena_alloc(qjson_arena *a, size_t size) {
    size = (size + 7) & ~(size_t)7; /* align to 8 */
    if (a->used + size > a->cap) return NULL;
    void *p = a->buf + a->used;
    a->used += size;
    return p;
}

static char *arena_strdup(qjson_arena *a, const char *s, int len) {
    char *p = qjson_arena_alloc(a, len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* ── JS64 encode/decode ──────────────────────────────────── */

static const char js64_alpha[] = "$0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";

/* Reverse lookup: ASCII code → 6-bit value (255 = invalid) */
static unsigned char js64_rev[128] = {0};
static int js64_rev_init = 0;

static void js64_init_rev(void) {
    if (js64_rev_init) return;
    memset(js64_rev, 255, sizeof(js64_rev));
    for (int i = 0; i < 64; i++) {
        js64_rev[(unsigned char)js64_alpha[i]] = (unsigned char)i;
    }
    js64_rev_init = 1;
}

int qjson_js64_decode(const char *js64, int js64_len, char *out, int out_cap) {
    js64_init_rev();

    /* Strip whitespace: count valid JS64 chars */
    /* We process in-place, skipping whitespace */
    int blob_len;
    {
        /* js64_len chars (without leading '$') produce this many bytes */
        /* First, count non-whitespace chars */
        int clean_len = 0;
        for (int i = 0; i < js64_len; i++) {
            unsigned char c = (unsigned char)js64[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            clean_len++;
        }
        blob_len = (clean_len * 3) >> 2;
    }

    if (blob_len > out_cap) return -1;

    unsigned int code = 0;
    int bits = 0;
    int byte_idx = 0;

    for (int i = 0; i < js64_len; i++) {
        unsigned char c = (unsigned char)js64[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (c >= 128 || js64_rev[c] == 255) return -1;
        unsigned int v = js64_rev[c];
        code = code | (v << bits);
        bits += 6;

        if (bits >= 8) {
            if (byte_idx < blob_len) {
                out[byte_idx] = (char)(code & 0xFF);
            }
            code = code >> 8;
            bits -= 8;
            byte_idx++;
        }
    }

    return blob_len;
}

int qjson_js64_encode(const char *data, int data_len, char *out, int out_cap) {
    /* Output length (without leading '$'): js64len = ((data_len * 4 + 2) / 3) */
    int js64len = ((data_len * 4 + 2) / 3);
    if (js64len > out_cap) return -1;

    /* The full JS64 encoding produces js64len+1 chars (including leading '$').
       We produce all js64len+1 chars internally but skip the first one ('$'). */
    unsigned int code = 0;
    int bits = 6; /* start with 6 zero bits (the implicit '$') */
    int byte_idx = 0;
    int out_idx = 0;

    for (int i = 0; i <= js64len; i++) {
        char ch = js64_alpha[code & 0x3F];
        if (i > 0) { /* skip the leading '$' */
            if (out_idx < out_cap) out[out_idx] = ch;
            out_idx++;
        }
        code = code >> 6;
        bits -= 6;
        if (bits < 6 || i == js64len) {
            if (byte_idx < data_len) {
                code = code | ((unsigned int)(unsigned char)data[byte_idx] << bits);
                bits += 8;
                byte_idx++;
            }
        }
    }

    return js64len;
}

/* ── Lemon parser driver ────────────────────────────────── */

#include "qjson_lex.h"
#include "qjson_parse_ctx.h"

/* Lemon-generated parser interface */
void *ParseAlloc(void *(*mallocProc)(size_t));
void  ParseFree(void *p, void (*freeProc)(void *));
void  Parse(void *yyp, int yymajor, qjson_token yyminor, qjson_parse_ctx *ctx);

/* ── Public parse (Lemon driver) ─────────────────────────── */

qjson_val *qjson_parse(qjson_arena *a, const char *text, int len) {
    qjson_lexer lex;
    qjson_lex_init(&lex, text, len, a);

    qjson_parse_ctx ctx;
    qjson_ctx_init(&ctx, a);

    void *parser = ParseAlloc(malloc);
    if (!parser) return NULL;

    qjson_token tok;
    int tt;
    while ((tt = qjson_lex(&lex, &tok)) > 0) {
        Parse(parser, tt, tok, &ctx);
        if (ctx.error) break;
    }
    if (!ctx.error && tt == TK_EOF)
        Parse(parser, 0, tok, &ctx);

    ParseFree(parser, free);

    if (ctx.error || ctx.top != 1)
        return NULL;
    return ctx.stack[0];
}

/* ── Type predicates ─────────────────────────────────────── */

int qjson_is_json(const qjson_val *v) {
    if (!v) return 1;
    switch (v->type) {
    case QJSON_NULL: case QJSON_TRUE: case QJSON_FALSE:
    case QJSON_NUMBER: case QJSON_STRING:
        return 1;
    case QJSON_ARRAY:
        for (int i = 0; i < v->arr.count; i++)
            if (!qjson_is_json(v->arr.items[i])) return 0;
        return 1;
    case QJSON_OBJECT:
        for (int i = 0; i < v->obj.count; i++) {
            /* JSON requires string keys */
            if (!v->obj.pairs[i].key || v->obj.pairs[i].key->type != QJSON_STRING)
                return 0;
            if (!qjson_is_json(v->obj.pairs[i].val)) return 0;
        }
        return 1;
    default: /* BIGINT, BIGDECIMAL, BIGFLOAT, BLOB, UNBOUND */
        return 0;
    }
}

int qjson_is_bound(const qjson_val *v) {
    if (!v) return 1;
    if (v->type == QJSON_UNBOUND) return 0;
    if (v->type == QJSON_ARRAY) {
        for (int i = 0; i < v->arr.count; i++)
            if (!qjson_is_bound(v->arr.items[i])) return 0;
    }
    if (v->type == QJSON_OBJECT) {
        for (int i = 0; i < v->obj.count; i++) {
            if (!qjson_is_bound(v->obj.pairs[i].key)) return 0;
            if (!qjson_is_bound(v->obj.pairs[i].val)) return 0;
        }
    }
    return 1;
}

/* ── Stringify ───────────────────────────────────────────── */

static int emit(char *buf, int pos, int cap, const char *s, int len) {
    if (pos + len <= cap) memcpy(buf + pos, s, len);
    return pos + len;
}

static int emit_char(char *buf, int pos, int cap, char c) {
    if (pos < cap) buf[pos] = c;
    return pos + 1;
}

static int emit_str_escaped(char *buf, int pos, int cap, const char *s, int len) {
    pos = emit_char(buf, pos, cap, '"');
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == '"')       { pos = emit(buf, pos, cap, "\\\"", 2); }
        else if (c == '\\') { pos = emit(buf, pos, cap, "\\\\", 2); }
        else if (c == '\n') { pos = emit(buf, pos, cap, "\\n", 2); }
        else if (c == '\r') { pos = emit(buf, pos, cap, "\\r", 2); }
        else if (c == '\t') { pos = emit(buf, pos, cap, "\\t", 2); }
        else                { pos = emit_char(buf, pos, cap, c); }
    }
    pos = emit_char(buf, pos, cap, '"');
    return pos;
}

static int stringify_val(const qjson_val *v, char *buf, int pos, int cap) {
    if (!v) return emit(buf, pos, cap, "null", 4);
    switch (v->type) {
    case QJSON_NULL:  return emit(buf, pos, cap, "null", 4);
    case QJSON_TRUE:  return emit(buf, pos, cap, "true", 4);
    case QJSON_FALSE: return emit(buf, pos, cap, "false", 5);
    case QJSON_NUMBER: {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%.17g", v->num);
        return emit(buf, pos, cap, tmp, n);
    }
    case QJSON_BIGINT:
        pos = emit(buf, pos, cap, v->str.s, v->str.len);
        return emit_char(buf, pos, cap, 'N');
    case QJSON_BIGDECIMAL:
        pos = emit(buf, pos, cap, v->str.s, v->str.len);
        return emit_char(buf, pos, cap, 'M');
    case QJSON_BIGFLOAT:
        pos = emit(buf, pos, cap, v->str.s, v->str.len);
        return emit_char(buf, pos, cap, 'L');
    case QJSON_UNBOUND: {
        pos = emit_char(buf, pos, cap, '?');
        /* Check if name needs quoting (not a simple identifier) */
        int needs_quote = 0;
        if (v->str.len == 0) needs_quote = 0; /* empty → just ? */
        else {
            char c0 = v->str.s[0];
            if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_'))
                needs_quote = 1;
            if (!needs_quote) {
                for (int i = 1; i < v->str.len; i++) {
                    char ci = v->str.s[i];
                    if (!((ci >= 'a' && ci <= 'z') || (ci >= 'A' && ci <= 'Z') ||
                          (ci >= '0' && ci <= '9') || ci == '_'))
                        { needs_quote = 1; break; }
                }
            }
        }
        if (needs_quote)
            return emit_str_escaped(buf, pos, cap, v->str.s, v->str.len);
        return emit(buf, pos, cap, v->str.s, v->str.len);
    }
    case QJSON_BLOB: {
        pos = emit(buf, pos, cap, "0j", 2);
        /* Compute JS64 output length: ((data_len * 4 + 2) / 3) */
        int enc_len = ((v->blob.len * 4 + 2) / 3);
        if (pos + enc_len <= cap) {
            qjson_js64_encode(v->blob.data, v->blob.len, buf + pos, enc_len);
        }
        return pos + enc_len;
    }
    case QJSON_STRING:
        return emit_str_escaped(buf, pos, cap, v->str.s, v->str.len);
    case QJSON_ARRAY:
        pos = emit_char(buf, pos, cap, '[');
        for (int i = 0; i < v->arr.count; i++) {
            if (i > 0) pos = emit_char(buf, pos, cap, ',');
            pos = stringify_val(v->arr.items[i], buf, pos, cap);
        }
        return emit_char(buf, pos, cap, ']');
    case QJSON_OBJECT: {
        pos = emit_char(buf, pos, cap, '{');
        /* Check if set shorthand (all values are TRUE) */
        int is_set = 1;
        for (int i = 0; i < v->obj.count; i++) {
            if (!v->obj.pairs[i].val || v->obj.pairs[i].val->type != QJSON_TRUE)
                { is_set = 0; break; }
        }
        for (int i = 0; i < v->obj.count; i++) {
            if (i > 0) pos = emit_char(buf, pos, cap, ',');
            qjson_val *key = v->obj.pairs[i].key;
            if (is_set) {
                /* Set shorthand: use bare ident for simple string keys */
                if (key && key->type == QJSON_STRING) {
                    int bare = 1;
                    if (key->str.len == 0) bare = 0;
                    else {
                        char c0 = key->str.s[0];
                        if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_' || c0 == '$'))
                            bare = 0;
                        if (bare) {
                            for (int j = 1; j < key->str.len; j++) {
                                char cj = key->str.s[j];
                                if (!((cj >= 'a' && cj <= 'z') || (cj >= 'A' && cj <= 'Z') ||
                                      (cj >= '0' && cj <= '9') || cj == '_' || cj == '$'))
                                    { bare = 0; break; }
                            }
                        }
                        if (bare && key->str.len == 4 && memcmp(key->str.s, "true", 4) == 0) bare = 0;
                        if (bare && key->str.len == 4 && memcmp(key->str.s, "null", 4) == 0) bare = 0;
                        if (bare && key->str.len == 5 && memcmp(key->str.s, "false", 5) == 0) bare = 0;
                    }
                    if (bare) pos = emit(buf, pos, cap, key->str.s, key->str.len);
                    else pos = emit_str_escaped(buf, pos, cap, key->str.s, key->str.len);
                } else {
                    pos = stringify_val(key, buf, pos, cap);
                }
            } else {
                /* Map syntax: string keys always quoted, non-string keys serialized as values */
                if (key && key->type == QJSON_STRING)
                    pos = emit_str_escaped(buf, pos, cap, key->str.s, key->str.len);
                else
                    pos = stringify_val(key, buf, pos, cap);
                pos = emit_char(buf, pos, cap, ':');
                pos = stringify_val(v->obj.pairs[i].val, buf, pos, cap);
            }
        }
        return emit_char(buf, pos, cap, '}');
    }
    /* ── View / Datalog types ───────────────────────────────── */
    case QJSON_VIEW:
        pos = stringify_val(v->view.pattern, buf, pos, cap);
        pos = emit(buf, pos, cap, " where ", 7);
        pos = stringify_val(v->view.cond, buf, pos, cap);
        return pos;
    case QJSON_MATCH:
        pos = stringify_val(v->match.pattern, buf, pos, cap);
        pos = emit(buf, pos, cap, " in ", 4);
        pos = stringify_val(v->match.source, buf, pos, cap);
        return pos;
    case QJSON_BINOP:
        pos = stringify_val(v->binop.left, buf, pos, cap);
        pos = emit_char(buf, pos, cap, ' ');
        pos = emit(buf, pos, cap, v->binop.op, v->binop.op_len);
        pos = emit_char(buf, pos, cap, ' ');
        pos = stringify_val(v->binop.right, buf, pos, cap);
        return pos;
    case QJSON_NOTOP:
        pos = emit(buf, pos, cap, "not ", 4);
        pos = stringify_val(v->notop.operand, buf, pos, cap);
        return pos;
    case QJSON_EQUATION:
        pos = stringify_val(v->equation.left, buf, pos, cap);
        pos = emit(buf, pos, cap, " = ", 3);
        pos = stringify_val(v->equation.right, buf, pos, cap);
        return pos;
    case QJSON_ARITH: {
        int need_parens = v->binop.left && v->binop.left->type == QJSON_ARITH;
        if (need_parens) pos = emit_char(buf, pos, cap, '(');
        pos = stringify_val(v->binop.left, buf, pos, cap);
        if (need_parens) pos = emit_char(buf, pos, cap, ')');
        pos = emit_char(buf, pos, cap, ' ');
        pos = emit(buf, pos, cap, v->binop.op, v->binop.op_len);
        pos = emit_char(buf, pos, cap, ' ');
        need_parens = v->binop.right && v->binop.right->type == QJSON_ARITH;
        if (need_parens) pos = emit_char(buf, pos, cap, '(');
        pos = stringify_val(v->binop.right, buf, pos, cap);
        if (need_parens) pos = emit_char(buf, pos, cap, ')');
        return pos;
    }
    default: break;
    }
    return pos;
}

int qjson_stringify(const qjson_val *v, char *buf, int cap) {
    int n = stringify_val(v, buf, 0, cap > 0 ? cap - 1 : 0);
    if (cap > 0) buf[n < cap ? n : cap - 1] = '\0';
    return n;
}

/* ── Object key lookup ───────────────────────────────────── */

qjson_val *qjson_obj_get(const qjson_val *v, const char *key) {
    if (!v || v->type != QJSON_OBJECT) return NULL;
    int klen = (int)strlen(key);
    for (int i = 0; i < v->obj.count; i++) {
        qjson_val *k = v->obj.pairs[i].key;
        if (k && k->type == QJSON_STRING &&
            k->str.len == klen && memcmp(k->str.s, key, klen) == 0)
            return v->obj.pairs[i].val;
    }
    return NULL;
}

/* ── Interval projection ────────────────────────────────── */

#ifdef QJSON_USE_LIBBF

void qjson_project(const char *raw, int len, double *lo, double *hi) {
    char buf[320];
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, raw, len);
    buf[len] = '\0';

    bf_context_t ctx;
    bf_context_init(&ctx, _qjson_bf_realloc, NULL);
    bf_t val;
    bf_init(&ctx, &val);
    bf_atof(&val, buf, NULL, 10, BF_PREC_INF, BF_RNDN);
    bf_get_float64(&val, lo, BF_RNDD);
    bf_get_float64(&val, hi, BF_RNDU);
    bf_delete(&val);
    bf_context_end(&ctx);
}

#else /* fesetround + strtod fallback */

void qjson_project(const char *raw, int len, double *lo, double *hi) {
    char buf[320];
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, raw, len);
    buf[len] = '\0';

    int saved = fegetround();

    fesetround(FE_DOWNWARD);
    volatile double vlo = strtod(buf, NULL);

    fesetround(FE_UPWARD);
    volatile double vhi = strtod(buf, NULL);

    fesetround(saved);
    *lo = vlo;
    *hi = vhi;
}

#endif

void qjson_val_project(const qjson_val *v, double *lo, double *hi) {
    if (!v) { *lo = *hi = 0; return; }
    switch (v->type) {
    case QJSON_NUMBER:
        *lo = *hi = v->num;
        return;
    case QJSON_BIGINT:
    case QJSON_BIGDECIMAL:
    case QJSON_BIGFLOAT:
        qjson_project(v->str.s, v->str.len, lo, hi);
        return;
    case QJSON_UNBOUND:
        *lo = -INFINITY;
        *hi = INFINITY;
        return;
    case QJSON_BLOB:
        *lo = *hi = 0;
        return;
    default:
        *lo = *hi = 0;
        return;
    }
}

/* ── Decimal string comparison ──────────────────────────── */

#ifdef QJSON_USE_LIBBF

int qjson_decimal_cmp(const char *a, int a_len, const char *b, int b_len) {
    char ab[320], bb[320];
    if (a_len >= (int)sizeof(ab)) a_len = (int)sizeof(ab) - 1;
    if (b_len >= (int)sizeof(bb)) b_len = (int)sizeof(bb) - 1;
    memcpy(ab, a, a_len); ab[a_len] = '\0';
    memcpy(bb, b, b_len); bb[b_len] = '\0';

    bf_context_t ctx;
    bf_context_init(&ctx, _qjson_bf_realloc, NULL);
    bf_t av, bv;
    bf_init(&ctx, &av);
    bf_init(&ctx, &bv);
    bf_atof(&av, ab, NULL, 10, BF_PREC_INF, BF_RNDN);
    bf_atof(&bv, bb, NULL, 10, BF_PREC_INF, BF_RNDN);
    int r = bf_cmp(&av, &bv);
    bf_delete(&av);
    bf_delete(&bv);
    bf_context_end(&ctx);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

#else /* string-based fallback */

static int abs_decimal_cmp(const char *a, int al, const char *b, int bl) {
    /* Find decimal points */
    int a_dot = -1, b_dot = -1;
    for (int i = 0; i < al; i++) if (a[i] == '.') { a_dot = i; break; }
    for (int i = 0; i < bl; i++) if (b[i] == '.') { b_dot = i; break; }

    int a_int_len = a_dot >= 0 ? a_dot : al;
    int b_int_len = b_dot >= 0 ? b_dot : bl;

    /* Skip leading zeros */
    int ai = 0, bi = 0;
    while (ai < a_int_len && a[ai] == '0') ai++;
    while (bi < b_int_len && b[bi] == '0') bi++;

    int a_sig = a_int_len - ai;
    int b_sig = b_int_len - bi;

    /* Compare integer part length (more digits = larger) */
    if (a_sig != b_sig) return a_sig > b_sig ? 1 : -1;

    /* Compare integer part digits */
    for (int i = 0; i < a_sig; i++) {
        if (a[ai + i] != b[bi + i])
            return a[ai + i] > b[bi + i] ? 1 : -1;
    }

    /* Integer parts equal — compare fractional digits */
    const char *af = a_dot >= 0 ? a + a_dot + 1 : "";
    int af_len = a_dot >= 0 ? al - a_dot - 1 : 0;
    const char *bf = b_dot >= 0 ? b + b_dot + 1 : "";
    int bf_len = b_dot >= 0 ? bl - b_dot - 1 : 0;

    int max_frac = af_len > bf_len ? af_len : bf_len;
    for (int i = 0; i < max_frac; i++) {
        char ac = i < af_len ? af[i] : '0';
        char bc = i < bf_len ? bf[i] : '0';
        if (ac != bc) return ac > bc ? 1 : -1;
    }

    return 0;
}

int qjson_decimal_cmp(const char *a, int a_len, const char *b, int b_len) {
    int a_neg = (a_len > 0 && a[0] == '-');
    int b_neg = (b_len > 0 && b[0] == '-');
    if (a_neg && !b_neg) return -1;
    if (!a_neg && b_neg) return 1;

    int cmp = abs_decimal_cmp(
        a + a_neg, a_len - a_neg,
        b + b_neg, b_len - b_neg
    );
    return a_neg ? -cmp : cmp;
}

#endif

/* ── Interval comparison ────────────────────────────────── */

#ifdef QJSON_USE_LIBBF
/* Compare exact double value against decimal string using libbf. */
static int _cmp_double_vs_str(double dval, const char *str, int str_len) {
    char buf[320];
    if (str_len >= (int)sizeof(buf)) str_len = (int)sizeof(buf) - 1;
    memcpy(buf, str, str_len); buf[str_len] = '\0';

    bf_context_t ctx;
    bf_context_init(&ctx, _qjson_bf_realloc, NULL);
    bf_t a, b;
    bf_init(&ctx, &a);
    bf_init(&ctx, &b);
    bf_set_float64(&a, dval);
    bf_atof(&b, buf, NULL, 10, BF_PREC_INF, BF_RNDN);
    int r = bf_cmp(&a, &b);
    bf_delete(&a);
    bf_delete(&b);
    bf_context_end(&ctx);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}
#else
/* Fallback: convert double to decimal string, compare strings. */
static int _cmp_double_vs_str(double dval, const char *str, int str_len) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%.21g", dval);
    return -abs_decimal_cmp(str, str_len, buf, n);
}
#endif

/* Internal: check if both unbounds have the same name */
static int _unbound_same_name(const char *a_str, int a_len,
                              const char *b_str, int b_len) {
    if (a_len != b_len) return 0;
    if (a_len == 0) return 1; /* both anonymous empty-name */
    if (!a_str || !b_str) return (!a_str && !b_str);
    return memcmp(a_str, b_str, a_len) == 0;
}

/* Internal: compute -1/0/1 ordering for bound (non-unbound) values */
static int _qjson_cmp_ord(double a_lo, const char *a_str, int a_str_len, double a_hi,
                           double b_lo, const char *b_str, int b_str_len, double b_hi) {
    if (a_hi < b_lo) return -1;
    if (a_lo > b_hi) return  1;
    if (a_lo == a_hi && b_lo == b_hi) return 0;
    /* Overlap zone */
    if (a_lo == a_hi)
        return _cmp_double_vs_str(a_lo, b_str, b_str_len);
    if (b_lo == b_hi)
        return -_cmp_double_vs_str(b_lo, a_str, a_str_len);
    return qjson_decimal_cmp(a_str, a_str_len, b_str, b_str_len);
}

/*
 * Six comparison functions.  Each returns 0 or 1.
 *
 * Unbound rules:
 *   Both unbound, same name → treat as equal (like 42 vs 42)
 *   Otherwise if either is unbound → always 1 (unknown satisfies all)
 */

int qjson_cmp_eq(QJSON_CMP_ARGS) {
    if (a_type == QJSON_UNBOUND || b_type == QJSON_UNBOUND) {
        if (a_type == QJSON_UNBOUND && b_type == QJSON_UNBOUND)
            return _unbound_same_name(a_str, a_str_len, b_str, b_str_len) ? 1 : 1;
            /* same name → eq=1; diff name → unknown=1 */
        return 1; /* unbound vs concrete → could be equal */
    }
    return _qjson_cmp_ord(a_lo, a_str, a_str_len, a_hi,
                           b_lo, b_str, b_str_len, b_hi) == 0 ? 1 : 0;
}

int qjson_cmp_ne(QJSON_CMP_ARGS) {
    if (a_type == QJSON_UNBOUND || b_type == QJSON_UNBOUND) {
        if (a_type == QJSON_UNBOUND && b_type == QJSON_UNBOUND)
            return _unbound_same_name(a_str, a_str_len, b_str, b_str_len) ? 0 : 1;
        return 1; /* unbound vs concrete → could be not-equal */
    }
    return _qjson_cmp_ord(a_lo, a_str, a_str_len, a_hi,
                           b_lo, b_str, b_str_len, b_hi) != 0 ? 1 : 0;
}

int qjson_cmp_lt(QJSON_CMP_ARGS) {
    if (a_type == QJSON_UNBOUND || b_type == QJSON_UNBOUND) {
        if (a_type == QJSON_UNBOUND && b_type == QJSON_UNBOUND)
            return _unbound_same_name(a_str, a_str_len, b_str, b_str_len) ? 0 : 1;
        return 1;
    }
    return _qjson_cmp_ord(a_lo, a_str, a_str_len, a_hi,
                           b_lo, b_str, b_str_len, b_hi) < 0 ? 1 : 0;
}

int qjson_cmp_le(QJSON_CMP_ARGS) {
    if (a_type == QJSON_UNBOUND || b_type == QJSON_UNBOUND) {
        /* same-name: le is true (equal implies <=); diff/concrete: unknown=1 */
        return 1;
    }
    return _qjson_cmp_ord(a_lo, a_str, a_str_len, a_hi,
                           b_lo, b_str, b_str_len, b_hi) <= 0 ? 1 : 0;
}

int qjson_cmp_gt(QJSON_CMP_ARGS) {
    if (a_type == QJSON_UNBOUND || b_type == QJSON_UNBOUND) {
        if (a_type == QJSON_UNBOUND && b_type == QJSON_UNBOUND)
            return _unbound_same_name(a_str, a_str_len, b_str, b_str_len) ? 0 : 1;
        return 1;
    }
    return _qjson_cmp_ord(a_lo, a_str, a_str_len, a_hi,
                           b_lo, b_str, b_str_len, b_hi) > 0 ? 1 : 0;
}

int qjson_cmp_ge(QJSON_CMP_ARGS) {
    if (a_type == QJSON_UNBOUND || b_type == QJSON_UNBOUND) {
        return 1; /* same-name: ge true (equal); diff/concrete: unknown=1 */
    }
    return _qjson_cmp_ord(a_lo, a_str, a_str_len, a_hi,
                           b_lo, b_str, b_str_len, b_hi) >= 0 ? 1 : 0;
}
